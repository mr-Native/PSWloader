/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Protocol/RealTimeClock.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include <Library/BaseLib.h>
#include <Library/SynchronizationLib.h>
#include "ps5-time-calendar.h"

/* Volatile software UTC clock, initialized from an explicit validated seed.
 * No persistence, no alarm or suspend-time promise, no PC CMOS accesses.
 * Runtime state contains no pointers and calls no boot services.
 */
#define PS5_TSC_HZ 1596300000ULL
/* v1.1 (2026-11-09): build-time default UTC (2026-11-09T00:00:00Z) so the software clock is
 * valid from DXE init even without an external PS5UTC.SEED. Windows corrects it via NTP after
 * boot; a valid seed refines it earlier. Makes native boot independent of the per-boot seed. */
#define PS5_DEFAULT_EPOCH 1794182400ULL
STATIC UINT64 mEpoch,mTicks;
STATIC UINT32 mNanos;
STATIC volatile UINT32 mClockLock;
STATIC BOOLEAN mClockValid;

EFI_STATUS EFIAPI
Ps5GetTime (OUT EFI_TIME *Time, OUT EFI_TIME_CAPABILITIES *Capabilities OPTIONAL)
{
  if(Time==NULL) return EFI_INVALID_PARAMETER;
  if(InterlockedCompareExchange32(&mClockLock,0,1)!=0) return EFI_NOT_READY;
  EFI_STATUS Status=EFI_NOT_READY;
  if(mClockValid) {
    UINT64 Now=AsmReadTsc();
    if(Now>=mTicks) {
      UINT64 Delta=Now-mTicks;
      UINT64 Nanos=(Delta%PS5_TSC_HZ)*1000000000ULL/PS5_TSC_HZ+mNanos;
      UINT64 Seconds=mEpoch+Delta/PS5_TSC_HZ+Nanos/1000000000ULL;
      if(Ps5EpochToTime(Seconds,(UINT32)(Nanos%1000000000ULL),Time)) {
        if(Capabilities!=NULL) {
          Capabilities->Resolution=1;Capabilities->Accuracy=10000;
          Capabilities->SetsToZero=FALSE;
        }
        Status=EFI_SUCCESS;
      } else Status=EFI_DEVICE_ERROR;
    } else Status=EFI_DEVICE_ERROR;
  }
  MemoryFence();mClockLock=0;return Status;
}

EFI_STATUS EFIAPI
Ps5SetTime (IN EFI_TIME *Time)
{
  UINT64 Seconds;
  if(!Ps5TimeToEpoch(Time,&Seconds)) return EFI_INVALID_PARAMETER;
  if(InterlockedCompareExchange32(&mClockLock,0,1)!=0) return EFI_NOT_READY;
  mTicks=AsmReadTsc();mEpoch=Seconds;mNanos=Time->Nanosecond;
  mClockValid=TRUE;MemoryFence();mClockLock=0;return EFI_SUCCESS;
}

EFI_STATUS EFIAPI
Ps5GetWakeupTime (OUT BOOLEAN *Enabled, OUT BOOLEAN *Pending, OUT EFI_TIME *Time)
{
  if (Enabled == NULL || Pending == NULL || Time == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  return EFI_UNSUPPORTED;
}

EFI_STATUS EFIAPI
Ps5SetWakeupTime (IN BOOLEAN Enable, IN EFI_TIME *Time OPTIONAL)
{
  return Enable && Time == NULL ? EFI_INVALID_PARAMETER : EFI_UNSUPPORTED;
}

EFI_STATUS EFIAPI
Ps5RtcInitialize (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_HANDLE Handle = NULL;
  EFI_STATUS Status;
  EFI_GET_TIME OldGetTime;
  EFI_SET_TIME OldSetTime;
  EFI_GET_WAKEUP_TIME OldGetWakeupTime;
  EFI_SET_WAKEUP_TIME OldSetWakeupTime;

  (VOID)ImageHandle;
  (VOID)SystemTable;
  OldGetTime = gRT->GetTime;
  OldSetTime = gRT->SetTime;
  OldGetWakeupTime = gRT->GetWakeupTime;
  OldSetWakeupTime = gRT->SetWakeupTime;
  /* Default-initialise the volatile software clock to the build-time epoch so GetTime returns a
   * valid UTC immediately, independent of any external seed (fix for all time). */
  mEpoch = PS5_DEFAULT_EPOCH; mNanos = 0; mTicks = AsmReadTsc(); mClockValid = TRUE;
  gRT->GetTime = Ps5GetTime;
  gRT->SetTime = Ps5SetTime;
  gRT->GetWakeupTime = Ps5GetWakeupTime;
  gRT->SetWakeupTime = Ps5SetWakeupTime;
  /* DXE core observes this architectural protocol and refreshes the runtime
   * service table CRC, just as for the standard PcRtc driver. */
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle, &gEfiRealTimeClockArchProtocolGuid, NULL, NULL);
  if (EFI_ERROR (Status)) {
    gRT->GetTime = OldGetTime;
    gRT->SetTime = OldSetTime;
    gRT->GetWakeupTime = OldGetWakeupTime;
    gRT->SetWakeupTime = OldSetWakeupTime;
  }
  return Status;
}
