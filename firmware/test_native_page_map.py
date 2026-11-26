import os
from pathlib import Path
import subprocess,tempfile,unittest
R=Path(__file__).parent.resolve();EDK=Path(os.environ.get('PS5_EDK2_ROOT',os.environ.get('PSW_WORK', '/opt/pswloader-work') + '/edk2'))
@unittest.skipUnless((EDK/'MdePkg/Include/Base.h').exists(),'EDK headers required')
class NativePageTests(unittest.TestCase):
 def test_table_reads_require_ram_and_firmware_wrapper_handles_errors(self):
  with tempfile.TemporaryDirectory() as work:
   p=Path(work);(p/'test.c').write_text(r'''
#define _GNU_SOURCE
#include <stdlib.h>
#include <assert.h>
#include <sys/mman.h>
#undef NULL
#include "ps5-native-page-map.h"
static EFI_BOOT_SERVICES Bs;EFI_BOOT_SERVICES *gBS=&Bs;
static UINTN Cr4,Cr3;static unsigned Freed,Walks,Mode;static EFI_MEMORY_DESCRIPTOR Descriptor;
UINTN EFIAPI AsmReadCr4(VOID){return Cr4;}
UINTN EFIAPI AsmReadCr3(VOID){return Cr3;}
VOID *EFIAPI AllocatePool(UINTN N){return Mode==3?NULL:malloc(N);}
VOID EFIAPI FreePool(VOID *P){Freed++;free(P);}
static EFI_STATUS EFIAPI Get(UINTN*N,EFI_MEMORY_DESCRIPTOR*M,UINTN*K,UINTN*S,UINT32*V){
 *S=sizeof(*M);*V=Mode==2?2:1;*K=1;
 if(!M || Mode==4){*N=sizeof(*M);return EFI_BUFFER_TOO_SMALL;}
 if(Mode==5)return EFI_DEVICE_ERROR;
 *N=sizeof(*M);*M=Descriptor;return EFI_SUCCESS;
}
int ps5_framebuffer_identity_writable(uint64_t cr3,uint64_t addr,uint64_t size,ps5_page_entry_read r,void*c){
 assert(cr3==Cr3&&addr==0x46f810000ULL&&size==8294400);Walks++;
 uint64_t value=0;return r(c,Cr3,&value) || value!=0x12345678;
}
int main(void){
 UINT64 *Table=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
 assert(Table!=MAP_FAILED);*Table=0x12345678;Cr3=(UINTN)Table;
 Descriptor=(EFI_MEMORY_DESCRIPTOR){.Type=EfiBootServicesData,.PhysicalStart=Cr3,.NumberOfPages=1};
 Bs.GetMemoryMap=Get;struct ps5_page_map_context C={&Descriptor,sizeof(Descriptor),sizeof(Descriptor)};
 uint64_t v=0;assert(!Ps5ReadPageEntry(&C,Cr3,&v)&&v==*Table);
 Descriptor.Type=EfiReservedMemoryType;v=9;assert(Ps5ReadPageEntry(&C,Cr3,&v)==-1&&v==9);
 Descriptor.Type=EfiMemoryMappedIO;assert(Ps5ReadPageEntry(&C,Cr3,&v)==-1);
 Descriptor.Type=EfiBootServicesData;
 assert(Ps5ReadPageEntry(&C,Cr3+1,&v)==-1);assert(Ps5ReadPageEntry(&C,0x450000000ULL,&v)==-1);
 assert(Ps5ReadPageEntry(&C,0,&v)==-1);assert(Ps5ReadPageEntry(&C,Cr3+4096,&v)==-1);
 for(Mode=0;Mode<=5;Mode++){
  Freed=Walks=0;Cr4=Mode==1?BIT12:0;
  EFI_STATUS S=Ps5NativeFramebufferPageCheck(0x46f810000ULL,8294400);
  if(Mode==0)assert(S==EFI_SUCCESS&&Walks==1&&Freed==1);
  else {assert(EFI_ERROR(S)&&Walks==0);if(Mode==4)assert(Freed==3);}
 }
 munmap(Table,4096);return 0;
}''')
   subprocess.run(['gcc','-Wall','-Wextra','-Werror','-Wno-unused-function','-fshort-wchar','-I'+str(R),'-I'+str(EDK/'MdePkg/Include'),'-I'+str(EDK/'MdePkg/Include/X64'),str(p/'test.c'),'-o',str(p/'test')],check=True)
   subprocess.run([str(p/'test')],check=True)
