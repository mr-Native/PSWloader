/** @file
  TimerLib for the measured retail PS5 platform, not a generic AMD timer.
  The production PS5 kernel uses the same 1,596,300,000 Hz TSC calibration.
  Avoids CPUID.15 (not established on this CPU) and the unusable Sony PM timer.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <Base.h>
#include <Library/BaseLib.h>
#include <Library/TimerLib.h>

#define PS5_TSC_HZ 1596300000U

STATIC VOID
DelayTicks (UINT64 Ticks)
{
  UINT64 Start;
  Start = AsmReadTsc ();
  while ((UINT64)(AsmReadTsc () - Start) < Ticks) {
    CpuPause ();
  }
}

UINTN EFIAPI
MicroSecondDelay (UINTN MicroSeconds)
{
  UINTN Remaining;
  UINT32 Chunk;
  Remaining = MicroSeconds;
  while (Remaining != 0) {
    // Bound multiplication and the wrap-safe interval to at most one second.
    Chunk = (UINT32)((Remaining > 1000000) ? 1000000 : Remaining);
    DelayTicks (DivU64x32 (MultU64x32 (Chunk, PS5_TSC_HZ) + 999999, 1000000));
    Remaining -= Chunk;
  }
  return MicroSeconds;
}

UINTN EFIAPI
NanoSecondDelay (UINTN NanoSeconds)
{
  UINTN Remaining;
  UINT32 Chunk;
  Remaining = NanoSeconds;
  while (Remaining != 0) {
    Chunk = (UINT32)((Remaining > 1000000000) ? 1000000000 : Remaining);
    DelayTicks (DivU64x32 (MultU64x32 (Chunk, PS5_TSC_HZ) + 999999999, 1000000000));
    Remaining -= Chunk;
  }
  return NanoSeconds;
}

UINT64 EFIAPI
GetPerformanceCounter (VOID)
{
  return AsmReadTsc ();
}

UINT64 EFIAPI
GetPerformanceCounterProperties (OUT UINT64 *StartValue OPTIONAL, OUT UINT64 *EndValue OPTIONAL)
{
  if (StartValue != NULL) { *StartValue = 0; }
  if (EndValue != NULL) { *EndValue = MAX_UINT64; }
  return PS5_TSC_HZ;
}

UINT64 EFIAPI
GetTimeInNanoSecond (IN UINT64 Ticks)
{
  UINT32 Remainder;
  UINT64 Seconds;
  Seconds = DivU64x32Remainder (Ticks, PS5_TSC_HZ, &Remainder);
  return MultU64x32 (Seconds, 1000000000) +
         DivU64x32 (MultU64x32 (Remainder, 1000000000), PS5_TSC_HZ);
}
