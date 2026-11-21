/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Optional read-only architecture-protocol snapshot at LogExport entry.
 * Presence does not assert CPU/MP health or that any hardware driver finished.
 */
#include <Uefi.h>
#include <Protocol/Cpu.h>
#include <Protocol/RealTimeClock.h>
#include <Protocol/Timer.h>
#include <Library/UefiBootServicesTableLib.h>
VOID ps5_power_report (unsigned Code);

VOID Ps5LogEntryProbe (VOID)
{
  VOID *Interface;
  unsigned Missing = 0;
  /* Architectural marker protocols (notably RTC) can have NULL interfaces.
   * Only LocateProtocol's status determines presence; never dereference it. */
  if (gBS->LocateProtocol (&gEfiCpuArchProtocolGuid, NULL, &Interface) != EFI_SUCCESS) {
    Missing |= 1;
  }
  if (gBS->LocateProtocol (&gEfiRealTimeClockArchProtocolGuid, NULL, &Interface) != EFI_SUCCESS) {
    Missing |= 2;
  }
  if (gBS->LocateProtocol (&gEfiTimerArchProtocolGuid, NULL, &Interface) != EFI_SUCCESS) {
    Missing |= 4;
  }
  ps5_power_report (Missing ? 0x710U | Missing : 0x708U);
}
