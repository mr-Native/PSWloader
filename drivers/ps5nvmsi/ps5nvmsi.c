/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * ps5nvmsi.c v0.8.9 (26.11.2026: the proxy function comes from the device itself, any NVMe brand and slot position on
 * bus 0x40 as in PS5 Linux; v0.8.4 14.-15.11.2026) - way (b1) for the internal M.2 under Windows: WDM lower filter between
 * stornvme (FDO) and pci.sys (PDO) of the M.2 NVMe (reference: SanDisk 15B7:5063 at the Sony proxy position 0x40:02.5, ARI 0x15).
 *
 * Why: pci.sys reads the SHADOW config of the proxy. Its MSI-X capability is a stub (table size 1 at BIR0+0, read-only,
 * nvprobe 13.11. 19:48), so Windows programs its single vector into the NVMe registers at BAR0+0 while the REAL
 * SanDisk has 65 entries at BIR0+0x3000 with MSI-X disabled - no interrupt ever arrives, stornvme fails (code 10).
 *
 * What this filter does on IRP_MN_START_DEVICE, after pci.sys completed it (resources assigned):
 *  1. reads the REAL config through the Sony SFC window of function 0 on bus 0x40 (linux.patch sfc_read/sfc_write),
 *     target = RID@0x150 of the shadow (0x2000 = internal bus 32, slot 0, function 0); checks identity 15B7:5063;
 *  2. pairs the raw and translated resources (v0.2): every full descriptor of both lists, the two lists must have the
 *     same shape (count and type per position, else no arm); the raw memory descriptor must equal the REAL BAR0 bus
 *     address, its translated twin is what gets mapped (and must equal it - identity translation on this platform);
 *     exactly one message interrupt with MessageCount 1;
 *  3. builds the MSI address/data ONLY from the format the read-only gate ps5msiprobe observed (v0.2): Parameters
 *     MsiAddrTemplate (0xFEE00000 | RH/DM bits, destination field 0), MsiDataTemplate (delivery/trigger bits, vector
 *     0) and MsiDestKind (0 physical APIC ID, 1 logical flat LDR) - no parameters, no arm. x2APIC is refused (the
 *     8-bit xAPIC destination cannot address it). Physical: exactly one processor in the affinity. Logical: flat DFR
 *     on every target, destination = OR of their LDR bytes;
 *  4. SNAPSHOT first (v0.2): real command register, MSI-X control and every table dword of all entries; then masks
 *     entries 1..n-1, writes entry 0, INTx disable, MSI-X enable, verifies every write by readback, unmasks entry 0
 *     last. ANY failure (and STOP / SURPRISE_REMOVAL / REMOVE later) restores the snapshot in reverse order (MSI-X
 *     control first, then the table with the vector-control dword last per entry, then the command register) and
 *     verifies the restore; the START IRP then fails with STATUS_DEVICE_CONFIGURATION_ERROR before stornvme sees it.
 *     (SURPRISE_REMOVAL is the exception since v0.4: the device counts as gone, nothing is restored or accessed.)
 * Memory decode and bus master are left as the firmware set them (required to be on). IRPs that go down hold the
 * remove lock until THEIR completion (v0.2 completion routine), not just until IoCallDriver returns.
 * Only this one device instance (device-level LowerFilters, install-ps5nvmsi.ps1), never a class filter.
 * v0.3 (v0.2 re-review + PnP start-order addendum):
 *  - quiescence gate: the vector goes live before stornvme connects its ISR, so the controller must be DISABLED at our
 *    START (NVMe CC.EN = 0 at BAR0+0x14, CSTS readable) - an enabled controller (firmware left it running, admin CQ
 *    always uses vector 0) is refused;
 *  - a failed restore keeps the snapshot and the mappings for the next STOP / REMOVE (only REMOVE is final), a later
 *    START with an unrestored device is refused, QUERY_STOP is vetoed while armed (no BAR rebalance under us);
 *  - D0 re-arm: after a device SET_POWER D0 completes, the real MSI-X state is compared with what was armed and
 *    re-applied when the D3 transition lost it (completion routine, <= DISPATCH, under the state lock that also guards
 *    restore/unmap); REMOVE waits for all outstanding IRPs before it restores and unmaps;
 *  - data template: only bits 15 (trigger), 14 (level) and 10:8 (fixed / lowest priority) may be set.
 * v0.4 (v0.3 re-review): quiescence also needs CSTS.RDY = 0 (bounded wait up to 500 ms after CC.EN = 0,
 *    else refused); a failed D0 re-arm rolls back to the snapshot and FAILS the D0 IRP (stornvme does not continue with
 *    an unarmed vector), Armed stays set so the next D0 tries again; after SURPRISE_REMOVAL the device is treated as
 *    gone - no SFC/MMIO access any more, REMOVE only releases the mappings.
 * v0.5 (v0.4 re-review): StillArmed() also requires every entry 1..n-1 to be masked; ApplyArm() sets and
 *    verifies the MSI-X FUNCTION mask first whenever MSI-X is already enabled (D0 repair), so no other vector can fire
 *    while the table is masked and reprogrammed; the final control write clears the function mask again.
 * v0.6 (v0.5 follow-ups): STOP and REMOVE always release the filter's MMIO mappings before the IRP goes
 *    down (WDM: a filter that acquired resources releases them on STOP_DEVICE) - a failed restore only leaves the
 *    logical "unrestored" mark (Touched), so the next START maps the new resources afresh and refuses to arm; after
 *    SURPRISE_REMOVAL new ordinary IRPs are failed with STATUS_NO_SUCH_DEVICE (CLOSE/CLEANUP still pass, PnP/power
 *    keep their own handling).
 * v0.7 (15.11., first real ps5msiprobe run 20:49): Windows on the PS5 programs MSI in LOGICAL CLUSTER mode
 *    (DFR 0x0fffffff, LDR cluster<<4|bit), lowest priority, data bit 11 set (Linux: dest_mode_logical), e.g. 13EA/9104/
 *    9107: addr fee3f00c data 49xx. Instead of re-deriving the destination, MsiDestKind = 2 takes address and data 1:1
 *    from the HAL: at START (after pci.sys, before stornvme) the filter connects a message-based dummy ISR to the PDO,
 *    copies IO_INTERRUPT_MESSAGE_INFO entry 0 (MessageAddress/MessageData) and disconnects again at once. Still gated:
 *    exactly one message, high address 0, 0xFEE / reserved address bits clear, vector byte = translated vector, and the
 *    message must match the probe templates (address with 19:12 cleared, data with 7:0 cleared). Kinds 0/1 unchanged.
 * v0.8.4 (16.11., Windows-only write errors on large transfers): logs the REAL PCIe DevCap/DevCtl/LnkSta next to the
 *    shadow DevCtl that pci.sys programmed; optional RealDevCtlMode 1 copies MPS/MRRS from the shadow into the real
 *    function, 2 uses RealDevCtl (only bits 14:12 MRRS, 7:5 MPS; MPS capped by DevCap). Restored on STOP/REMOVE,
 *    re-applied on D0. Default 0 = log only.
 * v0.8.3 (16.11.): PairResources logs every descriptor and the exact refusal (first live result: 'resources' refused,
 *    MessageMaximum 32 -> MessageNumberLimit=1 needed).
 * v0.8.2 (16.11.): FIX - the DriverEntry log line missed its msi-mode argument (v0.8), so %s read gAddrTemplate
 *    0xFEE0000C as a pointer; harmless with DbgPrintEx (filtered, not formatted), a bugcheck once v0.8.1 always formats.
 *    all LOGP formats checked once with a printf format attribute (only benign ULONG width notes remain).
 * v0.8.1 (16.11.): log ring -> Parameters\LastLog / LastStartStatus / LastLowerStartStatus at the end of START.
 * v0.8 (16.11., prepared for the case 'armed but no interrupt'): optional Parameters\MsiUseMsi=1 programs ONE message
 *    on the REAL MSI capability (@0x90 on the SanDisk) instead of MSI-X entry 0; MSI-X stays disabled. Same
 *    address/data source (kind 0/1/2), snapshot/restore/D0 re-arm for the MSI registers. Absent/0 = v0.7.2 behaviour.
 * v0.7.2 (first NVMe boot 16.11.): quiesce an enabled controller (CC.EN=0, wait RDY=0 up to CAP.TO) instead of
 *    refusing it - the firmware boot device is left enabled by NvmExpressDxe.
 * v0.7.1 (independent review 15.11.): also require MessageInfo[0].Vector == translated vector; log NVMe INTMS around the
 *    transient connect. v0.7 = for review; test only with the parameters msi-params-from-probe.py v1.1 prints.
 */
#define NT_PROCESSOR_GROUPS 1
#include <ntddk.h>
#include <ntstrsafe.h>

#define TAG 'mvn5'
#define ECAM_PHYS 0xF0000000ULL
#define PROXY_BUS 0x40
#define PROXY_DEVFN 0x15                      /* device 2, function 5 = ARI function 0x15 */
#define NVME_CLASS_CODE 0x010802U             /* PCI Mass Storage / Non-Volatile Memory / NVM Express */
#define BAR0_SIZE 0x4000U
#define BAR0_MAX_SIZE 0x100000U
#define MAX_ENTRIES 256U                      /* supports any NVMe SSD up to 256 MSI-X entries */
/* v0.8.1: every log line also goes into a small ring; START writes it to Parameters\LastLog (REG_MULTI_SZ) and
 * LastStartStatus (DWORD) at PASSIVE, so the result is readable live and offline from the hive. */
#define LOG_LINES 48U
#define LOG_CHARS 168U
static CHAR gLog[LOG_LINES][LOG_CHARS];
static ULONG gLogNext, gLogCount;
static KSPIN_LOCK gLogLock;
static WCHAR gParamPath[256];
/* v0.8.2: all LOGP formats were checked once with __attribute__((format(printf,1,2))) (-Wno-error=format): only
 * benign %u/%x-vs-ULONG width notes remain (LLP64, same size); the one real mismatch (DriverEntry) is fixed. */
static VOID Ps5Log (PCSTR Fmt, ...)
{
  CHAR Line[LOG_CHARS]; va_list Ap; KIRQL Irql;
  va_start (Ap, Fmt);
  if (!NT_SUCCESS (RtlStringCbVPrintfA (Line, sizeof (Line), Fmt, Ap))) Line[LOG_CHARS - 1] = 0;
  va_end (Ap);
  DbgPrintEx (DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "ps5nvmsi: %s", Line);
  KeAcquireSpinLock (&gLogLock, &Irql);
  RtlCopyMemory (gLog[gLogNext], Line, sizeof (Line));
  gLogNext = (gLogNext + 1) % LOG_LINES; if (gLogCount < LOG_LINES) gLogCount++;
  KeReleaseSpinLock (&gLogLock, Irql);
}
#define LOGP(...) Ps5Log (__VA_ARGS__)
static VOID Ps5LogFlush (NTSTATUS Final, NTSTATUS Lower)      /* PASSIVE only (START path) */
{
  static WCHAR Multi[LOG_LINES * LOG_CHARS + 2];
  static CHAR Copy[LOG_LINES][LOG_CHARS];
  ULONG N, First, Pos = 0; KIRQL Irql;
  if (gParamPath[0] == 0 || KeGetCurrentIrql () != PASSIVE_LEVEL) return;
  KeAcquireSpinLock (&gLogLock, &Irql);
  N = gLogCount; First = (gLogNext + LOG_LINES - gLogCount) % LOG_LINES;
  for (ULONG I = 0; I < N; I++) RtlCopyMemory (Copy[I], gLog[(First + I) % LOG_LINES], LOG_CHARS);
  KeReleaseSpinLock (&gLogLock, Irql);
  for (ULONG I = 0; I < N; I++) {
    for (ULONG K = 0; K < LOG_CHARS - 1 && Copy[I][K]; K++) {
      CHAR C = Copy[I][K];
      if (C == '\n' || C == '\r') continue;
      Multi[Pos++] = (WCHAR)(UCHAR)C;
    }
    Multi[Pos++] = 0;
  }
  Multi[Pos++] = 0;
  ULONG St = (ULONG)Final, Lo = (ULONG)Lower;
  RtlWriteRegistryValue (RTL_REGISTRY_ABSOLUTE, gParamPath, L"LastLog", REG_MULTI_SZ, Multi, Pos * sizeof (WCHAR));
  RtlWriteRegistryValue (RTL_REGISTRY_ABSOLUTE, gParamPath, L"LastStartStatus", REG_DWORD, &St, sizeof (St));
  RtlWriteRegistryValue (RTL_REGISTRY_ABSOLUTE, gParamPath, L"LastLowerStartStatus", REG_DWORD, &Lo, sizeof (Lo));
}

static __inline ULONGLONG RdMsr (ULONG Register)
{
  ULONG Lo, Hi;
  __asm__ __volatile__ ("rdmsr" : "=a" (Lo), "=d" (Hi) : "c" (Register));
  return ((ULONGLONG)Hi << 32) | Lo;
}

typedef struct _SNAP {
  BOOLEAN Valid;
  USHORT Cmd, MsixCtl;
  ULONG Table[MAX_ENTRIES * 4];
} SNAP;

typedef struct _FDX {
  PDEVICE_OBJECT Self, Lower, Pdo;
  IO_REMOVE_LOCK RemoveLock;
  KSPIN_LOCK SfcLock;
  PUCHAR Sfc;                                 /* ECAM page of bus 0x40 function 0 (SFC window registers) */
  PUCHAR Shadow;                              /* ECAM page of the proxy function (RID@0x150) */
  PUCHAR Bar0;                                /* BAR0 of the device (translated address) */
  ULONG Bar0Size;                             /* mapped length of BAR0 */
  ULONG Rid, MsixCap, Entries, TableOff;
  BOOLEAN Touched;                            /* a write happened -> Restore must run */
  BOOLEAN Armed;                              /* fully armed: the D0 re-arm may run */
  BOOLEAN Gone;                               /* v0.4: surprise-removed - never touch SFC/MMIO again */
  BOOLEAN Bypass;                             /* v0.8.6: not on Sony proxy bus 0x40 -> 100% transparent passthrough */
  ULONG DevFn;                                /* v0.8.9: the proxy function of THIS device on bus 0x40 (0x15 on the reference console) */
  KSPIN_LOCK StateLock;                       /* re-arm vs restore/unmap (order: StateLock, then SfcLock) */
  ULONG ArmAddr, ArmData;                     /* what entry 0 must hold while armed */
  BOOLEAN UseMsi, Msi64;                      /* v0.8: real MSI capability instead of MSI-X (MsiUseMsi=1) */
  ULONG MsiCap, SnapMsiLo, SnapMsiHi;
  USHORT SnapMsiCtl, SnapMsiData;
  ULONG PcieCap;                              /* v0.8.4: real PCIe capability (0xC0 on the SanDisk) */
  USHORT SnapDevCtl, ArmDevCtl;               /* real Device Control before / while armed (0 = untouched) */
  USHORT ArmCmd, ArmCtl;                      /* command register / MSI-X control before arming */
  SNAP Snap;
} FDX, *PFDX;

/* Parameters (REG_DWORD), written from the ps5msiprobe result; all three are required to arm */
static ULONG gAddrTemplate, gDataTemplate, gDestKind;
static ULONG gUseMsi;
static ULONG gDevCtlMode, gDevCtlValue;       /* v0.8.4: RealDevCtlMode 0 log only, 1 MPS/MRRS from the shadow, 2 RealDevCtl */                         /* v0.8: optional Parameters\MsiUseMsi (default 0 = MSI-X as before) */
static BOOLEAN gFormatOk;

/* ---------- Sony SFC window (linux.patch): indexed access to the real function behind the ARI proxy ---------- */
static ULONG SfcSelect (PFDX F, ULONG Reg)
{
  ULONG Bus = (F->Rid >> 8) & 0xFF, Slot = (F->Rid & 0xFF) >> 3, Func = F->Rid & 7;
  ULONG Addr = (Slot << 19) | (Func << 16) | (Reg & 0xF80);
  if (Bus == 0 || Bus == 32) {
    WRITE_REGISTER_ULONG ((PULONG)(F->Sfc + 0xD90), 4);
    WRITE_REGISTER_ULONG ((PULONG)(F->Sfc + 0xD80), (32U << 24) | Addr);
    return 0xF80 + (Reg & 0x7F);
  }
  WRITE_REGISTER_ULONG ((PULONG)(F->Sfc + 0xD10), Bus == 1 ? 4 : 5);
  WRITE_REGISTER_ULONG ((PULONG)(F->Sfc + 0xD00), (Bus << 24) | Addr);
  return 0xF00 + (Reg & 0x7F);
}
static ULONG RealRead32 (PFDX F, ULONG Reg)
{
  KIRQL Irql; ULONG V;
  KeAcquireSpinLock (&F->SfcLock, &Irql);
  V = READ_REGISTER_ULONG ((PULONG)(F->Sfc + SfcSelect (F, Reg & ~3U)));
  KeReleaseSpinLock (&F->SfcLock, Irql);
  return V;
}
static VOID RealWrite16 (PFDX F, ULONG Reg, USHORT Value)
{
  KIRQL Irql;
  KeAcquireSpinLock (&F->SfcLock, &Irql);
  WRITE_REGISTER_USHORT ((PUSHORT)(F->Sfc + SfcSelect (F, Reg & ~1U)), Value);
  KeReleaseSpinLock (&F->SfcLock, Irql);
}
static USHORT RealRead16 (PFDX F, ULONG Reg) { return (USHORT)(RealRead32 (F, Reg & ~3U) >> ((Reg & 2) * 8)); }
static VOID RealWrite32 (PFDX F, ULONG Reg, ULONG Value)        /* v0.8: MSI address dwords */
{
  KIRQL Irql;
  KeAcquireSpinLock (&F->SfcLock, &Irql);
  WRITE_REGISTER_ULONG ((PULONG)(F->Sfc + SfcSelect (F, Reg & ~3U)), Value);
  KeReleaseSpinLock (&F->SfcLock, Irql);
}
#define MSI_DATA_REG(F) ((F)->MsiCap + ((F)->Msi64 ? 0xC : 0x8))

static NTSTATUS MapPages (PFDX F)
{
  PHYSICAL_ADDRESS P;
  P.QuadPart = (LONGLONG)(ECAM_PHYS + ((ULONGLONG)PROXY_BUS << 20));
  if (!F->Sfc) F->Sfc = (PUCHAR)MmMapIoSpaceEx (P, PAGE_SIZE, PAGE_READWRITE | PAGE_NOCACHE);   /* kept after a failed restore */
  P.QuadPart = (LONGLONG)(ECAM_PHYS + ((ULONGLONG)PROXY_BUS << 20) + ((ULONGLONG)(F->DevFn & 0xFF) << 12));
  if (!F->Shadow) F->Shadow = (PUCHAR)MmMapIoSpaceEx (P, PAGE_SIZE, PAGE_READONLY | PAGE_NOCACHE);
  return (F->Sfc && F->Shadow) ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
}
static VOID UnmapAll (PFDX F)
{
  if (F->Bar0) { MmUnmapIoSpace (F->Bar0, F->Bar0Size ? F->Bar0Size : BAR0_SIZE); F->Bar0 = NULL; F->Bar0Size = 0; }
  if (F->Shadow) { MmUnmapIoSpace (F->Shadow, PAGE_SIZE); F->Shadow = NULL; }
  if (F->Sfc) { MmUnmapIoSpace (F->Sfc, PAGE_SIZE); F->Sfc = NULL; }
}

/* ---------- snapshot / restore of everything Arm may touch ---------- */
static VOID TakeSnapshot (PFDX F)
{
  PUCHAR E = F->Bar0 + F->TableOff;
  F->Snap.Cmd = RealRead16 (F, 4);
  F->Snap.MsixCtl = RealRead16 (F, F->MsixCap + 2);
  if (F->PcieCap) F->SnapDevCtl = RealRead16 (F, F->PcieCap + 8);
  if (F->UseMsi) {
    F->SnapMsiCtl = RealRead16 (F, F->MsiCap + 2); F->SnapMsiLo = RealRead32 (F, F->MsiCap + 4);
    F->SnapMsiHi = F->Msi64 ? RealRead32 (F, F->MsiCap + 8) : 0; F->SnapMsiData = RealRead16 (F, MSI_DATA_REG (F));
  }
  for (ULONG I = 0; I < F->Entries * 4; I++) F->Snap.Table[I] = READ_REGISTER_ULONG ((PULONG)(E + I * 4));
  F->Snap.Valid = TRUE;
}
/* reverse order: MSI-X control (enable/function mask as before), the table (vector control last per entry), command */
static BOOLEAN Restore (PFDX F)
{
  if (!F->Touched) return TRUE;
  if (!F->Snap.Valid || !F->Sfc || !F->Bar0) { LOGP ("restore impossible (snapshot %u)\n", F->Snap.Valid); return FALSE; }
  BOOLEAN Ok = TRUE;
  PUCHAR E = F->Bar0 + F->TableOff;
  if (F->UseMsi) {                                                                 /* v0.8: MSI off first, then its registers */
    RealWrite16 (F, F->MsiCap + 2, F->SnapMsiCtl);
    RealWrite32 (F, F->MsiCap + 4, F->SnapMsiLo);
    if (F->Msi64) RealWrite32 (F, F->MsiCap + 8, F->SnapMsiHi);
    RealWrite16 (F, MSI_DATA_REG (F), F->SnapMsiData);
    if ((RealRead16 (F, F->MsiCap + 2) & 0x71) != (F->SnapMsiCtl & 0x71) || RealRead32 (F, F->MsiCap + 4) != F->SnapMsiLo) Ok = FALSE;
  }
  RealWrite16 (F, F->MsixCap + 2, F->Snap.MsixCtl);
  if ((RealRead16 (F, F->MsixCap + 2) & 0xC7FF) != (F->Snap.MsixCtl & 0xC7FF)) Ok = FALSE;
  for (ULONG I = 0; I < F->Entries; I++) {
    WRITE_REGISTER_ULONG ((PULONG)(E + I * 16 + 12), 1);                          /* masked while it changes */
    for (ULONG D = 0; D < 3; D++) WRITE_REGISTER_ULONG ((PULONG)(E + I * 16 + D * 4), F->Snap.Table[I * 4 + D]);
    WRITE_REGISTER_ULONG ((PULONG)(E + I * 16 + 12), F->Snap.Table[I * 4 + 3]);
    for (ULONG D = 0; D < 4; D++)
      if (READ_REGISTER_ULONG ((PULONG)(E + I * 16 + D * 4)) != F->Snap.Table[I * 4 + D]) Ok = FALSE;
  }
  if (F->PcieCap && F->ArmDevCtl) {                                               /* v0.8.4 */
    RealWrite16 (F, F->PcieCap + 8, F->SnapDevCtl);
    if ((RealRead16 (F, F->PcieCap + 8) & 0x70E0) != (F->SnapDevCtl & 0x70E0)) Ok = FALSE;
  }
  RealWrite16 (F, 4, F->Snap.Cmd);
  if (RealRead16 (F, 4) != F->Snap.Cmd) Ok = FALSE;
  LOGP ("restore %s\n", Ok ? "verified" : "FAILED (device left as read back)");
  if (Ok) F->Touched = FALSE;
  return Ok;
}

/* ---------- MSI destination in the observed format ---------- */
typedef struct _CPUAPIC { BOOLEAN X2; ULONG Id, Ldr, Dfr; } CPUAPIC;
static BOOLEAN ApicOf (PROCESSOR_NUMBER *Pn, CPUAPIC *Out)
{
  GROUP_AFFINITY Ga = { 0 }, Old;
  BOOLEAN Ok = FALSE;
  Ga.Group = Pn->Group; Ga.Mask = (KAFFINITY)1 << Pn->Number;
  KeSetSystemGroupAffinityThread (&Ga, &Old);
  ULONGLONG Base = RdMsr (0x1B);
  Out->X2 = (Base & (1ULL << 10)) != 0;
  if (!Out->X2) {
    PHYSICAL_ADDRESS P; P.QuadPart = (LONGLONG)(Base & 0xFFFFFF000ULL);
    PUCHAR L = (PUCHAR)MmMapIoSpaceEx (P, PAGE_SIZE, PAGE_READONLY | PAGE_NOCACHE);
    if (L) {
      Out->Id = READ_REGISTER_ULONG ((PULONG)(L + 0x20)) >> 24;
      Out->Ldr = READ_REGISTER_ULONG ((PULONG)(L + 0xD0)) >> 24;
      Out->Dfr = READ_REGISTER_ULONG ((PULONG)(L + 0xE0));
      MmUnmapIoSpace (L, PAGE_SIZE);
      Ok = TRUE;
    }
  }
  KeRevertToUserGroupAffinityThread (&Old);           /* single exit: affinity is always reverted */
  return Ok;
}
static NTSTATUS Destination (USHORT Group, KAFFINITY Aff, PULONG Dest)
{
  ULONG Count = 0, D = 0;
  for (UCHAR B = 0; B < (UCHAR)(sizeof (KAFFINITY) * 8); B++) {
    if (!(Aff & ((KAFFINITY)1 << B))) continue;
    PROCESSOR_NUMBER Pn = { 0 }; CPUAPIC A = { 0 };
    Pn.Group = Group; Pn.Number = B;
    if (!ApicOf (&Pn, &A)) { LOGP ("cpu %u:%u: x2APIC or LAPIC unreadable - refused\n", Group, B); return STATUS_NOT_SUPPORTED; }
    Count++;
    if (gDestKind == 0) D = A.Id;
    else {
      if ((A.Dfr >> 28) != 0xF) { LOGP ("cpu %u:%u: DFR %08x is not flat - refused\n", Group, B, A.Dfr); return STATUS_NOT_SUPPORTED; }
      D |= A.Ldr;
    }
  }
  if (Count == 0 || (gDestKind == 0 && Count != 1)) { LOGP ("affinity %Ix: %u cpus for kind %u - refused\n", Aff, Count, gDestKind); return STATUS_NOT_SUPPORTED; }
  if (D > 0xFF) return STATUS_NOT_SUPPORTED;
  *Dest = D;
  return STATUS_SUCCESS;
}

/* ---------- v0.7: MsiDestKind 2 - the HAL's own message for this device (address/data 1:1) ---------- */
static BOOLEAN DummyMessageIsr (PKINTERRUPT I, PVOID Ctx, ULONG Id)
{
  UNREFERENCED_PARAMETER (I); UNREFERENCED_PARAMETER (Ctx); UNREFERENCED_PARAMETER (Id);
  return FALSE;                                        /* never ours: connected only for the message info, then gone */
}
static NTSTATUS HalMessage (PFDX F, ULONG Vector, PULONG Addr, PULONG Data)
{
  IO_CONNECT_INTERRUPT_PARAMETERS P;
  PIO_INTERRUPT_MESSAGE_INFO T = NULL;
  RtlZeroMemory (&P, sizeof (P));
  P.Version = CONNECT_MESSAGE_BASED;
  P.MessageBased.PhysicalDeviceObject = F->Pdo;
  P.MessageBased.ConnectionContext.InterruptMessageTable = &T;
  P.MessageBased.MessageServiceRoutine = DummyMessageIsr;
  P.MessageBased.ServiceContext = F;
  NTSTATUS S = IoConnectInterruptEx (&P);              /* PASSIVE (START, after pci.sys); no fallback = message or fail */
  if (!NT_SUCCESS (S)) { LOGP ("HAL message: IoConnectInterruptEx %08x\n", S); return S; }
  ULONG Count = 0, A = 0, D = 0, Hi = 1, V = 0;
  if (P.Version == CONNECT_MESSAGE_BASED && T != NULL && T->MessageCount >= 1) {
    Count = T->MessageCount; V = T->MessageInfo[0].Vector; D = T->MessageInfo[0].MessageData;
    A = T->MessageInfo[0].MessageAddress.LowPart; Hi = (ULONG)T->MessageInfo[0].MessageAddress.HighPart;
  }
  IO_DISCONNECT_INTERRUPT_PARAMETERS Dp;
  RtlZeroMemory (&Dp, sizeof (Dp));
  Dp.Version = P.Version;
  Dp.ConnectionContext.InterruptMessageTable = T;
  IoDisconnectInterruptEx (&Dp);                      /* before stornvme connects; the vector is not live yet */
  LOGP ("HAL message: version %u count %u addr %08x:%08x data %08x idt %x (translated vector %x)\n", P.Version, Count, Hi, A, D, V, Vector);
  if (P.Version != CONNECT_MESSAGE_BASED || Count != 1) return STATUS_NOT_SUPPORTED;
  if (Hi != 0 || (A & 0xFFF00000U) != 0xFEE00000U || (A & 0x00000FF3U) != 0) return STATUS_NOT_SUPPORTED;
  if ((D & 0xFFFF0000U) != 0 || (D & 0xFF) != (Vector & 0xFF) || V != Vector) return STATUS_NOT_SUPPORTED;   /* v0.7.1: IDT vector too */
  if ((A & ~0x000FF000U) != gAddrTemplate || (D & ~0xFFU) != gDataTemplate) {
    LOGP ("HAL message does not match the probe templates (addr %08x data %08x) - refused\n", gAddrTemplate, gDataTemplate);
    return STATUS_NOT_SUPPORTED;
  }
  *Addr = A; *Data = D;
  return STATUS_SUCCESS;
}

/* ---------- resources: full lists, same shape, raw BAR vs translated mapping ---------- */
typedef struct _RES { PHYSICAL_ADDRESS BarT; ULONG BarLen; ULONG Vector; KAFFINITY Aff; USHORT Group; } RES;
static NTSTATUS PairResources (PCM_RESOURCE_LIST Raw, PCM_RESOURCE_LIST Tr, ULONGLONG RealBar, RES *Out)
{
  ULONG Mem = 0, Msg = 0;
  /* v0.8.3: every descriptor and every refusal reason into the log (LastLog) */
  if (Raw == NULL || Tr == NULL || Raw->Count != Tr->Count || Raw->Count == 0) {
    LOGP ("res: list raw %p tr %p counts %lu/%lu\n", Raw, Tr, Raw ? Raw->Count : 0, Tr ? Tr->Count : 0);
    return STATUS_DEVICE_CONFIGURATION_ERROR;
  }
  LOGP ("res: real BAR0 %I64x, %lu full list(s)\n", RealBar, Raw->Count);
  PCM_FULL_RESOURCE_DESCRIPTOR Fr = Raw->List, Ft = Tr->List;
  for (ULONG L = 0; L < Raw->Count; L++) {
    PCM_PARTIAL_RESOURCE_LIST Rl = &Fr->PartialResourceList, Tl = &Ft->PartialResourceList;
    if (Rl->Count != Tl->Count) { LOGP ("res: partial counts raw %lu tr %lu\n", Rl->Count, Tl->Count); return STATUS_DEVICE_CONFIGURATION_ERROR; }
    for (ULONG I = 0; I < Rl->Count; I++) {
      PCM_PARTIAL_RESOURCE_DESCRIPTOR R = &Rl->PartialDescriptors[I], T = &Tl->PartialDescriptors[I];
      if (R->Type == CmResourceTypeMemory)
        LOGP ("res %lu: mem flags %x raw %I64x+%lx tr %I64x+%lx\n", I, R->Flags, R->u.Memory.Start.QuadPart, R->u.Memory.Length,
              T->u.Memory.Start.QuadPart, T->u.Memory.Length);
      else if (R->Type == CmResourceTypeInterrupt)
        LOGP ("res %lu: int flags %x/%x raw msgcount %u vec %lx | tr vec %lx lvl %lx aff %I64x grp %u\n", I, R->Flags, T->Flags,
              (R->Flags & CM_RESOURCE_INTERRUPT_MESSAGE) ? R->u.MessageInterrupt.Raw.MessageCount : 0, R->u.Interrupt.Vector,
              T->u.Interrupt.Vector, T->u.Interrupt.Level, (ULONGLONG)T->u.Interrupt.Affinity, T->u.Interrupt.Group);
      else LOGP ("res %lu: type %u/%u flags %x\n", I, R->Type, T->Type, R->Flags);
      if (R->Type == CmResourceTypeDevicePrivate && T->Type == CmResourceTypeDevicePrivate) continue;
      if (R->Type != T->Type) { LOGP ("res: shape differs at %lu\n", I); return STATUS_DEVICE_CONFIGURATION_ERROR; }   /* not the same shape */
      if (R->Type == CmResourceTypeMemory && (ULONGLONG)R->u.Memory.Start.QuadPart == RealBar) {
        if (R->u.Memory.Length < BAR0_SIZE || R->u.Memory.Length > BAR0_MAX_SIZE || T->u.Memory.Length != R->u.Memory.Length ||
            T->u.Memory.Start.QuadPart != R->u.Memory.Start.QuadPart) { LOGP ("res: BAR0 length/translation refused\n"); return STATUS_DEVICE_CONFIGURATION_ERROR; }
        Out->BarT = T->u.Memory.Start; Out->BarLen = T->u.Memory.Length; Mem++;
      }
      if (R->Type == CmResourceTypeInterrupt && (R->Flags & CM_RESOURCE_INTERRUPT_MESSAGE)) {
        if (!(T->Flags & CM_RESOURCE_INTERRUPT_MESSAGE) || R->u.MessageInterrupt.Raw.MessageCount != 1) {
          LOGP ("res: message interrupt refused (translated message flag %u, count %u) - set MessageNumberLimit=1\n",
                (T->Flags & CM_RESOURCE_INTERRUPT_MESSAGE) != 0, R->u.MessageInterrupt.Raw.MessageCount);
          return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
        Out->Vector = T->u.MessageInterrupt.Translated.Vector;
        Out->Aff = T->u.MessageInterrupt.Translated.Affinity;
        Out->Group = T->u.MessageInterrupt.Translated.Group;
        Msg++;
      }
    }
    Fr = (PCM_FULL_RESOURCE_DESCRIPTOR)(Rl->PartialDescriptors + Rl->Count);   /* next full descriptor */
    Ft = (PCM_FULL_RESOURCE_DESCRIPTOR)(Tl->PartialDescriptors + Tl->Count);
  }
  if (Mem != 1 || Msg != 1 || Out->Aff == 0 || Out->Vector < 0x30 || Out->Vector > 0xFE) {
    LOGP ("res: summary refused: BAR0 matches %lu (need 1), message descriptors %lu (need 1 - MessageNumberLimit=1), aff %I64x vec %lx\n",
          Mem, Msg, (ULONGLONG)Out->Aff, Out->Vector);
    return STATUS_DEVICE_CONFIGURATION_ERROR;
  }
  return STATUS_SUCCESS;
}

/* ---------- arm ---------- */
#define FAIL(why) do { LOGP ("arm refused: %s\n", why); return STATUS_DEVICE_CONFIGURATION_ERROR; } while (0)
/* the write sequence (START and D0 re-arm): all entries masked, entry 0 written, INTx off, MSI-X on, entry 0 unmasked
 * last - every step read back. Runs at <= DISPATCH_LEVEL (MMIO + spin locks only). */
static NTSTATUS ApplyArm (PFDX F)
{
  PUCHAR E = F->Bar0 + F->TableOff;
  F->Touched = TRUE;                                                                /* from the first write on */
  if (F->PcieCap && F->ArmDevCtl) {                       /* v0.8.4: MPS/MRRS of the REAL function (pci.sys only sets the shadow) */
    RealWrite16 (F, F->PcieCap + 8, F->ArmDevCtl);
    if ((RealRead16 (F, F->PcieCap + 8) & 0x70E0) != (F->ArmDevCtl & 0x70E0)) FAIL ("real DevCtl MPS/MRRS readback");
  }
  if (F->UseMsi) {                                    /* v0.8: one MSI message on the real MSI capability, MSI-X stays off */
    USHORT Mc = RealRead16 (F, F->MsiCap + 2);
    RealWrite16 (F, F->MsiCap + 2, (USHORT)(Mc & ~0x71));                         /* disabled, MME 0 while it changes */
    RealWrite32 (F, F->MsiCap + 4, F->ArmAddr);
    if (F->Msi64) RealWrite32 (F, F->MsiCap + 8, 0);
    RealWrite16 (F, MSI_DATA_REG (F), (USHORT)F->ArmData);
    if (RealRead32 (F, F->MsiCap + 4) != F->ArmAddr || (F->Msi64 && RealRead32 (F, F->MsiCap + 8) != 0) ||
        RealRead16 (F, MSI_DATA_REG (F)) != (USHORT)F->ArmData) FAIL ("MSI address/data readback");
    RealWrite16 (F, 4, (USHORT)(F->ArmCmd | 0x400));                              /* INTx disable */
    RealWrite16 (F, F->MsiCap + 2, (USHORT)((Mc & ~0x70) | 1));                   /* MSI on, 1 message */
    if ((RealRead16 (F, 4) & 0x406) != 0x406 || (RealRead16 (F, F->MsiCap + 2) & 0x71) != 1 ||
        (RealRead16 (F, F->MsixCap + 2) & 0x8000)) FAIL ("MSI enable readback");
    return STATUS_SUCCESS;
  }
  USHORT Cur = RealRead16 (F, F->MsixCap + 2);
  if (Cur & 0x8000) {                                                               /* v0.5: enabled -> function mask first */
    RealWrite16 (F, F->MsixCap + 2, (USHORT)(Cur | 0x4000));
    if ((RealRead16 (F, F->MsixCap + 2) & 0xC000) != 0xC000) FAIL ("function mask readback");
  }
  for (ULONG I = 0; I < F->Entries; I++) WRITE_REGISTER_ULONG ((PULONG)(E + I * 16 + 12), 1);
  for (ULONG I = 0; I < F->Entries; I++) if ((READ_REGISTER_ULONG ((PULONG)(E + I * 16 + 12)) & 1) == 0) FAIL ("mask readback");
  WRITE_REGISTER_ULONG ((PULONG)(E + 0), F->ArmAddr);
  WRITE_REGISTER_ULONG ((PULONG)(E + 4), 0);
  WRITE_REGISTER_ULONG ((PULONG)(E + 8), F->ArmData);
  if (READ_REGISTER_ULONG ((PULONG)(E + 0)) != F->ArmAddr || READ_REGISTER_ULONG ((PULONG)(E + 4)) != 0 ||
      READ_REGISTER_ULONG ((PULONG)(E + 8)) != F->ArmData) FAIL ("entry 0 readback");
  RealWrite16 (F, 4, (USHORT)(F->ArmCmd | 0x400));                                 /* INTx disable */
  RealWrite16 (F, F->MsixCap + 2, (USHORT)((F->ArmCtl & ~0x4000) | 0x8000));       /* MSI-X on, function mask clear */
  if ((RealRead16 (F, 4) & 0x406) != 0x406 || (RealRead16 (F, F->MsixCap + 2) & 0xC000) != 0x8000) FAIL ("enable readback");
  WRITE_REGISTER_ULONG ((PULONG)(E + 12), 0);                                      /* entry 0 unmasked last */
  if (READ_REGISTER_ULONG ((PULONG)(E + 12)) & 1) FAIL ("unmask readback");
  return STATUS_SUCCESS;
}
static NTSTATUS Arm (PFDX F, PCM_RESOURCE_LIST Raw, PCM_RESOURCE_LIST Translated)
{
  RES Res = { 0 };
  ULONG Dest = 0;
  if (!gFormatOk) FAIL ("Msi* parameters missing or inconsistent (run ps5msiprobe first)");
  if (F->Touched) FAIL ("an earlier restore failed - device state unknown, not armed again");
  F->Rid = READ_REGISTER_USHORT ((PUSHORT)(F->Shadow + 0x150));
  if (F->Rid == 0 || F->Rid == 0xFFFF) FAIL ("no RID in the shadow");
  ULONG ShadowId = READ_REGISTER_ULONG ((PULONG)(F->Shadow + 0));
  ULONG RealId = RealRead32 (F, 0);
  ULONG RealClass = RealRead32 (F, 8) >> 8;
  if (RealId == 0 || RealId == 0xFFFFFFFFU || RealId != ShadowId || RealClass != NVME_CLASS_CODE) {
    LOGP ("real id %08lx shadow %08lx class %06lx - refused\n", RealId, ShadowId, RealClass);
    FAIL ("real identity / not NVMe");
  }
  LOGP ("real NVMe %04lx:%04lx class %06lx rid %04lx\n", RealId & 0xFFFFU, RealId >> 16, RealClass, F->Rid);
  ULONG Bar0Lo = RealRead32 (F, 0x10), Bar0Hi = (Bar0Lo & 6) == 4 ? RealRead32 (F, 0x14) : 0;
  ULONGLONG RealBar = ((ULONGLONG)Bar0Hi << 32) | (Bar0Lo & ~0xFU);
  if (!NT_SUCCESS (PairResources (Raw, Translated, RealBar, &Res))) FAIL ("resources (shape, BAR, single message)");
  USHORT Cmd = RealRead16 (F, 4);
  if ((Cmd & 6) != 6) FAIL ("memory decode / bus master off");
  ULONG Cap = RealRead32 (F, 0x34) & 0xFC; F->MsixCap = 0;
  for (ULONG N = 0; N < 48 && Cap >= 0x40 && Cap <= 0xF4; N++) {
    ULONG H = RealRead32 (F, Cap);
    if ((H & 0xFF) == 0x11) { F->MsixCap = Cap; break; }
    Cap = (H >> 8) & 0xFC;
  }
  if (F->MsixCap == 0) FAIL ("no real MSI-X capability");
  {                                                                                /* v0.8.4: PCIe device control, real vs shadow */
    ULONG C = RealRead32 (F, 0x34) & 0xFC; F->PcieCap = 0; F->ArmDevCtl = 0;
    for (ULONG N = 0; N < 48 && C >= 0x40 && C <= 0xF4; N++) { ULONG H = RealRead32 (F, C); if ((H & 0xFF) == 0x10) { F->PcieCap = C; break; } C = (H >> 8) & 0xFC; }
    ULONG Sc = READ_REGISTER_UCHAR (F->Shadow + 0x34) & 0xFC, ShadowPcie = 0;
    for (ULONG N = 0; N < 48 && Sc >= 0x40 && Sc <= 0xF4; N++) { ULONG H = READ_REGISTER_ULONG ((PULONG)(F->Shadow + Sc)); if ((H & 0xFF) == 0x10) { ShadowPcie = Sc; break; } Sc = (H >> 8) & 0xFC; }
    if (F->PcieCap) {
      ULONG DevCap = RealRead32 (F, F->PcieCap + 4), DevCtlSta = RealRead32 (F, F->PcieCap + 8), LnkSta = RealRead32 (F, F->PcieCap + 0x10);
      ULONG SDevCtl = ShadowPcie ? READ_REGISTER_ULONG ((PULONG)(F->Shadow + ShadowPcie + 8)) : 0;
      LOGP ("pcie real @%lx DevCap %08lx DevCtl/Sta %08lx (MPS %lu MRRS %lu) LnkCtl/Sta %08lx | shadow @%lx DevCtl %08lx (MPS %lu MRRS %lu) mode %lu\n",
            F->PcieCap, DevCap, DevCtlSta, 128UL << ((DevCtlSta >> 5) & 7), 128UL << ((DevCtlSta >> 12) & 7), LnkSta,
            ShadowPcie, SDevCtl, 128UL << ((SDevCtl >> 5) & 7), 128UL << ((SDevCtl >> 12) & 7), gDevCtlMode);
      USHORT Cur = (USHORT)DevCtlSta;
      if (gDevCtlMode == 1 && ShadowPcie) F->ArmDevCtl = (USHORT)((Cur & ~0x70E0) | (SDevCtl & 0x70E0));
      if (gDevCtlMode == 2) F->ArmDevCtl = (USHORT)((Cur & ~0x70E0) | (gDevCtlValue & 0x70E0));
      if (F->ArmDevCtl == Cur) F->ArmDevCtl = 0;                                   /* nothing to change */
      if (F->ArmDevCtl && ((F->ArmDevCtl >> 5) & 7) > (DevCap & 7)) { LOGP ("pcie: requested MPS above DevCap - not applied\n"); F->ArmDevCtl = 0; }
      if (F->ArmDevCtl) LOGP ("pcie: real DevCtl will be %04x (MPS %lu MRRS %lu)\n", F->ArmDevCtl, 128UL << ((F->ArmDevCtl >> 5) & 7), 128UL << ((F->ArmDevCtl >> 12) & 7));
    }
  }
  F->UseMsi = gUseMsi != 0; F->MsiCap = 0;
  if (F->UseMsi) {                                                                 /* v0.8 */
    ULONG C = RealRead32 (F, 0x34) & 0xFC;
    for (ULONG N = 0; N < 48 && C >= 0x40 && C <= 0xF4; N++) {
      ULONG H = RealRead32 (F, C);
      if ((H & 0xFF) == 0x05) { F->MsiCap = C; break; }
      C = (H >> 8) & 0xFC;
    }
    if (F->MsiCap == 0) FAIL ("MsiUseMsi=1 but no real MSI capability");
    USHORT Mc = RealRead16 (F, F->MsiCap + 2);
    F->Msi64 = (Mc & 0x80) != 0;
    if (Mc & 1) FAIL ("real MSI already enabled");
    LOGP ("MSI mode: cap @%x ctl %04x 64bit %u\n", F->MsiCap, Mc, (ULONG)F->Msi64);
  }
  USHORT Ctl = RealRead16 (F, F->MsixCap + 2);
  ULONG Tbl = RealRead32 (F, F->MsixCap + 4);
  F->Entries = (Ctl & 0x7FF) + 1; F->TableOff = Tbl & ~7U;
  if ((Ctl & 0x8000) != 0 || (Tbl & 7) != 0 || F->Entries > MAX_ENTRIES ||
      (ULONGLONG)F->TableOff + (ULONGLONG)F->Entries * 16 > Res.BarLen) FAIL ("MSI-X geometry / already enabled");
  ULONG HalAddr = 0, HalData = 0;
  if (gDestKind == 2) {
    /* v0.7.1 (review): the transient connect lets pci.sys write its stub table at BAR0+0 (vector control -> INTMS 0xC);
     * log INTMS/INTMC around it - harmless while CC.EN = 0 and with MSI-X on, but visible */
    PHYSICAL_ADDRESS Bp; Bp.QuadPart = (LONGLONG)RealBar;
    PUCHAR B = (PUCHAR)MmMapIoSpaceEx (Bp, PAGE_SIZE, PAGE_READONLY | PAGE_NOCACHE);
    ULONG Im0 = B ? READ_REGISTER_ULONG ((PULONG)(B + 0xC)) : 0xFFFFFFFFU;
    NTSTATUS Hs = HalMessage (F, Res.Vector, &HalAddr, &HalData);
    ULONG Im1 = B ? READ_REGISTER_ULONG ((PULONG)(B + 0xC)) : 0xFFFFFFFFU;
    if (B) MmUnmapIoSpace (B, PAGE_SIZE);
    LOGP ("HAL message: INTMS before %08x after %08x\n", Im0, Im1);
    if (!NT_SUCCESS (Hs)) FAIL ("HAL message (kind 2)");
  }
  else if (!NT_SUCCESS (Destination (Res.Group, Res.Aff, &Dest))) FAIL ("destination");
  F->Bar0 = (PUCHAR)MmMapIoSpaceEx (Res.BarT, Res.BarLen, PAGE_READWRITE | PAGE_NOCACHE);
  if (F->Bar0 == NULL) FAIL ("BAR0 map");
  F->Bar0Size = Res.BarLen;
  /* quiescence gate (v0.3): CC.EN must be 0 - then no queue exists that could raise vector 0 before stornvme connects */
  ULONG Cc = READ_REGISTER_ULONG ((PULONG)(F->Bar0 + 0x14)), Csts = READ_REGISTER_ULONG ((PULONG)(F->Bar0 + 0x1C));
  LOGP ("controller CC %08x CSTS %08x at START\n", Cc, Csts);
  if (Csts == 0xFFFFFFFFU) FAIL ("controller not responding (CSTS all ones)");
  /* v0.7.2: booting FROM this NVMe, the UEFI NvmExpressDxe leaves the controller enabled (it only shuts it down on a
   * reset notification, not at ExitBootServices). No command is outstanding after ExitBootServices, so quiesce it
   * here exactly as stornvme's own init does (CC.EN = 0, wait for CSTS.RDY = 0 up to CAP.TO x 500 ms, max 10 s)
   * instead of refusing - the refusal made the boot device fail (0x7B). */
  ULONG Cap0 = READ_REGISTER_ULONG ((PULONG)(F->Bar0 + 0x0)), WaitMs = ((Cap0 >> 24) & 0xFF) * 500;
  if (WaitMs < 500) WaitMs = 500;
  if (WaitMs > 10000) WaitMs = 10000;
  if (Cc & 1) {
    WRITE_REGISTER_ULONG ((PULONG)(F->Bar0 + 0x14), Cc & ~1U);
    LOGP ("controller was enabled at START (firmware boot device) - CC.EN cleared, waiting up to %u ms\n", WaitMs);
  }
  for (ULONG W = 0; (Csts & 1) && W < WaitMs / 10; W++) {                          /* v0.4: CSTS.RDY must drop to 0 */
    LARGE_INTEGER Ten; Ten.QuadPart = -10LL * 10000;                               /* 10 ms, START runs at PASSIVE */
    KeDelayExecutionThread (KernelMode, FALSE, &Ten);
    Csts = READ_REGISTER_ULONG ((PULONG)(F->Bar0 + 0x1C));
  }
  if (Csts == 0xFFFFFFFFU || (Csts & 1)) FAIL ("CSTS.RDY still 1 with CC.EN=0 - controller not quiescent");
  if (READ_REGISTER_ULONG ((PULONG)(F->Bar0 + 0x14)) & 1) FAIL ("CC.EN reads 1 after the disable write");
  TakeSnapshot (F);
  F->ArmAddr = gDestKind == 2 ? HalAddr : gAddrTemplate | (Dest << 12);
  F->ArmData = gDestKind == 2 ? HalData : gDataTemplate | (Res.Vector & 0xFF);
  F->ArmCmd = Cmd; F->ArmCtl = Ctl;
  NTSTATUS S = ApplyArm (F);
  if (!NT_SUCCESS (S)) return S;
  F->Armed = TRUE;
  LOGP ("armed rid %x vec %x aff %Ix dest %x addr %08x data %08x n %u\n", F->Rid, Res.Vector, Res.Aff, Dest, F->ArmAddr, F->ArmData, F->Entries);
  return STATUS_SUCCESS;
}

/* v0.5: entries 1..n-1 must all stay masked (ApplyArm's invariant) */
static BOOLEAN OthersMasked (PFDX F)
{
  PUCHAR E = F->Bar0 + F->TableOff;
  for (ULONG I = 1; I < F->Entries; I++) if ((READ_REGISTER_ULONG ((PULONG)(E + I * 16 + 12)) & 1) == 0) return FALSE;
  return TRUE;
}
/* is the real function still armed as recorded? (after D3 -> D0 the state may be gone) */
static BOOLEAN StillArmed (PFDX F)
{
  PUCHAR E = F->Bar0 + F->TableOff;
  if (F->UseMsi)
    return (RealRead16 (F, F->MsiCap + 2) & 0x71) == 1 && (RealRead16 (F, 4) & 0x406) == 0x406 &&
           RealRead32 (F, F->MsiCap + 4) == F->ArmAddr && RealRead16 (F, MSI_DATA_REG (F)) == (USHORT)F->ArmData;
  return (RealRead16 (F, F->MsixCap + 2) & 0xC000) == 0x8000 && (RealRead16 (F, 4) & 0x406) == 0x406 &&
         READ_REGISTER_ULONG ((PULONG)(E + 0)) == F->ArmAddr && READ_REGISTER_ULONG ((PULONG)(E + 4)) == 0 &&
         READ_REGISTER_ULONG ((PULONG)(E + 8)) == F->ArmData && (READ_REGISTER_ULONG ((PULONG)(E + 12)) & 1) == 0 &&
         OthersMasked (F);
}

/* ---------- WDM plumbing ---------- */
static NTSTATUS SyncCompletion (PDEVICE_OBJECT D, PIRP Irp, PVOID Ctx)
{
  UNREFERENCED_PARAMETER (D); UNREFERENCED_PARAMETER (Irp);
  KeSetEvent ((PKEVENT)Ctx, IO_NO_INCREMENT, FALSE);
  return STATUS_MORE_PROCESSING_REQUIRED;
}
static NTSTATUS ForwardSync (PFDX F, PIRP Irp)
{
  KEVENT Ev; KeInitializeEvent (&Ev, NotificationEvent, FALSE);
  IoCopyCurrentIrpStackLocationToNext (Irp);
  IoSetCompletionRoutine (Irp, SyncCompletion, &Ev, TRUE, TRUE, TRUE);
  NTSTATUS S = IoCallDriver (F->Lower, Irp);
  if (S == STATUS_PENDING) { KeWaitForSingleObject (&Ev, Executive, KernelMode, FALSE, NULL); S = Irp->IoStatus.Status; }
  return S;
}
/* v0.2: the remove lock is released when the IRP completes, not when IoCallDriver returns */
static NTSTATUS ReleaseOnCompletion (PDEVICE_OBJECT D, PIRP Irp, PVOID Ctx)
{
  UNREFERENCED_PARAMETER (D);
  if (Irp->PendingReturned) IoMarkIrpPending (Irp);
  IoReleaseRemoveLock (&((PFDX)Ctx)->RemoveLock, Irp);
  return STATUS_CONTINUE_COMPLETION;
}
static NTSTATUS ForwardHeld (PFDX F, PIRP Irp, BOOLEAN Power)
{
  IoCopyCurrentIrpStackLocationToNext (Irp);
  IoSetCompletionRoutine (Irp, ReleaseOnCompletion, F, TRUE, TRUE, TRUE);
  return Power ? PoCallDriver (F->Lower, Irp) : IoCallDriver (F->Lower, Irp);
}
static NTSTATUS PassThrough (PDEVICE_OBJECT D, PIRP Irp)
{
  PFDX F = (PFDX)D->DeviceExtension;
  UCHAR Mj = IoGetCurrentIrpStackLocation (Irp)->MajorFunction;
  if (ReadBooleanAcquire (&F->Gone) && Mj != IRP_MJ_CLOSE && Mj != IRP_MJ_CLEANUP) {  /* v0.6: no new I/O after surprise */
    Irp->IoStatus.Status = STATUS_NO_SUCH_DEVICE; Irp->IoStatus.Information = 0;
    IoCompleteRequest (Irp, IO_NO_INCREMENT);
    return STATUS_NO_SUCH_DEVICE;
  }
  NTSTATUS S = IoAcquireRemoveLock (&F->RemoveLock, Irp);
  if (!NT_SUCCESS (S)) { Irp->IoStatus.Status = S; IoCompleteRequest (Irp, IO_NO_INCREMENT); return S; }
  return ForwardHeld (F, Irp, FALSE);
}
/* v0.3: device D0 completed by pci.sys -> the function is powered; re-arm if the D3 transition lost the MSI-X state.
 * Runs below stornvme's own completion, i.e. before the miniport re-enables the controller. */
static NTSTATUS PowerUpCompletion (PDEVICE_OBJECT D, PIRP Irp, PVOID Ctx)
{
  PFDX F = (PFDX)Ctx;
  KIRQL Irql;
  UNREFERENCED_PARAMETER (D);
  if (Irp->PendingReturned) IoMarkIrpPending (Irp);
  if (NT_SUCCESS (Irp->IoStatus.Status)) {
    KeAcquireSpinLock (&F->StateLock, &Irql);
    if (F->Armed && !F->Gone && F->Bar0 && F->Sfc && !StillArmed (F)) {
      NTSTATUS S = ApplyArm (F);
      LOGP ("D0: MSI-X state was lost - re-armed %s\n", NT_SUCCESS (S) ? "(verified)" : "FAILED");
      if (!NT_SUCCESS (S)) {                                                         /* v0.4: fail closed */
        BOOLEAN Rb = Restore (F);
        LOGP ("D0: re-arm failed - rollback %s, D0 IRP failed (stornvme must not run without its vector)\n", Rb ? "verified" : "FAILED");
        Irp->IoStatus.Status = STATUS_DEVICE_CONFIGURATION_ERROR;                    /* Armed stays: next D0 retries */
      }
    }
    KeReleaseSpinLock (&F->StateLock, Irql);
  }
  IoReleaseRemoveLock (&F->RemoveLock, Irp);
  return STATUS_CONTINUE_COMPLETION;
}
static NTSTATUS DispatchPower (PDEVICE_OBJECT D, PIRP Irp)
{
  PFDX F = (PFDX)D->DeviceExtension;
  PIO_STACK_LOCATION Sp = IoGetCurrentIrpStackLocation (Irp);
  NTSTATUS S = IoAcquireRemoveLock (&F->RemoveLock, Irp);
  PoStartNextPowerIrp (Irp);
  if (!NT_SUCCESS (S)) { Irp->IoStatus.Status = S; IoCompleteRequest (Irp, IO_NO_INCREMENT); return S; }
  if (F->Bypass) return ForwardHeld (F, Irp, TRUE);
  if (Sp->MinorFunction == IRP_MN_SET_POWER && Sp->Parameters.Power.Type == DevicePowerState &&
      Sp->Parameters.Power.State.DeviceState == PowerDeviceD0) {
    IoCopyCurrentIrpStackLocationToNext (Irp);
    IoSetCompletionRoutine (Irp, PowerUpCompletion, F, TRUE, TRUE, TRUE);
    return PoCallDriver (F->Lower, Irp);
  }
  return ForwardHeld (F, Irp, TRUE);
}
/* v0.3: a failed restore keeps snapshot + mappings for the next attempt; only Final (REMOVE) lets them go */
static VOID DisarmAll (PFDX F, BOOLEAN Final)
{
  KIRQL Irql;
  KeAcquireSpinLock (&F->StateLock, &Irql);
  F->Armed = FALSE;
  /* v0.4: no MMIO on a removed device; v0.6: mappings are released on every STOP/REMOVE, restored or not */
  BOOLEAN Ok = F->Gone ? FALSE : Restore (F);
  if (F->Gone) LOGP ("%s after surprise removal - no device access, mappings released\n", Final ? "REMOVE" : "STOP");
  else if (!Ok) LOGP ("restore failed - device left as read back; mappings released, marked unrestored (no re-arm)\n");
  UnmapAll (F);                                                                      /* MmUnmapIoSpace: <= DISPATCH */
  F->Snap.Valid = FALSE;
  if (Ok || F->Gone || Final) F->Touched = FALSE;                                    /* else: next START refuses to arm */
  KeReleaseSpinLock (&F->StateLock, Irql);
}
/* v0.8.6: write MSISupported=1 and MessageNumberLimit=1 into the PDO's Device Parameters so first-boot PnP
 * assigns exactly 1 MSI message even on a brand-new Windows installation without a pre-existing Enum key. */
static VOID EnsurePdoMsiLimit (PDEVICE_OBJECT Pdo)
{
  HANDLE DevKey = NULL, IntKey = NULL, MsiKey = NULL;
  if (!NT_SUCCESS (IoOpenDeviceRegistryKey (Pdo, PLUGPLAY_REGKEY_DEVICE, KEY_ALL_ACCESS, &DevKey))) return;
  UNICODE_STRING U1, U2, V1, V2;
  OBJECT_ATTRIBUTES Oa;
  RtlInitUnicodeString (&U1, L"Interrupt Management");
  InitializeObjectAttributes (&Oa, &U1, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, DevKey, NULL);
  if (NT_SUCCESS (ZwCreateKey (&IntKey, KEY_ALL_ACCESS, &Oa, 0, NULL, 0, NULL))) {
    RtlInitUnicodeString (&U2, L"MessageSignaledInterruptProperties");
    InitializeObjectAttributes (&Oa, &U2, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, IntKey, NULL);
    if (NT_SUCCESS (ZwCreateKey (&MsiKey, KEY_ALL_ACCESS, &Oa, 0, NULL, 0, NULL))) {
      ULONG One = 1;
      RtlInitUnicodeString (&V1, L"MSISupported");
      RtlInitUnicodeString (&V2, L"MessageNumberLimit");
      ZwSetValueKey (MsiKey, &V1, 0, REG_DWORD, &One, sizeof (One));
      ZwSetValueKey (MsiKey, &V2, 0, REG_DWORD, &One, sizeof (One));
      ZwClose (MsiKey);
    }
    ZwClose (IntKey);
  }
  ZwClose (DevKey);
}

/* v0.8.6: clamp message-signaled interrupt requirements to 1 message (belt-and-suspenders for first boot) */
static VOID ClampResourceRequirements (PIO_RESOURCE_REQUIREMENTS_LIST List)
{
  if (List == NULL || List->AlternativeLists == 0) return;
  PIO_RESOURCE_LIST Alt = List->List;
  for (ULONG A = 0; A < List->AlternativeLists; A++) {
    for (ULONG I = 0; I < Alt->Count; I++) {
      PIO_RESOURCE_DESCRIPTOR D = &Alt->Descriptors[I];
      if (D->Type == CmResourceTypeInterrupt && (D->Flags & CM_RESOURCE_INTERRUPT_MESSAGE)) {
        if (D->u.Interrupt.MaximumVector > D->u.Interrupt.MinimumVector) {
          D->u.Interrupt.MaximumVector = D->u.Interrupt.MinimumVector;
        }
      }
    }
    Alt = (PIO_RESOURCE_LIST)(Alt->Descriptors + Alt->Count);
  }
}

static NTSTATUS DispatchPnp (PDEVICE_OBJECT D, PIRP Irp)
{
  PFDX F = (PFDX)D->DeviceExtension;
  PIO_STACK_LOCATION Sp = IoGetCurrentIrpStackLocation (Irp);
  NTSTATUS S = IoAcquireRemoveLock (&F->RemoveLock, Irp);
  if (!NT_SUCCESS (S)) { Irp->IoStatus.Status = S; IoCompleteRequest (Irp, IO_NO_INCREMENT); return S; }
  if (F->Bypass) {
    if (Sp->MinorFunction == IRP_MN_REMOVE_DEVICE) {
      IoReleaseRemoveLockAndWait (&F->RemoveLock, Irp);
      IoSkipCurrentIrpStackLocation (Irp);
      S = IoCallDriver (F->Lower, Irp);
      IoDetachDevice (F->Lower);
      IoDeleteDevice (D);
      return S;
    }
    return ForwardHeld (F, Irp, FALSE);
  }
  switch (Sp->MinorFunction) {
  case IRP_MN_FILTER_RESOURCE_REQUIREMENTS:
    S = ForwardSync (F, Irp);
    if (NT_SUCCESS (S) || S == STATUS_NOT_SUPPORTED) {
      PIO_RESOURCE_REQUIREMENTS_LIST Req = (PIO_RESOURCE_REQUIREMENTS_LIST)Irp->IoStatus.Information;
      if (Req == NULL) Req = Sp->Parameters.FilterResourceRequirements.IoResourceRequirementList;
      if (Req != NULL) ClampResourceRequirements (Req);
    }
    Irp->IoStatus.Status = S;
    IoCompleteRequest (Irp, IO_NO_INCREMENT);
    IoReleaseRemoveLock (&F->RemoveLock, Irp);
    return S;
  case IRP_MN_START_DEVICE:
  {
    NTSTATUS Lower = ForwardSync (F, Irp);
    S = Lower;
    if (!NT_SUCCESS (Lower)) LOGP ("START: lower stack (pci.sys) failed %08x - not armed\n", Lower);
    if (NT_SUCCESS (S)) {
      ULONG BusNum = PROXY_BUS, BusLen = 0;
      if (NT_SUCCESS (IoGetDeviceProperty (F->Pdo, DevicePropertyBusNumber, sizeof (BusNum), &BusNum, &BusLen)) && BusNum != PROXY_BUS) {
        LOGP ("START: bus 0x%lx != proxy bus 0x%x - passthrough\n", BusNum, PROXY_BUS);
      } else {
        /* v0.8.9 (generic M.2 position, like PS5 Linux, which reaches every ARI proxy function through RID@0x150): the shadow
         * page is this device's own function on bus 0x40, not the fixed 02.5 of the reference console (same value there) */
        ULONG Addr = 0, AddrLen = 0, Dev, Fn;
        F->DevFn = PROXY_DEVFN;
        if (NT_SUCCESS (IoGetDeviceProperty (F->Pdo, DevicePropertyAddress, sizeof (Addr), &Addr, &AddrLen)) && AddrLen == sizeof (Addr)) {
          Dev = Addr >> 16; Fn = Addr & 0xFFFF;
          if (Dev < 32 && Fn < 8) F->DevFn = (Dev << 3) | Fn;
          else if (Dev == 0 && Fn < 256) F->DevFn = Fn;                   /* ARI function number reported whole */
        }
        LOGP ("START: proxy function 0x%x:%02lx.%lu (devfn 0x%02lx)\n", PROXY_BUS, F->DevFn >> 3, F->DevFn & 7, F->DevFn);
        S = MapPages (F);
        if (!NT_SUCCESS (S)) LOGP ("START: ECAM map failed %08x\n", S);
        if (NT_SUCCESS (S)) S = Arm (F, Sp->Parameters.StartDevice.AllocatedResources, Sp->Parameters.StartDevice.AllocatedResourcesTranslated);
        if (!NT_SUCCESS (S)) { DisarmAll (F, FALSE); S = STATUS_DEVICE_CONFIGURATION_ERROR; }
      }
    }
    LOGP ("START result %08x (lower %08x)\n", S, Lower);
    Ps5LogFlush (S, Lower);
  }
    Irp->IoStatus.Status = S;
    IoCompleteRequest (Irp, IO_NO_INCREMENT);
    IoReleaseRemoveLock (&F->RemoveLock, Irp);
    return S;
  case IRP_MN_QUERY_STOP_DEVICE:                                                   /* v0.3: no rebalance while armed */
    if (F->Armed || F->Touched) {
      LOGP ("QUERY_STOP vetoed (armed)\n");
      Irp->IoStatus.Status = STATUS_UNSUCCESSFUL;
      IoCompleteRequest (Irp, IO_NO_INCREMENT);
      IoReleaseRemoveLock (&F->RemoveLock, Irp);
      return STATUS_UNSUCCESSFUL;
    }
    return ForwardHeld (F, Irp, FALSE);
  case IRP_MN_SURPRISE_REMOVAL:                                                    /* v0.4: device gone - no access */
    { KIRQL Irql; KeAcquireSpinLock (&F->StateLock, &Irql); F->Gone = TRUE; F->Armed = FALSE; KeReleaseSpinLock (&F->StateLock, Irql); }
    LOGP ("SURPRISE_REMOVAL - device treated as gone, no restore attempted\n");
    return ForwardHeld (F, Irp, FALSE);
  case IRP_MN_STOP_DEVICE:
    DisarmAll (F, FALSE);
    return ForwardHeld (F, Irp, FALSE);
  case IRP_MN_REMOVE_DEVICE:
    IoReleaseRemoveLockAndWait (&F->RemoveLock, Irp);                               /* v0.3: no IRP of ours in flight */
    DisarmAll (F, TRUE);
    IoSkipCurrentIrpStackLocation (Irp);
    S = IoCallDriver (F->Lower, Irp);
    IoDetachDevice (F->Lower);
    IoDeleteDevice (D);
    return S;
  default:
    return ForwardHeld (F, Irp, FALSE);
  }
}
static NTSTATUS AddDevice (PDRIVER_OBJECT Drv, PDEVICE_OBJECT Pdo)
{
  /* v0.8.8: never attach twice to the same PDO (e.g. if both SCSIAdapter class and NVMe device LowerFilters list ps5nvmsi;
   * a second instance would see MSI-X already enabled by the first and fail START_DEVICE -> 0x7B). */
  for (PDEVICE_OBJECT Cur = Pdo; Cur != NULL; Cur = Cur->AttachedDevice) {
    if (Cur->DriverObject == Drv) {
      return STATUS_SUCCESS;
    }
  }
  /* v0.8.7: only attach to PCI devices on Sony proxy bus 0x40; do NOT attach at all to non-proxy SCSIAdapter devices (SpacePort, UASP, etc.) */
  WCHAR EnumName[32]; ULONG EnumLen = 0, BusNum = 0xFFFFFFFFU, BusLen = 0;
  RtlZeroMemory (EnumName, sizeof (EnumName));
  BOOLEAN IsPci = NT_SUCCESS (IoGetDeviceProperty (Pdo, DevicePropertyEnumeratorName, sizeof (EnumName) - sizeof (WCHAR), EnumName, &EnumLen)) &&
                  EnumName[0] == L'P' && EnumName[1] == L'C' && EnumName[2] == L'I' && EnumName[3] == 0;
  BOOLEAN IsProxyBus = NT_SUCCESS (IoGetDeviceProperty (Pdo, DevicePropertyBusNumber, sizeof (BusNum), &BusNum, &BusLen)) &&
                       BusNum == PROXY_BUS;
  if (!IsPci || !IsProxyBus) {
    return STATUS_SUCCESS;
  }
  EnsurePdoMsiLimit (Pdo);
  PDEVICE_OBJECT D = NULL;
  NTSTATUS S = IoCreateDevice (Drv, sizeof (FDX), NULL, FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN, FALSE, &D);
  if (!NT_SUCCESS (S)) return S;
  PFDX F = (PFDX)D->DeviceExtension;
  RtlZeroMemory (F, sizeof (*F));
  F->Self = D; F->Pdo = Pdo;
  IoInitializeRemoveLock (&F->RemoveLock, TAG, 0, 0);
  KeInitializeSpinLock (&F->SfcLock);
  KeInitializeSpinLock (&F->StateLock);
  F->Lower = IoAttachDeviceToDeviceStack (D, Pdo);
  if (F->Lower == NULL) { IoDeleteDevice (D); return STATUS_NO_SUCH_DEVICE; }
  D->Flags |= F->Lower->Flags & (DO_BUFFERED_IO | DO_DIRECT_IO | DO_POWER_PAGABLE);
  D->DeviceType = F->Lower->DeviceType;
  D->Characteristics = F->Lower->Characteristics;
  D->Flags &= ~DO_DEVICE_INITIALIZING;
  return STATUS_SUCCESS;
}
static VOID Unload (PDRIVER_OBJECT Drv) { UNREFERENCED_PARAMETER (Drv); }

/* the observed format must be a plain xAPIC MSI: 0xFEE in 31:20, destination 19:12 zero, reserved 11:4 zero, DM (bit 2)
 * matching MsiDestKind; data: only bits 15 (trigger), 14 (level), 10:8 (fixed 000 / lowest priority 001) - v0.3.
 * v0.7 kind 2 (HAL message): DM free (the HAL decides), data bit 11 additionally allowed but only with DM = 1 (logical) */
static BOOLEAN FormatConsistent (VOID)
{
  if ((gAddrTemplate & 0xFFF00000U) != 0xFEE00000U || (gAddrTemplate & 0x000FFFF3U) != 0) return FALSE;
  if (gDestKind > 2 || (gDestKind < 2 && ((gAddrTemplate >> 2) & 1) != gDestKind)) return FALSE;
  if (((gDataTemplate >> 8) & 7) > 1) return FALSE;
  if (gDestKind < 2) return (gDataTemplate & ~0xC700U) == 0;                                 /* v0.3: 13:11 reserved */
  if ((gDataTemplate & ~0xCF00U) != 0) return FALSE;                                         /* 13:12 reserved */
  return !(gDataTemplate & 0x800U) || ((gAddrTemplate >> 2) & 1);
}
NTSTATUS DriverEntry (PDRIVER_OBJECT Drv, PUNICODE_STRING RegPath)
{
  /* Parameters\MsiAddrTemplate, MsiDataTemplate, MsiDestKind (REG_DWORD, no defaults) - from the ps5msiprobe result */
  RTL_QUERY_REGISTRY_TABLE Q[4], Q2[2];
  RtlZeroMemory (Q, sizeof (Q));
  PCWSTR Names[3] = { L"MsiAddrTemplate", L"MsiDataTemplate", L"MsiDestKind" };
  PULONG Slots[3] = { &gAddrTemplate, &gDataTemplate, &gDestKind };
  for (ULONG I = 0; I < 3; I++) {
    Q[I].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_TYPECHECK | RTL_QUERY_REGISTRY_REQUIRED;
    Q[I].Name = (PWSTR)Names[I]; Q[I].EntryContext = Slots[I];
    Q[I].DefaultType = (REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT) | REG_NONE;
  }
  WCHAR Path[256];
  KeInitializeSpinLock (&gLogLock);
  gFormatOk = FALSE;
  if (NT_SUCCESS (RtlStringCbPrintfW (Path, sizeof (Path), L"%wZ\\Parameters", RegPath)) &&
      (RtlStringCbCopyW (gParamPath, sizeof (gParamPath), Path), TRUE) &&
      NT_SUCCESS (RtlQueryRegistryValues (RTL_REGISTRY_ABSOLUTE, Path, Q, NULL, NULL)))
    gFormatOk = FormatConsistent ();
  RtlZeroMemory (Q2, sizeof (Q2));                                                 /* v0.8: optional, default 0 */
  Q2[0].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_TYPECHECK; Q2[0].Name = L"MsiUseMsi"; Q2[0].EntryContext = &gUseMsi;
  Q2[0].DefaultType = (REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT) | REG_NONE;
  gUseMsi = 0;
  if (gFormatOk) RtlQueryRegistryValues (RTL_REGISTRY_ABSOLUTE, Path, Q2, NULL, NULL);
  {                                                                                /* v0.8.4 optional */
    RTL_QUERY_REGISTRY_TABLE Q3[3]; RtlZeroMemory (Q3, sizeof (Q3));
    Q3[0].Flags = Q3[1].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_TYPECHECK;
    Q3[0].Name = L"RealDevCtlMode"; Q3[0].EntryContext = &gDevCtlMode; Q3[1].Name = L"RealDevCtl"; Q3[1].EntryContext = &gDevCtlValue;
    Q3[0].DefaultType = Q3[1].DefaultType = (REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT) | REG_NONE;
    gDevCtlMode = 0; gDevCtlValue = 0;
    if (gFormatOk) RtlQueryRegistryValues (RTL_REGISTRY_ABSOLUTE, Path, Q3, NULL, NULL);
    if (gDevCtlMode > 2) gDevCtlMode = 0;
  }
  LOGP ("v0.8.4 msi-mode %lu, format %s (addr %08lx data %08lx kind %lu)\n", gUseMsi, gFormatOk ? "ok" : "MISSING/INCONSISTENT - will not arm", gAddrTemplate, gDataTemplate, gDestKind);
  for (ULONG I = 0; I <= IRP_MJ_MAXIMUM_FUNCTION; I++) Drv->MajorFunction[I] = PassThrough;
  Drv->MajorFunction[IRP_MJ_PNP] = DispatchPnp;
  Drv->MajorFunction[IRP_MJ_POWER] = DispatchPower;
  Drv->DriverExtension->AddDevice = AddDevice;
  Drv->DriverUnload = Unload;
  return STATUS_SUCCESS;
}
