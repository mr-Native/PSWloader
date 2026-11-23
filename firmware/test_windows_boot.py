"""Execute ESP selection and delayed arrival with real EDK protocol types."""
import os
import ctypes
from pathlib import Path
import subprocess
import tempfile
import unittest

HERE=Path(__file__).parent.resolve()
EDK=Path(os.environ.get('PSW_WORK', '/opt/pswloader-work') + '/edk2')

@unittest.skipUnless((EDK/'MdePkg/Include/Uefi.h').exists(),'EDK required')
class WindowsBootTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work=tempfile.TemporaryDirectory();p=Path(cls.work.name)
        source='static unsigned long long Ps5TestWarm[16];\nstatic unsigned long long Ps5TestLogCell;\n#define PS5_WARM_LOGCELL ((unsigned long long)(unsigned long)&Ps5TestLogCell)\n#define PS5_WARM_RECORD ((unsigned long long)(unsigned long)&Ps5TestWarm[0])\n#define PS5_WARM_LASTBOOT ((unsigned long long)(unsigned long)&Ps5TestWarm[8])\n#include "'+str(HERE/'Ps5BootWindows.c')+'"\n'+r'''
#include <stdlib.h>
#include <string.h>
static EFI_BOOT_SERVICES Bs;
static EFI_RUNTIME_SERVICES Rt;
EFI_RUNTIME_SERVICES *gRT=&Rt;
static EFI_SYSTEM_TABLE St;
EFI_SYSTEM_TABLE *gST=&St;
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL Console;
static unsigned OutputCalls,Signals;
EFI_STATUS EFIAPI EfiEventGroupSignal(CONST EFI_GUID *Guid){Signals++;return EFI_SUCCESS;}
EFI_BOOT_SERVICES *gBS=&Bs;
EFI_GUID gEfiEventExitBootServicesGuid={3};
EFI_GUID gEfiSimpleFileSystemProtocolGuid={1},gEfiPartitionInfoProtocolGuid={2};
EFI_GUID gEfiDevicePathProtocolGuid={4},gEfiMpServiceProtocolGuid={5},gEfiBlockIoProtocolGuid={6},gEfiBlockIo2ProtocolGuid={7},gEfiSmbiosProtocolGuid={8};
static EFI_PARTITION_INFO_PROTOCOL Partition;
static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL Fs;
static EFI_FILE_PROTOCOL Root,File;
static unsigned Mode,Waits,Boots,Opens,Closed,Freed,TimeCalls,HighStalls,TscCalls;
static EFI_TPL CurrentTpl;
VOID EFIAPI FreePool(VOID *p){free(p);}
VOID * EFIAPI AllocatePool(UINTN s){return malloc(s);}
VOID * EFIAPI AllocateZeroPool(UINTN s){return calloc(1,s);}
INTN EFIAPI CompareMem(CONST VOID *a,CONST VOID *b,UINTN n){return memcmp(a,b,n);}
BOOLEAN EFIAPI CompareGuid(CONST GUID *a,CONST GUID *b){return memcmp(a,b,16)==0;}
UINT64 EFIAPI DivU64x32(UINT64 a,UINT32 b){return a/b;}   /* r5.21 boot names */
VOID EFIAPI EfiBootManagerRefreshAllBootOption(VOID){}   /* r5.21: Ps5UsbMassAllow */
EFI_BOOT_MANAGER_LOAD_OPTION *EFIAPI EfiBootManagerGetLoadOptions(UINTN *Count,EFI_BOOT_MANAGER_LOAD_OPTION_TYPE Type){(void)Type;*Count=0;return NULL;}   /* r5.25.4 prune */
EFI_STATUS EFIAPI EfiBootManagerDeleteLoadOptionVariable(UINTN N,EFI_BOOT_MANAGER_LOAD_OPTION_TYPE T){(void)N;(void)T;return EFI_SUCCESS;}
EFI_STATUS EFIAPI EfiBootManagerFreeLoadOptions(EFI_BOOT_MANAGER_LOAD_OPTION *O,UINTN N){(void)O;(void)N;return EFI_SUCCESS;}
UINT64 EFIAPI MultU64x32(UINT64 a,UINT32 b){return a*b;}
static EFI_STATUS EFIAPI Locate(EFI_LOCATE_SEARCH_TYPE t,EFI_GUID *g,VOID *k,UINTN *n,EFI_HANDLE **h){
 if(Mode==1 || (Mode==2 && Waits<3))return EFI_NOT_FOUND;
 *n=Mode==3?2:1;*h=malloc(*n*sizeof(EFI_HANDLE));for(unsigned i=0;i<*n;i++)(*h)[i]=(VOID*)(UINTN)(i+1);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Handle(EFI_HANDLE h,EFI_GUID *g,VOID **p){
 *p=g==&gEfiPartitionInfoProtocolGuid?(VOID*)&Partition:(VOID*)&Fs;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Volume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *s,EFI_FILE_PROTOCOL **r){*r=&Root;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Open(EFI_FILE_PROTOCOL *r,EFI_FILE_PROTOCOL **f,CHAR16 *n,UINT64 mode,UINT64 attr){
 Opens++;if(mode!=EFI_FILE_MODE_READ || attr!=0)abort();
 if(n[1]=='P') { if(Mode==7)return EFI_NOT_FOUND; }
 else if(memcmp(n,mWindowsPath,sizeof(mWindowsPath))) {   /* r5.15: an unpinned ESP is classified by these two probes */
  static CONST CHAR16 Bcd[]=L"\\EFI\\Microsoft\\Boot\\BCD",Wim[]=L"\\sources\\boot.wim",Ms[]=L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi";   /* r5.25.4: + bootmgfw.efi probe */
  if(memcmp(n,Bcd,sizeof(Bcd)) && memcmp(n,Wim,sizeof(Wim)) && memcmp(n,Ms,sizeof(Ms)))abort();
  return EFI_NOT_FOUND;
 }
 if(Mode==4)return EFI_NOT_FOUND;*f=&File;return EFI_SUCCESS;
}
UINT64 EFIAPI AsmReadMsr64(UINT32 Index){(void)Index;return 0;} /* v1.1 ext-APIC opener: APIC off -> no MMIO */
BOOLEAN EFIAPI SaveAndDisableInterrupts(VOID){return FALSE;} /* r5.25.5 ext-APIC EBS opener */
UINT16 EFIAPI AsmReadCs(VOID){return 0x38;} /* r5.25.7 warm cpu diagnostics */
VOID EFIAPI AsmReadGdtr(IA32_DESCRIPTOR *D){D->Base=0;D->Limit=0;}
VOID EFIAPI AsmReadIdtr(IA32_DESCRIPTOR *D){D->Base=0;D->Limit=0;}
UINTN EFIAPI AsmReadEflags(VOID){return 0x2;}
UINTN EFIAPI AsmReadDr7(VOID){return 0x400;}
BOOLEAN EFIAPI SetInterruptState(BOOLEAN State){return State;} /* r5.25.5 ext-APIC ISR drain */
UINT64 EFIAPI AsmWriteMsr64(UINT32 Index,UINT64 Value){(void)Index;return Value;} /* r4-nvme-name CPU brand MSRs */
UINT32 EFIAPI AsmCpuid(UINT32 Leaf,UINT32 *a,UINT32 *b,UINT32 *c,UINT32 *d){if(a)*a=0;if(b)*b=0;if(c)*c=0;if(d)*d=0;return Leaf;}
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN L){return memmove(D,S,L);}
VOID *EFIAPI ZeroMem(VOID *B,UINTN L){return memset(B,0,L);}
UINTN EFIAPI AsciiStrLen(CONST CHAR8 *S){return strlen((const char *)S);}
CHAR8 *EFIAPI AsciiStrStr(CONST CHAR8 *S,CONST CHAR8 *T){return (CHAR8 *)strstr((const char *)S,(const char *)T);}   /* r5.25.3: card-reader and size checks */
UINT64 EFIAPI AsmReadTsc(VOID){if(TimeCalls && Mode!=15)return TscCalls++?15965000:1000;return Mode==11?1596300000ULL*901+900:1000;}
static void Put(UINT8 *p,UINT64 v,unsigned n){for(unsigned i=0;i<n;i++)p[i]=(UINT8)(v>>(8*i));}
static EFI_STATUS EFIAPI Crc(VOID *p,UINTN n,UINT32 *out){
 UINT32 crc=0xffffffff;for(UINTN i=0;i<n;i++){crc^=((UINT8*)p)[i];for(unsigned j=0;j<8;j++)crc=(crc>>1)^((crc&1)?0xedb88320U:0);}*out=crc^0xffffffff;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Read(EFI_FILE_PROTOCOL *f,UINTN *n,VOID *p){
 UINT8 data[48]={0};memcpy(data,"PS5UTC01",8);Put(data+8,1704067200ULL,8);
 Put(data+16,Mode==9?2000:900,8);Put(data+24,Mode==10?1:1596300000ULL,8);
 Put(data+36,1000,4);Put(data+40,900,4);UINT32 crc;Crc(data,44,&crc);Put(data+44,crc,4);
 if(Mode==8)data[10]^=1;
 if(*n!=49)abort();memcpy(p,data,48);*n=48;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI SetTime(EFI_TIME *t){if(t->Year!=2024 || t->Month!=1 || t->Day!=1)abort();return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Close(EFI_FILE_PROTOCOL *f){Closed++;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI GetTime(EFI_TIME *t,EFI_TIME_CAPABILITIES *c){
 if(c!=NULL || CurrentTpl!=TPL_APPLICATION)abort();TimeCalls++;
 if(Mode==12)return EFI_DEVICE_ERROR;
 memset(t,0,sizeof(*t));t->Year=2024;t->Month=1;t->Day=1;
 t->Nanosecond=Mode==13?0:TimeCalls*10000000U;return EFI_SUCCESS;
}
static EFI_TPL EFIAPI Raise(EFI_TPL t){if(t!=TPL_HIGH_LEVEL || CurrentTpl!=TPL_APPLICATION)abort();EFI_TPL old=CurrentTpl;CurrentTpl=t;return old;}
static VOID EFIAPI Restore(EFI_TPL t){if(t!=TPL_APPLICATION || CurrentTpl!=TPL_HIGH_LEVEL)abort();CurrentTpl=t;}
static EFI_STATUS EFIAPI Stall(UINTN us){
 if(us==100000){if(CurrentTpl!=TPL_APPLICATION)abort();Waits++;return EFI_SUCCESS;}
 if(CurrentTpl!=TPL_HIGH_LEVEL || (us!=1 && us!=1000 && us!=9000))abort();
 HighStalls++;return Mode==14?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Output(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *self,CHAR16 *text){
 if(self!=&Console || text==NULL || (text[0]==0))abort();OutputCalls++;
 return EFI_UNSUPPORTED;
}
int Tap(void){
 CurrentTpl=TPL_APPLICATION;Bs.RaiseTPL=Raise;Bs.RestoreTPL=Restore;
 St.ConOut=&Console;Console.OutputString=Output;OutputCalls=Signals=0;
 EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *saved=InstallWindowsConsoleTap();
 CHAR16 text[]={'W','i','n','\n',0};
 for(unsigned i=0;i<100;i++)if(Console.OutputString(&Console,text)!=EFI_UNSUPPORTED)return 0;
 RestoreWindowsConsoleTap(saved);
 /* every output forwarded, snapshots bounded by the 80-line capture budget */
 return OutputCalls==100 && Signals==80 && mConsoleRequests==80 && Console.OutputString==Output;
}
int TapProgress(void){
 CurrentTpl=TPL_APPLICATION;Bs.RaiseTPL=Raise;Bs.RestoreTPL=Restore;
 St.ConOut=&Console;Console.OutputString=Output;OutputCalls=Signals=0;
 EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *saved=InstallWindowsConsoleTap();
 CHAR16 blank[]={' ','\t','\r','\n',0},message[]={'W','i','n','\n',0};
 for(unsigned i=0;i<100;i++)if(Console.OutputString(&Console,blank)!=EFI_UNSUPPORTED)return 0;
 if(Signals!=1 || mConsoleRequests!=1)return 0;
 for(unsigned i=0;i<100;i++)if(Console.OutputString(&Console,message)!=EFI_UNSUPPORTED)return 0;
 RestoreWindowsConsoleTap(saved);
 /* 100 blank outputs used one request; the remaining 79 of the 80-line budget go to real messages */
 return OutputCalls==200 && Signals==80 && mConsoleRequests==80 && Console.OutputString==Output;
}
static EFI_STATUS EFIAPI OutputSuccess(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *self,CHAR16 *text){return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Cursor(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *self,UINTN column,UINTN row){
 if(self!=&Console || column!=3 || row!=20)abort();return EFI_DEVICE_ERROR;
}
int TapCharacters(void){
 CurrentTpl=TPL_APPLICATION;Bs.RaiseTPL=Raise;Bs.RestoreTPL=Restore;
 St.ConOut=&Console;Console.OutputString=Output;Console.SetCursorPosition=Cursor;
 OutputCalls=Signals=0;
 EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *saved=InstallWindowsConsoleTap();
 CHAR16 message[]={'W','i','n','l','o','a','d',0};
 for(unsigned i=0;i<7;i++){CHAR16 one[]={message[i],0};
  // Use success here: the actual successful character stream must batch.
  EFI_TEXT_STRING original=mConsoleOutput;mConsoleOutput=OutputSuccess;
  EFI_STATUS status=Console.OutputString(&Console,one);mConsoleOutput=original;
  if(status!=EFI_SUCCESS)return 0;
 }
 if(Signals!=0 || mConsoleRequests!=0 || mConsoleLineLength!=7)return 0;
 if(Console.SetCursorPosition(&Console,3,20)!=EFI_DEVICE_ERROR)return 0;
 if(Signals!=1 || mConsoleRequests!=1 || mConsoleLineLength!=0)return 0;
 RestoreWindowsConsoleTap(saved);
 return Console.SetCursorPosition==Cursor && Console.OutputString==Output;
}
int TapBars(void){
 CurrentTpl=TPL_APPLICATION;Bs.RaiseTPL=Raise;Bs.RestoreTPL=Restore;
 St.ConOut=&Console;Console.OutputString=OutputSuccess;Console.SetCursorPosition=Cursor;
 Signals=0;EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *saved=InstallWindowsConsoleTap();
 CHAR16 bar[]={'l','l','l',' ',' ','\n',0};
 for(unsigned i=0;i<100;i++)if(Console.OutputString(&Console,bar)!=EFI_SUCCESS)return 0;
 if(Signals!=0 || mConsoleRequests!=0 || mConsoleLineLength!=0)return 0;
 CHAR16 error[]={'E','r','r','o','r',':',' ','0','x','c','0','0','0','0','0','b','b','\n',0};
 if(Console.OutputString(&Console,error)!=EFI_SUCCESS)return 0;
 if(Signals!=1 || mConsoleRequests!=1)return 0;
 RestoreWindowsConsoleTap(saved);return Console.OutputString==OutputSuccess;
}
BOOLEAN EFIAPI IsDevicePathEnd(CONST VOID *Node){(void)Node; return TRUE;}
EFI_DEVICE_PATH_PROTOCOL * EFIAPI NextDevicePathNode(CONST VOID *Node){(void)Node; return NULL;}
UINT8 EFIAPI DevicePathType(CONST VOID *Node){(void)Node; return 0;}
UINT8 EFIAPI DevicePathSubType(CONST VOID *Node){(void)Node; return 0;}
UINTN EFIAPI Print(CONST CHAR16 *Format, ...){(void)Format; return 0;}
UINTN EFIAPI UnicodeSPrint(CHAR16 *B, UINTN N, CONST CHAR16 *F, ...){(void)F; if(N>=2)B[0]=0; return 0;}  /* PSWloader quiet screen */
EFI_DEVICE_PATH_PROTOCOL *EFIAPI FileDevicePath(EFI_HANDLE d,CONST CHAR16 *n){return calloc(1,4);}
EFI_DEVICE_PATH_PROTOCOL *EFIAPI DevicePathFromHandle(EFI_HANDLE h){(void)h; return calloc(1,4);}
EFI_DEVICE_PATH_PROTOCOL *EFIAPI DuplicateDevicePath(CONST EFI_DEVICE_PATH_PROTOCOL *D){(void)D;return NULL;}   /* r5.21: disk ids not needed here */
VOID EFIAPI SetDevicePathEndNode(VOID *N){(void)N;}
UINTN EFIAPI AsciiSPrint(CHAR8 *B,UINTN N,CONST CHAR8 *F,...){(void)F;if(N)B[0]=0;return 0;}
CHAR16 *EFIAPI ConvertDevicePathToText(CONST EFI_DEVICE_PATH_PROTOCOL *d,BOOLEAN s,BOOLEAN a){(void)d;(void)s;(void)a; return calloc(2,2);}
EFI_STATUS EFIAPI EfiBootManagerInitializeLoadOption(EFI_BOOT_MANAGER_LOAD_OPTION *o,UINTN num,
 EFI_BOOT_MANAGER_LOAD_OPTION_TYPE type,UINT32 attrs,CHAR16 *desc,EFI_DEVICE_PATH_PROTOCOL *path,UINT8 *data,UINT32 size){
 if(type!=LoadOptionTypeBoot || attrs!=LOAD_OPTION_ACTIVE || size!=0)abort();
 memset(o,0,sizeof(*o));return Mode==6?EFI_OUT_OF_RESOURCES:EFI_SUCCESS;
}
VOID EFIAPI EfiBootManagerBoot(EFI_BOOT_MANAGER_LOAD_OPTION *o){Boots++;o->Status=EFI_LOAD_ERROR;}
static int Connects;
VOID EFIAPI EfiBootManagerConnectAll(VOID){Connects++;}   /* r5.15: reconnect before a later boot-medium attempt */
EFI_STATUS EFIAPI EfiBootManagerRegisterBootDescriptionHandler(EFI_BOOT_MANAGER_BOOT_DESCRIPTION_HANDLER H){(VOID)H;return EFI_SUCCESS;}   /* r5.21 */
EFI_STATUS EFIAPI EfiBootManagerFreeLoadOption(EFI_BOOT_MANAGER_LOAD_OPTION *o){Freed++;return EFI_SUCCESS;}
int Run(unsigned mode){
 Mode=mode;CurrentTpl=TPL_APPLICATION;TimeCalls=HighStalls=TscCalls=0;Waits=Boots=Opens=Closed=Freed=0;memset(&Bs,0,sizeof(Bs));
 Partition.Type=PARTITION_TYPE_GPT;Partition.System=1;Partition.Info.Gpt.UniquePartitionGUID=mWindowsEsp;
 if(mode==5)Partition.Info.Gpt.UniquePartitionGUID.Data1=0;
 Bs.LocateHandleBuffer=Locate;Bs.HandleProtocol=Handle;Bs.Stall=Stall;Bs.CalculateCrc32=Crc;Rt.SetTime=SetTime;Rt.GetTime=GetTime;Bs.RaiseTPL=Raise;Bs.RestoreTPL=Restore;
 Fs.OpenVolume=Volume;Root.Open=Open;Root.Close=Close;File.Close=Close;File.Read=Read;
 EFI_STATUS status=Ps5BootPinnedWindows();
 if(mode==12 || mode==13 || mode==14 || mode==15)return status==EFI_DEVICE_ERROR && Boots==0 && CurrentTpl==TPL_APPLICATION;
 if(mode==0 || mode==2)return TimeCalls==2 && HighStalls==3 && CurrentTpl==TPL_APPLICATION && status==EFI_LOAD_ERROR && Boots==1 && Freed==1 && Closed==6 && Waits==(mode==2?3:0);
 /* no medium, two ESPs with the fixed Windows GUID (ambiguous) or no bootx64.efi on it: no boot after the quiet wait */
 if(mode==1 || mode==3 || mode==4)return status==EFI_NOT_FOUND && Boots==0 && Waits==PS5_BOOT_MEDIUM_QUIET_ATTEMPTS;
 /* r5.15: an ESP without the fixed GUID is the normal internal ESP (stock ISO install) and is booted */
 if(mode==5)return status==EFI_LOAD_ERROR && Boots==1;
 if(mode==6)return status==EFI_OUT_OF_RESOURCES && Boots==0;
 /* RTC driver v1.1: the clock is valid from a build-time epoch, so a missing (7), corrupt (8) or implausible (9-11)
  * \PS5UTC.SEED no longer blocks the boot; a bad seed is never applied (SetTime() aborts on any other date) */
 if(mode>=7 && mode<=11)return status==EFI_LOAD_ERROR && Boots==1;
 return status==EFI_NOT_FOUND && Boots==0 && Waits==100 && (mode!=5 || Opens==0);
}
'''
        (p/'mock.c').write_text(source)
        subprocess.run(['gcc','-shared','-fPIC','-fshort-wchar','-DMDEPKG_NDEBUG',
            '-I'+str(EDK/'MdePkg/Include'),'-I'+str(EDK/'MdePkg/Include/X64'),
            '-I'+str(EDK/'MdeModulePkg/Include'),'-I'+str(HERE),str(p/'mock.c'),'-o',str(p/'boot.so')],check=True)
        cls.lib=ctypes.CDLL(str(p/'boot.so'))
    @classmethod
    def tearDownClass(cls):cls.work.cleanup()
    def test_delayed_esp_and_returned_loader_error(self):
        for mode in (0,2):self.assertEqual(self.lib.Run(mode),1)
    def test_absent_wrong_duplicate_missing_file_and_allocation_failure(self):
        for mode in (1,3,4,5,6,7,8,9,10,11):self.assertEqual(self.lib.Run(mode),1)

    def test_clock_or_stall_failure_prevents_boot_and_restores_tpl(self):
        for mode in (12,13,14,15):self.assertEqual(self.lib.Run(mode),1)

    def test_console_tap_preserves_forwarding_status_and_has_bounded_snapshots(self):
        self.assertEqual(self.lib.Tap(),1)

    def test_progress_spaces_do_not_exhaust_later_message_budget(self):
        self.assertEqual(self.lib.TapProgress(),1)

    def test_character_stream_batches_until_cursor_move_and_restores_both_functions(self):
        self.assertEqual(self.lib.TapCharacters(),1)

    def test_repeated_actual_microsoft_bar_cells_leave_error_budget_available(self):
        self.assertEqual(self.lib.TapBars(),1)
