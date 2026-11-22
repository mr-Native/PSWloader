"""Exercise the real native adapter with mocked UEFI services, never GPU MMIO."""
import os,pathlib,subprocess,tempfile,unittest
R=pathlib.Path(__file__).parent.resolve()
EDK=pathlib.Path(os.environ.get('PS5_EDK2_ROOT',os.environ.get('PSW_WORK', '/opt/pswloader-work') + '/edk2'))

@unittest.skipUnless((EDK/'MdePkg/Include/Uefi.h').exists(),'EDK headers required')
class NativeRouteTests(unittest.TestCase):
 def test_atom_cleared_frontend_is_reconnected_after_enable(self):
  with tempfile.TemporaryDirectory() as work:
   p=pathlib.Path(work)
   (p/'test.c').write_text(r'''
#include <assert.h>
#include <stdlib.h>
#include <sys/mman.h>
#undef NULL
#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DebugLib.h>
#include <Library/BaseMemoryLib.h>
static void Ps5GpuSnapshot(void){}
#include "ps5-native-dp-source.h"
static EFI_BOOT_SERVICES Bs;EFI_BOOT_SERVICES *gBS=&Bs;
EFI_GUID gEfiPciIoProtocolGuid={0};
static EFI_PCI_IO_PROTOCOL Pci;
static UINT32 Route;static unsigned Enabled,Initialized,FailRestore;
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){for(UINTN I=0;I<N;I++)((UINT8*)P)[I]=0;return P;}
VOID *EFIAPI AllocateZeroPool(UINTN N){return calloc(1,N);}
VOID EFIAPI FreePool(VOID *P){free(P);}
static EFI_STATUS EFIAPI Crc(VOID *P,UINTN N,UINT32 *C){assert(N==0xd800);*C=0xd5357201;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Locate(EFI_LOCATE_SEARCH_TYPE T,EFI_GUID *G,VOID *K,UINTN *N,EFI_HANDLE **H){*N=1;*H=malloc(sizeof(EFI_HANDLE));(*H)[0]=(VOID*)1;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Handle(EFI_HANDLE H,EFI_GUID *G,VOID **P){*P=&Pci;return EFI_SUCCESS;}
static UINT32 MockGpuId=0x13fc1002;
static EFI_STATUS EFIAPI ConfigRead(EFI_PCI_IO_PROTOCOL *P,EFI_PCI_IO_PROTOCOL_WIDTH W,UINT32 O,UINTN N,VOID *V){
 assert(N==1);if(O==0)*(UINT32*)V=MockGpuId;else {assert(O==4);*(UINT16*)V=2;}return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Read(EFI_PCI_IO_PROTOCOL *P,EFI_PCI_IO_PROTOCOL_WIDTH W,UINT8 B,UINT64 O,UINTN N,VOID *V){
 assert(B==5 && N==1);UINT32 Value=0;
 if(O==0x14004 || O==0x14204)Value=0x80000110;
 else if(O==0x14f50)Value=0x10ff012;
 else if(O==0x155bc)Value=Route;
 *(UINT32*)V=Value;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Write(EFI_PCI_IO_PROTOCOL *P,EFI_PCI_IO_PROTOCOL_WIDTH W,UINT8 B,UINT64 O,UINTN N,VOID *V){
 assert(B==5 && N==1);if(O==0x155bc){if(Enabled&&FailRestore)return EFI_DEVICE_ERROR;Route=*(UINT32*)V;}return EFI_SUCCESS;
}
EFI_STATUS Ps5AtomExecute(EFI_PCI_IO_PROTOCOL *P,const UINT8 *Rom,UINTN Bytes,UINTN Table,UINT32 *Params,UINTN N){
 assert(Table==76 && Bytes==65536 && N==8);
 if(((Params[0]>>8)&255)==7){Initialized++;return EFI_SUCCESS;}
 assert(Route==0x100 && Params[2]==1 && Params[1]==81000);
 /* Real platform ROM independently reproduced this frontend clear. */
 Route=0;Enabled++;return EFI_SUCCESS;
}
int main(void){
 void *Rom=mmap((void*)0xc0000,65536,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0);
 assert(Rom==(void*)0xc0000);
 UINT8 *R=(UINT8*)Rom;R[0]=0x55;R[1]=0xaa;R[2]=108;R[0x48]=0x00;R[0x49]=0x01;
 R[0x104]='A';R[0x105]='T';R[0x106]='O';R[0x107]='M';
 Bs.CalculateCrc32=Crc;Bs.LocateHandleBuffer=Locate;Bs.HandleProtocol=Handle;
 Pci.Pci.Read=ConfigRead;Pci.Mem.Read=Read;Pci.Mem.Write=Write;
 assert(Ps5NativeDpSource()==EFI_SUCCESS);
 assert(Initialized==1 && Enabled==1 && Route==0x100);
 /* Also accept Slim / Oberon Plus VBIOS size and GPU PCI ID. */
 Route=Enabled=Initialized=0;R[2]=112;MockGpuId=0x13dc1002;
 assert(Ps5NativeDpSource()==EFI_SUCCESS && Initialized==1 && Enabled==1 && Route==0x100);
 Route=Enabled=Initialized=0;FailRestore=1;MockGpuId=0x13fc1002;
 assert(Ps5NativeDpSource()==EFI_DEVICE_ERROR && Enabled==1 && Initialized==1 && Route==0);
 munmap(Rom,65536);return 0;
}''')
   subprocess.run(['gcc','-Wall','-Wno-unused-parameter','-fshort-wchar','-DMDEPKG_NDEBUG','-I'+str(R),'-I'+str(R/'atom-host'),'-I'+str(EDK/'MdePkg/Include'),'-I'+str(EDK/'MdePkg/Include/X64'),str(p/'test.c'),str(R/'dcn201-link.c'),'-o',str(p/'test')],check=True)
   subprocess.run([str(p/'test')],check=True)
