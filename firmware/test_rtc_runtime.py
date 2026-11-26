"""Exercise the actual RTC driver without hardware or a fabricated clock."""
import ctypes as C
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

HERE=Path(__file__).resolve().parent
EDK=Path(os.environ.get('PS5_EDK2_ROOT',os.environ.get('PSW_WORK', '/opt/pswloader-work') + '/edk2'))
spec=importlib.util.spec_from_file_location('build_uefi_rtc',HERE/'build-uefi.py')
build=importlib.util.module_from_spec(spec);spec.loader.exec_module(build)

class RtcBuildTests(unittest.TestCase):
    def test_both_platform_descriptions_replace_cmos_driver(self):
        for suffix in ('dsc','fdf'):
            path=EDK/f'UefiPayloadPkg/UefiPayloadPkg.{suffix}'
            if not path.exists():self.skipTest('EDK sources required')
            source=build.replace_rtc(path.read_text())
            self.assertNotIn(build.RTC_ORIGINAL,source)
            self.assertEqual(source.count(build.RTC_PS5),1)
            self.assertNotIn("gPcAtChipsetPkgTokenSpaceGuid.PcdRtc",source)
        for invalid in ('',build.RTC_ORIGINAL*2):
            with self.assertRaises(RuntimeError):build.replace_rtc(invalid)

@unittest.skipUnless((EDK/'MdePkg/Include/Base.h').exists(),'EDK headers required')
class RtcRuntimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work=tempfile.TemporaryDirectory();work=Path(cls.work.name)
        source=HERE/'Ps5PlatformPkg/Ps5RtcRuntimeDxe/Ps5RtcRuntimeDxe.c'
        (work/'mock.c').write_text('#include "'+str(source)+'"\n'+r'''
static EFI_BOOT_SERVICES Bs;
static EFI_RUNTIME_SERVICES Rt;
EFI_BOOT_SERVICES *gBS=&Bs;
EFI_RUNTIME_SERVICES *gRT=&Rt;
EFI_GUID gEfiRealTimeClockArchProtocolGuid={0};
static int Fail,Installs;
static UINT64 Ticks;
UINT64 EFIAPI AsmReadTsc(VOID) { return Ticks; }
UINT32 EFIAPI InterlockedCompareExchange32(volatile UINT32 *P,UINT32 C,UINT32 V) {
 UINT32 Old=*P;if(Old==C)*P=V;return Old;
}
VOID EFIAPI MemoryFence(VOID) {}
UINT64 TestClock(void) {
 EFI_TIME T={0},R={0};EFI_TIME_CAPABILITIES Caps;
 mClockValid=FALSE;mClockLock=0;Ticks=100;
 T.Year=2024;T.Month=2;T.Day=29;T.Hour=23;T.Minute=59;T.Second=59;
 T.Nanosecond=900000000;T.TimeZone=0;
 if(Ps5SetTime(&T)!=EFI_SUCCESS)return 1;
 Ticks+=PS5_TSC_HZ/2;
 gBS=NULL;gRT=NULL;
 if(Ps5GetTime(&R,&Caps)!=EFI_SUCCESS)return 2;
 if(R.Year!=2024 || R.Month!=3 || R.Day!=1 || R.Hour || R.Minute || R.Second || R.Nanosecond!=400000000)return 3;
 T.Month=2;T.Day=30;if(Ps5SetTime(&T)!=EFI_INVALID_PARAMETER)return 4;
 Ticks=1;if(Ps5GetTime(&R,NULL)!=EFI_DEVICE_ERROR)return 5;
 mClockLock=1;if(Ps5GetTime(&R,NULL)!=EFI_NOT_READY)return 6;mClockLock=0;
 mClockValid=FALSE;return 0;
}
static EFI_STATUS EFIAPI Install(EFI_HANDLE *Handle,...) {
  Installs++; *Handle=(EFI_HANDLE)(UINTN)1;
  return Fail ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}
UINT64 TestInit(int Failure) {
  Fail=Failure;Installs=0;mClockValid=FALSE;mClockLock=0;Bs.InstallMultipleProtocolInterfaces=Install;
  Rt.GetTime=NULL;Rt.SetTime=NULL;Rt.GetWakeupTime=NULL;Rt.SetWakeupTime=NULL;
  gBS=&Bs;gRT=&Rt;
  return Ps5RtcInitialize(NULL,NULL);
}
UINT64 TestInstalled(void) {
  return Installs==1 && Rt.GetTime==Ps5GetTime && Rt.SetTime==Ps5SetTime &&
    Rt.GetWakeupTime==Ps5GetWakeupTime && Rt.SetWakeupTime==Ps5SetWakeupTime;
}
UINT64 TestRestored(void) {
  return Rt.GetTime==NULL && Rt.SetTime==NULL && Rt.GetWakeupTime==NULL && Rt.SetWakeupTime==NULL;
}
void TestAfterExit(void) { gBS=NULL;gRT=NULL; }
UINT64 TestCall(unsigned Kind,unsigned Nulls) {
  EFI_TIME Time;EFI_TIME_CAPABILITIES Caps;BOOLEAN Enabled=TRUE,Pending=TRUE;
  UINT8 *P=(UINT8*)&Time,*Q=(UINT8*)&Caps;UINTN I;
  EFI_STATUS Status;
  for(I=0;I<sizeof(Time);I++)P[I]=0xa5;
  for(I=0;I<sizeof(Caps);I++)Q[I]=0x5a;
  switch(Kind){
    case 0:Status=Rt.GetTime(Nulls&1?NULL:&Time,Nulls&2?NULL:&Caps);break;
    case 1:Status=Rt.SetTime(Nulls&1?NULL:&Time);break;
    case 2:Status=Rt.GetWakeupTime(Nulls&1?NULL:&Enabled,Nulls&2?NULL:&Pending,Nulls&4?NULL:&Time);break;
    default:Status=Rt.SetWakeupTime(Kind==3,Nulls&1?NULL:&Time);break;
  }
  if(Status!=EFI_SUCCESS){   /* a failing call must not touch the caller's buffers; a successful GetTime fills them */
  for(I=0;I<sizeof(Time);I++)if(P[I]!=0xa5)return 0x100;
  for(I=0;I<sizeof(Caps);I++)if(Q[I]!=0x5a)return 0x101;
  }
  if(!Enabled||!Pending)return 0x102;
  return Status;
}
''')
        lib=work/'rtc.so'
        subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-shared','-fPIC',
                        '-fshort-wchar','-DMDEPKG_NDEBUG','-I'+str(EDK/'MdePkg/Include'),
                        '-I'+str(EDK/'MdePkg/Include/X64'),'-I'+str(HERE),str(work/'mock.c'),'-o',str(lib)],check=True)
        cls.lib=C.CDLL(str(lib))
        for name in ('TestInit','TestInstalled','TestRestored','TestCall'):
            getattr(cls.lib,name).restype=C.c_uint64
    @classmethod
    def tearDownClass(cls):cls.work.cleanup()
    def test_seeded_clock_rollover_runtime_and_invalid_inputs(self):
        self.assertEqual(self.lib.TestClock(),0)

    def test_installs_all_runtime_callbacks(self):
        self.assertEqual(self.lib.TestInit(0),0)
        self.assertEqual(self.lib.TestInstalled(),1)
    def test_failed_install_restores_previous_services(self):
        self.assertEqual(self.lib.TestInit(1),0x8000000000000007)
        self.assertEqual(self.lib.TestRestored(),1)
    def test_runtime_calls_need_no_boot_or_runtime_globals(self):
        self.lib.TestInit(0);self.lib.TestAfterExit()
        for kind in range(5):
            for nulls in range(8):
                invalid=(kind in (0,1,3) and nulls&1) or (kind==2 and nulls&7)
                # driver v1.1: the software clock is valid from init (build-time epoch), so GetTime succeeds
                self.assertEqual(self.lib.TestCall(kind,nulls),
                                 0x8000000000000002 if invalid or kind==1 else (0 if kind==0 else 0x8000000000000003))

if __name__=='__main__':unittest.main()
