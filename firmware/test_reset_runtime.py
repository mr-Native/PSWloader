"""Exercise real reset library lifecycle with mocked UEFI services; no power calls."""
import ctypes as C
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
HERE=Path(__file__).resolve().parent
EDK=Path(os.environ.get('PS5_EDK2_ROOT',os.environ.get('PSW_WORK', '/opt/pswloader-work') + '/edk2'))

@unittest.skipUnless((EDK/'MdePkg/Include/Base.h').exists(),'EDK headers required')
class ResetRuntimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work=tempfile.TemporaryDirectory();work=Path(cls.work.name)
        source=HERE/'Ps5PlatformPkg/Library/Ps5ResetSystemLib/Ps5ResetSystemLib.c'
        (work/'mock.c').write_text('#define PS5_FB_BASE 0\n#include "'+str(source)+'"\n'+r'''
static int Valid,FailMap,FailConvert,EventCount,SetCount,ConvertCount,Writes;
static UINT64 LastAttributes;
static EFI_BOOT_SERVICES Bs;
static EFI_DXE_SERVICES Ds;
static EFI_RUNTIME_SERVICES Rt;
EFI_BOOT_SERVICES *gBS=&Bs;
EFI_DXE_SERVICES *gDS=&Ds;
EFI_RUNTIME_SERVICES *gRT=&Rt;
EFI_GUID gEfiEventReadyToBootGuid={0};
EFI_GUID gEfiEventExitBootServicesGuid={0};
EFI_GUID gEfiEventVirtualAddressChangeGuid={0};
EFI_GUID gEfiGlobalVariableGuid={0};
EFI_GUID gEfiGraphicsOutputProtocolGuid={0};
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN L){volatile UINT8 *d=D;CONST UINT8 *s=S;for(UINTN I=0;I<L;I++)d[I]=s[I];return D;}
UINT64 EFIAPI AsmReadMsr64(UINT32 I){(void)I;return 0;}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN L){CONST UINT8 *a=A,*b=B;for(UINTN I=0;I<L;I++)if(a[I]!=b[I])return a[I]-b[I];return 0;}
VOID *EFIAPI AllocateReservedPages(UINTN P){(void)P;return NULL;}
VOID *EFIAPI ZeroMem(VOID *B,UINTN L){volatile UINT8 *P=B;for(UINTN I=0;I<L;I++)P[I]=0;return B;}
UINTN EFIAPI AsmReadCr4(VOID){return 0;}
VOID EFIAPI AsmReadIdtr(IA32_DESCRIPTOR *D){D->Base=0;D->Limit=0;}
static EFI_STATUS EFIAPI Create(UINT32 Type,EFI_TPL Tpl,EFI_EVENT_NOTIFY Notify,CONST VOID *Context,CONST EFI_GUID *Group,EFI_EVENT *Event) { *Event=(EFI_EVENT)(UINTN)++EventCount; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Close(EFI_EVENT Event) { return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Get(EFI_PHYSICAL_ADDRESS Address,EFI_GCD_MEMORY_SPACE_DESCRIPTOR *D) { D->GcdMemoryType=EfiGcdMemoryTypeMemoryMappedIo; D->Attributes=EFI_MEMORY_WB; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Set(EFI_PHYSICAL_ADDRESS A,UINT64 Length,UINT64 Attributes) { SetCount++;LastAttributes=Attributes; return FailMap ? EFI_DEVICE_ERROR : EFI_SUCCESS; }
static EFI_STATUS EFIAPI Convert(UINTN Flags,VOID **Pointer) { ConvertCount++; if(FailConvert)return EFI_NOT_FOUND;*Pointer=(VOID*)((UINTN)*Pointer+0x1000000000ULL);return EFI_SUCCESS; }
UINT32 EFIAPI PciExpressRead32(UINTN A) { if(!Valid)return 0xffffffff;switch(A&0xfff){case 0:return 0x9107104d;case 0x18:return 0x8520000c;case 0x20:return 0x8540000c;default:return 0;} }
UINT16 EFIAPI PciExpressRead16(UINTN A) { return 7; }
UINT8 EFIAPI MmioRead8(UINTN A){return 0;}
UINT16 EFIAPI MmioRead16(UINTN A){return 0;}
UINT32 EFIAPI MmioRead32(UINTN A){return 0;}
UINT8 EFIAPI MmioWrite8(UINTN A,UINT8 V){Writes++;return V;}
UINT16 EFIAPI MmioWrite16(UINTN A,UINT16 V){Writes++;return V;}
UINT32 EFIAPI MmioWrite32(UINTN A,UINT32 V){Writes++;return V;}
UINT64 EFIAPI AsmReadTsc(VOID){return 0;}
VOID EFIAPI AsmWbinvd(VOID){}
VOID EFIAPI CpuPause(VOID){}
VOID EFIAPI DisableInterrupts(VOID){}
UINT32 EFIAPI InterlockedCompareExchange32(volatile UINT32 *V,UINT32 Cmp,UINT32 New){UINT32 Old=*V;if(Old==Cmp)*V=New;return Old;}
int TestInit(int ValidDevice,int MappingFailure) {
 Valid=ValidDevice;FailMap=MappingFailure;FailConvert=0;EventCount=0;SetCount=0;ConvertCount=0;Writes=0;
 mAvailable=FALSE;mMapped=FALSE;mMailbox=0x85400000;mDoorbell=0x85308000;
 Bs.CreateEventEx=Create;Bs.CloseEvent=Close;Ds.GetMemorySpaceDescriptor=Get;Ds.SetMemorySpaceAttributes=Set;Rt.ConvertPointer=Convert;gRT=&Rt;
 return Ps5ResetConstructor(NULL,NULL)!=EFI_SUCCESS;
}
void TestExit(void){ExitBootServices(NULL,NULL);}
void TestReady(int Failure){FailMap=Failure;MapRuntimePages(NULL,NULL);}
void TestConvert(int Failure){FailConvert=Failure;gRT=(VOID*)0x1234;ConvertRuntimePointers(NULL,NULL);}
UINT64 TestValue(unsigned Index){switch(Index){case 0:return mAvailable;case 1:return mMapped;case 2:return mMailbox;case 3:return mDoorbell;case 4:return SetCount;case 5:return ConvertCount;case 6:return LastAttributes;case 7:return Writes;case 8:return EventCount;default:return 0;}}
''')
        lib=work/'reset.so'
        subprocess.run(['gcc','-shared','-fPIC','-fshort-wchar','-DPS5_EDK2','-DMDEPKG_NDEBUG',
                        '-I'+str(EDK/'MdePkg/Include'),'-I'+str(EDK/'MdePkg/Include/X64'),
                        '-I'+str(EDK/'MdeModulePkg/Include'),'-I'+str(HERE),str(work/'mock.c'),str(HERE/'icc-power.c'),'-o',str(lib)],check=True)
        cls.lib=C.CDLL(str(lib));cls.lib.TestValue.argtypes=[C.c_uint];cls.lib.TestValue.restype=C.c_uint64
    @classmethod
    def tearDownClass(cls):cls.work.cleanup()
    def v(self,index):return self.lib.TestValue(index)
    def test_constructor_no_power_writes_and_runtime_pages(self):
        self.assertEqual(self.lib.TestInit(1,0),0)
        self.assertEqual([self.v(i) for i in (0,1,4,7,8)],[1,1,2,0,3])
        self.assertEqual(self.v(6),0x8000000000000001)
    def test_unverified_device_has_no_mapping_or_power_writes(self):
        self.lib.TestInit(0,0);self.lib.TestConvert(0)
        self.assertEqual([self.v(i) for i in (0,1,4,5,7)],[0,0,0,0,0])
    def test_ready_event_reapplies_attributes(self):
        self.lib.TestInit(1,0);self.lib.TestReady(0)
        self.assertEqual(self.v(4),4);self.assertEqual(self.v(1),1)
    def test_mapping_failure_disables_virtual_runtime_access(self):
        self.lib.TestInit(1,1);self.lib.TestConvert(0)
        self.assertEqual(self.v(0),0);self.assertEqual(self.v(5),0)
    def test_conversion_uses_private_rt_and_converts_both_regions(self):
        self.lib.TestInit(1,0);self.lib.TestConvert(0)
        self.assertEqual(self.v(2),0x1085400000);self.assertEqual(self.v(3),0x1085308000)
        self.assertEqual(self.v(5),3);self.assertEqual(self.v(0),1)
    def test_exit_without_runtime_mapping_disables_device(self):
        self.lib.TestInit(1,1);self.lib.TestExit()
        self.assertEqual(self.v(0),0)
    def test_failed_conversion_disables_device(self):
        self.lib.TestInit(1,0);self.lib.TestConvert(1)
        self.assertEqual(self.v(0),0)

if __name__=='__main__':unittest.main()
