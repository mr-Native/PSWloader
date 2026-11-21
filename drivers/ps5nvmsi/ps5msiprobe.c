/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * ps5msiprobe.c v1.2 (13.11.2026) - READ-ONLY gate for way (b1) of the internal M.2 under Windows.
 *
 * Question: which MSI address/data format does the Windows HAL program on the PS5 (xAPIC physical or logical
 * destination, flat or cluster)? The (b1) filter has to write that format itself into the real MSI-X entry of the
 * SanDisk, because Windows programs the shadow's stub table at BAR0+0.
 *
 * What it does, once, in DriverEntry (legacy non-PnP kernel driver, `sc create ps5msiprobe type= kernel`):
 *  1. walks PCI from bus 0 through the bridges (like pci.sys, at most 64 buses), raw ECAM reads only (0xF0000000);
 *     for every present function with an ENABLED MSI or MSI-X capability it records vendor/device, control,
 *     address and data (MSI-X: control and table/PBA offsets only, v1.1);
 *  2. on every active processor reads IA32_APIC_BASE, the APIC ID, LDR and DFR (xAPIC MMIO or x2APIC MSRs) and the
 *     CPUID initial APIC ID;
 *  3. writes everything as REG_SZ values under HKLM\SOFTWARE\ps5msiprobe and returns. `sc stop` unloads it.
 * v1.1 (review): the MSI-X TABLE is no longer read from the BAR - its aperture size is unknown without a
 * sizing write, so a table offset could reach foreign MMIO. Only config space is read (MSI address/data, MSI-X control,
 * table/PBA offset). The GPU reference uses one MSI message, whose address/data live in config space.
 * No config write, no MMIO write, no PnP, no interrupt. The GPU 13FC (one message, programmed by pci.sys/HAL) is the
 * reference: its address/data plus the processor table tell the formula.
 */
#include <ntddk.h>
#include <ntstrsafe.h>

/* clang-cl with the WDK: <intrin.h> collides with wdm.h (__readcr8), so the two instructions are inline asm */
static __inline ULONGLONG ProbeReadMsr (ULONG Register)
{
  ULONG Lo, Hi;
  __asm__ __volatile__ ("rdmsr" : "=a" (Lo), "=d" (Hi) : "c" (Register));
  return ((ULONGLONG)Hi << 32) | Lo;
}
static __inline VOID ProbeCpuid (ULONG Leaf, ULONG Out[4])
{
  __asm__ __volatile__ ("cpuid" : "=a" (Out[0]), "=b" (Out[1]), "=c" (Out[2]), "=d" (Out[3]) : "a" (Leaf), "c" (0));
}

#define PROBE_TAG 'pmsP'
#define ECAM_PHYS 0xF0000000ULL
#define ECAM_BUSES 128
#define MAX_BUSES 64

static HANDLE gKey;

static VOID PutValue (PCWSTR Name, PCWSTR Fmt, ...)
{
  WCHAR Buffer[384];
  UNICODE_STRING N;
  va_list Args;
  va_start (Args, Fmt);
  NTSTATUS S = RtlStringCbVPrintfW (Buffer, sizeof (Buffer), Fmt, Args);
  va_end (Args);
  if (!NT_SUCCESS (S) || gKey == NULL) return;
  RtlInitUnicodeString (&N, Name);
  ZwSetValueKey (gKey, &N, 0, REG_SZ, Buffer, (ULONG)((wcslen (Buffer) + 1) * sizeof (WCHAR)));
}

static PUCHAR MapBus (ULONG Bus)
{
  PHYSICAL_ADDRESS P;
  P.QuadPart = (LONGLONG)(ECAM_PHYS + ((ULONGLONG)Bus << 20));
  return (PUCHAR)MmMapIoSpaceEx (P, 0x100000, PAGE_READONLY | PAGE_NOCACHE);
}

static ULONG Rd32 (PUCHAR Fn, ULONG Off) { return READ_REGISTER_ULONG ((PULONG)(Fn + Off)); }

static VOID ProbeFunction (ULONG Bus, ULONG DevFn, PUCHAR Fn, PULONG Found)
{
  ULONG Id = Rd32 (Fn, 0);
  ULONG Status = Rd32 (Fn, 4) >> 16;
  if ((Status & 0x10) == 0) return;                      /* no capability list */
  ULONG Cap = Rd32 (Fn, 0x34) & 0xFC;
  for (ULONG Count = 0; Count < 48 && Cap >= 0x40 && Cap <= 0xF4; Count++) {
    ULONG Head = Rd32 (Fn, Cap);
    ULONG Ctl = Head >> 16;
    WCHAR Name[64];
    if ((Head & 0xFF) == 0x05 && (Ctl & 1)) {            /* MSI, enabled */
      BOOLEAN Is64 = (Ctl & 0x80) != 0;
      if (Is64 && Cap > 0xF0) { Cap = (Head >> 8) & 0xFC; continue; }   /* v1.2: cap+12 must stay inside 0x100 */
      RtlStringCbPrintfW (Name, sizeof (Name), L"msi_%02x_%02x", Bus, DevFn);
      PutValue (Name, L"id %08x ctl %04x addr %08x%08x data %04x", Id, Ctl,
        Is64 ? Rd32 (Fn, Cap + 8) : 0, Rd32 (Fn, Cap + 4), Rd32 (Fn, Cap + (Is64 ? 12 : 8)) & 0xFFFF);
      (*Found)++;
    } else if ((Head & 0xFF) == 0x11 && (Ctl & 0x8000)) {  /* MSI-X, enabled */
      RtlStringCbPrintfW (Name, sizeof (Name), L"msix_%02x_%02x", Bus, DevFn);
      PutValue (Name, L"id %08x ctl %04x table %08x pba %08x", Id, Ctl, Rd32 (Fn, Cap + 4), Rd32 (Fn, Cap + 8));
      (*Found)++;
    }
    Cap = (Head >> 8) & 0xFC;
  }
}

static VOID ProbePci (VOID)
{
  ULONG Queue[MAX_BUSES], Head = 0, Tail = 0, Found = 0, Functions = 0;
  BOOLEAN Seen[ECAM_BUSES] = { 0 };
  Queue[Tail++] = 0; Seen[0] = TRUE;
  while (Head < Tail) {
    ULONG Bus = Queue[Head++];
    PUCHAR Map = MapBus (Bus);
    if (Map == NULL) continue;
    for (ULONG DevFn = 0; DevFn < 256; DevFn++) {
      PUCHAR Fn = Map + ((ULONG_PTR)DevFn << 12);
      ULONG Id = Rd32 (Fn, 0);
      if ((Id & 0xFFFF) == 0xFFFF || (Id & 0xFFFF) == 0) continue;   /* every devfn: ARI buses (0x40) have no fn-0 rule */
      Functions++;
      ProbeFunction (Bus, DevFn, Fn, &Found);
      ULONG HdrType = (Rd32 (Fn, 0x0C) >> 16) & 0x7F;
      if (HdrType == 1) {                                /* bridge: queue the secondary bus */
        ULONG Sec = (Rd32 (Fn, 0x18) >> 8) & 0xFF;
        if (Sec > 0 && Sec < ECAM_BUSES && !Seen[Sec] && Tail < MAX_BUSES) { Seen[Sec] = TRUE; Queue[Tail++] = Sec; }
      }
    }
    MmUnmapIoSpace (Map, 0x100000);
  }
  PutValue (L"pci_summary", L"buses %u functions %u enabled_msi_or_msix %u", Tail, Functions, Found);
}

static VOID ProbeCpus (VOID)
{
  ULONG Count = KeQueryActiveProcessorCountEx (ALL_PROCESSOR_GROUPS);
  PHYSICAL_ADDRESS Lapic = { 0 };
  PUCHAR LapicMap = NULL;
  for (ULONG I = 0; I < Count && I < 64; I++) {
    PROCESSOR_NUMBER Pn;
    if (!NT_SUCCESS (KeGetProcessorNumberFromIndex (I, &Pn))) continue;
    GROUP_AFFINITY Ga = { 0 }, Old;
    Ga.Group = Pn.Group; Ga.Mask = (KAFFINITY)1 << Pn.Number;
    KeSetSystemGroupAffinityThread (&Ga, &Old);
    ULONGLONG Base = ProbeReadMsr (0x1B);
    ULONG Regs[4];
    ProbeCpuid (1, Regs);
    ULONG InitialId = (Regs[1] >> 24) & 0xFF;
    ULONG Id = 0, Ldr = 0, Dfr = 0;
    BOOLEAN X2 = (Base & (1ULL << 10)) != 0;
    if (X2) {
      Id = (ULONG)ProbeReadMsr (0x802); Ldr = (ULONG)ProbeReadMsr (0x80D); Dfr = 0;
    } else {
      if (LapicMap == NULL) {
        Lapic.QuadPart = (LONGLONG)(Base & 0xFFFFFF000ULL);
        LapicMap = (PUCHAR)MmMapIoSpaceEx (Lapic, 0x1000, PAGE_READONLY | PAGE_NOCACHE);
      }
      if (LapicMap != NULL) {
        Id = READ_REGISTER_ULONG ((PULONG)(LapicMap + 0x20));
        Ldr = READ_REGISTER_ULONG ((PULONG)(LapicMap + 0xD0));
        Dfr = READ_REGISTER_ULONG ((PULONG)(LapicMap + 0xE0));
      }
    }
    KeRevertToUserGroupAffinityThread (&Old);
    WCHAR Name[32];
    RtlStringCbPrintfW (Name, sizeof (Name), L"cpu_%02u", I);
    PutValue (Name, L"apicbase %016I64x x2apic %u id %08x ldr %08x dfr %08x cpuid_apic %02x",
      Base, (ULONG)X2, Id, Ldr, Dfr, InitialId);
  }
  if (LapicMap != NULL) MmUnmapIoSpace (LapicMap, 0x1000);
}

static VOID Unload (PDRIVER_OBJECT DriverObject) { UNREFERENCED_PARAMETER (DriverObject); }

NTSTATUS DriverEntry (PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
  UNREFERENCED_PARAMETER (RegistryPath);
  DriverObject->DriverUnload = Unload;
  UNICODE_STRING K;
  OBJECT_ATTRIBUTES Oa;
  RtlInitUnicodeString (&K, L"\\Registry\\Machine\\SOFTWARE\\ps5msiprobe");
  InitializeObjectAttributes (&Oa, &K, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
  ULONG Disp;
  if (!NT_SUCCESS (ZwCreateKey (&gKey, KEY_SET_VALUE, &Oa, 0, NULL, REG_OPTION_NON_VOLATILE, &Disp))) gKey = NULL;
  PutValue (L"version", L"ps5msiprobe v1.2");
  ProbeCpus ();
  ProbePci ();
  PutValue (L"done", L"ok");
  if (gKey != NULL) { ZwClose (gKey); gKey = NULL; }
  return STATUS_SUCCESS;
}
