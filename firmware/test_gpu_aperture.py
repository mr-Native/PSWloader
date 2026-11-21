import ctypes as C,os
from pathlib import Path
import subprocess,tempfile,unittest
R=Path(__file__).parent.resolve();EDK=Path(os.environ.get('PS5_EDK2_ROOT',os.environ.get('PSW_WORK', '/opt/pswloader-work') + '/edk2'))
@unittest.skipUnless((EDK/'MdePkg/Include/Base.h').exists(),'EDK headers required')
class ApertureTests(unittest.TestCase):
    def test_real_protocol_descriptor_validation_and_cleanup(self):
        with tempfile.TemporaryDirectory() as work:
            p=Path(work);(p/'test.c').write_text(r'''
#include <stdlib.h>
#include <assert.h>
#undef NULL
#include "ps5-gpu-aperture.h"
static unsigned Mode,Freed;
VOID EFIAPI FreePool(VOID *P){Freed++;free(P);}
static EFI_STATUS EFIAPI Get(EFI_PCI_IO_PROTOCOL *P,UINT8 Bar,UINT64 *Supports,VOID **Resources){
 assert(Bar==0);if(Mode==1)return EFI_UNSUPPORTED;if(Mode==2)return EFI_SUCCESS;
 EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *D=calloc(1,sizeof(*D));assert(D);*Resources=D;
 D->Desc=ACPI_ADDRESS_SPACE_DESCRIPTOR;D->Len=sizeof(*D)-3;D->ResType=ACPI_ADDRESS_SPACE_TYPE_MEM;
 D->AddrRangeMin=0x800000000ULL;D->AddrLen=0x10000000;
 if(Mode==3)D->ResType=ACPI_ADDRESS_SPACE_TYPE_IO;
 if(Mode==4)D->Len=0;
 if(Mode==5)D->AddrTranslationOffset=1;
 if(Mode==6)D->AddrRangeMin=~0ULL;
 if(Mode==7)D->AddrLen=0;
 if(Mode==8)D->AddrRangeMin=0;
 if(Mode==9)return EFI_DEVICE_ERROR;
 return EFI_SUCCESS;
}
int main(void){
 EFI_PCI_IO_PROTOCOL P={0};P.GetBarAttributes=Get;
 for(Mode=0;Mode<10;Mode++) {
  struct ps5_gpu_cpu_aperture Out={1,2};Freed=0;EFI_STATUS S=Ps5GpuCpuAperture(&P,0,&Out);
  if(!Mode)assert(S==EFI_SUCCESS && Out.Base==0x800000000ULL && Out.Bytes==0x10000000);
  else assert(EFI_ERROR(S) && Out.Base==1 && Out.Bytes==2);
  assert(Freed==((Mode==1 || Mode==2)?0:1));
 }
 return 0;
}''')
            subprocess.run(['gcc','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-fshort-wchar','-I'+str(R),'-I'+str(EDK/'MdePkg/Include'),'-I'+str(EDK/'MdePkg/Include/X64'),str(p/'test.c'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)
