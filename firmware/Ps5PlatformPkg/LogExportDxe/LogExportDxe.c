/** SPDX-License-Identifier: BSD-2-Clause-Patent
 * Early filesystem-availability and ReadyToBoot snapshots to every volume with a prepared \PS5UEFI.LOG slot
 * (r5.25.4; before: the pinned Windows ESP only). No filesystem/file creation, partition writes, or runtime callbacks.
 */
#include <PiDxe.h>
#include <Guid/EventGroup.h>
#include <Guid/FileInfo.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/PartitionInfo.h>
#include <Protocol/SimpleFileSystem.h>
#ifdef PS5_WARM_RESCUE
#include "ps5-warm-probe.h"
#endif
#ifndef PS5_WARM_LOGCELL
#define PS5_WARM_LOGCELL 0x111806380ULL   /* r5.25.11: set by Ps5WarmReadRecord on a warm run (host tests: a variable) */
#endif
STATIC CONST UINT8 mMarker[16]="PS5LOGFILE1";
#define RING_BYTES 65552U
STATIC UINT64 mSnapshot[RING_BYTES / sizeof(UINT64)];
STATIC BOOLEAN mDone;
STATIC BOOLEAN mEarlyDone;
STATIC BOOLEAN mBusy;
STATIC UINTN mReadySnapshots;
STATIC UINTN mBootSnapshots;
STATIC EFI_GUID mPs5BootSnapshotGuid={0x873f7798,0xe801,0x4ec4,{0x86,0x97,0x30,0xb7,0xb3,0xa1,0xfa,0x52}};
STATIC VOID *mFsRegistration;
STATIC VOID *mPartitionRegistration;
STATIC EFI_STATUS Export(EFI_HANDLE Handle,CONST CHAR16 *Name) {
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;
  EFI_FILE_PROTOCOL *Root=NULL,*File=NULL;
  EFI_FILE_INFO *Info=NULL;
  /* r5.25.4 (18.11.): any volume with a prepared slot file - \PS5UEFI.LOG of exactly 16+65552 bytes starting with the
   * marker (checked below; no file is ever created) - gets the snapshot, e.g. the setup stick, which GameOS can read over
   * FTP. Before, only the two lab ESPs (fixed partition GUIDs) were written. */
  EFI_STATUS Status=gBS->HandleProtocol(Handle,&gEfiSimpleFileSystemProtocolGuid,(VOID **)&Fs);
  if(EFI_ERROR(Status)) return Status;
  Status=Fs->OpenVolume(Fs,&Root);
  if(EFI_ERROR(Status)) return Status;
  Status=Root->Open(Root,&File,(CHAR16 *)Name,EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE,0);
  if(EFI_ERROR(Status)) goto Done;
  UINTN Size=0;
  Status=File->GetInfo(File,&gEfiFileInfoGuid,&Size,NULL);
  if(Status!=EFI_BUFFER_TOO_SMALL || Size< SIZE_OF_EFI_FILE_INFO || Size>4096) { Status=EFI_COMPROMISED_DATA;goto Done; }
  Info=AllocatePool(Size);
  if(Info==NULL) { Status=EFI_OUT_OF_RESOURCES;goto Done; }
  Status=File->GetInfo(File,&gEfiFileInfoGuid,&Size,Info);
  if(EFI_ERROR(Status)) goto Done;
  if(Info->FileSize!=sizeof(mMarker)+RING_BYTES || (Info->Attribute&(EFI_FILE_DIRECTORY|EFI_FILE_READ_ONLY))) {
    Status=EFI_COMPROMISED_DATA;goto Done;
  }
  UINT8 Marker[16];Size=sizeof(Marker);
  Status=File->Read(File,&Size,Marker);
  if(EFI_ERROR(Status)) goto Done;
  if(Size!=sizeof(Marker) || CompareMem(Marker,mMarker,sizeof(Marker))) { Status=EFI_COMPROMISED_DATA;goto Done; }
  // Snapshot before filesystem I/O can emit more serial diagnostics.
  BOOLEAN Interrupts=SaveAndDisableInterrupts();
#ifdef PS5_WARM_RESCUE
  if(Ps5WarmRescueRun() && *(volatile UINT64 *)(UINTN)PS5_WARM_RESCUE_AREA==PS5_WARM_RESCUE_MAGIC)
    CopyMem(mSnapshot,(VOID *)(UINTN)(PS5_WARM_RESCUE_AREA+16),RING_BYTES);   /* lab RUN 2: the log of the hung run */
  else
#endif
  CopyMem(mSnapshot,(VOID *)(UINTN)0x111001000ULL,RING_BYTES);
  SetInterruptState(Interrupts);
  if(mSnapshot[0]!=0x31474F4C355350ULL) { Status=EFI_NOT_READY;goto Done; }
  Status=File->SetPosition(File,sizeof(mMarker));
  if(EFI_ERROR(Status)) goto Done;
  Size=RING_BYTES;
  Status=File->Write(File,&Size,mSnapshot);
  if(!EFI_ERROR(Status) && Size!=RING_BYTES) Status=EFI_DEVICE_ERROR;
  if(!EFI_ERROR(Status)) Status=File->Flush(File);
Done:
  if(Info!=NULL) FreePool(Info);
  if(File!=NULL) File->Close(File);
  Root->Close(Root);
  return Status;
}
STATIC VOID Snapshot(BOOLEAN Early) {
  if(mBusy || mDone || (Early && mEarlyDone)) return;
  mBusy=TRUE;
  EFI_HANDLE *Handles=NULL;
  UINTN Count=0;
  EFI_STATUS Status=gBS->LocateHandleBuffer(ByProtocol,&gEfiSimpleFileSystemProtocolGuid,NULL,&Count,&Handles);
  /* lab rescue (22.11.): the log of the hung run goes to its own slot \PS5RESCU.LOG, so the next normal start (which
   * snapshots into \PS5UEFI.LOG) does not overwrite it before it is read */
  CONST CHAR16 *Name=L"\\PS5UEFI.LOG";
  /* r5.25.11: after a warm re-entry (cell set by Ps5WarmReadRecord) into \PS5WARM.LOG if a volume has that slot, so a
   * hung restart's log survives the next normal start */
  BOOLEAN Warm=*(volatile UINT64 *)(UINTN)PS5_WARM_LOGCELL==0x31474F4C4D524157ULL;
  if(Warm) Name=L"\\PS5WARM.LOG";
#ifdef PS5_WARM_RESCUE
  if(Ps5WarmRescueRun() && *(volatile UINT64 *)(UINTN)PS5_WARM_RESCUE_AREA==PS5_WARM_RESCUE_MAGIC) Name=L"\\PS5RESCU.LOG";
#endif
  if(!EFI_ERROR(Status)) {
    Status=EFI_NOT_FOUND;
    for(UINTN Index=0;Index<Count;Index++) {   /* r5.25.4: every volume with a valid slot; a failing volume (old slot of
                                                  * another size, write-protected) no longer ends the search */
      EFI_STATUS Attempt=Export(Handles[Index],Name);
      if(!EFI_ERROR(Attempt)) Status=EFI_SUCCESS;
      else if(Attempt!=EFI_NOT_FOUND && Status==EFI_NOT_FOUND) Status=Attempt;
    }
    if(Warm && Status==EFI_NOT_FOUND) {   /* no \PS5WARM.LOG slot anywhere: the normal slot as before */
      Name=L"\\PS5UEFI.LOG";
      for(UINTN Index=0;Index<Count;Index++) {
        EFI_STATUS Attempt=Export(Handles[Index],Name);
        if(!EFI_ERROR(Attempt)) Status=EFI_SUCCESS;
        else if(Attempt!=EFI_NOT_FOUND && Status==EFI_NOT_FOUND) Status=Attempt;
      }
    }
    FreePool(Handles);
  }
  if(!EFI_ERROR(Status)) {
    if(Early) mEarlyDone=TRUE;
    else mDone=TRUE;
  }
#ifdef PS5_WARM_RESCUE
  if(!EFI_ERROR(Status) && Ps5WarmRescueRun() && *(volatile UINT64 *)(UINTN)PS5_WARM_RESCUE_AREA==PS5_WARM_RESCUE_MAGIC) {
    DEBUG((DEBUG_ERROR,"PS5 warm rescue: log of the hung run (last point %Lu) exported - switching off\n",
           *(volatile UINT64 *)(UINTN)(PS5_WARM_RESCUE_AREA+8)));
    *(volatile UINT64 *)(UINTN)PS5_WARM_RESCUE_AREA=0;
    *(volatile UINT64 *)(UINTN)0x111806040ULL=0x314D524157575350ULL;
    Ps5WarmProbeOff();
  }
#endif
  DEBUG((DEBUG_INFO,"PS5 log export early=%d: %r\n",Early,Status));
  mBusy=FALSE;
}
STATIC VOID EFIAPI Ready(EFI_EVENT Event,VOID *Context) {
  // A returning Windows boot manager is followed by another normal BDS boot
  // attempt. Preserve its error text on that second ReadyToBoot notification.
  if(mReadySnapshots>=2) return;
  mDone=FALSE;
  Snapshot(FALSE);
  if(mDone) mReadySnapshots++;
}
STATIC VOID EFIAPI BootMilestone(EFI_EVENT Event,VOID *Context) {
  // Synchronous BDS call sites only, before/after image services. No timer,
  // runtime or ExitBootServices callback; never recurse through file I/O.
  if(mBusy || mBootSnapshots>=300) return;
  mDone=FALSE;
  Snapshot(FALSE);
  if(mDone) mBootSnapshots++;
}
STATIC VOID EFIAPI Available(EFI_EVENT Event,VOID *Context) { Snapshot(TRUE); }
EFI_STATUS EFIAPI Ps5LogExportEntry(EFI_HANDLE ImageHandle,EFI_SYSTEM_TABLE *SystemTable) {
  EFI_EVENT ReadyEvent=NULL,FsEvent=NULL,PartitionEvent=NULL,BootEvent=NULL;
  EFI_STATUS Status=gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_CALLBACK,Ready,NULL,&gEfiEventReadyToBootGuid,&ReadyEvent);
  if(EFI_ERROR(Status)) return Status;
  Status=gBS->CreateEvent(EVT_NOTIFY_SIGNAL,TPL_CALLBACK,Available,NULL,&FsEvent);
  if(EFI_ERROR(Status)) goto Error;
  Status=gBS->CreateEvent(EVT_NOTIFY_SIGNAL,TPL_CALLBACK,Available,NULL,&PartitionEvent);
  if(EFI_ERROR(Status)) goto Error;
  // Either protocol may arrive first. Keep both notifications until unload is
  // impossible; a late filesystem must still get its first guarded snapshot.
  Status=gBS->RegisterProtocolNotify(&gEfiSimpleFileSystemProtocolGuid,FsEvent,&mFsRegistration);
  if(EFI_ERROR(Status)) goto Error;
  Status=gBS->RegisterProtocolNotify(&gEfiPartitionInfoProtocolGuid,PartitionEvent,&mPartitionRegistration);
  if(EFI_ERROR(Status)) goto Error;
  Status=gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_CALLBACK,BootMilestone,NULL,&mPs5BootSnapshotGuid,&BootEvent);
  if(EFI_ERROR(Status)) goto Error;
  // Cover protocols already present before registration. At most two successful
  // early snapshot plus up to two ReadyToBoot snapshots per boot.
  Snapshot(TRUE);
  return EFI_SUCCESS;
Error:
  if(BootEvent!=NULL) gBS->CloseEvent(BootEvent);
  if(PartitionEvent!=NULL) gBS->CloseEvent(PartitionEvent);
  if(FsEvent!=NULL) gBS->CloseEvent(FsEvent);
  gBS->CloseEvent(ReadyEvent);
  return Status;
}
