/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * ps5-extapic.h v1.1 - open the AMD extended-APIC interrupt filter in UEFI.
 * Multi-core aware: BSP opens and logs its own state; APs are measured and opened
 * via EFI_MP_SERVICES_PROTOCOL->StartupAllAPs without calling DEBUG() from AP context.
 * The BSP logs the entire per-core inventory afterwards.
 */
#ifndef PS5_EXTAPIC_H
#define PS5_EXTAPIC_H
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Pi/PiMultiPhase.h>
#include <Protocol/MpService.h>
#include <Library/BaseMemoryLib.h>
#include "ps5-cpu-name.h"
#if defined (PS5_WARM_PROBE) || defined (PS5_WARM_WATCHDOG)
#include "ps5-warm-probe.h"
#endif

typedef struct {
  UINT32 Efeat, Ectrl, IerC0Before, IerC0After, Ier20After;
  UINT32 Applied;
} PS5_EXTAPIC_DIAG;
STATIC PS5_EXTAPIC_DIAG mPs5ExtApic;

typedef struct {
  UINT32 Efeat;
  UINT32 Ectrl;
  UINT32 IerC0Before;
  UINT32 IerC0After;
  UINT32 Ier20After;
  UINT32 State; /* 0=unreached, 1=opened, 2=x2apic/disabled, 3=no-ier/filter-off */
} PS5_AP_EXTAPIC_RECORD;
STATIC PS5_AP_EXTAPIC_RECORD mPs5ApExtApic[16];

#define PS5_XAPIC(Base, Off) (*(volatile UINT32 *)(UINTN)((Base) + (Off)))

STATIC
VOID
EFIAPI
Ps5ExtApicApWorker (
  IN VOID *Context
  )
{
  EFI_MP_SERVICES_PROTOCOL *MpService = (EFI_MP_SERVICES_PROTOCOL *)Context;
  UINTN CpuIndex = 0;
  if (MpService != NULL) {
    MpService->WhoAmI (MpService, &CpuIndex);
  }
  if (CpuIndex >= 16) {
    return;
  }

  UINT64 Msr = AsmReadMsr64 (0x1B);
  UINT64 Base = Msr & 0xFFFFF000ULL;
  if ((Msr & BIT10) != 0 || (Msr & BIT11) == 0) {
    mPs5ApExtApic[CpuIndex].State = 2;
    return;
  }

  mPs5ApExtApic[CpuIndex].Efeat = PS5_XAPIC (Base, 0x400);
  mPs5ApExtApic[CpuIndex].Ectrl = PS5_XAPIC (Base, 0x410);
  mPs5ApExtApic[CpuIndex].IerC0Before = PS5_XAPIC (Base, 0x480 + 0x10 * 6);

  if ((mPs5ApExtApic[CpuIndex].Efeat & BIT0) == 0 || (mPs5ApExtApic[CpuIndex].Ectrl & BIT0) == 0) {
    mPs5ApExtApic[CpuIndex].State = 3;
    return;
  }

  for (UINTN Index = 0; Index < 8; Index++) {
    PS5_XAPIC (Base, 0x480 + 0x10 * Index) = 0xFFFFFFFFU;
  }
  PS5_XAPIC (Base, 0x80) = 0;
  PS5_XAPIC (Base, 0xB0) = 0;

  mPs5ApExtApic[CpuIndex].IerC0After = PS5_XAPIC (Base, 0x480 + 0x10 * 6);
  mPs5ApExtApic[CpuIndex].Ier20After = PS5_XAPIC (Base, 0x480 + 0x10 * 1);
  mPs5ApExtApic[CpuIndex].State = 1;
}

#if defined (PS5_WARM_WATCHDOG)
STATIC VOID EFIAPI Ps5WarmWatchdogAp (IN VOID *Context) { (VOID)Context; Ps5WarmWatchdog (); }   /* lab */
#endif
#if defined (PS5_WARM_PROBE) && PS5_WARM_PROBE == 79
STATIC VOID Ps5Probe79Isr (VOID)   /* runs on the AP: flag for the BSP, then park */
{
  *(volatile UINT64 *)(UINTN)0x1118061C8ULL = 0x544E495041575350ULL;   /* "PSWAPINT" */
  for ( ; ; ) __asm__ __volatile__ ("cli; hlt");
}
STATIC VOID EFIAPI Ps5Probe79Ap (IN VOID *Context)
{
  UINT64 Base = AsmReadMsr64 (0x1B) & 0xFFFFF000ULL;
  __asm__ __volatile__ ("cli");
  PS5_XAPIC (Base, 0x80) = 0;
  PS5_XAPIC (Base, 0x480 + 0x10 * 1) |= 0x2U;
  PS5_XAPIC (Base, 0x310) = 0;
  PS5_XAPIC (Base, 0x300) = 0x00044021U;                       /* self, fixed, edge, 0x21 */
  for (UINTN N = 0; N < 1000000 && (PS5_XAPIC (Base, 0x300) & BIT12) != 0; N++) CpuPause ();
  __asm__ __volatile__ ("sti");
  for ( ; ; ) CpuPause ();                                      /* the interrupt leaves through Ps5Probe79Isr */
}
#endif
#if defined (PS5_WARM_PROBE) && (PS5_WARM_PROBE == 74 || PS5_WARM_PROBE == 77 || PS5_WARM_PROBE == 78 || PS5_WARM_PROBE == 79)
/* lab: IDT gate offset write that cannot #PF on a read-only IDT page (CR0.WP off for the store), with readback.
 * A gate that does not read back switches off only after ~60 s nominal - distinct from the probe outcomes (0 / 30 s). */
STATIC VOID Ps5ProbeSetGate (IA32_IDT_GATE_DESCRIPTOR *G, UINTN H)
{
  UINTN   Cr0 = AsmReadCr0 ();
  BOOLEAN Ie  = SaveAndDisableInterrupts ();
  AsmWriteCr0 (Cr0 & ~(UINTN)BIT16);
  G->Bits.OffsetLow   = (UINT16)H;
  G->Bits.OffsetHigh  = (UINT16)(H >> 16);
  G->Bits.OffsetUpper = (UINT32)(H >> 32);
  AsmWriteCr0 (Cr0);
  SetInterruptState (Ie);
  if ((G->Bits.OffsetLow | ((UINTN)G->Bits.OffsetHigh << 16) | ((UINTN)G->Bits.OffsetUpper << 32)) != H) {
    UINT64 T0 = AsmReadTsc ();
    while (AsmReadTsc () - T0 < 60ULL * 1596300000ULL) CpuPause ();
    Ps5WarmProbeOff ();
  }
}
#endif
#if defined (PS5_WARM_PROBE) && (PS5_WARM_PROBE == 74 || PS5_WARM_PROBE == 77 || PS5_WARM_PROBE == 78)
/* lab: IDT target for the self-IPI 0x21 - entered through the interrupt gate (CpuDxe's selector 0x38, type
 * interrupt): off and never return (no iretq). The gate's pushes leave RSP = 8 mod 16 like a call; EDK2 builds with
 * -mno-red-zone. */
STATIC VOID Ps5Probe74Isr (VOID)
{
  Ps5WarmProbeOff ();
  for ( ; ; ) {
    __asm__ __volatile__ ("cli; hlt");
  }
}
#endif
STATIC UINT64 mPs5IerBase;
STATIC BOOLEAN mPs5IerWarm;   /* r5.25.5: BDS kept the filter closed after a warm re-entry */
STATIC VOID EFIAPI Ps5ExtApicOpenAllIer (IN EFI_EVENT Event, IN VOID *Context)
{
  /* ExitBootServices: the OS gets the whole filter open again - IER[0] (Windows APC 0x1F etc.) and any block the
   * firmware's stray-interrupt guard closed; a pending 0x1F then reaches Windows' own IDT */
  BOOLEAN Ie = SaveAndDisableInterrupts ();
#if defined (PS5_WARM_EBS_WATCH)
  Ps5WarmProgress (54);
#endif
#if defined (PS5_WARM_EBS_STEPS)
  { volatile UINT64 *C = (volatile UINT64 *)(UINTN)0x111806300ULL; if (C[0] == 0x3142465153575350ULL && C[1] != 0) { volatile UINT32 *F = (volatile UINT32 *)(UINTN)C[1]; for (UINT32 Y = 0; Y < 48; Y++) for (UINT32 X = 0; X < 48; X++) F[Y * C[2] + 320 + 14 * 60 + X] = 0x00008080U; AsmWbinvd (); } }   /* lab */
#endif
  if ((mPs5IerBase != 0) && mPs5IerWarm) {
    /* r5.25.5 (HW 20.11. lab81: everything froze at ExitBootServices): the LAPIC timer ran with 0x20 filtered the
     * whole warm UEFI phase - IRR[0x20] is pending and would be accepted the moment the dispatcher re-enables IF after this
     * notify (the warm killer). Hard reset like the trampoline: EN off/on clears IRR/ISR/TMR/LVTs and the filter. */
    UINT64 M = AsmReadMsr64 (0x1B);
    PS5_XAPIC (mPs5IerBase, 0x320) |= 0x10000U;                       /* timer LVT masked */
    AsmWriteMsr64 (0x1B, M & ~0xC00ULL);                              /* EN and EXTD off: APIC reset */
    AsmWriteMsr64 (0x1B, (M & ~0x400ULL) | 0x800ULL);                 /* xAPIC on again, same base and BSC */
    PS5_XAPIC (mPs5IerBase, 0xF0) = 0x1FF;                            /* SVR: enabled, spurious 0xFF */
    for (UINTN L = 0; L < 4; L++) PS5_XAPIC (mPs5IerBase, 0x500 + 0x10 * L) |= 0x10000U;   /* extended LVTs masked */
    if ((PS5_XAPIC (mPs5IerBase, 0x400) & BIT0) != 0) PS5_XAPIC (mPs5IerBase, 0x410) |= BIT0;   /* filter active as cold */
  }
  if (mPs5IerBase != 0) {
    for (UINTN Index = 0; Index < 8; Index++) PS5_XAPIC (mPs5IerBase, 0x480 + 0x10 * Index) = 0xFFFFFFFFU;
  }
#if defined (PS5_WARM_EBS_WATCH)
  Ps5WarmProgress (55);
#endif
#if defined (PS5_WARM_EBS_STEPS)
  { volatile UINT64 *C = (volatile UINT64 *)(UINTN)0x111806300ULL; if (C[0] == 0x3142465153575350ULL && C[1] != 0) { volatile UINT32 *F = (volatile UINT32 *)(UINTN)C[1]; for (UINT32 Y = 0; Y < 48; Y++) for (UINT32 X = 0; X < 48; X++) F[Y * C[2] + 320 + 15 * 60 + X] = 0x00808000U; AsmWbinvd (); } }   /* lab */
#endif
  (VOID)Ie;   /* CoreExitBootServices keeps interrupts off after the notifies - not re-enabled here */
}
STATIC VOID Ps5ExtApicArmIerAtEbs (UINT64 Base)
{
  EFI_EVENT Ev;
  mPs5IerBase = Base;
  if (EFI_ERROR (gBS->CreateEvent (EVT_SIGNAL_EXIT_BOOT_SERVICES, TPL_NOTIFY, Ps5ExtApicOpenAllIer, NULL, &Ev))) {
    PS5_XAPIC (Base, 0x480) = 0xFFFFFFFFU;   /* no event: the old behaviour */
  }
}

VOID
Ps5ExtApicOpenIer (
  VOID
  )
{
#if defined (PS5_WARM_PROBE) && PS5_WARM_PROBE == 42
  Ps5WarmProbeOff ();   /* lab: BDS reached the ext-APIC/CPU-name step after a warm re-entry */
#endif
  Ps5CpuNameApply ();   /* r4-nvme-name (16.11.): CPU/GPU names, before the ext-APIC work */
#if defined (PS5_WARM_RESCUE)
  if (Ps5WarmRescueRun ()) {   /* lab RUN 2: no IER opening (the hang trigger), no watchdog, no timer - just reach a filesystem */
    DEBUG ((DEBUG_ERROR, "PS5 warm rescue run: extended-APIC filter left closed, no watchdog - exporting the rescued log\n"));
    return;
  }
#endif
#if defined (PS5_WARM_WATCHDOG_SELFTEST)
  *(volatile UINT64 *)(UINTN)0x111806040ULL = 0x314D524157575350ULL;   /* lab: cold-boot self-test of the watchdog - pretend a warm re-entry */
#endif
#if defined (PS5_WARM_WATCHDOG_EARLY)
  if (*(volatile UINT64 *)(UINTN)0x111806040ULL == 0x314D524157575350ULL) {   /* lab: watchdog before the IER opening */
    EFI_MP_SERVICES_PROTOCOL *WdMp = NULL; UINTN WdN = 0, WdE = 0; EFI_EVENT WdDone = NULL; EFI_STATUS WdS;
    WdS = gBS->LocateProtocol (&gEfiMpServiceProtocolGuid, NULL, (VOID **)&WdMp);
    if (!EFI_ERROR (WdS)) WdS = WdMp->GetNumberOfProcessors (WdMp, &WdN, &WdE);
    if (!EFI_ERROR (WdS)) WdS = gBS->CreateEvent (0, 0, NULL, NULL, &WdDone);
    Ps5WarmArm ();
    if (!EFI_ERROR (WdS) && WdN > 1) WdS = WdMp->StartupThisAP (WdMp, Ps5WarmWatchdogAp, WdN - 1, WdDone, 0, NULL, NULL);
    DEBUG ((DEBUG_ERROR, "PS5 warm watchdog (early): AP %u %r\n", (UINT32)(WdN - 1), WdS));
  }
#endif
#if defined (PS5_WARM_PROBE) && PS5_WARM_PROBE == 43
  Ps5WarmProbeOff ();   /* lab: CPU names on all APs done */
#endif
#if defined (PS5_WARM_PROBE) && PS5_WARM_PROBE == 70
  { /* lab: did the APs run Ps5CpuNameApWorker after the warm re-entry? all 15 -> off now, some -> 60 s, none -> 120 s (TSC nominal) */
    UINT32 Ok = 0; for (UINTN I = 1; I < 16; I++) if (mPs5ApNameState[I] == 1) Ok++;
    UINT64 W = (Ok == 15) ? 0 : (Ok > 0 ? 60ULL : 120ULL), T0 = AsmReadTsc ();
    while (AsmReadTsc () - T0 < W * 1596300000ULL) CpuPause ();
    Ps5WarmProbeOff ();
  }
#endif
  /* 1. BSP measurement & opening */
  UINT64 Msr = AsmReadMsr64 (0x1B);
  UINT64 Base = Msr & 0xFFFFF000ULL;
  if ((Msr & BIT10) != 0 || (Msr & BIT11) == 0) {
    DEBUG ((DEBUG_WARN, "PS5 ext-APIC BSP: x2APIC or disabled (MSR 1B=%lx)\n", Msr));
  } else {
    mPs5ExtApic.Efeat = PS5_XAPIC (Base, 0x400);
    mPs5ExtApic.Ectrl = PS5_XAPIC (Base, 0x410);
    mPs5ExtApic.IerC0Before = PS5_XAPIC (Base, 0x480 + 0x10 * 6);

    DEBUG ((DEBUG_ERROR, "PS5 ext-APIC BSP init: EFEAT=%x ECTRL=%x IER[C0]=%x\n",
            mPs5ExtApic.Efeat, mPs5ExtApic.Ectrl, mPs5ExtApic.IerC0Before));

#if defined (PS5_WARM_PROBE) && PS5_WARM_PROBE == 57
    Ps5WarmProbeOff ();   /* lab: after the first ext-APIC DEBUG line */
#endif
#if defined (PS5_WARM_PROBE) && PS5_WARM_PROBE == 65
    { UINT64 T0 = AsmReadTsc (); while (AsmReadTsc () - T0 < 60ULL * 1596300000ULL) CpuPause ();   /* lab: time vs event - */
      Ps5WarmProbeOff (); }                                                                        /* 60 s alive with IER still closed? */
#endif
    if ((mPs5ExtApic.Efeat & BIT0) == 0 || (mPs5ExtApic.Ectrl & BIT0) == 0) {
      DEBUG ((DEBUG_ERROR, "PS5 ext-APIC BSP: IER capability missing or filter inactive\n"));
    } else {
#if defined (PS5_WARM_PROBE) && PS5_WARM_PROBE == 61
      { UINT32 Irr0 = PS5_XAPIC (Base, 0x200), Isr = 0; for (UINTN R = 0; R < 8; R++) Isr |= PS5_XAPIC (Base, 0x100 + 0x10 * R);
        if (Irr0 & 0xFFFF0000U) { UINT32 V = 31 - (UINT32)__builtin_clz (Irr0 & 0xFFFF0000U);   /* highest pending 16..31 */
          gBS->Stall ((V - 15) * 10000000U); Ps5WarmProbeOff (); }                              /* lab: off after (V-15)*10 s */
        if (Isr) { gBS->Stall (200U * 1000000U); Ps5WarmProbeOff (); }                          /* lab: stale ISR: off after 200 s */
      }
#endif
      /* r5.25.5 (HW 19.11.: probe 57 before this loop switched off, 58 after it did not): vectors 0-31
       * stay filtered while UEFI runs. UEFI uses no device vector below 32, and a stale request there (after a Windows
       * restart most likely the HAL's APC self-IPI 0x1F, requested at HIGH_LEVEL and still pending) was taken as a CPU
       * exception and dead-looped with interrupts off (17/21/29 would even get a wrong frame - their stub expects an
       * error code). IER[0] - and every other block - is opened again at ExitBootServices with interrupts off. */
#if defined (PS5_WARM_WATCHDOG)
      Ps5WarmProgress (41);
#endif
      /* r5.25.7: debug/trace state and a CPUID timing (a hypervisor that intercepts CPUID costs >1000 cycles) - cold and warm */
      { UINT64 T, Best = ~0ULL; UINT32 A, B, C, D;
        for (UINTN I = 0; I < 64; I++) { T = AsmReadTsc (); AsmCpuid (0, &A, &B, &C, &D); T = AsmReadTsc () - T; if (T < Best) Best = T; }
        /* HW 20.11. lab96: with the IBS/PERFSEL MSRs in this line the cold boot never reached Windows - only CPUID and DR7 */
        DEBUG ((DEBUG_ERROR, "PS5 warm dbg: cpuid-cycles=%Lu DR7=%Lx\n", Best, (UINT64)AsmReadDr7 ()));
      }
      /* HW 20.11. bisect: the warm cpu/svm diagnostic lines (TSS read through TR, VM_CR/HSAVE) stopped every warm run
       * here (a9386dcb6) - removed */
      { /* r5.25.6: what is left of the previous OS in the BSP's local APIC (cold and warm, one line each) */
        UINT32 Irr[8], Isr[8], Tmr[8];
        for (UINTN R = 0; R < 8; R++) {
          Irr[R] = PS5_XAPIC (Base, 0x200 + 0x10 * R); Isr[R] = PS5_XAPIC (Base, 0x100 + 0x10 * R); Tmr[R] = PS5_XAPIC (Base, 0x180 + 0x10 * R);
        }
        PS5_XAPIC (Base, 0x280) = 0;
        DEBUG ((DEBUG_ERROR, "PS5 apic IRR %08x %08x %08x %08x %08x %08x %08x %08x\n", Irr[7], Irr[6], Irr[5], Irr[4], Irr[3], Irr[2], Irr[1], Irr[0]));
        DEBUG ((DEBUG_ERROR, "PS5 apic ISR %08x %08x %08x %08x %08x %08x %08x %08x\n", Isr[7], Isr[6], Isr[5], Isr[4], Isr[3], Isr[2], Isr[1], Isr[0]));
        DEBUG ((DEBUG_ERROR, "PS5 apic TMR %08x %08x %08x %08x %08x %08x %08x %08x ESR=%x\n", Tmr[7], Tmr[6], Tmr[5], Tmr[4], Tmr[3], Tmr[2], Tmr[1], Tmr[0],
                PS5_XAPIC (Base, 0x280)));
      }
      { /* r5.25.5: stale in-service bits drained with interrupts off, so PPR is 0 when the filter opens */
        BOOLEAN Ie = SaveAndDisableInterrupts ();
        UINT32 Isr = 0, Drained = 0;
        for (UINTN N = 0; N < 256; N++) {
          Isr = 0;
          for (UINTN R = 0; R < 8; R++) Isr |= PS5_XAPIC (Base, 0x100 + 0x10 * R);
          if (Isr == 0) break;
          PS5_XAPIC (Base, 0xB0) = 0; Drained++;
        }
        PS5_XAPIC (Base, 0x80) = 0;                                       /* TPR 0 before the filter opens */
        SetInterruptState (Ie);
        DEBUG ((DEBUG_ERROR, "PS5 ext-APIC BSP: %u stale ISR EOIs, PPR=%x SVR=%x LVT-timer=%x init=%x cur=%x div=%x\n",
                Drained, PS5_XAPIC (Base, 0xA0), PS5_XAPIC (Base, 0xF0), PS5_XAPIC (Base, 0x320),
                PS5_XAPIC (Base, 0x380), PS5_XAPIC (Base, 0x390), PS5_XAPIC (Base, 0x3E0)));
      }
#if defined (PS5_WARM_WATCHDOG)
      Ps5WarmProgress (42);
#endif
#if defined (PS5_WARM_PROBE) && PS5_WARM_PROBE == 67
      Ps5WarmProbeOff ();   /* lab: after the ISR drain, before the timer vector opens */
#endif
#if defined (PS5_WARM_PROBE) && PS5_WARM_PROBE == 72
      __asm__ __volatile__ ("cli");                           /* lab: does the IER WRITE itself kill, or the acceptance? */
      PS5_XAPIC (Base, 0x480 + 0x10 * 1) |= 0x1U;
      Ps5WarmProbeOff ();
#endif
#if defined (PS5_WARM_PROBE) && PS5_WARM_PROBE == 73
      if (*(volatile UINT64 *)(UINTN)0x111806040ULL == 0x314D524157575350ULL)   /* warm runs only: a cold boot must not take the probe's interrupt */
      { /* lab: IF on and IER[0x20] open, but TPR 0xF0 blocks every class - alive without any acceptance? */
        UINT64 T0;
        __asm__ __volatile__ ("cli");
        PS5_XAPIC (Base, 0x80) = 0xF0;
        PS5_XAPIC (Base, 0x480 + 0x10 * 1) |= 0x1U;
        __asm__ __volatile__ ("sti");
        T0 = AsmReadTsc ();
        while (AsmReadTsc () - T0 < 2ULL * 1596300000ULL) CpuPause ();
        Ps5WarmProbeOff ();   /* cli inside */
      }
#endif
#if defined (PS5_WARM_PROBE) && PS5_WARM_PROBE == 74
      if (*(volatile UINT64 *)(UINTN)0x111806040ULL == 0x314D524157575350ULL)   /* warm runs only: a cold boot must not take the probe's interrupt */
      { /* lab: LAPIC-internal self-IPI 0x21 (fixed, edge), only 0x21 open in the filter, own IDT target */
        IA32_DESCRIPTOR           Idtr;
        IA32_IDT_GATE_DESCRIPTOR  *G;
        UINTN                     H = (UINTN)Ps5Probe74Isr;
        UINT32                    Icr = 0;
        UINT64                    T0;
        __asm__ __volatile__ ("cli");
        AsmReadIdtr (&Idtr);
        G = (IA32_IDT_GATE_DESCRIPTOR *)Idtr.Base + 0x21;
        Ps5ProbeSetGate (G, H);   /* CR0.WP off + readback */
        PS5_XAPIC (Base, 0x80) = 0;
        PS5_XAPIC (Base, 0x480 + 0x10 * 1) |= 0x2U;            /* vector 0x21 only (0x20 stays filtered) */
        PS5_XAPIC (Base, 0x310) = 0;
        PS5_XAPIC (Base, 0x300) = 0x00044021U;                 /* shorthand self, assert, edge, fixed, vector 0x21 */
        for (UINTN N = 0; N < 1000000 && (PS5_XAPIC (Base, 0x300) & BIT12) != 0; N++) CpuPause ();
        Icr = PS5_XAPIC (Base, 0x300);
        __asm__ __volatile__ ("sti");                          /* the pending 0x21 is accepted here */
        T0 = AsmReadTsc ();
        while (AsmReadTsc () - T0 < 5ULL * 1596300000ULL) CpuPause ();
        __asm__ __volatile__ ("cli");                          /* not accepted within 5 s: alive, off after 30 s */
        DEBUG ((DEBUG_ERROR, "PS5 probe 74: self-IPI 0x21 not taken, ICR=%x IRR1=%x IER1=%x\n",
                Icr, PS5_XAPIC (Base, 0x200 + 0x10 * 1), PS5_XAPIC (Base, 0x480 + 0x10 * 1)));
        T0 = AsmReadTsc ();
        while (AsmReadTsc () - T0 < 25ULL * 1596300000ULL) CpuPause ();
        Ps5WarmProbeOff ();
      }
#endif
#if defined (PS5_WARM_PROBE) && PS5_WARM_PROBE == 77
      if (*(volatile UINT64 *)(UINTN)0x111806040ULL == 0x314D524157575350ULL)   /* warm runs only: a cold boot must not take the probe's interrupt */
      { /* lab: LAPIC timer one-shot on 0x21 (divide by 1), only 0x21 open in the filter, own IDT target */
        IA32_DESCRIPTOR           Idtr;
        IA32_IDT_GATE_DESCRIPTOR  *G;
        UINTN                     H = (UINTN)Ps5Probe74Isr;
        UINT32                    Irr1 = 0, Cur = 0;
        UINT64                    T0;
        __asm__ __volatile__ ("cli");
        AsmReadIdtr (&Idtr);
        G = (IA32_IDT_GATE_DESCRIPTOR *)Idtr.Base + 0x21;
        Ps5ProbeSetGate (G, H);   /* CR0.WP off + readback */
        PS5_XAPIC (Base, 0x80) = 0;
        PS5_XAPIC (Base, 0x480 + 0x10 * 1) |= 0x2U;            /* vector 0x21 only (0x20 stays filtered) */
        PS5_XAPIC (Base, 0x380) = 0;                           /* timer stopped while it is set up */
        PS5_XAPIC (Base, 0x3E0) = 0xB;                         /* divide by 1 */
        PS5_XAPIC (Base, 0x320) = 0x21;                        /* one-shot, unmasked, fixed vector 0x21 */
        PS5_XAPIC (Base, 0x380) = 100000;                      /* initial count: fires within well under 1 ms */
        T0 = AsmReadTsc ();                                    /* wait (IF=0) until the timer has requested 0x21 */
        while (((Irr1 = PS5_XAPIC (Base, 0x200 + 0x10 * 1)) & 0x2U) == 0 && AsmReadTsc () - T0 < 1596300000ULL) CpuPause ();
        Cur = PS5_XAPIC (Base, 0x390);
        __asm__ __volatile__ ("sti");                          /* the pending timer interrupt is accepted here */
        T0 = AsmReadTsc ();
        while (AsmReadTsc () - T0 < 5ULL * 1596300000ULL) CpuPause ();
        __asm__ __volatile__ ("cli");                          /* not accepted within 5 s: alive, off after 30 s */
        PS5_XAPIC (Base, 0x320) = 0x10021;                     /* masked again */
        DEBUG ((DEBUG_ERROR, "PS5 probe 77: LAPIC timer 0x21 not taken, IRR1 before sti=%x now=%x TMCCT=%x IER1=%x LVT=%x\n",
                Irr1, PS5_XAPIC (Base, 0x200 + 0x10 * 1), Cur, PS5_XAPIC (Base, 0x480 + 0x10 * 1), PS5_XAPIC (Base, 0x320)));
        T0 = AsmReadTsc ();
        while (AsmReadTsc () - T0 < 25ULL * 1596300000ULL) CpuPause ();
        Ps5WarmProbeOff ();
      }
#endif
#if defined (PS5_WARM_PROBE) && PS5_WARM_PROBE == 78
      if (*(volatile UINT64 *)(UINTN)0x111806040ULL == 0x314D524157575350ULL)   /* warm runs only */
      { /* lab: software int 0x21 into the probe-74 target - same gate/stack, no LAPIC delivery */
        IA32_DESCRIPTOR           Idtr;
        IA32_IDT_GATE_DESCRIPTOR  *G;
        UINTN                     H = (UINTN)Ps5Probe74Isr;
        UINT64                    T0;
        __asm__ __volatile__ ("cli");
        AsmReadIdtr (&Idtr);
        G = (IA32_IDT_GATE_DESCRIPTOR *)Idtr.Base + 0x21;
        Ps5ProbeSetGate (G, H);   /* CR0.WP off + readback */
        __asm__ __volatile__ ("int $0x21");                    /* the target switches off and never returns */
        T0 = AsmReadTsc ();                                    /* not reached unless the int returned somehow */
        while (AsmReadTsc () - T0 < 30ULL * 1596300000ULL) CpuPause ();
        Ps5WarmProbeOff ();
      }
#endif
#if defined (PS5_WARM_PROBE) && PS5_WARM_PROBE == 79
      if (*(volatile UINT64 *)(UINTN)0x111806040ULL == 0x314D524157575350ULL)   /* warm runs only */
      { /* lab: hardware interrupt on AP 1 - BSP-specific killer or global? */
        IA32_DESCRIPTOR Idtr; IA32_IDT_GATE_DESCRIPTOR *G; UINTN H = (UINTN)Ps5Probe79Isr; UINT64 T0;
        EFI_MP_SERVICES_PROTOCOL *Mp = NULL; EFI_EVENT Ev = NULL; EFI_STATUS S;
        volatile UINT64 *Flag = (volatile UINT64 *)(UINTN)0x1118061C8ULL;
        *Flag = 0;
        AsmReadIdtr (&Idtr);
        G = (IA32_IDT_GATE_DESCRIPTOR *)Idtr.Base + 0x21;
        Ps5ProbeSetGate (G, H);   /* CR0.WP off + readback */
        S = gBS->LocateProtocol (&gEfiMpServiceProtocolGuid, NULL, (VOID **)&Mp);
        if (!EFI_ERROR (S)) S = gBS->CreateEvent (0, 0, NULL, NULL, &Ev);
        if (!EFI_ERROR (S)) S = Mp->StartupThisAP (Mp, Ps5Probe79Ap, 1, Ev, 0, NULL, NULL);
        __asm__ __volatile__ ("cli");                          /* the BSP takes nothing itself */
        T0 = AsmReadTsc ();
        while (*Flag != 0x544E495041575350ULL && AsmReadTsc () - T0 < 5ULL * 1596300000ULL) CpuPause ();
        if (*Flag == 0x544E495041575350ULL) Ps5WarmProbeOff ();   /* AP accepted: off at once */
        DEBUG ((DEBUG_ERROR, "PS5 probe 79: AP 1 did not report (StartupThisAP %r)\n", S));
        T0 = AsmReadTsc ();
        while (AsmReadTsc () - T0 < 25ULL * 1596300000ULL) CpuPause ();
        Ps5WarmProbeOff ();
      }
#endif
#if defined (PS5_WARM_PROBE) && PS5_WARM_PROBE == 69
      __asm__ __volatile__ ("int $0x20");   /* lab: software delivery of 0x20 - the LAPIC timer handler (probe 68 part) switches off */
#endif
      if (*(volatile UINT64 *)(UINTN)0x111806040ULL == 0x314D524157575350ULL) {
        /* r5.25.5 (HW 19.11. lab66: alive for 60 s with the filter closed, stall as soon as it opens): after a warm
         * re-entry only what UEFI needs is opened - the timer vector 0x20 (C0-C4 stay as handed over). Everything else
         * opens at ExitBootServices for the OS. */
        mPs5IerWarm = TRUE;
#ifdef PS5_WARM_TIMER_IER
        PS5_XAPIC (Base, 0x480 + 0x10 * 1) |= 0x1U;   /* lab: timer vector only */
#else
        /* r5.25.5 (HW 19.11.: even the timer vector alone stops the platform after a restart from Windows): the filter
         * stays closed - the direct boot of the last device needs no interrupt (Stall is TSC based, storage polls);
         * ExitBootServices opens it for the OS */
        DEBUG ((DEBUG_ERROR, "PS5 ext-APIC BSP: warm re-entry - filter left closed until ExitBootServices\n"));
#endif
      } else {
        for (UINTN Index = 1; Index < 8; Index++) {
          PS5_XAPIC (Base, 0x480 + 0x10 * Index) = 0xFFFFFFFFU;
        }
      }
#if defined (PS5_WARM_WATCHDOG)
      Ps5WarmProgress (43);
#endif
      Ps5ExtApicArmIerAtEbs (Base);
#if defined (PS5_WARM_PROBE) && PS5_WARM_PROBE == 62
      { EFI_EVENT T; BOOLEAN Fired = FALSE;                     /* lab: a 100 ms timer event must fire within 2 s */
        if (!EFI_ERROR (gBS->CreateEvent (EVT_TIMER, TPL_CALLBACK, NULL, NULL, &T)) &&
            !EFI_ERROR (gBS->SetTimer (T, TimerRelative, 1000000))) {
          for (UINTN I = 0; I < 2000 && !Fired; I++) { gBS->Stall (1000); Fired = !EFI_ERROR (gBS->CheckEvent (T)); }
        }
        if (!Fired) Ps5WarmProbeOff ();                         /* off at once = the DXE timer does not tick */
      }
#endif
#if defined (PS5_WARM_PROBE) && PS5_WARM_PROBE == 58
      Ps5WarmProbeOff ();   /* lab: IER opened, TPR/EOI not yet written */
#endif
#if defined (PS5_WARM_PROBE) && PS5_WARM_PROBE == 71
      if (*(volatile UINT64 *)(UINTN)0x111806040ULL == 0x314D524157575350ULL) Ps5WarmLoadBlink ();   /* lab: load square wave */
#endif
      PS5_XAPIC (Base, 0x80) = 0;
      PS5_XAPIC (Base, 0xB0) = 0;
#if defined (PS5_WARM_PROBE) && PS5_WARM_PROBE == 59
      Ps5WarmProbeOff ();   /* lab: TPR 0 and EOI written */
#endif
      mPs5ExtApic.IerC0After = PS5_XAPIC (Base, 0x480 + 0x10 * 6);
      mPs5ExtApic.Ier20After = PS5_XAPIC (Base, 0x480 + 0x10 * 1);
      mPs5ExtApic.Applied = 1;
      DEBUG ((DEBUG_ERROR, "PS5 ext-APIC BSP opened: EFEAT=%x ECTRL=%x IER[C0]=%x->%x IER[20]=%x (filter opened)\n",
              mPs5ExtApic.Efeat, mPs5ExtApic.Ectrl, mPs5ExtApic.IerC0Before, mPs5ExtApic.IerC0After, mPs5ExtApic.Ier20After));
    }
  }
#if defined (PS5_WARM_PROBE) && PS5_WARM_PROBE == 50
  Ps5WarmProbeOff ();   /* lab: BSP filter opened (every vector reaches the BSP from here on) */
#endif

#if defined (PS5_WARM_WATCHDOG_EARLY)
  (VOID)Ps5ExtApicApWorker;   /* lab: AP part skipped below */
#endif
#if !defined (PS5_WARM_WATCHDOG_EARLY)   /* lab: the watchdog AP is busy - StartupAllAPs would return NOT_READY */
  /* 2. Multi-Core AP measurement & opening (unconditional, independent of BSP state) */
  EFI_MP_SERVICES_PROTOCOL *MpService = NULL;
  EFI_STATUS Status = gBS->LocateProtocol (&gEfiMpServiceProtocolGuid, NULL, (VOID **)&MpService);
  if (!EFI_ERROR (Status) && MpService != NULL) {
    UINTN NumCpus = 0, NumEnabled = 0;
    UINTN *FailedList = NULL;
    MpService->GetNumberOfProcessors (MpService, &NumCpus, &NumEnabled);
    DEBUG ((DEBUG_ERROR, "PS5 ext-APIC: MP Services located, %u CPUs total (%u enabled)\n",
            (UINT32)NumCpus, (UINT32)NumEnabled));

    if (NumCpus > 1) {
      /* Timeout: 2,000,000 microseconds (2 seconds). SingleThread=TRUE for sequential execution. */
      EFI_STATUS MpRun = MpService->StartupAllAPs (MpService, Ps5ExtApicApWorker, TRUE, NULL, 2000000, (VOID *)MpService, &FailedList);
      DEBUG ((DEBUG_ERROR, "PS5 ext-APIC: StartupAllAPs returned %r (FailedList=%p)\n", MpRun, FailedList));
#if defined (PS5_WARM_PROBE) && PS5_WARM_PROBE == 51
      Ps5WarmProbeOff ();   /* lab: AP filters opened */
#endif

      /* BSP logs the results for each AP */
      for (UINTN I = 1; I < NumCpus && I < 16; I++) {
        DEBUG ((DEBUG_ERROR, "PS5 ext-APIC AP %u: state=%u EFEAT=%x ECTRL=%x IER[C0]=%x->%x IER[20]=%x\n",
                (UINT32)I, mPs5ApExtApic[I].State, mPs5ApExtApic[I].Efeat, mPs5ApExtApic[I].Ectrl,
                mPs5ApExtApic[I].IerC0Before, mPs5ApExtApic[I].IerC0After, mPs5ApExtApic[I].Ier20After));
      }
#if defined (PS5_WARM_WATCHDOG)
      if (*(volatile UINT64 *)(UINTN)0x111806040ULL == 0x314D524157575350ULL) {   /* lab: warm re-entry only */
        EFI_EVENT Done = NULL;   /* non-blocking: StartupThisAP needs a WaitEvent */
        EFI_STATUS Ws = gBS->CreateEvent (0, 0, NULL, NULL, &Done);
        Ps5WarmArm ();
        if (!EFI_ERROR (Ws)) Ws = MpService->StartupThisAP (MpService, Ps5WarmWatchdogAp, NumCpus - 1, Done, 0, NULL, NULL);
        DEBUG ((DEBUG_ERROR, "PS5 warm watchdog: AP %u %r\n", (UINT32)(NumCpus - 1), Ws));
      }
#endif
    }
  } else {
    DEBUG ((DEBUG_WARN, "PS5 ext-APIC: MP Services protocol locate failed: %r\n", Status));
  }
#endif
#if defined (PS5_WARM_WATCHDOG)
  Ps5WarmProgress (44);
#endif
#if defined (PS5_WARM_WATCHDOG_SELFTEST)
  for (;;) { DisableInterrupts (); __asm__ __volatile__ ("hlt"); }   /* lab: BSP stops at point 44 - the watchdog must switch off ~110 s later */
#endif
#if defined (PS5_WARM_PROBE) && PS5_WARM_PROBE == 44
  Ps5WarmProbeOff ();   /* lab: ext-APIC opened on the BSP and all APs */
#endif
}
#endif
