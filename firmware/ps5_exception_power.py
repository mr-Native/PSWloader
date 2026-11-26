"""Explicit diagnostic fallback for unhandled DXE vectors, not registered IRQs."""
import subprocess

DIRECTORY = 'UefiCpuPkg/Library/CpuExceptionHandlerLib'
SOURCE = DIRECTORY + '/DxeException.c'
INF = DIRECTORY + '/DxeCpuExceptionHandlerLib.inf'
HOOK = '''  if (((UINTN)ExceptionType >= 32) && ((UINTN)ExceptionType < X86_CPU_INTERRUPT_NUM) &&
      (mExternalInterruptHandlerTable[(UINTN)ExceptionType] == NULL)) {
    // r5.25.5 (HW 19.11.): a device interrupt without a handler (e.g. the ICC's MSI on 0xC0-0xC4 once DXE enables
    // interrupts after CpuDxe) is no fault - acknowledge it at the local APIC and resume. It dead-looped here before:
    // every restart from Windows, and the random early hang right after CpuDxe/Metronome on cold boots.
    // A vector that keeps coming with nobody listening (HW 19.11.: after a restart from Windows the BSP never got past
    // opening the extended-APIC filter - an interrupt flood) is taken out of the AMD extended-APIC filter (IER).
    UINT64 Ps5ApicBase = AsmReadMsr64 (0x1B);
    if ((Ps5ApicBase & BIT10) != 0) {
      AsmWriteMsr64 (0x80B, 0);
    } else {
      volatile UINT8 *Ps5Apic = (volatile UINT8 *)(UINTN)(Ps5ApicBase & 0xFFFFF000ULL);
      *(volatile UINT32 *)(Ps5Apic + 0xB0) = 0;
      if ((++mPs5StrayCount[(UINTN)ExceptionType] >= 16) && ((*(volatile UINT32 *)(Ps5Apic + 0x400) & BIT0) != 0)) {
        *(volatile UINT32 *)(Ps5Apic + 0x480 + 0x10 * ((UINTN)ExceptionType >> 5)) &= ~(1U << ((UINTN)ExceptionType & 31));
      }
    }
    return;
  }
  if (((UINTN)ExceptionType >= 16) && ((UINTN)ExceptionType < 32) && ((UINTN)ExceptionType != 17) &&
      ((UINTN)ExceptionType != 21) && ((UINTN)ExceptionType != 29) &&
      (mExternalInterruptHandlerTable[(UINTN)ExceptionType] == NULL)) {
    // r5.25.5: a device interrupt can carry a vector below 32 (a stale MSI after a restart from Windows): the local
    // APIC's in-service bit tells it from a CPU exception (exceptions never set ISR). Acknowledge and resume.
    UINT64 Ps5IsrBase = AsmReadMsr64 (0x1B);
    if ((Ps5IsrBase & (BIT10 | BIT11)) == BIT11) {
      volatile UINT8 *Ps5Apic = (volatile UINT8 *)(UINTN)(Ps5IsrBase & 0xFFFFF000ULL);
      if ((*(volatile UINT32 *)(Ps5Apic + 0x100) & (1U << (UINTN)ExceptionType)) != 0) {
        // EOI only - masking IER[0] would take 0x1F (Windows' APC self-IPI) away from Windows for good. 17/21/29 are
        // left out above: their stub expects a CPU error code a device interrupt does not push.
        *(volatile UINT32 *)(Ps5Apic + 0xB0) = 0;
        return;
      }
    }
  }
  if (((UINTN)ExceptionType < X86_CPU_INTERRUPT_NUM) &&
      (mExternalInterruptHandlerTable[(UINTN)ExceptionType] == NULL)) {
    // This path would otherwise dump to UART and dead-loop. Never resume it.
    DisableInterrupts ();
    if (InterlockedCompareExchange32 (&mPs5FaultReporting, 0, 1) == 0) {
      ps5_fault_power_calibrate ();
      ps5_fault_power_report (0x800U + (unsigned)ExceptionType);
    }
    CpuDeadLoop ();
  }
'''


def prepare_exception_power(root, enabled, encoder):
    originals = {p: subprocess.check_output(['git', 'show', 'HEAD:' + p],
                 cwd=root, text=True) for p in (SOURCE, INF)}
    source, inf = originals[SOURCE], originals[INF]
    declaration = 'CONST UINTN  mDoFarReturnFlag = 0;'
    anchor = '  CommonExceptionHandlerWorker (ExceptionType, SystemContext, &mExceptionHandlerData);'
    inf_anchor = '[Sources.X64]\n'
    if any(text.count(item) != 1 for text, item in
           ((source, declaration), (source, anchor), (inf, inf_anchor))):
        raise RuntimeError('Unexpected pinned DXE exception power anchors')
    if enabled:
        source = source.replace(declaration,
            'STATIC volatile UINT32 mPs5FaultReporting;\nSTATIC UINT32 mPs5StrayCount[X86_CPU_INTERRUPT_NUM];\n'
            'VOID ps5_fault_power_report (unsigned Code);\nVOID ps5_fault_power_calibrate (VOID);\n\n' + declaration)
        source = source.replace(anchor, HOOK + anchor)
        inf = inf.replace(inf_anchor, inf_anchor + '  Ps5FaultPower.c\n')
    (root / SOURCE).write_text(source)
    (root / INF).write_text(inf)
    target = root / DIRECTORY / 'Ps5FaultPower.c'
    if enabled:
        target.write_text('#define PS5_POWER_EDK2\n'
            '#define ps5_power_report ps5_fault_power_report\n'
            '#define ps5_power_calibrate ps5_fault_power_calibrate\n'
            '#define ps5_power_report_pci_byte ps5_fault_power_report_pci_byte\n' + encoder)
    elif target.exists():
        target.unlink()
