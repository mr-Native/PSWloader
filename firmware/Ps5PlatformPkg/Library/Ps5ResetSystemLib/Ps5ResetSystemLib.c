/** SPDX-License-Identifier: BSD-2-Clause-Patent
 * ICC ResetSystem candidate. No power command is sent during initialization.
 * Fixed MMIO pages are accepted only after verifying SPCIE identity and BARs.
 */
#include <PiDxe.h>
#include <Guid/EventGroup.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/IoLib.h>
#include <Library/PciExpressLib.h>
#include <Library/ResetSystemLib.h>
#include <Library/SynchronizationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include "icc-power.h"

STATIC UINTN mMailbox = 0x85400000;
STATIC UINTN mDoorbell = 0x85308000;
STATIC BOOLEAN mAvailable;
STATIC BOOLEAN mMapped;
STATIC volatile UINT32 mResetActive;
STATIC volatile INT32 mLastResult;
STATIC EFI_EVENT mVaEvent;
STATIC EFI_EVENT mReadyEvent;
STATIC EFI_EVENT mExitEvent;
STATIC EFI_RUNTIME_SERVICES *mPrivateRt;

STATIC VOID WarmTakeFramebuffer (VOID);
STATIC VOID Power (BOOLEAN Reboot);
STATIC VOID EFIAPI MapRuntimePages (EFI_EVENT Event, VOID *Context) {
  if (Event != NULL) WarmTakeFramebuffer ();   /* r5.25.4: ReadyToBoot */
  if (!mAvailable) return;
  UINTN Pages[2] = {mMailbox, mDoorbell};
  mMapped = FALSE;
  for (UINTN Index = 0; Index < 2; Index++) {
    EFI_GCD_MEMORY_SPACE_DESCRIPTOR Descriptor;
    EFI_STATUS Status = gDS->GetMemorySpaceDescriptor (Pages[Index], &Descriptor);
    if (EFI_ERROR (Status) || Descriptor.GcdMemoryType != EfiGcdMemoryTypeMemoryMappedIo) return;
    Status = gDS->SetMemorySpaceAttributes (Pages[Index], EFI_PAGE_SIZE,
                  (Descriptor.Attributes & ~(EFI_MEMORY_UC | EFI_MEMORY_WC | EFI_MEMORY_WT | EFI_MEMORY_WB | EFI_MEMORY_UCE)) | EFI_MEMORY_UC | EFI_MEMORY_RUNTIME);
    if (EFI_ERROR (Status)) return;
  }
  mMapped = TRUE;
}
STATIC VOID WarmTakeFramebuffer (VOID);
STATIC BOOLEAN mWarmEbs;
STATIC VOID EFIAPI ExitBootServices (EFI_EVENT Event, VOID *Context) {
  mWarmEbs = TRUE;   /* r5.25.4: no boot services from here on (the framebuffer is taken at ReadyToBoot) */
  if (!mMapped) mAvailable = FALSE;
}
STATIC VOID WarmMapVirtual (VOID);
STATIC VOID WarmPrepare (VOID);
STATIC VOID EFIAPI ConvertRuntimePointers (EFI_EVENT Event, VOID *Context) {
  WarmMapVirtual ();   /* r5.25.4: before mPrivateRt itself is converted */
  if (!mAvailable) return;
  if (!mMapped) { mAvailable = FALSE; return; }
  EFI_STATUS A = mPrivateRt->ConvertPointer (0, (VOID **)&mMailbox);
  EFI_STATUS B = mPrivateRt->ConvertPointer (0, (VOID **)&mDoorbell);
  if (EFI_ERROR (A) || EFI_ERROR (B)) mAvailable = FALSE;
  mPrivateRt->ConvertPointer (0, (VOID **)&mPrivateRt);
}
RETURN_STATUS EFIAPI Ps5ResetConstructor (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
  mPrivateRt = gRT; // Isolate this callback from other libraries converting gRT.
  UINTN Device = 0x04005000;
  mAvailable = PciExpressRead32 (Device) == 0x9107104D &&
      (PciExpressRead16 (Device + 4) & BIT1) != 0 &&
      (PciExpressRead32 (Device + 0x18) & ~0xFU) == 0x85200000 &&
      PciExpressRead32 (Device + 0x1C) == 0 &&
      (PciExpressRead32 (Device + 0x20) & ~0xFU) == 0x85400000 &&
      PciExpressRead32 (Device + 0x24) == 0;
  // Installing the architectural protocol does not prove the device is usable.
  // In TCG this guard is false; any Reset call then stops without MMIO writes.
  DEBUG ((DEBUG_INFO, "PS5 ICC ResetSystem device verified: %d\n", mAvailable));
  EFI_STATUS Status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_NOTIFY, ConvertRuntimePointers,
                      NULL, &gEfiEventVirtualAddressChangeGuid, &mVaEvent);
  if (EFI_ERROR (Status)) return Status;
  Status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_NOTIFY, MapRuntimePages,
                      NULL, &gEfiEventReadyToBootGuid, &mReadyEvent);
  if (EFI_ERROR (Status)) { gBS->CloseEvent (mVaEvent); return Status; }
  Status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_NOTIFY, ExitBootServices,
                      NULL, &gEfiEventExitBootServicesGuid, &mExitEvent);
  if (EFI_ERROR (Status)) {
    gBS->CloseEvent (mReadyEvent); gBS->CloseEvent (mVaEvent); return Status;
  }
  // Repeat at ReadyToBoot because PCI host-bridge initialization can set the
  // containing aperture's attributes after this constructor ran.
  MapRuntimePages (NULL, NULL);
#if defined (PS5_WARM_PROBE) && PS5_WARM_PROBE == 1
  /* lab (HW 19.11.): a warm re-entry from Windows stays dark; off here = trampoline, UPL entry and DXE core up to this driver ran */
  if (*(volatile UINT64 *)(UINTN)0x111806040ULL == 0x314D524157575350ULL) Power (FALSE);
#endif
#ifdef PS5_WARM_REENTRY
  WarmPrepare ();   /* r5.25.4: only in builds with --warm-reentry until it is proven on hardware */
#else
  (VOID)WarmPrepare;
#endif
  return RETURN_SUCCESS;
}
STATIC uint32_t ReadIcc (void *Context, unsigned Region, unsigned Offset, unsigned Width) {
  UINTN Address = (Region ? mDoorbell : mMailbox) + Offset;
  if (Width == 1) return MmioRead8 (Address);
  if (Width == 2) return MmioRead16 (Address);
  return MmioRead32 (Address);
}
STATIC void WriteIcc (void *Context, unsigned Region, unsigned Offset, unsigned Width, uint32_t Value) {
  UINTN Address = (Region ? mDoorbell : mMailbox) + Offset;
  if (Width == 1) MmioWrite8 (Address, (UINT8)Value);
  else if (Width == 2) MmioWrite16 (Address, (UINT16)Value);
  else MmioWrite32 (Address, Value);
}
STATIC uint64_t Ticks (void *Context) { return AsmReadTsc (); }
STATIC void Relax (void *Context) { CpuPause (); }
STATIC VOID Stop (VOID) {
  // ResetSystem must not continue OS execution after an incomplete power path.
  for (;;) { DisableInterrupts (); __asm__ __volatile__ ("hlt"); }
}
/* r5.25.7 (HW 20.11. lab101: ResetSystem(Shutdown) after a warm re-entry did not switch off): the transport
 * preparation ps5pwr does before every power request (icc_prepare) - drain inbound messages, restore the idle state if
 * a host message is still pending (warm: ButtonPoll's failed button enable) - and one retry on a timeout */
STATIC void IccDrop (void *Context, uint8_t Service, uint16_t Type, const uint8_t *Payload, unsigned Bytes) {}
STATIC VOID IccPrepare (CONST struct ps5_icc_io *Io) {
  struct ps5_icc_io Short = *Io;
  Short.timeout_ticks = 1596300000ULL / 5;
  for (UINTN I = 0; I < 16 && ps5_icc_poll (&Short, IccDrop, NULL) > 0; I++) {}
  if (ReadIcc (NULL, 0, 0x7f4, 2) != 1 || ReadIcc (NULL, 0, 0x7f0, 2) != 0) {
    WriteIcc (NULL, 1, 0x14, 4, 3);
    WriteIcc (NULL, 0, 0x7f0, 2, 0);
    WriteIcc (NULL, 0, 0x7f4, 2, 1);
  }
}
STATIC VOID Power (BOOLEAN Reboot) {
  DisableInterrupts ();
  if (!mAvailable || InterlockedCompareExchange32 (&mResetActive, 0, 1) != 0) {
    mLastResult = -10;
    Stop ();
  }
  // Construct callback pointers at call time so they use the current firmware
  // virtual address, rather than stale pointers captured before SetVA.
  struct ps5_icc_io Io = {NULL, ReadIcc, WriteIcc, Ticks, Relax, 1596300000ULL * 12};
  IccPrepare (&Io);
  mLastResult = ps5_icc_power (&Io, Reboot ? 1 : 0);
  if (mLastResult == -2 || mLastResult == -3) { IccPrepare (&Io); mLastResult = ps5_icc_power (&Io, Reboot ? 1 : 0); }
  Stop ();
}
/* ---------------------------------------------------------------------------------------------------------------
 * r5.25.4 warm re-entry (user, 18.11.2026: a Windows restart should land in the PSWloader UEFI, not in GameOS, and the
 * restart must still finish cleanly). Windows restarts through this ResetSystem (FADT has no reset register). Instead of
 * the ICC reboot (console -> GameOS) the firmware is started again, like kexec into itself:
 *   DXE (constructor): find the untouched firmware container ("PS5UPL01", CRC) in the loader's reserved arena
 *   0x1_0000_0000..0x1_1100_0000, copy the bootstrap's HOB list (PS5_HOB_BASE) and the ACPI arena (PS5_ACPI_BASE) into
 *   reserved pages, build 1:1 page tables (1 GiB pages, 512 GiB) and one trampoline page (runtime code, Windows maps it).
 *   SetVirtualAddressMap: the trampoline's Windows address is added to those page tables (4 KiB mapping).
 *   Reset (BSP only, else the ICC reboot): the trampoline switches CR3, jumps to its physical copy, clears CR4
 *   CET/PCIDE/SMEP/SMAP/PGE, loads its own GDT, turns off bus mastering of storage/network/USB controllers, goes back
 *   from x2APIC to xAPIC, sends INIT to all other cores, copies container+4096 -> PS5_IMAGE_BASE, HOBs and ACPI back,
 *   resets the shared PCI counter like the bootstrap and calls the firmware entry (MS ABI, RCX = HOB list).
 * Any step that cannot be prepared leaves the old ICC reboot in place. */
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Protocol/GraphicsOutput.h>
#include <Guid/GlobalVariable.h>
#define PS5W_HOB_BASE   0x40000000ULL
#define PS5W_IMAGE_BASE 0x41000000ULL
#define PS5W_ACPI_BASE  0x40200000ULL
#define PS5W_ARENA_LO   0x100000000ULL
#define PS5W_ARENA_HI   0x111000000ULL
typedef struct {
  UINT64 PhysPage, Cr3, StackTop, Entry, HobBase;
  UINT64 ImgSrc, ImgDst, ImgLen, HobSrc, HobDst, HobLen, AcpiSrc, AcpiDst, AcpiLen;
  UINT16 GdtLimit; UINT64 GdtBase; UINT16 Pad[3];
  UINT64 Gdt[3];
  UINT64 FbBase; UINT32 FbStride, FbPad;   /* 152: breadcrumbs on the screen (one coloured bar per step) */
  UINT64 NextFlags; UINT8 NextGuid[16];     /* 168: for the restarted firmware - bit0 open the Boot Manager (OS asked for
                                             * the firmware UI), bit1 boot the ESP with this partition GUID at once */
  UINT64 ProbeFn, ProbeMbox, ProbeBell;      /* 192: lab - physical WarmIccOff, ICC mailbox/doorbell (physical) */
} __attribute__((packed)) PS5_WARM_PARAMS;
#define PS5W_FIXED      0x111800000ULL   /* page tables (5 pages) + stack (64 KiB) in the loader's reserved arena: the
                                          * restarted firmware sees that range as reserved and never overwrites it */
extern UINT8 ps5_warm_tramp_start[], ps5_warm_tramp_params[], ps5_warm_tramp_end[];
__asm__ (
  ".text\n.global ps5_warm_tramp_start\n.global ps5_warm_tramp_params\n.global ps5_warm_tramp_end\n"
  ".hidden ps5_warm_tramp_start\n.hidden ps5_warm_tramp_params\n.hidden ps5_warm_tramp_end\n"   /* PC-relative refs also link into the host-test shared object */
  "ps5_warm_tramp_start:\n"
  "  cli\n"
  "  lea ps5_warm_tramp_params(%rip), %rbx\n"
  "  mov 8(%rbx), %rax\n  mov %rax, %cr3\n"                       /* our tables also map this page's current address */
  "  lea 1f(%rip), %rax\n  lea ps5_warm_tramp_start(%rip), %rcx\n  sub %rcx, %rax\n  add (%rbx), %rax\n  jmp *%rax\n"
  "1:\n"
  "  lea ps5_warm_tramp_params(%rip), %rbx\n"                   /* physical now */
#if defined (PS5_WARM_PROBE) && PS5_WARM_PROBE == 11
  "  jmp 40f\n"   /* lab: GPU-D3 signal at the physical copy (CR3 switched) */
#endif
  "  mov %cr4, %rax\n  and $0x40000, %rax\n  or $0x620, %rax\n  mov %rax, %cr4\n"   /* keep OSXSAVE, PAE|OSFXSR|OSXMMEXCPT */
  "  mov %cr0, %rax\n  and $~0xc, %rax\n  or $0x22, %rax\n  mov %rax, %cr0\n"
  "  lgdt 112(%rbx)\n"
  "  mov 16(%rbx), %rsp\n"
  "  mov $1, %edi\n  mov $0x00ff0000, %esi\n  call 20f\n"
  "  pushq $8\n  lea 2f(%rip), %rax\n  push %rax\n  lretq\n"
  "2:\n  mov $16, %eax\n  mov %ax, %ds\n  mov %ax, %es\n  mov %ax, %ss\n  xor %eax, %eax\n  mov %ax, %fs\n  mov %ax, %gs\n"
  /* r5.25.5: a 256-entry IDT in the reserved arena (WarmPrepare, 0x1_1183_0000) like the bootstrap's on a cold boot - with
   * the empty IDT (limit 0) the DXE core installed no exception handler at all (IdtEntryCount = (limit + 1) / 16 = 0), and
   * the first exception after a restart from Windows was a silent triple fault (HW 19.11. probes 32/38 off, 33 not) */
  /* r5.25.11 (HW 23.11. lab122): the table WarmPrepare built is named in the cell 0x1_1182_FFF8 - it alternates between
   * 0x1_1183_0000 and 0x1_1183_6000, because after a warm re-entry the loaded table is updated in place by the DXE core and
   * CpuDxe (selector 0x38) and must never be overwritten by the preparation of the next restart */
  "  sub $16, %rsp\n  movw $0xfff, (%rsp)\n  movabs $0x11182fff8, %rax\n  mov (%rax), %rax\n  test %rax, %rax\n  jnz 57f\n"
  "  movabs $0x111830000, %rax\n57:\n  mov %rax, 2(%rsp)\n  lidt (%rsp)\n  add $16, %rsp\n"
#if defined (PS5_WARM_PROBE) && PS5_WARM_PROBE == 21
  "  jmp 46f\n"   /* lab: ICC off at new GDT/stack/no IDT */
#endif
#if defined (PS5_WARM_PROBE) && PS5_WARM_PROBE == 12
  "  jmp 40f\n"   /* lab: GPU-D3 signal at new GDT/stack/no IDT */
#endif
#if defined (PS5_WARM_PROBE) && PS5_WARM_PROBE == 4
  "  ud2\n"   /* lab (HW 19.11.): triple fault right after the IDT went away - calibrates probe 3 (does a triple fault reset the PS5?) */
#endif
  "  mov $2, %edi\n  mov $0x00ff8000, %esi\n  call 20f\n"
  /* r5.25.5: PCI hand-over state back (list from WarmPciSnapshot at 0x1_1182_0000) */
  "  movabs $0x111820000, %rsi\n  movabs $0xF0000000, %r8\n"
  "30:\n  mov (%rsi), %eax\n  cmp $0xfffffffe, %eax\n  je 33f\n  cmp $0xffffffff, %eax\n  jne 31f\n"
  "  mov $20000000, %ecx\n32:\n  pause\n  dec %ecx\n  jnz 32b\n  jmp 34f\n"
  "31:\n  mov 4(%rsi), %edx\n  mov %edx, (%r8,%rax)\n"
  "34:\n  add $8, %rsi\n  jmp 30b\n"
  "33:\n"
#if defined (PS5_WARM_PROBE) && PS5_WARM_PROBE == 22
  "  jmp 46f\n"   /* lab: ICC off at PCI hand-over state replayed */
#endif
#if defined (PS5_WARM_PROBE) && PS5_WARM_PROBE == 13
  "  jmp 40f\n"   /* lab: GPU-D3 signal at PCI hand-over state replayed */
#endif
  "  mov $7, %edi\n  mov $0x0000ff80, %esi\n  call 20f\n"
  /* PCI: bus master off for USB (0c03) and NVMe (0108) controllers; ECAM 0xF0000000, 128 buses */
  "  movabs $0xF0000000, %r8\n  xor %r9d, %r9d\n"
  "3:\n  mov %r9, %r10\n  shl $12, %r10\n  add %r8, %r10\n"
  "  mov (%r10), %eax\n  cmp $0xffffffff, %eax\n  je 4f\n"
  "  mov 8(%r10), %eax\n  shr $8, %eax\n  mov %eax, %edx\n  shr $8, %edx\n"
  /* r5.25.4: only the controllers the firmware itself drives and turns on again (USB 0c03, NVMe 0108); SATA/network keep
   * the GameOS state - with bus mastering off Windows' AHCI start hung at the logo (HW 19.11.) */
  "  cmp $0x0c03, %edx\n  je 5f\n  cmp $0x0108, %edx\n  jne 4f\n"
  "5:\n  movw 4(%r10), %ax\n  and $~4, %ax\n  movw %ax, 4(%r10)\n"
  "4:\n  inc %r9\n  cmp $0x8000, %r9\n  jb 3b\n"
  "  mov $3, %edi\n  mov $0x00ffff00, %esi\n  call 20f\n"
  /* x2APIC -> xAPIC; INIT to all excluding self */
  /* local APIC: always off and on again in MSR 0x1B (clears IRR/ISR, timer and LVT of the previous run - HW 18.11.: the
   * restarted firmware hung right after the entry); ends in xAPIC mode also when the OS used x2APIC */
  "  mov $0x1b, %ecx\n  rdmsr\n  and $~0xc00, %eax\n  wrmsr\n  or $0x900, %eax\n  wrmsr\n"   /* + BSC (bit 8): this core boots */
  "6:\n  mov $0x1b, %ecx\n  rdmsr\n  and $0xfffff000, %eax\n  mov %rax, %r11\n"
  "  movl $0x1ff, 0xf0(%r11)\n"                                  /* SVR: APIC software-enabled, spurious vector 0xff, for the IPI */
  /* r5.25.5: AMD extended LVTs (0x500..0x530) masked - Windows may have armed them (MCA threshold / IBS /
   * deferred error), and that the MSR 0x1B reset clears them is not documented */
  "  orl $0x10000, 0x500(%r11)\n  orl $0x10000, 0x510(%r11)\n  orl $0x10000, 0x520(%r11)\n  orl $0x10000, 0x530(%r11)\n"
  /* r5.25.6 (HW 20.11. lab84: Windows' LVT vectors D8/E2 survive the EN toggle): every LVT masked explicitly, TPR 0,
   * divider/initial count 0, error status cleared (ESR is write-then-read) */
  "  movl $0x10000, 0x320(%r11)\n  movl $0x10000, 0x330(%r11)\n  movl $0x10000, 0x340(%r11)\n"
  "  movl $0x10000, 0x350(%r11)\n  movl $0x10000, 0x360(%r11)\n  movl $0x10000, 0x370(%r11)\n"
  "  movl $0, 0x380(%r11)\n  movl $0, 0x3e0(%r11)\n  movl $0, 0x80(%r11)\n"
  "  movl $0, 0x280(%r11)\n  movl $0, 0x280(%r11)\n"
  "  movl $0, 0x310(%r11)\n  movl $0x000c4500, 0x300(%r11)\n"
  "7:\n  pause\n  testl $0x1000, 0x300(%r11)\n  jnz 7b\n"
  "  mov $2000000, %ecx\n8:\n  pause\n  dec %ecx\n  jnz 8b\n"
  /* AMD extended-APIC interrupt filter as GameOS hands it over (IerEn=1, only vectors 0xC0-0xC4 enabled): the reset above
   * cleared it, and stray device interrupts then reached the early DXE phase without handlers (HW 18.11.: random hang
   * right after the DXE core started). The firmware opens the filter itself later (Ps5ExtApicOpenIer). */
  "  testl $1, 0x400(%r11)\n  jz 10f\n"
  "  xor %ecx, %ecx\n11:\n  mov %ecx, %eax\n  shl $4, %eax\n  movl $0, 0x480(%r11,%rax)\n  inc %ecx\n  cmp $8, %ecx\n  jb 11b\n"
  "  movl $0x1f, 0x4e0(%r11)\n"
  "  orl $1, 0x410(%r11)\n"
  "10:\n"
#if defined (PS5_WARM_PROBE) && PS5_WARM_PROBE == 23
  "  jmp 46f\n"   /* lab: ICC off at APIC reset, INIT, IER */
#endif
#if defined (PS5_WARM_PROBE) && PS5_WARM_PROBE == 14
  "  jmp 40f\n"   /* lab: GPU-D3 signal at APIC reset, INIT, IER */
#endif
  "  mov $4, %edi\n  mov $0x0000ff00, %esi\n  call 20f\n"
  /* cache setup back to the GameOS state (list at 0x1_1180_7000: count, then MSR/value pairs; MTRR_DEF_TYPE last) */
  "  mov %cr0, %rax\n  or $0x40000000, %rax\n  mov %rax, %cr0\n  wbinvd\n"
  "  mov $0x2ff, %ecx\n  rdmsr\n  and $~0xc00, %eax\n  wrmsr\n"
  "  movabs $0x111807000, %rsi\n  mov (%rsi), %r9\n  add $8, %rsi\n"
  "12:\n  test %r9, %r9\n  jz 13f\n  mov (%rsi), %ecx\n  mov 8(%rsi), %eax\n  mov 12(%rsi), %edx\n  wrmsr\n  add $16, %rsi\n  dec %r9\n  jmp 12b\n"
  "13:\n  wbinvd\n  mov %cr0, %rax\n  and $~0x60000000, %rax\n  mov %rax, %cr0\n"
  /* CR4 back to the GameOS value (no PGE/PCIDE/SMEP/SMAP/CET there) */
  "  movabs $0x111807800, %rax\n  mov (%rax), %rax\n  test %rax, %rax\n  jz 14f\n  mov %rax, %cr4\n14:\n"
#if defined (PS5_WARM_PROBE) && PS5_WARM_PROBE == 24
  "  jmp 46f\n"   /* lab: ICC off at MTRR/CR4 restored */
#endif
#if defined (PS5_WARM_PROBE) && PS5_WARM_PROBE == 15
  "  jmp 40f\n"   /* lab: GPU-D3 signal at MTRR/CR4 restored */
#endif
  /* copies */
  "  cld\n"
  "  mov 40(%rbx), %rsi\n  mov 48(%rbx), %rdi\n  mov 56(%rbx), %rcx\n  rep movsb\n"
  "  mov 64(%rbx), %rsi\n  mov 72(%rbx), %rdi\n  mov 80(%rbx), %rcx\n  rep movsb\n"
  "  mov 88(%rbx), %rsi\n  mov 96(%rbx), %rdi\n  mov 104(%rbx), %rcx\n  rep movsb\n"
  "  movabs $0x111000100, %rax\n  movl $0, (%rax)\n  mfence\n"
  "  mov $5, %edi\n  mov $0x0000ffff, %esi\n  call 20f\n"
  "  mov $6, %edi\n  mov $0x00ffffff, %esi\n  call 20f\n"
  /* hand-over record for the restarted firmware at 0x1_1180_6040: magic, flags, partition GUID */
  "  movabs $0x111806040, %rdi\n  movabs $0x314D524157575350, %rax\n  mov %rax, (%rdi)\n"
  "  mov 168(%rbx), %rax\n  mov %rax, 8(%rdi)\n  mov 176(%rbx), %rax\n  mov %rax, 16(%rdi)\n  mov 184(%rbx), %rax\n  mov %rax, 24(%rdi)\n"
  /* warm-boot marker for the display start (stops the previous run's timing generator once) */
  "  movabs $0x111806020, %rdi\n  movabs $0x544F4F424D524157, %rax\n  mov %rax, (%rdi)\n"
  /* lab marker for Ps5SerialPortLib (one square per log line during the re-entry): magic, count, framebuffer, stride */
  "  movabs $0x111806000, %rdi\n  movabs $0x59525445524D5257, %rax\n  mov %rax, (%rdi)\n  movq $0, 8(%rdi)\n"
  "  mov 152(%rbx), %rax\n  mov %rax, 16(%rdi)\n  mov 160(%rbx), %eax\n  mov %rax, 24(%rdi)\n"
#if defined (PS5_WARM_PROBE) && PS5_WARM_PROBE == 25
  "  jmp 46f\n"   /* lab: ICC off at the firmware entry (copies done) */
#endif
#if defined (PS5_WARM_PROBE) && PS5_WARM_PROBE == 16
  "  jmp 40f\n"   /* lab: GPU-D3 signal at the firmware entry (copies done) */
#endif
#if defined (PS5_WARM_PROBE) && PS5_WARM_PROBE == 3
  "  ud2\n"   /* lab (HW 19.11.): triple fault (IDT limit 0) right before the firmware entry - a reset here = the trampoline ran through */
#endif
  "  mov 32(%rbx), %rcx\n  mov 24(%rbx), %rax\n  and $-16, %rsp\n  sub $32, %rsp\n  call *%rax\n"
  "9:\n  cli\n  hlt\n  jmp 9b\n"
  /* 40: lab probe signal (HW 19.11.: a triple fault does not reset the PS5, the screen stays dark) - the GPU (1002:13FC)
   * goes to D3 and the core halts: the console draws visibly less than in the hang */
  "40:\n  movabs $0xF0000000, %r8\n  xor %r9d, %r9d\n"
  "41:\n  mov %r9, %r10\n  shl $12, %r10\n  add %r8, %r10\n  cmpl $0x13fc1002, (%r10)\n  je 42f\n  inc %r9\n  cmp $0x8000, %r9\n  jb 41b\n  jmp 45f\n"
  "42:\n  movzbl 0x34(%r10), %eax\n  and $0xfc, %eax\n"
  "43:\n  test %eax, %eax\n  jz 45f\n  cmpb $1, (%r10,%rax)\n  je 44f\n  movzbl 1(%r10,%rax), %eax\n  and $0xfc, %eax\n  jmp 43b\n"
  "44:\n  orl $3, 4(%r10,%rax)\n"
  "45:\n  cli\n  hlt\n  jmp 45b\n"
  /* 46: lab probe - the console switches off through the ICC (WarmIccOff at its physical address, MS ABI) */
  "46:\n  mov 200(%rbx), %rcx\n  mov 208(%rbx), %rdx\n  mov 192(%rbx), %rax\n  and $-16, %rsp\n  sub $32, %rsp\n  call *%rax\n  jmp 45b\n"
  /* 20: bar for step EDI in colour ESI (rows 24*step .. +16, 480 pixels) if a framebuffer is known */
  "20:\n  push %rax\n  push %rcx\n  push %rdx\n  push %rdi\n  push %r8\n"
  "  mov 152(%rbx), %r8\n  test %r8, %r8\n  jz 22f\n"
  "  mov 160(%rbx), %edx\n  shl $2, %rdx\n"                     /* stride in bytes */
  "  imul $24, %rdi\n  imul %rdx, %rdi\n  add %rdi, %r8\n"
  "  mov $16, %ecx\n"
  "21:\n  mov %r8, %rdi\n  push %rcx\n  mov $480, %ecx\n  mov %esi, %eax\n  rep stosl\n  pop %rcx\n  add %rdx, %r8\n  dec %ecx\n  jnz 21b\n"
  "22:\n  pop %r8\n  pop %rdi\n  pop %rdx\n  pop %rcx\n  pop %rax\n  ret\n"
  ".balign 16\nps5_warm_tramp_params:\n  .fill 216,1,0\nps5_warm_tramp_end:\n"
);
STATIC BOOLEAN mWarmReady;
STATIC UINT64  mWarmPhys, mWarmVa, *mWarmPml4;
STATIC UINT64  *mWarmVaTables[3];
STATIC PS5_WARM_PARAMS *mWarmParams;
STATIC UINTN mWarmParOff;
/* framebuffer for the breadcrumb bars: taken while boot services exist (ReadyToBoot, or at the reset before EBS) */
STATIC VOID WarmTakeFramebuffer (VOID) {
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop = NULL;
  if (mWarmParams == NULL || mWarmEbs) return;
  if (mWarmReady) {   /* r5.25.5 (HW 19.11.): tells ps5pwr v1.16+ to leave a restart to Windows -> ResetSystem -> warm re-entry
                       * (its own ICC reboot sent every Windows restart to GameOS before ResetSystem was ever called) */
    STATIC EFI_GUID PswGuid = {0x5efc5ae0, 0xb753, 0x4f7a, {0xa6, 0x62, 0xe3, 0x4d, 0x15, 0xa6, 0xd8, 0xe5}};
    UINT8 One = 1;
    EFI_STATUS St = gRT->SetVariable (L"PswWarmReentry", &PswGuid, EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS, 1, &One);
    DEBUG ((DEBUG_ERROR, "PS5 warm: PswWarmReentry published: %r\n", St));
  }
  if (!EFI_ERROR (gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop)) && Gop != NULL && Gop->Mode != NULL && Gop->Mode->Info != NULL) {
    mWarmParams->FbBase = Gop->Mode->FrameBufferBase; mWarmParams->FbStride = Gop->Mode->Info->PixelsPerScanLine;
  }
}
STATIC UINT32 WarmCrc32 (CONST UINT8 *P, UINTN N) {
  UINT32 Crc = 0xFFFFFFFFu;
  while (N--) { Crc ^= *P++; for (UINTN K = 0; K < 8; K++) Crc = (Crc >> 1) ^ (0xEDB88320u & (0u - (Crc & 1u))); }
  return ~Crc;
}
/* r5.25.5 (HW 19.11.: a restart from Windows hung at a constant 170 W without picture, the firmware self-test did not):
 * Windows leaves the PCI functions in D3 with its own BARs/MSI programming; the restarted firmware expects them as GameOS
 * hands them over (D0). This early DXE point still sees the hand-over state: it is written down as a list of ECAM dword
 * writes at PS5W_PCI_LIST that the trampoline replays - power state D0 first, 10 ms, then BARs, bridge windows, MSI address/
 * data/control, MSI-X control (function-masked), PCIe device/link control and the command register last.
 * Entry = {UINT32 ECAM offset, UINT32 value}; offset 0xFFFFFFFF = wait 10 ms, 0xFFFFFFFE = end. */
/* lab: ICC power-off that runs at its physical address inside the trampoline - no globals, no function pointers, only
 * its arguments (mailbox and doorbell, physical); the three requests of ps5_icc_power (icc-power.c) with reboot = 0 */
STATIC VOID EFIAPI WarmIccOff (UINT64 Mb, UINT64 Bl) {
  volatile UINT8 *M = (volatile UINT8 *)(UINTN)Mb, *B = (volatile UINT8 *)(UINTN)Bl;
  UINT16 Id = (UINT16)(*(volatile UINT16 *)(M + 6) + 1);
  for (UINTN Q = 0; Q < 3; Q++) {
    UINT8 R[32]; UINT16 Sum = 0; UINT8 Svc = Q < 2 ? 2 : 4; UINT16 Type = Q < 2 ? 0x10 : 1;
    for (UINTN I = 0; I < 32; I++) R[I] = 0;
    R[0] = 0x42; R[1] = Svc; R[2] = (UINT8)Type; R[3] = (UINT8)(Type >> 8); R[4] = 3; R[6] = (UINT8)Id; R[7] = (UINT8)(Id >> 8); R[8] = 32;
    if (Q == 1) R[12] = 1;
    if (Q == 2) { R[14] = 2; R[16] = 1; }
    for (UINTN I = 0; I < 32; I++) if (I != 10 && I != 11) Sum = (UINT16)(Sum + R[I]);
    R[10] = (UINT8)Sum; R[11] = (UINT8)(Sum >> 8);
    for (UINTN N = 0; N < 200000000 && !(*(volatile UINT16 *)(M + 0x7F4) == 1 && *(volatile UINT16 *)(M + 0x7F0) == 0); N++) {
      UINT32 St = *(volatile UINT32 *)(B + 0x14) & 3;
      if (St) *(volatile UINT32 *)(B + 0x14) = St;
      if (St & 1) { *(volatile UINT16 *)(M + 0xFF0) = 0; *(volatile UINT16 *)(M + 0xFF4) = 1; *(volatile UINT32 *)(B + 4) = 2; }
      __asm__ __volatile__ ("pause");
    }
    *(volatile UINT32 *)(B + 0x14) = 2;
    *(volatile UINT16 *)(M + 0x7F4) = 0;
    for (UINTN I = 0; I < 32; I++) M[I] = R[I];
    *(volatile UINT16 *)(M + 0x7F0) = 1;
    *(volatile UINT32 *)(B + 4) = 1;
    for (UINTN N = 0; N < 200000000; N++) {
      UINT32 St = *(volatile UINT32 *)(B + 0x14) & 3;
      if (St) *(volatile UINT32 *)(B + 0x14) = St;
      if (St & 1) {
        BOOLEAN Mine = M[0x801] == Svc && *(volatile UINT16 *)(M + 0x806) == Id;
        *(volatile UINT16 *)(M + 0xFF0) = 0; *(volatile UINT16 *)(M + 0xFF4) = 1; *(volatile UINT32 *)(B + 4) = 2;
        if (Mine) break;
      }
      __asm__ __volatile__ ("pause");
    }
    Id++;
  }
}
#define PS5W_PCI_LIST   (PS5W_FIXED + 0x20000)
#define PS5W_PCI_MAX    8000
STATIC UINTN mWarmPciN;
STATIC VOID WarmPciPut (UINT32 Off, UINT32 Val) {
  volatile UINT32 *L = (volatile UINT32 *)(UINTN)PS5W_PCI_LIST;
  if (mWarmPciN < PS5W_PCI_MAX) { L[2 * mWarmPciN] = Off; L[2 * mWarmPciN + 1] = Val; mWarmPciN++; }
}
STATIC UINT32 WarmCfg (UINT32 Off) { return *(volatile UINT32 *)(UINTN)(0xF0000000ULL + Off); }
STATIC UINT8 WarmCap (UINT32 Fn, UINT8 Id) {
  if ((WarmCfg (Fn + 4) & 0x100000) == 0) return 0;
  UINT8 P = (UINT8)WarmCfg (Fn + 0x34) & 0xFC;
  for (UINTN G = 0; P && G < 48; G++) { UINT32 H = WarmCfg (Fn + P); if ((UINT8)H == Id) return P; P = (UINT8)(H >> 8) & 0xFC; }
  return 0;
}
STATIC VOID WarmPciSnapshot (VOID) {
  mWarmPciN = 0;
  /* r5.25.11: network functions (class 02: Ethernet, Wi-Fi) get a function level reset first - their Windows drivers may
   * leave the DMA rings running into Windows memory (ps5wifi's shutdown only flags the adapter), and the replay below turns
   * bus mastering back on with the hand-over command register. FLR stops every DMA; the drivers initialise the chips anew. */
  { UINTN Flr = 0;
    for (UINT32 I = 0; I < 0x8000; I++) {
      UINT32 Fn = I << 12;
      if (WarmCfg (Fn) == 0xFFFFFFFF || (WarmCfg (Fn + 8) >> 24) != 0x02 || ((WarmCfg (Fn + 0x0C) >> 16) & 0x7F) != 0) continue;
      UINT8 Exp = WarmCap (Fn, 0x10);
      if (!Exp || !(WarmCfg (Fn + Exp + 4) & BIT28)) continue;
      WarmPciPut (Fn + 4, WarmCfg (Fn + 4) & ~4u);                       /* bus master off first */
      WarmPciPut (Fn + Exp + 8, (WarmCfg (Fn + Exp + 8) & 0xFFFF) | 0x8000u);   /* initiate FLR */
      DEBUG ((DEBUG_ERROR, "PS5 warm re-entry: FLR planned for %x (%08x)\n", Fn, WarmCfg (Fn)));
      Flr++;
    }
    if (Flr) WarmPciPut (0xFFFFFFFF, 0);   /* one replay wait = 20M pause loops, well above the 100 ms FLR time */
  }
  for (UINT32 Pass = 0; Pass < 2; Pass++) {
    for (UINT32 I = 0; I < 0x8000; I++) {
      UINT32 Fn = I << 12;
      if (WarmCfg (Fn) == 0xFFFFFFFF) continue;
#ifdef PS5_WARM_KEEP_GPU
      if ((UINT16)WarmCfg (Fn) == 0x1002) continue;   /* lab: GPU/HDMI audio stay as the OS left them (no D3 -> D0 reset) */
#endif
      UINT8 Pm = WarmCap (Fn, 0x01);
      if (Pass == 0) { if (Pm) WarmPciPut (Fn + Pm + 4, WarmCfg (Fn + Pm + 4) & ~0x8000u); continue; }   /* power state as handed over */
      UINT8 Type = (UINT8)(WarmCfg (Fn + 0x0C) >> 16) & 0x7F;
      UINT32 Cmd = WarmCfg (Fn + 4);
      WarmPciPut (Fn + 4, 0);                                /* decode off while the BARs are written */
      UINT32 Last = Type == 1 ? 0x30 : 0x24;
      for (UINT32 R = 0x10; R <= Last; R += 4) WarmPciPut (Fn + R, WarmCfg (Fn + R));
      WarmPciPut (Fn + 0x3C, WarmCfg (Fn + 0x3C));
      UINT8 Msi = WarmCap (Fn, 0x05);
      if (Msi) {
        UINT32 Ctl = WarmCfg (Fn + Msi); BOOLEAN B64 = (Ctl & 0x800000) != 0;
        WarmPciPut (Fn + Msi + 4, WarmCfg (Fn + Msi + 4));
        if (B64) WarmPciPut (Fn + Msi + 8, WarmCfg (Fn + Msi + 8));
        WarmPciPut (Fn + Msi + (B64 ? 0xC : 8), WarmCfg (Fn + Msi + (B64 ? 0xC : 8)));
        if (Ctl & 0x1000000) WarmPciPut (Fn + Msi + (B64 ? 0x10 : 0xC), WarmCfg (Fn + Msi + (B64 ? 0x10 : 0xC)));
        WarmPciPut (Fn + Msi, Ctl);
      }
      UINT8 Msix = WarmCap (Fn, 0x11);
      if (Msix) WarmPciPut (Fn + Msix, WarmCfg (Fn + Msix) | 0x40000000u);   /* + function mask */
      UINT8 Exp = WarmCap (Fn, 0x10);
      if (Exp) { WarmPciPut (Fn + Exp + 8, WarmCfg (Fn + Exp + 8) & 0xFFFF); WarmPciPut (Fn + Exp + 0x10, WarmCfg (Fn + Exp + 0x10) & 0xFFFF); }
      WarmPciPut (Fn + 4, Cmd & 0xFFFF);
    }
    if (Pass == 0) WarmPciPut (0xFFFFFFFF, 0);
  }
  WarmPciPut (0xFFFFFFFE, 0);
  if (mWarmPciN >= PS5W_PCI_MAX) *(volatile UINT32 *)(UINTN)PS5W_PCI_LIST = 0xFFFFFFFE;   /* too many: replay nothing */
  DEBUG ((DEBUG_ERROR, "PS5 warm re-entry: PCI hand-over state %u writes\n", (UINT32)mWarmPciN));
}
STATIC VOID WarmPrepare (VOID) {
  *(volatile UINT64 *)(UINTN)0x1118061A0ULL = 0;   /* r5.25.5: no stale "cold restart" request from an earlier run */
  /* 1. untouched firmware container in the reserved arena */
  UINT64 Src = 0, Bytes = 0, Entry = 0;
  for (UINT64 A = PS5W_ARENA_LO; A < PS5W_ARENA_HI && !Src; A += EFI_PAGE_SIZE) {
    CONST UINT8 *H = (CONST UINT8 *)(UINTN)A;
    if (CompareMem (H, "PS5UPL01", 8) != 0 || *(CONST UINT32 *)(H + 12) != 4096 || *(CONST UINT64 *)(H + 16) != PS5W_IMAGE_BASE) continue;
    if (WarmCrc32 (H, 4092) != *(CONST UINT32 *)(H + 4092)) continue;
    Bytes = *(CONST UINT64 *)(H + 24); Entry = *(CONST UINT64 *)(H + 32);
    if (!Bytes || Bytes > SIZE_32MB || A + 4096 + Bytes > PS5W_ARENA_HI) continue;
    if (WarmCrc32 (H + 4096, (UINTN)Bytes) != *(CONST UINT32 *)(H + 44)) continue;
    Src = A;
  }
  if (!Src) { DEBUG ((DEBUG_ERROR, "PS5 warm re-entry: firmware container not found - restarts go through GameOS\n")); return; }
  /* 2. bootstrap HOB list: PHIT + HOBs up to the first end HOB */
  CONST UINT8 *Hob = (CONST UINT8 *)(UINTN)PS5W_HOB_BASE; UINTN Off = 0, HobLen = 0;
  if (*(CONST UINT16 *)Hob != 1) { DEBUG ((DEBUG_ERROR, "PS5 warm re-entry: no PHIT at the HOB base\n")); return; }
  while (Off < 32768) {
    UINT16 Type = *(CONST UINT16 *)(Hob + Off), Len = *(CONST UINT16 *)(Hob + Off + 2);
    if (Len < 8) break;
    Off += Len;
    if (Type == 0xFFFF) { HobLen = Off; break; }
  }
  if (!HobLen) { DEBUG ((DEBUG_ERROR, "PS5 warm re-entry: HOB list end not found\n")); return; }
  /* ACPI arena = the allocation HOB that starts at PS5_ACPI_BASE */
  UINT64 AcpiLen = 0;
  for (Off = 0; Off < HobLen; Off += *(CONST UINT16 *)(Hob + Off + 2))
    if (*(CONST UINT16 *)(Hob + Off) == 2 && *(CONST UINT64 *)(Hob + Off + 24) == PS5W_ACPI_BASE) AcpiLen = *(CONST UINT64 *)(Hob + Off + 32);
  /* 3. reserved copies, page tables, stack; runtime trampoline page */
  UINTN SavePages = EFI_SIZE_TO_PAGES (HobLen) + EFI_SIZE_TO_PAGES ((UINTN)AcpiLen);
  UINT8  *Save   = AllocateReservedPages (SavePages);
  UINT64 *Tables = (UINT64 *)(UINTN)PS5W_FIXED;
  UINT8  *Stack  = (UINT8 *)(UINTN)(PS5W_FIXED + 0x10000);
  EFI_PHYSICAL_ADDRESS Tramp = 0;
  if (Save == NULL || Tables == NULL || Stack == NULL ||
      EFI_ERROR (gBS->AllocatePages (AllocateAnyPages, EfiRuntimeServicesCode, 1, &Tramp))) {
    DEBUG ((DEBUG_ERROR, "PS5 warm re-entry: no memory for the copies\n")); return;
  }
  CopyMem (Save, Hob, HobLen);
  if (AcpiLen) CopyMem (Save + EFI_PAGES_TO_SIZE (EFI_SIZE_TO_PAGES (HobLen)), (VOID *)(UINTN)PS5W_ACPI_BASE, (UINTN)AcpiLen);
  ZeroMem (Tables, EFI_PAGES_TO_SIZE (5));
  *(volatile UINT64 *)(UINTN)(PS5W_FIXED + 0x6000) = 0;   /* lab square marker off for normal boots */
  { /* GameOS cache setup (MTRRs, PAT, AMD SYSCFG/TOP_MEM/TOP_MEM2) as found at this early DXE point; the trampoline writes it
     * back, so the restarted firmware and the next OS start from the same CPU cache state as after GameOS */
    volatile UINT64 *M = (volatile UINT64 *)(UINTN)(PS5W_FIXED + 0x7000); UINTN N = 0;
    UINT32 List[64]; UINTN L = 0;
    UINT32 Var = (UINT32)(AsmReadMsr64 (0xFE) & 0xFF); if (Var > 16) Var = 16;
    for (UINT32 I = 0; I < 2 * Var; I++) List[L++] = 0x200 + I;
    List[L++] = 0x250; List[L++] = 0x258; List[L++] = 0x259;
    for (UINT32 I = 0x268; I <= 0x26F; I++) List[L++] = I;
    List[L++] = 0x277; List[L++] = 0xC001001A; List[L++] = 0xC001001D; List[L++] = 0xC0010010;
    /* r5.25.6 (HW 20.11. lab84 cpu state): what Windows changed on the BSP - HWCR (CpbDis cleared, IRPerfEn set),
     * EFER (SCE, FFXSR: fxsave/fxrstor in EDK2's interrupt stub would skip XMM) and the P-state request */
    List[L++] = 0xC0010015;   /* HWCR */
    List[L++] = 0xC0000080;   /* EFER: LME/NXE stay as captured, LMA is read-only */
    List[L++] = 0xC0010062;   /* PStateCtl: the cold P-state request */
    List[L++] = 0x2FF;   /* MTRR_DEF_TYPE last: it enables the MTRRs again */
    for (UINTN I = 0; I < L; I++) { M[1 + 2 * N] = List[I]; M[2 + 2 * N] = AsmReadMsr64 (List[I]); N++; }
    M[0] = N;
    /* CR4 as GameOS left it (HW 19.11.: the warm OS start lacked FSGSBASE and MCE, cold 50668 vs warm 40628) */
    *(volatile UINT64 *)(UINTN)(PS5W_FIXED + 0x7800) = AsmReadCr4 ();
  }
  WarmPciSnapshot ();
  { /* IDT for the restarted firmware's early phase (see the trampoline): vectors 0-31 halt (the DXE core replaces them with
     * its handlers), 32-255 return (stray interrupts) - stubs and table in the reserved arena, never reused by anyone */
    volatile UINT8 *Stub = (volatile UINT8 *)(UINTN)(PS5W_FIXED + 0x31000);
    Stub[0] = 0xFA; Stub[1] = 0xF4; Stub[2] = 0xEB; Stub[3] = 0xFC;   /* cli; hlt; jmp back to hlt */
    Stub[16] = 0x48; Stub[17] = 0xCF;                                  /* iretq */
    /* r5.25.11 (HW 23.11. lab122): after a warm re-entry the table at 0x30000 IS the live IDT (the DXE core and CpuDxe
     * updated its entries in place, selector 0x38); writing the selector-8 stubs over it made every later interrupt or
     * exception a triple fault (probe 74/78, the Windows logo without spinner). Build the next table in the other slot. */
    IA32_DESCRIPTOR Live;
    AsmReadIdtr (&Live);
    UINT64 Slot = (Live.Base == (UINTN)(PS5W_FIXED + 0x30000)) ? PS5W_FIXED + 0x36000 : PS5W_FIXED + 0x30000;
    volatile UINT64 *Idt = (volatile UINT64 *)(UINTN)Slot;
    for (UINTN V = 0; V < 256; V++) {
      UINT64 H = PS5W_FIXED + 0x31000 + (V < 32 ? 0 : 16);
      Idt[2 * V] = (H & 0xFFFF) | (8ULL << 16) | (0x8EULL << 40) | (((H >> 16) & 0xFFFF) << 48);
      Idt[2 * V + 1] = H >> 32;
    }
    *(volatile UINT64 *)(UINTN)(PS5W_FIXED + 0x2FFF8) = Slot;
    DEBUG ((DEBUG_ERROR, "PS5 warm re-entry: next trampoline IDT at %Lx (live IDT %Lx)\n", Slot, (UINT64)Live.Base));
  }
  mWarmPml4 = Tables;
  UINT64 *Pdpt = Tables + 512;
  for (UINTN I = 0; I < 512; I++) Pdpt[I] = ((UINT64)I << 30) | 0x83;   /* present, writable, 1 GiB */
  mWarmPml4[0] = (UINT64)(UINTN)Pdpt | 0x3;
  /* r5.25.4: the pages for the trampoline's OS address are runtime data (mapped by the OS during SetVirtualAddressMap);
   * every upper PML4 entry points at that one PDPT now, so the VA-change event writes only runtime memory - it wrote the
   * arena before, which the OS does not map there (HW 19.11.: Windows hung at the logo whenever this code was built in) */
  { EFI_PHYSICAL_ADDRESS Va3 = 0;
    if (EFI_ERROR (gBS->AllocatePages (AllocateAnyPages, EfiRuntimeServicesData, 3, &Va3))) return;
    ZeroMem ((VOID *)(UINTN)Va3, EFI_PAGES_TO_SIZE (3));
    mWarmVaTables[0] = (UINT64 *)(UINTN)Va3; mWarmVaTables[1] = (UINT64 *)(UINTN)(Va3 + 0x1000); mWarmVaTables[2] = (UINT64 *)(UINTN)(Va3 + 0x2000);
    for (UINTN I = 1; I < 512; I++) mWarmPml4[I] = (UINT64)(UINTN)mWarmVaTables[0] | 0x3; }
  UINTN CodeLen = (UINTN)(ps5_warm_tramp_end - ps5_warm_tramp_start), ParOff = (UINTN)(ps5_warm_tramp_params - ps5_warm_tramp_start);
  if (CodeLen > EFI_PAGE_SIZE || ParOff + sizeof (PS5_WARM_PARAMS) > EFI_PAGE_SIZE) return;
  CopyMem ((VOID *)(UINTN)Tramp, ps5_warm_tramp_start, CodeLen);
  PS5_WARM_PARAMS *P = (PS5_WARM_PARAMS *)(UINTN)(Tramp + ParOff);
  ZeroMem (P, sizeof (*P));
  P->PhysPage = Tramp; P->Cr3 = (UINT64)(UINTN)Tables; P->StackTop = (UINT64)(UINTN)Stack + 0x10000 - 64;
  P->Entry = Entry; P->HobBase = PS5W_HOB_BASE;
  P->ImgSrc = Src + 4096; P->ImgDst = PS5W_IMAGE_BASE; P->ImgLen = Bytes;
  P->HobSrc = (UINT64)(UINTN)Save; P->HobDst = PS5W_HOB_BASE; P->HobLen = HobLen;
  P->AcpiSrc = (UINT64)(UINTN)Save + EFI_PAGES_TO_SIZE (EFI_SIZE_TO_PAGES (HobLen)); P->AcpiDst = PS5W_ACPI_BASE; P->AcpiLen = AcpiLen;
  P->Gdt[0] = 0; P->Gdt[1] = 0x00AF9A000000FFFFULL; P->Gdt[2] = 0x00CF92000000FFFFULL;   /* 64-bit code 0x08, data 0x10 */
  P->GdtLimit = sizeof (P->Gdt) - 1; P->GdtBase = Tramp + ParOff + OFFSET_OF (PS5_WARM_PARAMS, Gdt);
  P->ProbeFn = (UINT64)(UINTN)WarmIccOff; P->ProbeMbox = 0x85400000; P->ProbeBell = 0x85308000;   /* physical: before SetVirtualAddressMap */
  mWarmPhys = Tramp; mWarmVa = Tramp; mWarmParams = P; mWarmParOff = ParOff;
  mWarmReady = TRUE;
#ifdef PS5_WARM_RESCUE
  { /* lab: where the rescue watchdog AP finds the trampoline (boot services: current address = physical) */
    volatile UINT64 *Pub = (volatile UINT64 *)(UINTN)0x111806180ULL;
    Pub[1] = Tramp; Pub[2] = Tramp + ParOff; Pub[0] = 0x504D415254575350ULL;
  }
#endif
  DEBUG ((DEBUG_ERROR, "PS5 warm re-entry: ready - container %lx (%lu bytes, entry %lx), HOBs %u bytes, ACPI %lu bytes, trampoline %lx\n",
          Src, Bytes, Entry, (UINT32)HobLen, AcpiLen, Tramp));
}
/* SetVirtualAddressMap, still in physical mode: map the trampoline's new address in our tables (4 KiB) */
STATIC VOID WarmMapVirtual (VOID) {
  if (!mWarmReady) return;
  UINT64 Va = mWarmPhys;
  if (EFI_ERROR (mPrivateRt->ConvertPointer (0, (VOID **)&Va))) { mWarmReady = FALSE; return; }
  UINT64 *Pdpt = mWarmVaTables[0], *Pd = mWarmVaTables[1], *Pt = mWarmVaTables[2];
  UINTN I4 = (UINTN)((Va >> 39) & 511), I3 = (UINTN)((Va >> 30) & 511), I2 = (UINTN)((Va >> 21) & 511), I1 = (UINTN)((Va >> 12) & 511);
  if (Va != mWarmPhys) {
    if (I4 == 0) { mWarmReady = FALSE; return; }   /* would collide with the 1:1 range */
    Pdpt[I3] = (UINT64)(UINTN)Pd | 0x3; Pd[I2] = (UINT64)(UINTN)Pt | 0x3; Pt[I1] = mWarmPhys | 0x3;   /* runtime data only */
  }
  mWarmVa = Va;
}
/* What Windows asked for: OsIndications BOOT_TO_FW_UI (shutdown /r /fw) -> Boot Manager; BootNext, else the first
 * BootOrder entry named "Windows Boot Manager" (written by Windows Setup / bcdboot) -> boot that ESP directly. Runs in the
 * OS's address space through the runtime variable service; the result goes into the trampoline page (OS-mapped). */
STATIC BOOLEAN WarmGuidOf (UINT16 Option, UINT8 *Guid, BOOLEAN NeedWindows) {
  CHAR16 Name[9] = { L'B', L'o', L'o', L't', 0, 0, 0, 0, 0 }; STATIC CONST CHAR16 Hex[] = L"0123456789ABCDEF";
  for (UINTN I = 0; I < 4; I++) Name[4 + I] = Hex[(Option >> (12 - 4 * I)) & 0xF];
  UINT8 Buf[1024]; UINTN Size = sizeof (Buf); UINT32 Attr = 0;
  if (EFI_ERROR (mPrivateRt->GetVariable (Name, &gEfiGlobalVariableGuid, &Attr, &Size, Buf)) || Size < 8) return FALSE;
  UINT16 PathLen = *(UINT16 *)(Buf + 4); CHAR16 *Desc = (CHAR16 *)(Buf + 6); UINTN D = 0;
  while (6 + 2 * D + 2 <= Size && Desc[D] != 0) D++;
  if (NeedWindows) {
    STATIC CONST CHAR16 Want[] = L"Windows Boot Manager";
    for (UINTN I = 0; I < 20; I++) if (I >= D || Desc[I] != Want[I]) return FALSE;
  }
  UINTN Off = 6 + 2 * (D + 1), End = Off + PathLen;
  if (End > Size) return FALSE;
  while (Off + 4 <= End) {
    UINT8 Type = Buf[Off], Sub = Buf[Off + 1]; UINT16 Len = *(UINT16 *)(Buf + Off + 2);
    if (Len < 4) break;
    if (Type == 4 && Sub == 1 && Len >= 42 && Buf[Off + 41] == 2) { CopyMem (Guid, Buf + Off + 24, 16); return TRUE; }   /* HD(), GPT */
    if (Type == 0x7F) break;
    Off += Len;
  }
  return FALSE;
}
STATIC VOID WarmReadOsRequest (PS5_WARM_PARAMS *P) {
  UINT64 Ind = 0; UINTN Size = sizeof (Ind); UINT16 Next = 0, Order[32]; EFI_STATUS S;
  /* r5.25.11: bits 8.. of NextFlags carry what was found (the restarted firmware logs them; it uses only bits 0/1):
   * 8-15 BootOrder status (low byte, 0 = ok), 16-23 BootOrder entries, 24-31 entries with a GPT HD() path,
   * 32-39 BootNext status (low byte), 40-47 index of the chosen BootOrder entry, 48 diagnostics present */
  P->NextFlags = 1ULL << 48;
  if (!EFI_ERROR (mPrivateRt->GetVariable (L"OsIndications", &gEfiGlobalVariableGuid, NULL, &Size, &Ind)) && (Ind & 1)) P->NextFlags |= 1;
  Size = sizeof (Next);
  S = mPrivateRt->GetVariable (L"BootNext", &gEfiGlobalVariableGuid, NULL, &Size, &Next);
  P->NextFlags |= (UINT64)(S & 0xFF) << 32;
  if (!EFI_ERROR (S) && Size == 2 && WarmGuidOf (Next, P->NextGuid, FALSE)) { P->NextFlags |= 2; return; }
  Size = sizeof (Order);
  S = mPrivateRt->GetVariable (L"BootOrder", &gEfiGlobalVariableGuid, NULL, &Size, Order);
  P->NextFlags |= (UINT64)(S & 0xFF) << 8;
  if (EFI_ERROR (S)) return;
  P->NextFlags |= (UINT64)((Size / 2) & 0xFF) << 16;
  UINT8 Tmp[16]; UINTN Gpt = 0;
  for (UINTN I = 0; I < Size / 2; I++) if (WarmGuidOf (Order[I], Tmp, FALSE)) Gpt++;
  P->NextFlags |= (UINT64)(Gpt & 0xFF) << 24;
  for (UINTN I = 0; I < Size / 2; I++)
    if (WarmGuidOf (Order[I], P->NextGuid, TRUE)) { P->NextFlags |= 2 | ((UINT64)(I & 0xFF) << 40); return; }
}
STATIC VOID WarmReentry (VOID) {
#ifdef PS5_WARM_OFF_TEST
  /* lab (HW 19.11.): the console switches off (ICC) here - shows without a picture that the OS reached ResetSystem */
  Power (FALSE);
#endif
#ifdef PS5_WARM_SPIN_TEST
  /* lab (HW 19.11.): stop here - the console stays on if Windows calls ResetSystem, it resets if Windows goes around it */
  DisableInterrupts ();
  for (;;) __asm__ __volatile__ ("hlt");
#endif
  if (!mWarmReady) return;
  /* r5.25.5: the warm firmware's BDS asks for a cold restart through GameOS (direct-boot target missing) -
   * ResetCold then falls through to Power (TRUE). Before EBS only: afterwards the arena is not mapped for runtime code. */
  if (!mWarmEbs && *(volatile UINT64 *)(UINTN)0x1118061A0ULL == 0x31444C4F43575350ULL) return;   /* "PSWCOLD1" */
  WarmReadOsRequest ((PS5_WARM_PARAMS *)(UINTN)(mWarmVa + mWarmParOff));
  WarmTakeFramebuffer ();
  /* r5.25.5 (HW 19.11.: a restart from Windows hung dark right away): after the OS the framebuffer belongs to a GPU the OS
   * may have left in D3 - no breadcrumb bars and no lab squares there (only the firmware self-test before EBS paints) */
  if (mWarmEbs) ((PS5_WARM_PARAMS *)(UINTN)(mWarmVa + mWarmParOff))->FbBase = 0;   /* the OS address - mWarmParams is physical */
#if defined (PS5_WARM_PROBE) && PS5_WARM_PROBE == 5
  Power (FALSE);   /* lab (HW 19.11.): off here = OsIndications/BootNext/BootOrder were read inside ResetSystem */
#endif
  /* r5.25.4 (HW 19.11.: a Windows restart still went through GameOS): Windows may restart on any core - the trampoline makes
   * the calling core the boot core (APIC_BASE BSC bit) and INIT reaches the former one with all others.
   * Note (HW 19.11. spin test): it was not Windows going around ResetSystem but ps5pwr rebooting through the ICC first. */
  DisableInterrupts ();
  ((VOID (*)(VOID))(UINTN)mWarmVa) ();
}
VOID EFIAPI ResetCold (VOID) { WarmReentry (); Power (TRUE); }
VOID EFIAPI ResetWarm (VOID) { WarmReentry (); Power (TRUE); }
VOID EFIAPI ResetShutdown (VOID) { Power (FALSE); }
VOID EFIAPI ResetPlatformSpecific (UINTN DataSize, VOID *ResetData) { WarmReentry (); Power (TRUE); }
