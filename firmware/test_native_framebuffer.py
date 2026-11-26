import ctypes as C,os
from pathlib import Path
import subprocess,tempfile,unittest
R=Path(__file__).parent.resolve();EDK=Path(os.environ.get('PS5_EDK2_ROOT',os.environ.get('PSW_WORK', '/opt/pswloader-work') + '/edk2'))
@unittest.skipUnless((EDK/'MdePkg/Include/Base.h').exists(),'EDK headers required')
class FramebufferTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory();p=Path(cls.tmp.name)
        (p/'test.c').write_text(r'''
#include <stdlib.h>
#include <assert.h>
#undef NULL
#include "ps5-native-framebuffer.h"
static EFI_BOOT_SERVICES Bs;EFI_BOOT_SERVICES *gBS=&Bs;
static unsigned Mode,Calls,Pools,Frees,Allocated,Cleared,Checkpoints;
VOID *EFIAPI AllocatePool(UINTN Size){if(Mode==9)return NULL;Pools++;return malloc(Size);}
VOID EFIAPI FreePool(VOID *P){Frees++;free(P);}
VOID *EFIAPI SetMem(VOID *P,UINTN Size,UINT8 Value){assert(Size==PS5_FRAMEBUFFER_BYTES && Value==0 && Allocated);Cleared++;return P;}
static EFI_STATUS EFIAPI Map(UINTN *Bytes,EFI_MEMORY_DESCRIPTOR *Out,UINTN *Key,UINTN *Stride,UINT32 *Version){
 Calls++;*Key=1;*Stride=Mode==6?0:sizeof(EFI_MEMORY_DESCRIPTOR);*Version=Mode==5?2:1;
 if(!Out || Mode==7 || (Mode==4 && Calls==2)){*Bytes=2*sizeof(EFI_MEMORY_DESCRIPTOR);return EFI_BUFFER_TOO_SMALL;}
 if(Mode==8)return EFI_DEVICE_ERROR;
 *Bytes=2*sizeof(EFI_MEMORY_DESCRIPTOR);
 Out[0]=(EFI_MEMORY_DESCRIPTOR){.Type=(Mode==1?EfiReservedMemoryType:EfiConventionalMemory),.PhysicalStart=0x10000000,.NumberOfPages=4096,.Attribute=(Mode==3?EFI_MEMORY_RUNTIME:0)};
 Out[1]=(EFI_MEMORY_DESCRIPTOR){.Type=EfiReservedMemoryType,.PhysicalStart=0x40000000,.NumberOfPages=4096};return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Pages(EFI_ALLOCATE_TYPE A,EFI_MEMORY_TYPE T,UINTN N,EFI_PHYSICAL_ADDRESS *P){
 assert(A==AllocateAddress && T==EfiReservedMemoryType && N==2025);
 assert(*P>=0x10000000 && *P+PS5_FRAMEBUFFER_BYTES<=0x11000000 && !(*P&65535));
 Allocated++;return Mode==2?EFI_OUT_OF_RESOURCES:EFI_SUCCESS;
}
static VOID Checkpoint(UINT32 Phase,EFI_PHYSICAL_ADDRESS A){
 assert(Phase==++Checkpoints && A>=0x10000000);
 if(Phase==1)assert(Allocated==0 && Cleared==0);
 if(Phase==2)assert(Allocated==1 && Cleared==0);
 if(Phase==3)assert(Allocated==1 && Cleared==1);
}
int Run(unsigned Test){
 Mode=Test;Calls=Pools=Frees=Allocated=Cleared=Checkpoints=0;Bs.GetMemoryMap=Map;Bs.AllocatePages=Pages;
 struct ps5_dcn201_memory M={0x400000000,0x800000000,0,0x400000000};EFI_PHYSICAL_ADDRESS Out=0xabc;
 EFI_STATUS S=Ps5NativeFramebufferReserve(&M,&Out,Checkpoint);assert(Pools==Frees);
 if(Test==0 || Test==4){assert(S==EFI_SUCCESS && Out!=0xabc && Allocated==1 && Cleared==1 && Checkpoints==3);}
 else {assert(EFI_ERROR(S) && Out==0xabc && Cleared==0);if(Test!=2)assert(Allocated==0 && Checkpoints==0);else assert(Checkpoints==1);}
 if(Test==7)assert(Calls==4);
 return 1;
}''')
        subprocess.run(['gcc','-shared','-fPIC','-fshort-wchar','-DMDEPKG_NDEBUG','-I'+str(R),'-I'+str(EDK/'MdePkg/Include'),'-I'+str(EDK/'MdePkg/Include/X64'),str(p/'test.c'),str(R/'dcn201-memory.c'),'-o',str(p/'fb.so')],check=True)
        cls.lib=C.CDLL(str(p/'fb.so'))
    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()
    def test_verified_ram_only_and_bounded_error_cleanup(self):
        for mode in range(10):
            with self.subTest(mode=mode):self.assertEqual(self.lib.Run(mode),1)
