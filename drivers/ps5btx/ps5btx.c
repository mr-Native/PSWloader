/* SPDX-License-Identifier: GPL-2.0-only
 * ps5btx v0.7 - PS5 Bluetooth for native Windows 11: KMDF bus driver for the Sony Salina xHCI (PCI 104D:9108) that runs the
 * NXP/Marvell IW620 Bluetooth function (USB 1286:2059) and hands its HCI traffic to the Windows Bluetooth stack as a
 * Bluetooth extensible transport (child MS_BTHX_BTHMINI, inbox bth.inf / BthMini.sys).
 * The Salina PHY set-up, the controller reset and the polling follow the PS5 Linux xHCI helper drivers/usb/host/xhci-ps5.c
 * (Copyright c0w-ar, GPL-2.0) - this file is therefore GPL-2.0-only.
 *
 * History: v0.7 (25.11.2026; v0.7 log opened late when C: is not ready at boot; v0.6 capabilities as BthMini demands (SCO bypass, 1 channel); v0.5 PDO queues not power-managed (BthMini deadlock); v0.2 IOCTL counters; v0.3 IOCTL trace; v0.4 BthMini contract from its code: IRP_MJ_DEVICE_CONTROL,
 * READ input = packet type, separate event/ACL reads, WRITE output ULONG = bytes written) - PS5 Bluetooth for native Windows: KMDF bus driver on the Salina xHCI 104D:9108.
 *
 * The IW620 Bluetooth function is USB device 1286:2059 on this controller. Windows' usbxhci cannot run the Salina xHCI
 * (PHY set-up through the SPCIE glue, own reset, no usable interrupt), so this driver runs it itself, polling, for exactly
 * that one device - the way PS5 Linux does (drivers/usb/host/xhci-ps5.c) - and hands the HCI traffic to Windows' own
 * Bluetooth stack through the Bluetooth extensible transport: a child PDO with the hardware ID MS_BTHX_BTHMINI, on which
 * the inbox BthMini.sys loads and sends IOCTL_BTHX_* (bthxddi.h).
 *   commands   -> control OUT class request (0x20, 0) to interface 0      events <- interrupt IN endpoint
 *   ACL out    -> bulk OUT                                                 ACL in <- bulk IN
 *   SCO        not offered (isochronous endpoints are not used)
 * One poll thread owns the controller (1 ms): it submits queued writes, keeps one interrupt IN and one bulk IN transfer
 * armed and completes pending reads. The controller is never put into D3 (Linux: the Salina xHCI does not come back).
 * Diagnostics: C:\PSWloader\logs\ps5btx.log (collected by PSWSelfCheck) (the first 400 HCI packets in hex, then counters every 60 s). */
#include <ntddk.h>
#include <wdf.h>
#include <ntstrsafe.h>
#include <bthxddi.h>

#define ECAM_BASE 0xF0000000ULL
#define TAG 'tb5p'
typedef struct { ULONG d[4]; } TRB;
typedef struct { TRB *v; ULONGLONG p; ULONG idx, cyc; } RING;

/* ------------------------------------------------------------------------------------------------ state (one device) */
static volatile UCHAR *gXhci, *gSpBar2, *gSpBar4; static ULONG gXhciLen;
static ULONG gChip, gLenWord = 0xFFFFFFFF, gOp, gRt, gDb, gMaxPorts, gCsz, gSlot, gPort, gSpeed, gMps0;
static RING gCmd, gEv, gEp0, gIntIn, gBulkIn, gBulkOut;
static ULONGLONG gErstPa, gDcbaaPa, gOutPa, gInPa, gBufPa; static ULONGLONG *gDcbaa; static UCHAR *gOutCtx, *gInCtx, *gBuf;
static UCHAR gEpInt, gEpBin, gEpBout, gIvInt, gCfgVal; static USHORT gMpsInt, gMpsBin, gMpsBout;
static ULONG gDciInt, gDciBin, gDciBout;
static PVOID gPages[48]; static ULONG gNPages; static BOOLEAN gRunning;
static HANDLE gLog; static ULONG gLogged;
static ULONG gNEv, gNAclIn, gNAclOut, gNCmd, gNDrop, gNErr;
typedef struct { ULONG code, major, outLen, inLen; LONG st; } IOTR; static IOTR gIoTr[32]; static volatile LONG gIoTrN, gIoTrShown;   /* v0.3 */
static volatile LONG gIoN[6], gIoOther; static volatile LONG gIoLastCode, gIoLastSt, gIoLastOut, gIoLastIn;   /* v0.2: IOCTL counters (any IRQL) */
/* buffers inside gBuf (one page): 0 control data, 1024 interrupt IN (event), 2048 bulk IN (ACL, 1024 bytes + 4) */
#define EVB (gBuf + 1024)
#define EVP (gBufPa + 1024)
static UCHAR *gAclBuf; static ULONGLONG gAclPa;      /* separate page for ACL IN (up to 4096) */
static UCHAR *gOutBuf; static ULONGLONG gOutBufPa;   /* separate page for ACL OUT / command data */
#define ACL_IN_MAX 1028
static ULONG gEvArmed, gAclArmed;
static UCHAR gEvAsm[260]; static ULONG gEvAsmLen;    /* event reassembly across interrupt transfers (one packet each) */
static UCHAR gAclAsm[ACL_IN_MAX + 512]; static ULONG gAclAsmLen;   /* ACL reassembly across bulk packets */

static WDFDEVICE gFdo, gPdo; static WDFQUEUE gReadEvQ, gReadAclQ, gWriteQ;   /* v0.4: BthMini reads per packet type */
static PKTHREAD gThread; static KEVENT gWake; static volatile LONG gStop;
static KSPIN_LOCK gFifoLock;
#define FIFO_N 64
typedef struct { ULONG len; UCHAR type; UCHAR data[ACL_IN_MAX]; } PKT;
typedef struct { PKT p[FIFO_N]; ULONG head, count; } FIFO; static FIFO gFifoEv, gFifoAcl;   /* static, ~130 KB .bss */
#define gFifoCount (gFifoEv.count + gFifoAcl.count)

/* ------------------------------------------------------------------------------------------------ log */
static void LogF(const char *fmt, ...)
{
    char b[600]; va_list a; IO_STATUS_BLOCK io; size_t n = 0;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return;
    va_start(a, fmt); RtlStringCbVPrintfA(b, sizeof b - 2, fmt, a); va_end(a);
    RtlStringCbLengthA(b, sizeof b, &n); b[n++] = '\r'; b[n++] = '\n';
    if (gLog) ZwWriteFile(gLog, NULL, NULL, NULL, &io, b, (ULONG)n, NULL, NULL);
}
static void LogOpen(void)
{
    UNICODE_STRING p; OBJECT_ATTRIBUTES oa; IO_STATUS_BLOCK io;
    RtlInitUnicodeString(&p, L"\\??\\C:\\PSWloader\\logs\\ps5btx.log");
    InitializeObjectAttributes(&oa, &p, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateFile(&gLog, FILE_APPEND_DATA | SYNCHRONIZE, &oa, &io, NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ,
                                 FILE_OPEN_IF, FILE_SYNCHRONOUS_IO_NONALERT | FILE_WRITE_THROUGH, NULL, 0))) gLog = NULL;
}
static void LogHex(const char *tag, UCHAR type, const UCHAR *d, ULONG n)
{
    char hex[3 * 40 + 1]; static const char hx[] = "0123456789abcdef"; ULONG k = 0;
    if (gLogged >= 400) return; gLogged++;
    for (ULONG i = 0; i < n && i < 40; i++) { hex[k++] = hx[d[i] >> 4]; hex[k++] = hx[d[i] & 15]; hex[k++] = ' '; }
    hex[k] = 0; LogF("%s type %u len %u: %s%s", tag, type, n, hex, n > 40 ? "..." : "");
}
static void Sleep(ULONG us) { LARGE_INTEGER t; t.QuadPart = -(LONGLONG)us * 10; KeDelayExecutionThread(KernelMode, FALSE, &t); }

/* ------------------------------------------------------------------------------------------------ PCI / Salina (as ps5bt.c) */
static ULONGLONG CfgBar(volatile UCHAR *c, ULONG i)
{
    ULONG lo = *(volatile ULONG *)(c + 0x10 + 4 * i); ULONGLONG v = lo & ~0xFULL;
    if ((lo & 6) == 4) v |= (ULONGLONG)*(volatile ULONG *)(c + 0x14 + 4 * i) << 32;
    return v;
}
static volatile UCHAR *FindSpcie(void)   /* config space of 104D:9107 (bus 0x40 first) */
{
    for (ULONG n = 0; n < 0x80; n++) {
        ULONG bus = (n + 0x40) & 0x7F; PHYSICAL_ADDRESS pa; pa.QuadPart = (LONGLONG)(ECAM_BASE + ((ULONGLONG)bus << 20));
        volatile UCHAR *b = (volatile UCHAR *)MmMapIoSpace(pa, 1 << 20, MmNonCached); if (!b) continue;
        for (ULONG df = 0; df < 256; df++) {
            ULONG id = *(volatile ULONG *)(b + ((ULONG_PTR)df << 12));
            if (id == 0x9107104DUL) { MmUnmapIoSpace((PVOID)b, 1 << 20); pa.QuadPart += (LONGLONG)df << 12; return (volatile UCHAR *)MmMapIoSpace(pa, 4096, MmNonCached); }
            if ((df & 7) == 0 && id == 0xFFFFFFFF) df += 7;
        }
        MmUnmapIoSpace((PVOID)b, 1 << 20);
    }
    return NULL;
}
static ULONG XR(ULONG off) { return *(volatile ULONG *)(gXhci + off); }
static void XW(ULONG off, ULONG v) { *(volatile ULONG *)(gXhci + off) = v; }
static void XOr(ULONG off, ULONG v) { XW(off, XR(off) | v); }
static void XAnd(ULONG off, ULONG v) { XW(off, XR(off) & v); }
static void XSet(ULONG off, int bit) { XOr(off, 1UL << bit); }
static void XClr(ULONG off, int bit) { XAnd(off, ~(1UL << bit)); }
static void XTog(ULONG off, int bit) { XSet(off, bit); XClr(off, bit); }
static void PhySet(ULONG r, int bit) { volatile ULONG *p = (volatile ULONG *)(gSpBar2 + r); *p = *p | (1UL << bit); }
static void PhyClr(ULONG r, int bit) { volatile ULONG *p = (volatile ULONG *)(gSpBar2 + r); *p = *p & ~(1UL << bit); }
static ULONG CfgRd(ULONG r) { return *(volatile ULONG *)(gSpBar4 + 0x4000 + r); }
static ULONG UsbPhyOff(int n) { return n == 1 ? 0x124000 : n == 2 ? 0x128000 : 0x120000; }
static ULONG TraceTrim(int n, ULONG len)
{
    if (len < 3) return 8; if (len < 6) return 7; if (len < 8) return 6; if (len < 10) return 5; if (len < 13) return 4;
    return n == 1 ? 6 : n == 2 ? 7 : 4;
}
static void PhyStep1(int n)
{
    ULONG o = UsbPhyOff(n), v = CfgRd(0x6c), ef, len;
    ef = ((v >> 0x19 & v >> 0x11 & 1) == 0) ? 0x98 : (CfgRd(0x44) >> (n * 5 + 0x10)) & 0x1f;
    XW(o + 0x800, 0x480a); XW(o + 0x814, 0x10a01000); XClr(o + 0x818, 23); XSet(o + 0x800, 5); XClr(o + 0x868, 18);
    XAnd(o + 0x804, 0xff07ffff); XOr(o + 0x804, ef << 0x13);
    len = gLenWord == 0xFFFFFFFF ? 0 : (gLenWord >> (n * 8)) & 0x1f;
    XAnd(o + 0x818, 0xffffff0f); XOr(o + 0x818, 0xa0); XAnd(o + 0x818, 0xfffffff0); XOr(o + 0x818, TraceTrim(n, len));
}
static int PhyStep2(int n)
{
    ULONG o = UsbPhyOff(n), m = 0;
    XSet(o + 0x814, 15); KeStallExecutionProcessor(2); XSet(o + 0x110, 8);
    XAnd(o + 0x100, 0xff000000); XOr(o + 0x100, 0x400); XSet(o + 0x100, 24);
    for (int a = 100; a; a--) { m = XR(o + 0x10c); KeStallExecutionProcessor(2); if (m) break; }
    XClr(o + 0x100, 24); XClr(o + 0x110, 8);
    if (!m) return -1;
    ULONG cal = (ULONG)((0xb9000ULL / m) / 1000);
    XAnd(o + 0x814, 0xffff8fff); XOr(o + 0x814, cal << 12); XClr(o + 0x814, 15);
    return 0;
}
static void PhyStep3(int n) { ULONG o = UsbPhyOff(n); XSet(o + 0x800, 0); XClr(o + 0x808, 17); XSet(o + 0x880, 0); }
static void PhyStep4(void)
{
    XAnd(0xc100, 0xfffffff0); XOr(0xc100, 0xe); XW(0xc12c, 0x7fc0c010); XW(0xc200, 0x1548);
    if (gChip == 0x110000) { XW(0xc204, 0x1548); XW(0xc208, 0x1548); }
}
static int PhyInit(void)
{
    int ports = gChip == 0x110000 ? 3 : 1;
    PhySet(0x24, 1); PhySet(0x24, 0); PhyClr(0x24, 1);
    for (int n = 0; n < ports; n++) PhyStep1(n);
    PhyClr(0x24, 0); Sleep(2000);
    for (int n = 0; n < ports; n++) if (PhyStep2(n)) { LogF("phy%d: no calibration measurement", n); return -1; }
    for (int n = 0; n < ports; n++) PhyStep3(n);
    XClr(0x100000 + 0x4, 0);
    PhyStep4();
    return 0;
}

/* ------------------------------------------------------------------------------------------------ xHCI core (polling) */
#define OPR(r) XR(gOp + (r))
#define OPW(r, v) XW(gOp + (r), v)
static void W64(ULONG off, ULONGLONG v) { XW(off + 4, (ULONG)(v >> 32)); XW(off, (ULONG)v); }   /* XHCI_WRITE_64_HI_LO */
static int Wait(ULONG off, ULONG mask, ULONG want, ULONG ms)
{
    for (ULONG i = 0; i < ms * 10; i++) { if ((XR(off) & mask) == want) return 0; KeStallExecutionProcessor(100); }
    return -1;
}
static PVOID Page(ULONGLONG *pa)
{
    PHYSICAL_ADDRESS lo = { 0 }, hi, b = { 0 }; hi.QuadPart = 0xFFFFFFFF;
    PVOID v = MmAllocateContiguousMemorySpecifyCache(PAGE_SIZE, lo, hi, b, MmNonCached);
    if (!v) return NULL;
    RtlZeroMemory(v, PAGE_SIZE); *pa = (ULONGLONG)MmGetPhysicalAddress(v).QuadPart;
    if (gNPages < 48) gPages[gNPages++] = v;
    return v;
}
static int RingInit(RING *r)
{
    r->v = (TRB *)Page(&r->p); if (!r->v) return -1;
    r->idx = 0; r->cyc = 1;
    TRB *l = &r->v[255]; l->d[0] = (ULONG)r->p; l->d[1] = (ULONG)(r->p >> 32); l->d[3] = (6 << 10) | 2;
    return 0;
}
static ULONGLONG RingPush(RING *r, ULONG d0, ULONG d1, ULONG d2, ULONG d3)
{
    TRB *t = &r->v[r->idx]; ULONGLONG pa = r->p + r->idx * 16ULL;
    t->d[0] = d0; t->d[1] = d1; t->d[2] = d2; KeMemoryBarrier(); t->d[3] = (d3 & ~1UL) | r->cyc;
    if (++r->idx == 255) { TRB *l = &r->v[255]; l->d[3] = (l->d[3] & ~1UL) | r->cyc; r->idx = 0; r->cyc ^= 1; }
    return pa;
}
static int EvPeek(TRB *out)   /* next event if any (consumes it) */
{
    TRB *e = &gEv.v[gEv.idx];
    if ((e->d[3] & 1) != gEv.cyc) return 0;
    *out = *e;
    if (++gEv.idx == 256) { gEv.idx = 0; gEv.cyc ^= 1; }
    W64(gRt + 0x20 + 0x18, (gEv.p + gEv.idx * 16ULL) | 8);
    return 1;
}
static void Dispatch(TRB *e);
/* waits for a command completion / a transfer event on dci; other transfer events are dispatched meanwhile */
static int WaitEvent(ULONG type, ULONG dci, ULONG ms, TRB *out)
{
    for (ULONG i = 0; i < ms * 10; i++) {
        TRB e;
        while (EvPeek(&e)) {
            ULONG t = (e.d[3] >> 10) & 0x3F;
            if (t == type && (type != 32 || ((e.d[3] >> 16) & 0x1F) == dci)) { if (out) *out = e; return 0; }
            Dispatch(&e);
        }
        KeStallExecutionProcessor(100);
    }
    return -1;
}
static int Command(ULONG d0, ULONG d1, ULONG d2, ULONG d3, TRB *out)
{
    RingPush(&gCmd, d0, d1, d2, d3); KeMemoryBarrier(); XW(gDb, 0);
    TRB e; if (WaitEvent(33, 0, 500, &e)) { LogF("command %u: no completion", (d3 >> 10) & 0x3F); return -1; }
    if (out) *out = e;
    if ((e.d[2] >> 24) != 1) { LogF("command %u: completion code %u", (d3 >> 10) & 0x3F, e.d[2] >> 24); return -1; }
    return 0;
}
static UCHAR *Ctx(UCHAR *base, ULONG i) { return base + i * (gCsz ? 64 : 32); }
/* control transfer on EP0 with data in gBuf (IN) or src (OUT) */
static int Control(UCHAR rt, UCHAR rq, USHORT val, USHORT idx, USHORT len, int in, const UCHAR *src, ULONG *got)
{
    ULONG trt = len ? (in ? 3 : 2) : 0;
    if (got) *got = len;
    if (len && !in && src) RtlCopyMemory(gBuf, src, len);
    RingPush(&gEp0, rt | rq << 8 | (ULONG)val << 16, idx | (ULONG)len << 16, 8, (2 << 10) | (1 << 6) | (trt << 16));
    if (len) RingPush(&gEp0, (ULONG)gBufPa, (ULONG)(gBufPa >> 32), len, (3 << 10) | (1 << 2) | ((in ? 1UL : 0) << 16));
    ULONGLONG st = RingPush(&gEp0, 0, 0, 0, (4 << 10) | (1 << 5) | ((len && in) ? 0 : 1UL << 16));
    KeMemoryBarrier(); XW(gDb + 4 * gSlot, 1);
    for (;;) {
        TRB e;
        if (WaitEvent(32, 1, 1000, &e)) { LogF("control %02x/%02x: no transfer event", rt, rq); return -1; }
        ULONG cc = e.d[2] >> 24; ULONGLONG ptr = (ULONGLONG)e.d[0] | (ULONGLONG)e.d[1] << 32;
        if (cc == 13) { if (got) *got = len - (e.d[2] & 0xFFFFFF); if (ptr != st) continue; }
        if (cc != 1 && cc != 13) { LogF("control %02x/%02x: completion code %u", rt, rq, cc); gNErr++; return -1; }
        if (ptr == st) return 0;
    }
}
static int XhciStart(void)
{
    ULONG hcs1 = XR(4), hcs2 = XR(8), hcc1 = XR(0x10);
    gOp = XR(0) & 0xFF; gRt = XR(0x18) & ~0x1FUL; gDb = XR(0x14) & ~3UL; gMaxPorts = hcs1 >> 24; gCsz = (hcc1 >> 2) & 1;
    ULONG sp = ((hcs2 >> 21) & 0x1F) << 5 | (hcs2 >> 27);
    LogF("xhci: version %x ports %u csz %u scratchpads %u", XR(0) >> 16, gMaxPorts, gCsz, sp);
    OPW(0, OPR(0) & ~1UL); Wait(gOp + 4, 1, 1, 50);
    if (gChip == 0x110000) XClr(0x100000, 2);
    OPW(0, 2); Sleep(1000);
    if (Wait(gOp, 2, 0, 5) || Wait(gOp + 4, 1UL << 11, 0, 5)) { LogF("xhci: reset did not finish (cmd %08x sts %08x)", OPR(0), OPR(4)); return -1; }
    if (gChip == 0x110000) { XTog(0x100000, 1); XTog(0x100000, 17); XTog(0x100000, 25); Sleep(30000); }
    XClr(0x100000, 1); Sleep(20000);
    if (gChip == 0x110000) { XSet(0x100000, 2); Sleep(20000); }
    PhyStep4();
    gDcbaa = (ULONGLONG *)Page(&gDcbaaPa); if (!gDcbaa) return -1;
    if (sp) {
        ULONGLONG ap; ULONGLONG *arr = (ULONGLONG *)Page(&ap); if (!arr || sp > 31) return -1;
        for (ULONG i = 0; i < sp; i++) { ULONGLONG p; if (!Page(&p)) return -1; arr[i] = p; }
        gDcbaa[0] = ap;
    }
    if (RingInit(&gCmd)) return -1;
    gEv.v = (TRB *)Page(&gEv.p); gEv.idx = 0; gEv.cyc = 1; if (!gEv.v) return -1;
    ULONGLONG *erst = (ULONGLONG *)Page(&gErstPa); if (!erst) return -1;
    erst[0] = gEv.p; erst[1] = 256;
    OPW(0x38, 1); W64(gOp + 0x30, gDcbaaPa); W64(gOp + 0x18, gCmd.p | 1);
    XW(gRt + 0x28, 1); W64(gRt + 0x38, gEv.p); W64(gRt + 0x30, gErstPa); XW(gRt + 0x20, 0);
    OPW(0, 1);
    if (Wait(gOp + 4, 1, 0, 50)) { LogF("xhci: does not run (sts %08x)", OPR(4)); return -1; }
    gRunning = TRUE;
    return Command(0, 0, 0, 23 << 10, NULL);
}
static int DeviceUp(void)
{
    gPort = 0;
    for (int w = 0; w < 20 && !gPort; w++) {          /* the BT function shows up once the combo firmware runs (ps5wifi) */
        for (ULONG p = 1; p <= gMaxPorts; p++) if (OPR(0x400 + 0x10 * (p - 1)) & 1) { gPort = p; break; }
        if (!gPort) Sleep(250000);
    }
    if (!gPort) { LogF("device: no connected port"); return -1; }
    ULONG pr = 0x400 + 0x10 * (gPort - 1), s = OPR(pr);
    OPW(pr, (s & 0x0E00C3E0) | (1 << 4));
    if (Wait(gOp + pr, 1UL << 21, 1UL << 21, 200)) LogF("device: port %u reset not complete (%08x)", gPort, OPR(pr));
    s = OPR(pr); OPW(pr, (s & 0x0E00C3E0) | (s & 0x00FE0000));
    s = OPR(pr); gSpeed = (s >> 10) & 0xF;
    LogF("device: port %u portsc %08x speed %u", gPort, s, gSpeed);
    Sleep(20000);
    TRB e; if (Command(0, 0, 0, 9 << 10, &e)) return -1; gSlot = e.d[3] >> 24;
    gOutCtx = (UCHAR *)Page(&gOutPa); gInCtx = (UCHAR *)Page(&gInPa); gBuf = (UCHAR *)Page(&gBufPa);
    gAclBuf = (UCHAR *)Page(&gAclPa); gOutBuf = (UCHAR *)Page(&gOutBufPa);
    if (!gOutCtx || !gInCtx || !gBuf || !gAclBuf || !gOutBuf || RingInit(&gEp0)) return -1;
    gDcbaa[gSlot] = gOutPa;
    gMps0 = gSpeed == 3 ? 64 : gSpeed == 2 ? 8 : 64;
    ULONG *icc = (ULONG *)Ctx(gInCtx, 0), *sl = (ULONG *)Ctx(gInCtx, 1), *ep0 = (ULONG *)Ctx(gInCtx, 2);
    icc[1] = 3; sl[0] = (1UL << 27) | (gSpeed << 20); sl[1] = gPort << 16;
    ep0[1] = (3 << 1) | (4 << 3) | (gMps0 << 16); ep0[2] = (ULONG)gEp0.p | 1; ep0[3] = (ULONG)(gEp0.p >> 32); ep0[4] = 8;
    if (Command((ULONG)gInPa, (ULONG)(gInPa >> 32), 0, (11 << 10) | (gSlot << 24), NULL)) return -1;
    ULONG got = 0;
    if (Control(0x80, 6, 0x0100, 0, 18, 1, NULL, &got)) return -1;
    LogF("device: vid %02x%02x pid %02x%02x class %02x mps %u", gBuf[9], gBuf[8], gBuf[11], gBuf[10], gBuf[4], gBuf[7]);
    if (gBuf[7] && gBuf[7] != gMps0) {
        gMps0 = gBuf[7]; RtlZeroMemory(gInCtx, PAGE_SIZE); icc[1] = 2; ep0[1] = (3 << 1) | (4 << 3) | (gMps0 << 16);
        Command((ULONG)gInPa, (ULONG)(gInPa >> 32), 0, (13 << 10) | (gSlot << 24), NULL);
    }
    if (Control(0x80, 6, 0x0200, 0, 9, 1, NULL, &got)) return -1;
    USHORT tot = gBuf[2] | gBuf[3] << 8; gCfgVal = gBuf[5]; if (tot > 1024) tot = 1024;
    if (Control(0x80, 6, 0x0200, 0, tot, 1, NULL, &got)) return -1;
    UCHAR ifn = 0xFF, alt = 0;
    for (ULONG o = 0; o + 2 <= got && gBuf[o]; o += gBuf[o]) {
        UCHAR t = gBuf[o + 1];
        if (t == 4) { ifn = gBuf[o + 2]; alt = gBuf[o + 3]; }
        if (t == 5 && ifn == 0 && alt == 0) {
            UCHAR a = gBuf[o + 2], at = gBuf[o + 3] & 3; USHORT mps = gBuf[o + 4] | gBuf[o + 5] << 8;
            if (at == 3 && (a & 0x80)) { gEpInt = a; gMpsInt = mps; gIvInt = gBuf[o + 6]; }
            if (at == 2 && (a & 0x80)) { gEpBin = a; gMpsBin = mps; }
            if (at == 2 && !(a & 0x80)) { gEpBout = a; gMpsBout = mps; }
        }
    }
    if (!gEpInt || !gEpBin || !gEpBout) { LogF("device: interface 0 endpoints missing"); return -1; }
    if (Control(0x00, 9, gCfgVal, 0, 0, 0, NULL, NULL)) return -1;
    if (RingInit(&gIntIn) || RingInit(&gBulkIn) || RingInit(&gBulkOut)) return -1;
    gDciInt = (gEpInt & 0xF) * 2 + 1; gDciBin = (gEpBin & 0xF) * 2 + 1; gDciBout = (gEpBout & 0xF) * 2;
    ULONG dmax = gDciInt; if (gDciBin > dmax) dmax = gDciBin; if (gDciBout > dmax) dmax = gDciBout;
    RtlZeroMemory(gInCtx, PAGE_SIZE);
    icc[1] = 1 | (1UL << gDciInt) | (1UL << gDciBin) | (1UL << gDciBout);
    sl[0] = (dmax << 27) | (gSpeed << 20); sl[1] = gPort << 16;
    ULONG iv = gIvInt;
    if (gSpeed == 3) iv = iv ? iv - 1 : 0; else { ULONG f = iv * 8, x = 0; while ((1UL << (x + 1)) <= f) x++; iv = x; }
    ULONG *c;
    c = (ULONG *)Ctx(gInCtx, 1 + gDciInt); c[0] = iv << 16; c[1] = (3 << 1) | (7 << 3) | ((ULONG)gMpsInt << 16);
    c[2] = (ULONG)gIntIn.p | 1; c[3] = (ULONG)(gIntIn.p >> 32); c[4] = gMpsInt | ((ULONG)gMpsInt << 16);
    c = (ULONG *)Ctx(gInCtx, 1 + gDciBin); c[1] = (3 << 1) | (6 << 3) | ((ULONG)gMpsBin << 16);
    c[2] = (ULONG)gBulkIn.p | 1; c[3] = (ULONG)(gBulkIn.p >> 32); c[4] = 512;
    c = (ULONG *)Ctx(gInCtx, 1 + gDciBout); c[1] = (3 << 1) | (2 << 3) | ((ULONG)gMpsBout << 16);
    c[2] = (ULONG)gBulkOut.p | 1; c[3] = (ULONG)(gBulkOut.p >> 32); c[4] = 512;
    if (Command((ULONG)gInPa, (ULONG)(gInPa >> 32), 0, (12 << 10) | (gSlot << 24), NULL)) return -1;
    LogF("device: configured, int %02x/%u bulk in %02x/%u bulk out %02x/%u", gEpInt, gMpsInt, gEpBin, gMpsBin, gEpBout, gMpsBout);
    return 0;
}
static void Halt(void)
{
    if (gXhci && gRunning) { OPW(0, OPR(0) & ~1UL); Wait(gOp + 4, 1, 1, 50); gRunning = FALSE; }
}

/* ------------------------------------------------------------------------------------------------ HCI packets <-> requests */
static void FillRead(WDFREQUEST r, UCHAR type, const UCHAR *d, ULONG n)
{
    PIRP irp = WdfRequestWdmGetIrp(r); PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    ULONG cap = s->Parameters.DeviceIoControl.OutputBufferLength; BTHX_HCI_READ_WRITE_CONTEXT *o = (BTHX_HCI_READ_WRITE_CONTEXT *)irp->UserBuffer;
    if (!o || cap < FIELD_OFFSET(BTHX_HCI_READ_WRITE_CONTEXT, Data) + n) { WdfRequestComplete(r, STATUS_BUFFER_TOO_SMALL); gNDrop++; return; }
    o->Type = type; o->DataLen = n; RtlCopyMemory(o->Data, d, n);
    WdfRequestCompleteWithInformation(r, STATUS_SUCCESS, FIELD_OFFSET(BTHX_HCI_READ_WRITE_CONTEXT, Data) + n);
}
static void Deliver(UCHAR type, const UCHAR *d, ULONG n)
{
    WDFREQUEST r; KIRQL q; FIFO *f = type == HciPacketEvent ? &gFifoEv : &gFifoAcl; WDFQUEUE rq = type == HciPacketEvent ? gReadEvQ : gReadAclQ;
    LogHex("in ", type, d, n);
    if (type == HciPacketEvent) gNEv++; else gNAclIn++;
    KeAcquireSpinLock(&gFifoLock, &q);
    if (f->count == 0 && NT_SUCCESS(WdfIoQueueRetrieveNextRequest(rq, &r))) { KeReleaseSpinLock(&gFifoLock, q); FillRead(r, type, d, n); return; }
    if (f->count < FIFO_N && n <= ACL_IN_MAX) { PKT *p = &f->p[(f->head + f->count) % FIFO_N]; p->type = type; p->len = n; RtlCopyMemory(p->data, d, n); f->count++; }
    else gNDrop++;
    KeReleaseSpinLock(&gFifoLock, q);
}
static void ArmEvent(void)
{
    if (gEvArmed) return;
    RingPush(&gIntIn, (ULONG)EVP, (ULONG)(EVP >> 32), gMpsInt, (1 << 10) | (1 << 5) | (1 << 2)); KeMemoryBarrier();   /* one packet, like btusb */
    XW(gDb + 4 * gSlot, gDciInt); gEvArmed = 1;
}
static void ArmAcl(void)
{
    if (gAclArmed) return;
    RingPush(&gBulkIn, (ULONG)gAclPa, (ULONG)(gAclPa >> 32), gMpsBin, (1 << 10) | (1 << 5) | (1 << 2)); KeMemoryBarrier();   /* one packet */
    XW(gDb + 4 * gSlot, gDciBin); gAclArmed = 1;
}
static void Dispatch(TRB *e)
{
    ULONG t = (e->d[3] >> 10) & 0x3F, dci = (e->d[3] >> 16) & 0x1F, cc = e->d[2] >> 24;
    if (t != 32) return;                                 /* port status etc. */
    if (dci == gDciInt) {
        ULONG n = gMpsInt - (e->d[2] & 0xFFFFFF); gEvArmed = 0;
        if (cc == 1 || cc == 13) {
            for (ULONG i = 0; i < n && gEvAsmLen < sizeof gEvAsm; i++) gEvAsm[gEvAsmLen++] = EVB[i];
            while (gEvAsmLen >= 2 && gEvAsmLen >= 2u + gEvAsm[1]) {   /* complete event(s) */
                ULONG len = 2u + gEvAsm[1]; Deliver(HciPacketEvent, gEvAsm, len);
                RtlMoveMemory(gEvAsm, gEvAsm + len, gEvAsmLen - len); gEvAsmLen -= len;
            }
        } else { gNErr++; gEvAsmLen = 0; }
    } else if (dci == gDciBin) {
        ULONG n = gMpsBin - (e->d[2] & 0xFFFFFF); gAclArmed = 0;
        if (cc == 1 || cc == 13) {
            for (ULONG i = 0; i < n && gAclAsmLen < sizeof gAclAsm; i++) gAclAsm[gAclAsmLen++] = gAclBuf[i];
            while (gAclAsmLen >= 4) {
                ULONG len = 4u + (gAclAsm[2] | (ULONG)gAclAsm[3] << 8);
                if (len > ACL_IN_MAX) { gNErr++; gAclAsmLen = 0; break; }          /* lost sync: drop */
                if (gAclAsmLen < len) break;
                Deliver(HciPacketAclData, gAclAsm, len);
                RtlMoveMemory(gAclAsm, gAclAsm + len, gAclAsmLen - len); gAclAsmLen -= len;
            }
        } else { gNErr++; gAclAsmLen = 0; }
    }
}
/* one write request: command or ACL; runs on the poll thread */
static NTSTATUS DoWrite(WDFREQUEST r)
{
    PIRP irp = WdfRequestWdmGetIrp(r); PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    BTHX_HCI_READ_WRITE_CONTEXT *w = (BTHX_HCI_READ_WRITE_CONTEXT *)s->Parameters.DeviceIoControl.Type3InputBuffer;
    ULONG inLen = s->Parameters.DeviceIoControl.InputBufferLength;
    if (!w || inLen < FIELD_OFFSET(BTHX_HCI_READ_WRITE_CONTEXT, Data) || w->DataLen > inLen - FIELD_OFFSET(BTHX_HCI_READ_WRITE_CONTEXT, Data) || w->DataLen > 4000)
        return STATUS_INVALID_PARAMETER;
    LogHex("out", w->Type, w->Data, w->DataLen);
    if (irp->UserBuffer && s->Parameters.DeviceIoControl.OutputBufferLength >= sizeof(ULONG)) *(ULONG *)irp->UserBuffer = 0;
    if (w->Type == HciPacketCommand) {
        gNCmd++;
        if (Control(0x20, 0, 0, 0, (USHORT)w->DataLen, 0, w->Data, NULL)) return STATUS_IO_DEVICE_ERROR;
        if (irp->UserBuffer && s->Parameters.DeviceIoControl.OutputBufferLength >= sizeof(ULONG)) *(ULONG *)irp->UserBuffer = w->DataLen;
        return STATUS_SUCCESS;
    }
    if (w->Type == HciPacketAclData) {
        gNAclOut++;
        RtlCopyMemory(gOutBuf, w->Data, w->DataLen);
        ULONGLONG trb = RingPush(&gBulkOut, (ULONG)gOutBufPa, (ULONG)(gOutBufPa >> 32), w->DataLen, (1 << 10) | (1 << 5));
        KeMemoryBarrier(); XW(gDb + 4 * gSlot, gDciBout);
        TRB e; if (WaitEvent(32, gDciBout, 2000, &e)) { gNErr++; return STATUS_IO_TIMEOUT; }
        (void)trb;
        if ((e.d[2] >> 24) != 1) return STATUS_IO_DEVICE_ERROR;
        if (irp->UserBuffer && s->Parameters.DeviceIoControl.OutputBufferLength >= sizeof(ULONG)) *(ULONG *)irp->UserBuffer = w->DataLen;
        return STATUS_SUCCESS;
    }
    return STATUS_NOT_SUPPORTED;
}
static VOID PollThread(PVOID ctx)
{
    UNREFERENCED_PARAMETER(ctx);
    LARGE_INTEGER ms1; ms1.QuadPart = -10000; ULONG tick = 0;
    ExSetTimerResolution(10000, TRUE);
    while (!gStop) {
        if (!gLog && tick % 1000 == 0) { LogOpen(); if (gLog) LogF("==== ps5btx v0.7 log opened late (boot start: C: was not ready)"); }   /* v0.7 */
        WDFREQUEST r;
        while (NT_SUCCESS(WdfIoQueueRetrieveNextRequest(gWriteQ, &r))) {   /* v0.4: Information = sizeof(ULONG) (bytes written in the output) */
            NTSTATUS ws = DoWrite(r); WdfRequestCompleteWithInformation(r, ws, NT_SUCCESS(ws) ? sizeof(ULONG) : 0); }
        ArmEvent(); ArmAcl();
        TRB e; while (EvPeek(&e)) Dispatch(&e);
        ULONG st = OPR(4);
        if (st & ((1 << 2) | (1 << 12)) || st == 0xFFFFFFFF) { LogF("poll: controller error, usbsts %08x - stopping", st); break; }
        while (gIoTrShown < gIoTrN && gIoTrShown < 32) { IOTR *t = &gIoTr[gIoTrShown];
            LogF("ioctl #%d: code %08x (fn %03x) major %u out %u in %u", gIoTrShown, t->code, (t->code >> 2) & 0xFFF, t->major, t->outLen, t->inLen); gIoTrShown++; }
        if (++tick % 60000 == 0 || (tick < 60000 && tick % 3000 == 0))
            LogF("stats: events %u acl in %u acl out %u cmds %u dropped %u errors %u | ioctl ver %d setver %d caps %d write %d read %d other %d last %08x major %d out %d in %d | fifo %u",
                 gNEv, gNAclIn, gNAclOut, gNCmd, gNDrop, gNErr, gIoN[0], gIoN[1], gIoN[2], gIoN[3], gIoN[4], gIoOther, gIoLastCode, gIoLastSt, gIoLastOut, gIoLastIn, gFifoCount);
        KeWaitForSingleObject(&gWake, Executive, KernelMode, FALSE, &ms1);
    }
    ExSetTimerResolution(0, FALSE);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* ------------------------------------------------------------------------------------------------ PDO (BthX) */
static VOID PdoIoctl(WDFQUEUE q, WDFREQUEST r, size_t outLen, size_t inLen, ULONG code)
{
    UNREFERENCED_PARAMETER(q);
    PIRP irp = WdfRequestWdmGetIrp(r); PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    ULONG fn = (code >> 2) & 0xFFF;
    if (fn >= 0x100 && fn <= 0x104) InterlockedIncrement(&gIoN[fn - 0x100]); else InterlockedIncrement(&gIoOther);
    InterlockedExchange(&gIoLastCode, (LONG)code); InterlockedExchange(&gIoLastOut, (LONG)outLen); InterlockedExchange(&gIoLastIn, (LONG)inLen);
    InterlockedExchange(&gIoLastSt, (LONG)s->MajorFunction);
    { LONG k = InterlockedIncrement(&gIoTrN) - 1; if (k < 32) { gIoTr[k].code = code; gIoTr[k].major = s->MajorFunction; gIoTr[k].outLen = (ULONG)outLen; gIoTr[k].inLen = (ULONG)inLen; gIoTr[k].st = 0x7FFFFFFF; } }
    switch (code) {
    case IOCTL_BTHX_GET_VERSION: {
        BTHX_VERSION *v = (BTHX_VERSION *)irp->UserBuffer;
        if (!v || outLen < sizeof *v) { WdfRequestComplete(r, STATUS_BUFFER_TOO_SMALL); return; }
        v->Version = BTHX_DDI_VERSION_1; WdfRequestCompleteWithInformation(r, STATUS_SUCCESS, sizeof *v); return; }
    case IOCTL_BTHX_SET_VERSION: {
        BTHX_VERSION *v = (BTHX_VERSION *)s->Parameters.DeviceIoControl.Type3InputBuffer;
        LogF("bthx: set version %u", v ? v->Version : 0);
        WdfRequestComplete(r, v && v->Version == BTHX_DDI_VERSION_1 ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED); return; }
    case IOCTL_BTHX_QUERY_CAPABILITIES: {
        BTHX_CAPABILITIES *c = (BTHX_CAPABILITIES *)irp->UserBuffer;
        if (!c || outLen < sizeof *c) { WdfRequestComplete(r, STATUS_BUFFER_TOO_SMALL); return; }
        RtlZeroMemory(c, sizeof *c); c->MaxAclTransferInSize = ACL_IN_MAX;
        /* v0.6: BthMini (10.0.26100) rejects anything but ScoSupportHCIBypass with exactly 1 SCO channel and an ACL size >= 0x103
         * (checks after IOCTL_BTHX_QUERY_CAPABILITIES) - Code 10 otherwise. SCO itself is not carried (no isochronous endpoints). */
        c->ScoSupport = ScoSupportHCIBypass; c->MaxScoChannels = 1;
        LogF("bthx: capabilities queried");
        WdfRequestCompleteWithInformation(r, STATUS_SUCCESS, sizeof *c); return; }
    case IOCTL_BTHX_WRITE_HCI:
        if (!NT_SUCCESS(WdfRequestForwardToIoQueue(r, gWriteQ))) WdfRequestComplete(r, STATUS_DEVICE_NOT_READY);
        else KeSetEvent(&gWake, 0, FALSE);
        return;
    case IOCTL_BTHX_READ_HCI: {   /* v0.4: input ULONG = packet type wanted (2 ACL, 4 event), output = context */
        ULONG want = (s->Parameters.DeviceIoControl.Type3InputBuffer && inLen >= sizeof(ULONG)) ? *(ULONG *)s->Parameters.DeviceIoControl.Type3InputBuffer : HciPacketEvent;
        FIFO *f = want == HciPacketAclData ? &gFifoAcl : &gFifoEv; WDFQUEUE rq = want == HciPacketAclData ? gReadAclQ : gReadEvQ;
        KIRQL ql; KeAcquireSpinLock(&gFifoLock, &ql);
        if (f->count) {
            PKT p = f->p[f->head]; f->head = (f->head + 1) % FIFO_N; f->count--;   /* copy out, then complete without the lock */
            KeReleaseSpinLock(&gFifoLock, ql);
            FillRead(r, p.type, p.data, p.len); return;
        }
        NTSTATUS st = WdfRequestForwardToIoQueue(r, rq);
        KeReleaseSpinLock(&gFifoLock, ql);
        if (!NT_SUCCESS(st)) WdfRequestComplete(r, st);
        return; }
    default:
        WdfRequestComplete(r, STATUS_INVALID_DEVICE_REQUEST);
    }
}
static NTSTATUS CreatePdo(WDFDEVICE fdo)
{
    DECLARE_CONST_UNICODE_STRING(devId, L"PS5BT\\BTHX");
    DECLARE_CONST_UNICODE_STRING(hwId, L"MS_BTHX_BTHMINI");
    DECLARE_CONST_UNICODE_STRING(instId, L"0");
    DECLARE_CONST_UNICODE_STRING(text, L"PS5 Bluetooth (IW620)");
    DECLARE_CONST_UNICODE_STRING(loc, L"Salina USB");
    PWDFDEVICE_INIT init = WdfPdoInitAllocate(fdo); NTSTATUS st;
    if (!init) return STATUS_INSUFFICIENT_RESOURCES;
    WdfDeviceInitSetDeviceType(init, FILE_DEVICE_BLUETOOTH);
    if (!NT_SUCCESS(st = WdfPdoInitAssignDeviceID(init, &devId)) || !NT_SUCCESS(st = WdfPdoInitAddHardwareID(init, &hwId)) ||
        !NT_SUCCESS(st = WdfPdoInitAssignInstanceID(init, &instId)) ||
        !NT_SUCCESS(st = WdfPdoInitAddDeviceText(init, &text, &loc, 0x409))) { WdfDeviceInitFree(init); return st; }
    WdfPdoInitSetDefaultLocale(init, 0x409);
    if (!NT_SUCCESS(st = WdfDeviceCreate(&init, WDF_NO_OBJECT_ATTRIBUTES, &gPdo))) { if (init) WdfDeviceInitFree(init); return st; }
    WDF_IO_QUEUE_CONFIG qc; WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&qc, WdfIoQueueDispatchParallel);
    qc.EvtIoInternalDeviceControl = PdoIoctl; qc.EvtIoDeviceControl = PdoIoctl;   /* v0.2: both (BthMini uses IRP_MJ_DEVICE_CONTROL) */
    qc.PowerManaged = WdfFalse;   /* v0.5: BthMini sends IOCTL_BTHX_GET_VERSION synchronously before the PDO is in D0 (AddDevice/early
                                   * start) - a power-managed queue holds it until D0, which never comes: deadlock (install, boot and
                                   * removal hung, 0x9F). The transport answers at any time; the controller is up before the PDO exists. */
    if (!NT_SUCCESS(st = WdfIoQueueCreate(gPdo, &qc, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE))) return st;
    WDF_IO_QUEUE_CONFIG_INIT(&qc, WdfIoQueueDispatchManual); qc.PowerManaged = WdfFalse;   /* v0.5 */
    if (!NT_SUCCESS(st = WdfIoQueueCreate(gPdo, &qc, WDF_NO_OBJECT_ATTRIBUTES, &gReadEvQ))) return st;
    if (!NT_SUCCESS(st = WdfIoQueueCreate(gPdo, &qc, WDF_NO_OBJECT_ATTRIBUTES, &gReadAclQ))) return st;
    if (!NT_SUCCESS(st = WdfIoQueueCreate(gPdo, &qc, WDF_NO_OBJECT_ATTRIBUTES, &gWriteQ))) return st;
    WDF_DEVICE_PNP_CAPABILITIES pc; WDF_DEVICE_PNP_CAPABILITIES_INIT(&pc); pc.Removable = WdfFalse; pc.SurpriseRemovalOK = WdfTrue;
    pc.UniqueID = WdfTrue; pc.Address = 0; pc.UINumber = 0; WdfDeviceSetPnpCapabilities(gPdo, &pc);
    return WdfFdoAddStaticChild(fdo, gPdo);
}

/* ------------------------------------------------------------------------------------------------ FDO */
static void FreeAll(void)
{
    Halt();
    for (ULONG i = 0; i < gNPages; i++) MmFreeContiguousMemorySpecifyCache(gPages[i], PAGE_SIZE, MmNonCached);
    gNPages = 0;
    if (gXhci) { MmUnmapIoSpace((PVOID)gXhci, gXhciLen); gXhci = NULL; }
    if (gSpBar2) { MmUnmapIoSpace((PVOID)gSpBar2, 4096); gSpBar2 = NULL; }
    if (gSpBar4) { MmUnmapIoSpace((PVOID)gSpBar4, 0x8000); gSpBar4 = NULL; }
}
static NTSTATUS PrepareHw(WDFDEVICE dev, WDFCMRESLIST raw, WDFCMRESLIST tr)
{
    UNREFERENCED_PARAMETER(raw);
    LogOpen(); LogF("==== ps5btx v0.7 start");
    for (ULONG i = 0; i < WdfCmResourceListGetCount(tr); i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR d = WdfCmResourceListGetDescriptor(tr, i);
        if (d->Type == CmResourceTypeMemory && !gXhci && d->u.Memory.Length >= 0x130000) {
            gXhciLen = d->u.Memory.Length; gXhci = (volatile UCHAR *)MmMapIoSpace(d->u.Memory.Start, gXhciLen, MmNonCached);
            LogF("bar0 %I64x length %x", d->u.Memory.Start.QuadPart, gXhciLen);
        }
    }
    if (!gXhci) { LogF("no BAR0 resource"); return STATUS_DEVICE_CONFIGURATION_ERROR; }
    volatile UCHAR *sc = FindSpcie(); if (!sc) { LogF("SPCIE 104D:9107 not found"); FreeAll(); return STATUS_DEVICE_CONFIGURATION_ERROR; }
    ULONGLONG s2 = CfgBar(sc, 2), s4 = CfgBar(sc, 4); MmUnmapIoSpace((PVOID)sc, 4096);
    PHYSICAL_ADDRESS pa;
    pa.QuadPart = (LONGLONG)(s2 + 0x180000); gSpBar2 = (volatile UCHAR *)MmMapIoSpace(pa, 4096, MmNonCached);
    pa.QuadPart = (LONGLONG)s4; gSpBar4 = (volatile UCHAR *)MmMapIoSpace(pa, 0x8000, MmNonCached);
    if (!gSpBar2 || !gSpBar4) { FreeAll(); return STATUS_INSUFFICIENT_RESOURCES; }
    { PHYSICAL_ADDRESS lp; lp.QuadPart = 0x9bf40; volatile ULONG *p = (volatile ULONG *)MmMapIoSpace(lp, 16, MmNonCached);
      gLenWord = p ? p[1] : 0xFFFFFFFF; if (p) MmUnmapIoSpace((PVOID)p, 16); }   /* Linux: (word >> 8n) & 0x1f, no plausibility check */
    { ULONG rev = *(volatile ULONG *)(gSpBar4 + 0x4000); gChip = (rev >> 24) == 0x11 ? 0x110000 : (rev >> 24) == 0x12 ? 0x120000 : 0; }   /* spcie_probe */
    LogF("salina chip %06x trace length word %08x", gChip, gLenWord);
    if (gChip != 0x110000 && gChip != 0x120000) { FreeAll(); return STATUS_DEVICE_CONFIGURATION_ERROR; }
    if (PhyInit() || XhciStart() || DeviceUp()) { LogF("start failed"); FreeAll(); return STATUS_DEVICE_NOT_READY; }
    gFifoEv.head = gFifoEv.count = gFifoAcl.head = gFifoAcl.count = 0;
    NTSTATUS st = CreatePdo(dev);
    LogF("bthx child: %08x", st);
    if (!NT_SUCCESS(st)) { FreeAll(); return st; }
    gStop = 0; HANDLE h; OBJECT_ATTRIBUTES oa; InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    st = PsCreateSystemThread(&h, THREAD_ALL_ACCESS, &oa, NULL, NULL, PollThread, NULL);
    if (NT_SUCCESS(st)) { ObReferenceObjectByHandle(h, THREAD_ALL_ACCESS, *PsThreadType, KernelMode, (PVOID *)&gThread, NULL); ZwClose(h); }
    return st;
}
static NTSTATUS ReleaseHw(WDFDEVICE dev, WDFCMRESLIST tr)
{
    UNREFERENCED_PARAMETER(dev); UNREFERENCED_PARAMETER(tr);
    if (gThread) { gStop = 1; KeSetEvent(&gWake, 0, FALSE); KeWaitForSingleObject(gThread, Executive, KernelMode, FALSE, NULL); ObDereferenceObject(gThread); gThread = NULL; }
    LogF("stop: events %u acl in %u acl out %u cmds %u dropped %u errors %u", gNEv, gNAclIn, gNAclOut, gNCmd, gNDrop, gNErr);
    FreeAll();
    if (gLog) { ZwClose(gLog); gLog = NULL; }
    return STATUS_SUCCESS;
}
static NTSTATUS DeviceAdd(WDFDRIVER drv, PWDFDEVICE_INIT init)
{
    UNREFERENCED_PARAMETER(drv);
    WDF_PNPPOWER_EVENT_CALLBACKS pp; WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pp);
    pp.EvtDevicePrepareHardware = PrepareHw; pp.EvtDeviceReleaseHardware = ReleaseHw;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &pp);
    WdfDeviceInitSetDeviceType(init, FILE_DEVICE_BUS_EXTENDER);
    NTSTATUS st = WdfDeviceCreate(&init, WDF_NO_OBJECT_ATTRIBUTES, &gFdo);
    if (!NT_SUCCESS(st)) return st;
    WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS idle; WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS_INIT(&idle, IdleCannotWakeFromS0);
    idle.Enabled = WdfFalse; idle.UserControlOfIdleSettings = IdleDoNotAllowUserControl;   /* never D3 at runtime */
    WdfDeviceAssignS0IdleSettings(gFdo, &idle);
    return STATUS_SUCCESS;
}
NTSTATUS DriverEntry(PDRIVER_OBJECT d, PUNICODE_STRING rp)
{
    KeInitializeEvent(&gWake, SynchronizationEvent, FALSE); KeInitializeSpinLock(&gFifoLock);
    WDF_DRIVER_CONFIG c; WDF_DRIVER_CONFIG_INIT(&c, DeviceAdd);
    return WdfDriverCreate(d, rp, WDF_NO_OBJECT_ATTRIBUTES, &c, WDF_NO_HANDLE);
}
