"""Bounded synchronous FAT read milestones for the two Windows RAM-disk files."""
import subprocess
SOURCE='FatPkg/EnhancedFatDxe/ReadWrite.c'

def prepare_boot_reads(root):
    source=subprocess.check_output(['git','show','HEAD:'+SOURCE],cwd=root,text=True)
    anchor='  return FatIFileAccess (FHand, ReadData, BufferSize, Buffer, NULL);'
    if source.count(anchor)!=1:raise RuntimeError('Unexpected pinned FAT read anchor')
    helper='''
STATIC EFI_GUID mPs5ReadSnapshotGuid={0x873f7798,0xe801,0x4ec4,{0x86,0x97,0x30,0xb7,0xb3,0xa1,0xfa,0x52}};
STATIC EFI_GUID mPs5WimEofGuid={0x873f7798,0xe801,0x4ec4,{0x86,0x97,0x30,0xb7,0xb3,0xa1,0xfa,0x53}};
STATIC UINTN mPs5ReadSnapshots;
STATIC BOOLEAN mPs5ReadBusy;
'''
    source=source.replace('#include "Fat.h"','#include "Fat.h"\n'+helper)
    replacement='''  FAT_IFILE *IFile=IFILE_FROM_FHAND(FHand);
  FAT_OFILE *OFile=IFile->OFile;
  CONST CHAR16 *Name=(OFile!=NULL && OFile->DirEnt!=NULL)?OFile->DirEnt->FileString:NULL;
  BOOLEAN Selected=Name!=NULL && (StrCmp(Name,L"boot.wim")==0 || StrCmp(Name,L"boot.sdi")==0);
  UINT64 Before=IFile->Position;
  UINTN Requested=BufferSize!=NULL?*BufferSize:0;
  EFI_STATUS Status=FatIFileAccess(FHand,ReadData,BufferSize,Buffer,NULL);
  // FatIFileAccess has returned and released its filesystem lock. Snapshot
  // only synchronous reads, never from an async completion or held FAT lock.
  if(Selected && !mPs5ReadBusy && mPs5ReadSnapshots<8 && BufferSize!=NULL) {
    UINT64 After=IFile->Position;
    BOOLEAN Milestone=Before==0 || EFI_ERROR(Status) ||
      After/(256ULL*1024*1024)!=Before/(256ULL*1024*1024) ||
      (After>=OFile->FileSize && Before<OFile->FileSize);
    if(Milestone) {
      mPs5ReadBusy=TRUE;mPs5ReadSnapshots++;
      DEBUG((DEBUG_INFO,"PS5 boot file read %s: before=%Lu after=%Lu size=%Lu requested=%Lu returned=%Lu status=%r\\n",
        Name,Before,After,(UINT64)OFile->FileSize,(UINT64)Requested,(UINT64)*BufferSize,Status));
      EFI_TPL Previous=gBS->RaiseTPL(TPL_HIGH_LEVEL);gBS->RestoreTPL(Previous);
      if(Previous<=TPL_CALLBACK) {
        EfiEventGroupSignal(&mPs5ReadSnapshotGuid);
        // Arm the post-WIM window only after this FAT export has finished.
        // Its own USB/filesystem allocations must not consume that window.
        if(!EFI_ERROR(Status) && StrCmp(Name,L"boot.wim")==0 &&
           After>=OFile->FileSize && Before<OFile->FileSize)
          EfiEventGroupSignal(&mPs5WimEofGuid);
      }
      mPs5ReadBusy=FALSE;
    }
  }
  return Status;'''
    (root/SOURCE).write_text(source.replace(anchor,replacement))
