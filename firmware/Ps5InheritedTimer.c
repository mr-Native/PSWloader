/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Drain the measured inherited timer vector before DXE first enables IF.
 * This is not a general unexpected-IRQ handler. UEFI owns vector 32 later.
 */
#include "CpuDxe.h"
#include <Library/SynchronizationLib.h>

#define PS5_INHERITED_TIMER_VECTOR 0xef
#define PS5_INHERITED_TIMER_BUDGET 8
STATIC volatile UINT32 mPs5InheritedTimerCount;

VOID EFIAPI CommonExceptionHandler (
  IN EFI_EXCEPTION_TYPE Type, IN EFI_SYSTEM_CONTEXT Context);

STATIC VOID EFIAPI Ps5InheritedTimerInterrupt (
  IN EFI_EXCEPTION_TYPE Type, IN EFI_SYSTEM_CONTEXT Context)
{
  UINT8 Vector;
  if (InterlockedIncrement (&mPs5InheritedTimerCount) > PS5_INHERITED_TIMER_BUDGET) {
    // Restore the normal fatal path, including optional exact-vector telemetry.
    RegisterCpuInterruptHandler (PS5_INHERITED_TIMER_VECTOR, NULL);
    CommonExceptionHandler (Type, Context);
    CpuDeadLoop ();
  }
  GetApicTimerState (NULL, NULL, &Vector);
  if (Vector == PS5_INHERITED_TIMER_VECTOR) {
    DisableApicTimerInterrupt ();
  }
  // Masking the LVT does not consume an already delivered pending interrupt.
  // Never mask the new vector after LocalApicTimerDxe has reprogrammed it.
  SendApicEoi ();
}

EFI_STATUS Ps5PrepareInheritedTimer (VOID)
{
  UINT8 Vector;
  GetApicTimerState (NULL, NULL, &Vector);
  if (Vector != PS5_INHERITED_TIMER_VECTOR) {
    return EFI_SUCCESS;
  }
  DisableApicTimerInterrupt ();
  // Do not replace an existing handler or claim an unverified device vector.
  return RegisterCpuInterruptHandler (Vector, Ps5InheritedTimerInterrupt);
}
