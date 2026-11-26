import os
"""Execute the real service wrappers: map-key ordering and exact forwarding."""
import subprocess,tempfile,unittest
from pathlib import Path
WORK = Path(os.environ.get('PSW_WORK', '/opt/pswloader-work'))   # build tree (EDK2, testdata); set PSW_WORK for your own
HERE=Path(__file__).parent.resolve()
EDK=(WORK / 'edk2')
class PostWimTests(unittest.TestCase):
 def test_snapshot_precedes_memory_map_and_wrappers_preserve_status_and_bounds(self):
  code=r'''
#include <Uefi.h>
#include <Guid/EventGroup.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
static EFI_BOOT_SERVICES Bs;EFI_BOOT_SERVICES *gBS=&Bs;
EFI_GUID gEfiEventExitBootServicesGuid={1};
static UINTN Generation,Maps,Allocs,Snapshots,Events,Closed,Crcs;
static EFI_TPL Tpl;
static EFI_STATUS Result;
static BOOLEAN ValidMap;
static UINT8 KernelBuffer[3200*4096];
static void ConsoleSnapshot(void);
#include "ps5-post-wim.h"
UINT64 EFIAPI AsmReadTsc(VOID){return 123;}
static EFI_TPL EFIAPI Raise(EFI_TPL t){EFI_TPL old=Tpl;Tpl=t;return old;}
static void EFIAPI Restore(EFI_TPL t){Tpl=t;}
static EFI_STATUS EFIAPI Map(UINTN *n,EFI_MEMORY_DESCRIPTOR *m,UINTN *k,UINTN *s,UINT32 *v){
 Maps++;if(n)*n=ValidMap && m!=NULL?2*sizeof(EFI_MEMORY_DESCRIPTOR):123;if(k)*k=Generation;if(s)*s=ValidMap && m!=NULL?sizeof(EFI_MEMORY_DESCRIPTOR):48;if(v)*v=1;return Result;
}
static EFI_STATUS EFIAPI Alloc(EFI_ALLOCATE_TYPE t,EFI_MEMORY_TYPE mt,UINTN n,EFI_PHYSICAL_ADDRESS *a){
 assert(t==AllocateAnyPages && mt==EfiLoaderData && (n==7 || n==3200 || n==5200));Allocs++;Generation++;if(a)*a=n==3200?(UINTN)KernelBuffer:0x12345000;return Result;
}
static EFI_STATUS EFIAPI Free(EFI_PHYSICAL_ADDRESS a,UINTN n){assert(mKernelCandidate==0);return Result;}
static void ConsoleSnapshot(void){
 Snapshots++;Generation++;
 UINTN n=0,key=0,s=0;UINT32 v=0;
 // The filesystem export can reenter GetMemoryMap. Never recursively export.
 assert(gBS->GetMemoryMap(&n,NULL,&key,&s,&v)==Result);
}
static EFI_STATUS EFIAPI Create(UINT32 t,EFI_TPL p,EFI_EVENT_NOTIFY f,CONST VOID *c,CONST EFI_GUID *g,EFI_EVENT *e){
 Events++;*e=(EFI_EVENT)Events;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Close(EFI_EVENT e){Closed++;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Crc(VOID *p,UINTN n,UINT32 *c){assert(p==gBS && n==sizeof(Bs));*c=42;Crcs++;return EFI_SUCCESS;}
int main(int argc,char **argv){
 assert(KernelCrc32((CONST UINT8 *)"123456789",9)==0xcbf43926U);
 EFI_MEMORY_DESCRIPTOR descriptors[2]={{0}};
 descriptors[0].Type=EfiConventionalMemory;descriptors[0].PhysicalStart=0x100000;
 descriptors[0].NumberOfPages=32;descriptors[1].Type=EfiReservedMemoryType;
 assert(PostWimDumpMap(sizeof(descriptors),descriptors,sizeof(descriptors[0])));
 assert(descriptors[0].NumberOfPages==32 && descriptors[1].Type==EfiReservedMemoryType);
 assert(!PostWimDumpMap(sizeof(descriptors)-1,descriptors,sizeof(descriptors[0])));
 assert(!PostWimDumpMap(sizeof(descriptors),NULL,sizeof(descriptors[0])));
 assert(!PostWimDumpMap(sizeof(descriptors),descriptors,1));
 assert(!PostWimDumpMap(129*sizeof(descriptors[0]),descriptors,sizeof(descriptors[0])));
 Bs.Hdr.HeaderSize=sizeof(Bs);Bs.GetMemoryMap=Map;Bs.AllocatePages=Alloc;Bs.FreePages=Free;Bs.CalculateCrc32=Crc;
 Bs.CreateEventEx=Create;Bs.CloseEvent=Close;Bs.RaiseTPL=Raise;Bs.RestoreTPL=Restore;Tpl=TPL_APPLICATION;
 assert(InstallPostWimTap() && Events==2 && Crcs==1 && Bs.Hdr.CRC32==42);
 UINTN n=0,key=0,size=0;UINT32 version=0;
 assert(Bs.GetMemoryMap(&n,NULL,&key,&size,&version)==0 && Maps==1 && Snapshots==0);
 WimEof(NULL,NULL);assert(Bs.GetMemoryMap(&n,NULL,&key,&size,&version)==0);
 assert(Snapshots==1 && Maps==3 && key==Generation && n==123 && size==48 && version==1);
 Result=EFI_OUT_OF_RESOURCES;EFI_PHYSICAL_ADDRESS address;
 assert(Bs.AllocatePages(AllocateAnyPages,EfiLoaderData,7,&address)==Result && address==0x12345000 && Allocs==1 && Snapshots==3);
 Tpl=TPL_NOTIFY;assert(Bs.GetMemoryMap(&n,NULL,&key,&size,&version)==Result && Snapshots==3 && key==Generation);
 Tpl=TPL_APPLICATION;for(int i=0;i<20;i++)assert(Bs.GetMemoryMap(&n,NULL,&key,&size,&version)==Result && key==Generation);
 assert(Snapshots==8 && mPostWimMaps==6 && mPostWimAllocations==1 && mPostWimRequests==7);
 Result=EFI_SUCCESS;for(int i=0;i<20;i++)assert(Bs.AllocatePages(AllocateAnyPages,EfiLoaderData,7,&address)==0);
 assert(Snapshots==13 && mPostWimAllocations==6 && mPostWimMaps==6);
 assert(Bs.AllocatePages(AllocateAnyPages,EfiLoaderData,3200,&address)==0 && Snapshots==14);
 assert(Bs.AllocatePages(AllocateAnyPages,EfiLoaderData,5200,&address)==0 && Snapshots==15);
 Result=EFI_OUT_OF_RESOURCES;
 for(int i=0;i<20;i++)assert(Bs.AllocatePages(AllocateAnyPages,EfiLoaderData,7,&address)==Result);
 assert(Snapshots==17 && mPostWimSpecial==4);
 Result=EFI_SUCCESS;
 mPostWimRequests=0;PostWimExit(NULL,NULL);Bs.GetMemoryMap(&n,NULL,&key,&size,&version);assert(Snapshots==17);
 RestorePostWimTap(TRUE);assert(Bs.GetMemoryMap==Map && Bs.AllocatePages==Alloc && Closed==2 && Crcs==2);
 // Actual v165 failure order: allocations exhaust first, map must still export.
 assert(InstallPostWimTap());WimEof(NULL,NULL);Snapshots=0;
 for(int i=0;i<20;i++)assert(Bs.AllocatePages(AllocateAnyPages,EfiLoaderData,7,&address)==0);
 assert(Snapshots==6 && mPostWimAllocations==6 && mPostWimMaps==0);
 assert(Bs.GetMemoryMap(&n,NULL,&key,&size,&version)==0 && key==Generation);
 assert(Snapshots==7 && mPostWimMaps==1);RestorePostWimTap(TRUE);
 assert(InstallPostWimTap());WimEof(NULL,NULL);ValidMap=TRUE;
 assert(Bs.GetMemoryMap(&n,descriptors,&key,&size,&version)==0);
 assert(mPostWimMapDumped && key==Generation && descriptors[0].NumberOfPages==32);
 RestorePostWimTap(TRUE);
 assert(InstallPostWimTap());WimEof(NULL,NULL);
 assert(Bs.AllocatePages(AllocateAnyPages,EfiLoaderData,3200,&address)==0);
 assert(mKernelCandidate==(UINTN)KernelBuffer);KernelCandidateProbe();assert(!mKernelVerified);
 if(argc==2) {
  FILE *f=fopen(argv[1],"rb");assert(f);assert(fread(KernelBuffer,1,13104632,f)==13104632);fclose(f);
  assert(Bs.GetMemoryMap(&n,descriptors,&key,&size,&version)==0 && key==Generation);
  assert(mKernelVerified && mKernelChecks==1);
  assert(mPostWimMaps==1 && mPostWimMapCalls==1 && mPostWimAllocations==0);
  assert(Bs.AllocatePages(AllocateAnyPages,EfiLoaderData,7,&address)==0 && mPostWimAllocations==1);
  KernelCandidateProbe();assert(mPostWimAllocations==1 && mKernelChecks==1);
 }
 Result=EFI_DEVICE_ERROR;
 assert(Bs.FreePages((UINTN)KernelBuffer+4096,1)==Result && mKernelCandidate==0);
 // Even a failed partial free invalidates the candidate conservatively.
 mKernelVerified=FALSE;KernelCandidateProbe();assert(!mKernelVerified);
 RestorePostWimTap(TRUE);assert(Bs.FreePages==Free);
}
'''
  with tempfile.TemporaryDirectory() as temp:
   p=Path(temp);(p/'main.c').write_text(code)
   subprocess.run(['gcc','-fshort-wchar','-DMDEPKG_NDEBUG','-I'+str(EDK/'MdePkg/Include'),'-I'+str(EDK/'MdePkg/Include/X64'),'-I'+str(HERE),str(p/'main.c'),'-o',str(p/'test')],check=True)
   reference=(WORK / 'ntoskrnl-from-v151.exe')
   subprocess.run([str(p/'test')]+([str(reference)] if reference.exists() else []),check=True)
