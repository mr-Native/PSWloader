/*
 * ps5cmos v1.5 (16.11.2026; UNTESTED ON HARDWARE) - v1.5: HW v1.4: scan OK (rtc-reads 34), but the service stop hung
 *   in STOP_PENDING: with a device attached the I/O manager defers the unload (DOE_UNLOAD_PENDING in the target
 *   device's DEVOBJ_EXTENSION, no DriverUnload, so no DRVO_UNLOAD_INVOKED). The monitor now also detaches on
 *   DOE_UNLOAD_PENDING; IoDetachDevice then completes the deferred unload. The flag is read only after checking the
 *   extension layout (Type 13, DeviceObject back pointer) - otherwise only DRVO_UNLOAD_INVOKED is used.
 * ps5cmos v1.4 - v1.4: HW v1.3: the SDK closes its first handle after
 *   14404 CPUID/MSR/PCI IOCTLs and reopens without reloading; the v1.3 detach-on-CLOSE let the RTC accesses pass.
 *   Now the filter stays attached while the driver lives; a monitor thread (50 ms) detaches when the I/O manager
 *   marks the driver object DRVO_UNLOAD_INVOKED (service stop), holding a driver-object reference until then.
 * ps5cmos v1.3 - v1.3: the SDK loads cpuz165 from a randomly named temp file
 *   (\??\C:\Windows\TEMP\VtabyJuic, no .sys) -> recognise the driver OBJECT, not the file name: after every signed
 *   kernel image load a watcher looks for \Driver\cpuz100..cpuz199 whose DriverStart is exactly that image base.
 * ps5cmos v1.2 - platform RTC/CMOS compatibility layer for PS5 native Windows.
 *
 * The PS5 has no legacy RTC/CMOS (FADT: CMOS absent; PS5 Linux: x86_platform.legacy.rtc = 0). Software that times
 * the CPU through RTC Status A (the CPUID SDK driver cpuz1xx used by 3DMark SystemInfo, CPU-Z, HWiNFO-class tools)
 * reads a constant value and loops forever. This boot-start driver is part of the PS5 driver set and changes no
 * third-party software: whenever a signed CPUID SDK driver (image cpuz*.sys, driver object \Driver\cpuz*) is loaded
 * by its own installer, ps5cmos attaches a filter device on top of its device and answers ONLY port accesses to
 * 0x70..0x73 as an MC146818 RTC from the Windows clock (rtc-emu.h). Every other request goes to the real driver.
 *
 *  - detection (v1.3): after every kernel image load a watcher looks for \\Driver\\cpuz100..199 with DriverStart ==
 *    that image base (file names are random temp names); a system
 *    thread then waits (<= 10 s) until \Driver\<name> has created its device and attaches. Attaching after the SDK
 *    opened its handle still works: the I/O manager routes every IRP of that handle to the top of the stack.
 *  - never opens the device (cpuz165 is exclusive and checks a privilege in IRP_MJ_CREATE, F22).
 *  - detach: the CPUID device is exclusive (one handle). On IRP_MJ_CLOSE the filter detaches and deletes itself in a
 *    work item, so the SDK can stop/unload its driver per scan without a dangling reference; the next load re-arms.
 *  - cpuz165 IOCTL protocol (from the genuine binary, METHOD_BUFFERED): 0x9C402480/84/88 port read {port} ->
 *    {value, 0x87654321}; 0x9C4024C0/C4/C8 port write {port, value}; 0x9C402500 indexed read {iport, dport, index}
 *    -> {value}; 0x9C402504 indexed write {iport, dport, index, value}. Writes to the RTC are never applied.
 *  - statistics: \\.\PS5CMOS, IOCTL 0x80036400 -> {'CM12', attached, rtc reads, index writes, dropped writes,
 *    passed IOCTLs, last index, attach count, detach count}.
 * No unload (registered callback + possible attachment); disable = service start 4 + reboot.
 * GPL-2.0-only like the rest of the PS5 platform drivers.
 */
#include <ntddk.h>
#include "rtc-emu.h"

#define CPUZ_READ_B  0x9C402480u
#define CPUZ_READ_W  0x9C402484u
#define CPUZ_READ_D  0x9C402488u
#define CPUZ_WRITE_B 0x9C4024C0u
#define CPUZ_WRITE_W 0x9C4024C4u
#define CPUZ_WRITE_D 0x9C4024C8u
#define CPUZ_IDX_RD  0x9C402500u
#define CPUZ_IDX_WR  0x9C402504u
#define IOCTL_PS5CMOS_STATS CTL_CODE(0x8003, 0x900, METHOD_BUFFERED, FILE_READ_ACCESS)
#define PS5CMOS_TAG 'somc'

NTKERNELAPI NTSTATUS ObReferenceObjectByName(PUNICODE_STRING Name, ULONG Attributes, PACCESS_STATE AccessState,
    ACCESS_MASK DesiredAccess, POBJECT_TYPE ObjectType, KPROCESSOR_MODE AccessMode, PVOID ParseContext, PVOID *Object);
extern POBJECT_TYPE *IoDriverObjectType;

typedef struct { PDEVICE_OBJECT lower; PDRIVER_OBJECT target; PDEVICE_OBJECT lowest; } EXT, *PEXT;
static PDRIVER_OBJECT g_drv;
static PDEVICE_OBJECT g_ctl;
static PDEVICE_OBJECT volatile g_filter;     /* current attachment (one CPUID device at a time) */
static FAST_MUTEX g_attach_lock;
static volatile LONG g_index, g_reads, g_writes, g_dropped, g_passed, g_attaches, g_detaches, g_watching;

DRIVER_INITIALIZE DriverEntry;
static DRIVER_DISPATCH dispatch;

/* ---------------------------------------------------------------- RTC */
static void rtc_now(struct rtc_time *t)
{
    LARGE_INTEGER sys, loc; TIME_FIELDS f;
    KeQuerySystemTimePrecise(&sys);
    ExSystemTimeToLocalTime(&sys, &loc);
    RtlTimeToTimeFields(&loc, &f);
    t->year = (unsigned)f.Year; t->month = (unsigned)f.Month; t->day = (unsigned)f.Day;
    t->hour = (unsigned)f.Hour; t->minute = (unsigned)f.Minute; t->second = (unsigned)f.Second;
    t->weekday = (unsigned)f.Weekday; t->subsec_100ns = (uint32_t)(loc.QuadPart % 10000000LL);
}
static ULONG rtc_read(ULONG port)
{
    struct rtc_time t;
    if (port == 0x70 || port == 0x72) return (ULONG)(g_index & 0x7f);
    rtc_now(&t); InterlockedIncrement(&g_reads);
    return rtc_emu_reg(&t, (unsigned)g_index);
}
static void rtc_write(ULONG port, ULONG value)
{
    if (port == 0x70 || port == 0x72) { InterlockedExchange(&g_index, (LONG)(value & 0x7f)); InterlockedIncrement(&g_writes); }
    else InterlockedIncrement(&g_dropped);
}

/* ---------------------------------------------------------------- dispatch */
static NTSTATUS done(PIRP irp, NTSTATUS st, ULONG_PTR info)
{
    irp->IoStatus.Status = st; irp->IoStatus.Information = info;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return st;
}
static NTSTATUS ctl(PIRP irp, PIO_STACK_LOCATION s)
{
    if (s->MajorFunction == IRP_MJ_CREATE || s->MajorFunction == IRP_MJ_CLOSE || s->MajorFunction == IRP_MJ_CLEANUP) return done(irp, STATUS_SUCCESS, 0);
    if (s->MajorFunction == IRP_MJ_DEVICE_CONTROL && s->Parameters.DeviceIoControl.IoControlCode == IOCTL_PS5CMOS_STATS &&
        s->Parameters.DeviceIoControl.OutputBufferLength >= 9 * sizeof(ULONG)) {
        ULONG *o = (ULONG *)irp->AssociatedIrp.SystemBuffer;
        o[0] = 0x35314D43u; o[1] = g_filter != NULL; o[2] = (ULONG)g_reads; o[3] = (ULONG)g_writes; o[4] = (ULONG)g_dropped;
        o[5] = (ULONG)g_passed; o[6] = (ULONG)g_index; o[7] = (ULONG)g_attaches; o[8] = (ULONG)g_detaches;
        return done(irp, STATUS_SUCCESS, 9 * sizeof(ULONG));
    }
    return done(irp, STATUS_INVALID_DEVICE_REQUEST, 0);
}
static NTSTATUS dispatch(PDEVICE_OBJECT d, PIRP irp)
{
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    if (d == g_ctl) return ctl(irp, s);
    PEXT e = (PEXT)d->DeviceExtension;
    if (s->MajorFunction == IRP_MJ_DEVICE_CONTROL) {
        ULONG code = s->Parameters.DeviceIoControl.IoControlCode;
        ULONG in = s->Parameters.DeviceIoControl.InputBufferLength, out = s->Parameters.DeviceIoControl.OutputBufferLength;
        ULONG *b = (ULONG *)irp->AssociatedIrp.SystemBuffer;
        switch (code) {
        case CPUZ_READ_B: case CPUZ_READ_W: case CPUZ_READ_D:
            if (b && in >= 4 && out >= 8 && rtc_emu_port(b[0])) {
                ULONG v = rtc_read(b[0]);
                b[0] = code == CPUZ_READ_B ? (v & 0xff) : code == CPUZ_READ_W ? (v & 0xff) | 0xff00u : (v & 0xff) | 0xffffff00u;
                b[1] = 0x87654321u;
                return done(irp, STATUS_SUCCESS, 8);
            }
            break;
        case CPUZ_WRITE_B: case CPUZ_WRITE_W: case CPUZ_WRITE_D:
            if (b && in >= 8 && out >= 8 && rtc_emu_port(b[0])) {
                rtc_write(b[0], b[1]); b[0] = b[1] = 0x87654321u;
                return done(irp, STATUS_SUCCESS, 8);
            }
            break;
        case CPUZ_IDX_RD:
            if (b && in >= 12 && out >= 4 && rtc_emu_port(b[0]) && rtc_emu_port(b[1])) {
                rtc_write(b[0], b[2]); b[0] = rtc_read(b[1]);
                return done(irp, STATUS_SUCCESS, 4);
            }
            break;
        case CPUZ_IDX_WR:
            if (b && in >= 16 && out >= 4 && rtc_emu_port(b[0]) && rtc_emu_port(b[1])) {
                rtc_write(b[0], b[2]); rtc_write(b[1], b[3]); b[0] = 0x12345678u;
                return done(irp, STATUS_SUCCESS, 4);
            }
            break;
        default: break;
        }
        InterlockedIncrement(&g_passed);
    }
    IoSkipCurrentIrpStackLocation(irp);
    return IoCallDriver(e->lower, irp);
}

/* ---------------------------------------------------------------- attach */
static KSTART_ROUTINE monitor_thread;
/* v1.5: DEVOBJ_EXTENSION (x64, stable since XP: Type +0, Size +2, DeviceObject +8, PowerFlags +0x10, Dope +0x18,
 * ExtensionFlags +0x20). DOE_UNLOAD_PENDING = 1 is set by IopCheckUnloadDriver when a stop request finds the device
 * still referenced or attached. Read only, and only when the layout checks out. */
#define PS5_DOE_UNLOAD_PENDING 0x1u
static BOOLEAN unload_pending(PDEVICE_OBJECT dev)
{
    PUCHAR x = (PUCHAR)dev->DeviceObjectExtension;
    if (x == NULL || *(CSHORT *)x != 13 /* IO_TYPE_DEVICE_OBJECT_EXTENSION */ || *(PDEVICE_OBJECT *)(x + 8) != dev) return FALSE;
    return (*(volatile ULONG *)(x + 0x20) & PS5_DOE_UNLOAD_PENDING) != 0;
}
/* v1.4: stay attached while the CPUID driver lives; detach when the I/O manager starts its unload (service stop) */
static void monitor_thread(PVOID ctx)
{
    PDEVICE_OBJECT me = (PDEVICE_OBJECT)ctx; PEXT e = (PEXT)me->DeviceExtension; LARGE_INTEGER delay;
    delay.QuadPart = -500000;                  /* 50 ms */
    while (!(e->target->Flags & DRVO_UNLOAD_INVOKED) && !unload_pending(e->lowest)) KeDelayExecutionThread(KernelMode, FALSE, &delay);
    ExAcquireFastMutex(&g_attach_lock);
    if (g_filter == me) g_filter = NULL;
    ExReleaseFastMutex(&g_attach_lock);
    IoDetachDevice(e->lower);                  /* the lower device is delete-pending now: no new handles, no IRPs */
    InterlockedIncrement(&g_detaches);
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "ps5cmos: detached (CPUID driver unloading)\n");
    PDRIVER_OBJECT t = e->target;
    IoDeleteDevice(me);
    ObDereferenceObject(t);                    /* lets the I/O manager finish the unload */
    PsTerminateSystemThread(STATUS_SUCCESS);
}
static NTSTATUS attach(PDRIVER_OBJECT cz)
{
    PDEVICE_OBJECT target = cz->DeviceObject, me = NULL; NTSTATUS st; HANDLE th;
    if (target == NULL) return STATUS_DEVICE_NOT_READY;
    if (cz->Flags & DRVO_UNLOAD_INVOKED) return STATUS_DELETE_PENDING;
    ExAcquireFastMutex(&g_attach_lock);
    if (g_filter != NULL) { ExReleaseFastMutex(&g_attach_lock); return STATUS_ALREADY_REGISTERED; }
    st = IoCreateDevice(g_drv, sizeof(EXT), NULL, target->DeviceType, target->Characteristics, FALSE, &me);
    if (NT_SUCCESS(st)) {
        PEXT e = (PEXT)me->DeviceExtension;
        RtlZeroMemory(e, sizeof(*e));
        me->Flags |= target->Flags & (DO_BUFFERED_IO | DO_DIRECT_IO);
        st = IoAttachDeviceToDeviceStackSafe(me, target, &e->lower);
        if (NT_SUCCESS(st)) {
            ObReferenceObject(cz); e->target = cz; e->lowest = target;
            me->Flags &= ~DO_DEVICE_INITIALIZING;
            st = PsCreateSystemThread(&th, THREAD_ALL_ACCESS, NULL, NULL, NULL, monitor_thread, me);
            if (NT_SUCCESS(st)) { ZwClose(th); g_filter = me; InterlockedIncrement(&g_attaches); }
            else { IoDetachDevice(e->lower); ObDereferenceObject(cz); IoDeleteDevice(me); }
        } else IoDeleteDevice(me);
    }
    ExReleaseFastMutex(&g_attach_lock);
    return st;
}
typedef struct { PVOID base; } WATCH;
static KSTART_ROUTINE watch_thread;
/* \Driver\cpuz<NNN> whose image is exactly 'base' (the image that was just loaded), or NULL */
static PDRIVER_OBJECT find_cpuz(PVOID base)   /* base NULL: any loaded cpuz1NN */
{
    WCHAR buf[24]; UNICODE_STRING n;
    for (ULONG v = 100; v <= 199; v++) {
        PDRIVER_OBJECT cz = NULL;
        RtlCopyMemory(buf, L"\\Driver\\cpuz", 12 * sizeof(WCHAR));
        buf[12] = (WCHAR)(L'0' + v / 100); buf[13] = (WCHAR)(L'0' + (v / 10) % 10); buf[14] = (WCHAR)(L'0' + v % 10); buf[15] = 0;
        RtlInitUnicodeString(&n, buf);
        if (!NT_SUCCESS(ObReferenceObjectByName(&n, OBJ_CASE_INSENSITIVE, NULL, 0, *IoDriverObjectType, KernelMode, NULL, (PVOID *)&cz))) continue;
        if (base == NULL || cz->DriverStart == base) return cz;
        ObDereferenceObject(cz);
    }
    return NULL;
}
static void watch_thread(PVOID ctx)
{
    WATCH *w = (WATCH *)ctx; LARGE_INTEGER delay; NTSTATUS st = STATUS_TIMEOUT; ULONG found = 0;
    delay.QuadPart = -10000;                   /* 1 ms */
    KeSetPriorityThread(KeGetCurrentThread(), HIGH_PRIORITY - 2);
    for (ULONG i = 0; i < 3000; i++) {         /* <= ~3 s with a 1 ms timer, longer with a coarse one */
        PDRIVER_OBJECT cz = find_cpuz(w->base);
        if (cz != NULL) {
            found = 1;
            st = cz->DeviceObject ? attach(cz) : STATUS_DEVICE_NOT_READY;
            ObDereferenceObject(cz);
            if (st != STATUS_DEVICE_NOT_READY) break;
        } else if (i > 200 && !found) {        /* 200 ms after the load and still no cpuz driver object: not a CPUID image */
            st = STATUS_NOT_FOUND; break;
        }
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
    }
    if (found) DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "ps5cmos: CPUID driver at %p -> attach %08X\n", w->base, st);
    ExFreePoolWithTag(w, PS5CMOS_TAG);
    InterlockedDecrement(&g_watching);
    PsTerminateSystemThread(STATUS_SUCCESS);
}
static void load_image(PUNICODE_STRING full, HANDLE pid, PIMAGE_INFO info)
{
    UNREFERENCED_PARAMETER(full);
    if (pid != NULL || info == NULL || !info->SystemModeImage || info->ImageBase == NULL) return;
    if (InterlockedIncrement(&g_watching) > 8) { InterlockedDecrement(&g_watching); return; }
    WATCH *w = (WATCH *)ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(WATCH), PS5CMOS_TAG);
    if (w == NULL) { InterlockedDecrement(&g_watching); return; }
    w->base = info->ImageBase;
    HANDLE th;
    if (!NT_SUCCESS(PsCreateSystemThread(&th, THREAD_ALL_ACCESS, NULL, NULL, NULL, watch_thread, w))) {
        ExFreePoolWithTag(w, PS5CMOS_TAG); InterlockedDecrement(&g_watching); return;
    }
    ZwClose(th);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
    UNICODE_STRING ctlname, link; NTSTATUS st;
    UNREFERENCED_PARAMETER(reg);
    g_drv = drv;
    ExInitializeFastMutex(&g_attach_lock);
    for (ULONG i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) drv->MajorFunction[i] = dispatch;
    RtlInitUnicodeString(&ctlname, L"\\Device\\Ps5Cmos"); RtlInitUnicodeString(&link, L"\\DosDevices\\Global\\PS5CMOS");
    st = IoCreateDevice(drv, 0, &ctlname, FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN, FALSE, &g_ctl);
    if (!NT_SUCCESS(st)) return st;
    IoCreateSymbolicLink(&link, &ctlname);
    g_ctl->Flags &= ~DO_DEVICE_INITIALIZING;
    st = PsSetLoadImageNotifyRoutine(load_image);
    if (!NT_SUCCESS(st)) { IoDeleteSymbolicLink(&link); IoDeleteDevice(g_ctl); return st; }
    /* a CPUID driver may already be running (loaded before us): attach to it once */
    { PDRIVER_OBJECT cz = find_cpuz(NULL); if (cz != NULL) { attach(cz); ObDereferenceObject(cz); } }
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "ps5cmos v1.5: RTC compatibility layer armed\n");
    return STATUS_SUCCESS;
}
