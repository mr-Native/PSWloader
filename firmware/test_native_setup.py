import os
"""Execute native UEFI Esc -> actual boot-manager UiApp dispatch and cleanup."""
import subprocess,tempfile,unittest
from pathlib import Path
WORK = Path(os.environ.get('PSW_WORK', '/opt/pswloader-work'))   # build tree (EDK2, testdata); set PSW_WORK for your own
HERE=Path(__file__).parent.resolve()
EDK=(WORK / 'edk2')
class NativeSetupTests(unittest.TestCase):
 def test_native_dispatch_requires_graphics_and_preserves_timeout_and_cleanup(self):
  code=r'''
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <assert.h>
#include <string.h>
static EFI_BOOT_SERVICES Bs;EFI_BOOT_SERVICES *gBS=&Bs;
static EFI_SYSTEM_TABLE St;EFI_SYSTEM_TABLE *gST=&St;
static int SetupTab=-1,TargetSet,OnceOn,OnceOff;static EFI_STATUS EFIAPI SetVar(CHAR16 *N,EFI_GUID *G,UINT32 A,UINTN S,VOID *D){(void)N;(void)G;(void)A;if(S==16)TargetSet=((UINT8*)D)[0]==0xfe;if(S==1)OnceOn++;if(S==0)OnceOff++;SetupTab=TargetSet&&OnceOn?3:-1;return EFI_SUCCESS;}   /* r5.25.4 */
static EFI_RUNTIME_SERVICES Rs;EFI_RUNTIME_SERVICES *gRT=&Rs;
EFI_GUID gEfiGraphicsOutputProtocolGuid={1};
static int Mode,Prints,Closes,Waits,Adds,Boots,Freed;
UINTN EFIAPI Print(CONST CHAR16 *Format,...){Prints++;return 1;}
UINT32 EFIAPI AsmCpuid(UINT32 Leaf,UINT32 *a,UINT32 *b,UINT32 *c,UINT32 *d){if(a)*a=0;return 0;}
UINT64 EFIAPI AsmReadTsc(VOID){return 0;}   /* r5.4 menu idle */
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
/* r5.17: start screen (ps5-boot-screen.h) and boot-menu entry need these library calls */
VOID EFIAPI FreePool(VOID *P){(void)P;}
GUID *EFIAPI CopyGuid(GUID *D,CONST GUID *S){memcpy(D,S,sizeof(GUID));return D;}
UINT8 EFIAPI DevicePathType(CONST VOID *N){return ((CONST EFI_DEVICE_PATH_PROTOCOL *)N)->Type;}
UINT8 EFIAPI DevicePathSubType(CONST VOID *N){return ((CONST EFI_DEVICE_PATH_PROTOCOL *)N)->SubType;}
BOOLEAN EFIAPI IsDevicePathEnd(CONST VOID *N){return DevicePathType(N)==END_DEVICE_PATH_TYPE && DevicePathSubType(N)==END_ENTIRE_DEVICE_PATH_SUBTYPE;}
EFI_DEVICE_PATH_PROTOCOL *EFIAPI NextDevicePathNode(CONST VOID *N){CONST UINT8 *P=N;return (EFI_DEVICE_PATH_PROTOCOL *)(P+(P[2]|(P[3]<<8)));}
UINT64 EFIAPI DivU64x64Remainder(UINT64 A,UINT64 B,UINT64 *R){if(R)*R=B?A%B:0;return B?A/B:0;}
#include "ps5-native-setup.h"
static EFI_SIMPLE_TEXT_INPUT_PROTOCOL Input;
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL Output;
static EFI_GRAPHICS_OUTPUT_MODE_INFORMATION Info;
static EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE GopMode;
static EFI_GRAPHICS_OUTPUT_PROTOCOL Gop;
static EFI_STATUS EFIAPI Locate(EFI_GUID *Guid,VOID *Registration,VOID **Out){*Out=&Gop;return Mode==0?EFI_NOT_FOUND:EFI_SUCCESS;}
static EFI_STATUS EFIAPI Create(UINT32 Type,EFI_TPL Tpl,EFI_EVENT_NOTIFY Notify,VOID *Context,EFI_EVENT *Event){assert(Type==EVT_TIMER && Notify==NULL);*Event=(EFI_EVENT)2;return Mode==3?EFI_OUT_OF_RESOURCES:EFI_SUCCESS;}
static EFI_STATUS EFIAPI Set(EFI_EVENT Event,EFI_TIMER_DELAY Type,UINT64 Trigger){assert(Event==(VOID*)2 && Type==TimerPeriodic && Trigger==50000000);return Mode==4?EFI_DEVICE_ERROR:EFI_SUCCESS;}
static EFI_STATUS EFIAPI Close(EFI_EVENT Event){assert(Event==(VOID*)2);Closes++;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Wait(UINTN Number,EFI_EVENT *Events,UINTN *Index){
 assert(Number==2 && Events[0]==(VOID*)1 && Events[1]==(VOID*)2);Waits++;
 *Index=Mode==1 || ((Mode==8 || Mode==9) && Waits>1)?1:0;
 return Mode==11?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Read(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,EFI_INPUT_KEY *Key){Key->UnicodeChar=0;Key->ScanCode=Mode==8?SCAN_F2:SCAN_ESC;return Mode==9?EFI_NOT_READY:EFI_SUCCESS;}
EFI_BOOT_MANAGER_LOAD_OPTION *EFIAPI EfiBootManagerGetLoadOptions(UINTN *Count,EFI_BOOT_MANAGER_LOAD_OPTION_TYPE Type){assert(Type==LoadOptionTypeBoot);*Count=0;return NULL;}
INTN EFIAPI EfiBootManagerFindLoadOption(CONST EFI_BOOT_MANAGER_LOAD_OPTION *Key,CONST EFI_BOOT_MANAGER_LOAD_OPTION *Array,UINTN Count){return Mode==7?0:-1;}
EFI_STATUS EFIAPI EfiBootManagerFreeLoadOptions(EFI_BOOT_MANAGER_LOAD_OPTION *Options,UINTN Count){assert(Options==NULL && Count==0);return EFI_SUCCESS;}
EFI_STATUS EFIAPI EfiBootManagerAddLoadOptionVariable(EFI_BOOT_MANAGER_LOAD_OPTION *Option,UINTN Position){assert(Position==MAX_UINTN);Adds++;return Mode==6?EFI_DEVICE_ERROR:EFI_SUCCESS;}
EFI_STATUS EFIAPI EfiBootManagerGetBootManagerMenu(EFI_BOOT_MANAGER_LOAD_OPTION *Option){Option->OptionNumber=42;return Mode==5?EFI_NOT_FOUND:EFI_SUCCESS;}
VOID EFIAPI EfiBootManagerBoot(EFI_BOOT_MANAGER_LOAD_OPTION *Option){assert(Option->OptionNumber==42);Boots++;}
EFI_STATUS EFIAPI EfiBootManagerFreeLoadOption(EFI_BOOT_MANAGER_LOAD_OPTION *Option){assert(Option->OptionNumber==42);Freed++;return EFI_SUCCESS;}
int main(void){
 Bs.LocateProtocol=Locate;Bs.CreateEvent=Create;Bs.SetTimer=Set;Bs.CloseEvent=Close;Bs.WaitForEvent=Wait;
 Input.WaitForKey=(VOID*)1;Input.ReadKeyStroke=Read;St.ConOut=&Output;Info.HorizontalResolution=1920;Info.VerticalResolution=1080;Info.PixelsPerScanLine=1920;GopMode.Info=&Info;Gop.Mode=&GopMode;
 EFI_BOOT_MANAGER_LOAD_OPTION Windows={0};
 for(Mode=0;Mode<=11;Mode++) {
  St.ConIn=Mode==10?NULL:&Input;Prints=Closes=Waits=Adds=Boots=Freed=0;
  EFI_STATUS Status=Ps5NativeSetupWindow(&Windows);
  if(Mode==0 || Mode==10){assert(Status==EFI_NOT_READY && Prints==0 && Closes==0 && Boots==0);continue;}
  assert(Prints>=4);
  if(Mode==3){assert(Status==EFI_OUT_OF_RESOURCES && Closes==0);continue;}
  assert(Closes==1);
  if(Mode==1 || Mode==8 || Mode==9){assert(Status==EFI_SUCCESS && Boots==0 && Adds==0);continue;}
  if(Mode==4 || Mode==5 || Mode==6 || Mode==11){assert(EFI_ERROR(Status) && Boots==0 && Freed==0);continue;}
  assert(Status==EFI_SUCCESS && Boots==1 && Freed==1 && Adds==(Mode==7?0:1));
 }
 Mode=2;St.ConIn=&Input;Prints=Closes=Waits=Adds=Boots=Freed=0;
 assert(Ps5NativeSetupWindow(NULL)==EFI_SUCCESS && Boots==1 && Adds==0 && Closes==1);
}
'''
  with tempfile.TemporaryDirectory() as temp:
   p=Path(temp);(p/'main.c').write_text(code)
   subprocess.run(['gcc','-fshort-wchar','-DMDEPKG_NDEBUG','-I'+str(EDK/'MdePkg/Include'),'-I'+str(EDK/'MdePkg/Include/X64'),'-I'+str(EDK/'MdeModulePkg/Include'),'-I'+str(HERE),'-I'+str(HERE/'Ps5PlatformPkg/Include'),str(p/'main.c'),'-o',str(p/'test')],check=True)
   subprocess.run([str(p/'test')],check=True)


START_SCREEN = r'''
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <assert.h>
#include <string.h>
#define PS5_QUIET_SCREEN 1
#define PS5_MENU_IDLE_S 0
#define PS5_BRAND_TITLE L"PSWloader"
#define PS5_BRAND_VERSION L"v1.0.5"
static unsigned UsbAllow; static void Ps5UsbMassAllow(const char *Why){(void)Why;UsbAllow++;}   /* r5.21 */
static EFI_BOOT_SERVICES Bs;EFI_BOOT_SERVICES *gBS=&Bs;
static EFI_SYSTEM_TABLE St;EFI_SYSTEM_TABLE *gST=&St;
static int SetupTab=-1,TargetSet,OnceOn,OnceOff;static EFI_STATUS EFIAPI SetVar(CHAR16 *N,EFI_GUID *G,UINT32 A,UINTN S,VOID *D){(void)N;(void)G;(void)A;if(S==16)TargetSet=((UINT8*)D)[0]==0xfe;if(S==1)OnceOn++;if(S==0)OnceOff++;SetupTab=TargetSet&&OnceOn?3:-1;return EFI_SUCCESS;}   /* r5.25.4 */
static EFI_RUNTIME_SERVICES Rs;EFI_RUNTIME_SERVICES *gRT=&Rs;
EFI_GUID gEfiGraphicsOutputProtocolGuid={1};
static UINT64 Tsc;static int Adds,Boots,Freed,Clears,Blts,NoGop,KeyAt,Key;
static EFI_GUID BootedFv;
UINTN EFIAPI Print(CONST CHAR16 *Format,...){return 1;}
UINT32 EFIAPI AsmCpuid(UINT32 Leaf,UINT32 *a,UINT32 *b,UINT32 *c,UINT32 *d){if(a)*a=0;return 0;}
UINT64 EFIAPI AsmReadTsc(VOID){return Tsc;}
VOID EFIAPI FreePool(VOID *P){(void)P;}
GUID *EFIAPI CopyGuid(GUID *D,CONST GUID *S){memcpy(D,S,sizeof(GUID));return D;}
UINT8 EFIAPI DevicePathType(CONST VOID *N){return ((CONST EFI_DEVICE_PATH_PROTOCOL *)N)->Type;}
UINT8 EFIAPI DevicePathSubType(CONST VOID *N){return ((CONST EFI_DEVICE_PATH_PROTOCOL *)N)->SubType;}
BOOLEAN EFIAPI IsDevicePathEnd(CONST VOID *N){return DevicePathType(N)==END_DEVICE_PATH_TYPE && DevicePathSubType(N)==END_ENTIRE_DEVICE_PATH_SUBTYPE;}
EFI_DEVICE_PATH_PROTOCOL *EFIAPI NextDevicePathNode(CONST VOID *N){CONST UINT8 *P=N;return (EFI_DEVICE_PATH_PROTOCOL *)(P+(P[2]|(P[3]<<8)));}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
UINT64 EFIAPI DivU64x64Remainder(UINT64 A,UINT64 B,UINT64 *R){if(R)*R=B?A%B:0;return B?A/B:0;}
#include "ps5-native-setup.h"
static EFI_GRAPHICS_OUTPUT_PROTOCOL Gop;static EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE GopMode;static EFI_GRAPHICS_OUTPUT_MODE_INFORMATION Info;
static EFI_SIMPLE_TEXT_INPUT_PROTOCOL Input;static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL Output;
static struct {MEDIA_FW_VOL_FILEPATH_DEVICE_PATH Fv;EFI_DEVICE_PATH_PROTOCOL End;} MenuPath;
static EFI_STATUS EFIAPI Locate(EFI_GUID *Guid,VOID *Registration,VOID **Out){
 if(CompareMem(Guid,&gEfiGraphicsOutputProtocolGuid,sizeof(EFI_GUID))==0){*Out=NoGop?NULL:&Gop;return NoGop?EFI_NOT_FOUND:EFI_SUCCESS;}
 *Out=NULL;return EFI_NOT_FOUND;}   /* no HII font: the screen keeps its gradient, the countdown still runs */
static EFI_STATUS EFIAPI Stall(UINTN Us){Tsc+=(UINT64)Us*1596ULL+(UINT64)Us*3/10;return EFI_SUCCESS;}   /* 1596.3 MHz */
static EFI_STATUS EFIAPI Blt(EFI_GRAPHICS_OUTPUT_PROTOCOL *T,EFI_GRAPHICS_OUTPUT_BLT_PIXEL *B,EFI_GRAPHICS_OUTPUT_BLT_OPERATION O,UINTN a,UINTN b,UINTN c,UINTN d,UINTN e,UINTN f,UINTN g){Blts++;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Clear(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *T){Clears++;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Cursor(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *T,BOOLEAN V){return EFI_SUCCESS;}
static int Reads;
static EFI_STATUS EFIAPI Read(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,EFI_INPUT_KEY *K){
 if(KeyAt<0 || Reads++<KeyAt)return EFI_NOT_READY;
 K->ScanCode=Key>0xffff?0:(UINT16)Key;K->UnicodeChar=Key>0xffff?(CHAR16)(Key&0xffff):0;KeyAt=-1;return EFI_SUCCESS;}
EFI_BOOT_MANAGER_LOAD_OPTION *EFIAPI EfiBootManagerGetLoadOptions(UINTN *Count,EFI_BOOT_MANAGER_LOAD_OPTION_TYPE Type){*Count=0;return NULL;}
INTN EFIAPI EfiBootManagerFindLoadOption(CONST EFI_BOOT_MANAGER_LOAD_OPTION *Key,CONST EFI_BOOT_MANAGER_LOAD_OPTION *Array,UINTN Count){return -1;}
EFI_STATUS EFIAPI EfiBootManagerFreeLoadOptions(EFI_BOOT_MANAGER_LOAD_OPTION *Options,UINTN Count){return EFI_SUCCESS;}
EFI_STATUS EFIAPI EfiBootManagerAddLoadOptionVariable(EFI_BOOT_MANAGER_LOAD_OPTION *Option,UINTN Position){Adds++;return EFI_SUCCESS;}
EFI_STATUS EFIAPI EfiBootManagerGetBootManagerMenu(EFI_BOOT_MANAGER_LOAD_OPTION *Option){
 static EFI_GUID UiApp={0x462caa21,0x7614,0x4503,{0x83,0x6e,0x8a,0xb6,0xf4,0x66,0x23,0x31}};
 MenuPath.Fv.Header.Type=MEDIA_DEVICE_PATH;MenuPath.Fv.Header.SubType=MEDIA_PIWG_FW_FILE_DP;
 MenuPath.Fv.Header.Length[0]=sizeof(MenuPath.Fv);MenuPath.Fv.FvFileName=UiApp;
 MenuPath.End.Type=END_DEVICE_PATH_TYPE;MenuPath.End.SubType=END_ENTIRE_DEVICE_PATH_SUBTYPE;MenuPath.End.Length[0]=4;
 Option->OptionNumber=42;Option->FilePath=(EFI_DEVICE_PATH_PROTOCOL *)&MenuPath;return EFI_SUCCESS;}
VOID EFIAPI EfiBootManagerBoot(EFI_BOOT_MANAGER_LOAD_OPTION *Option){assert(Option->OptionNumber==42);BootedFv=MenuPath.Fv.FvFileName;Boots++;}
EFI_STATUS EFIAPI EfiBootManagerFreeLoadOption(EFI_BOOT_MANAGER_LOAD_OPTION *Option){Freed++;return EFI_SUCCESS;}
static EFI_STATUS Run(int Fast,int At,int K,EFI_BOOT_MANAGER_LOAD_OPTION *W){
 Tsc=1;Adds=Boots=Freed=Clears=Blts=Reads=0;KeyAt=At;Key=K;mPs5FastBoot=Fast;memset(&BootedFv,0,sizeof(BootedFv));
 return Ps5NativeSetupWindow(W);}
int main(void){
 Rs.SetVariable=SetVar;Bs.LocateProtocol=Locate;Bs.Stall=Stall;Input.WaitForKey=(VOID*)1;Input.ReadKeyStroke=Read;St.ConIn=&Input;
 Output.ClearScreen=Clear;Output.EnableCursor=Cursor;St.ConOut=&Output;
 Info.HorizontalResolution=3440;Info.VerticalResolution=1440;Info.PixelsPerScanLine=3456;GopMode.Info=&Info;Gop.Mode=&GopMode;Gop.Blt=Blt;
 EFI_BOOT_MANAGER_LOAD_OPTION Windows={0};
 const UINT64 Hz=1596300000ULL;
 NoGop=1;assert(Run(0,-1,0,&Windows)==EFI_NOT_READY && Boots==0);NoGop=0;
 /* nothing pressed: default boot after 15 s, or 3 s with fastboot; the screen was drawn and is cleared at the end */
 assert(Run(0,-1,0,&Windows)==EFI_SUCCESS && Boots==0 && Clears>=1 && Blts>=64 && Tsc>=15*Hz && Tsc<16*Hz);
 assert(Run(1,-1,0,&Windows)==EFI_SUCCESS && Boots==0 && Tsc>=3*Hz && Tsc<4*Hz);
 /* ESC: UEFI BIOS (UiApp) - the Windows option is added when missing, the boot manager menu is booted */
 assert(Run(0,5,SCAN_ESC,&Windows)==EFI_SUCCESS && Adds==1 && Boots==1 && Freed==1 && Tsc<Hz);
 static EFI_GUID UiAppG={0x462caa21,0x7614,0x4503,{0x83,0x6e,0x8a,0xb6,0xf4,0x66,0x23,0x31}};
 /* r5.25.4: DEL and Down: the boot menu = setup (UiApp) on the EDK2 Boot Manager page (Ps5SetupTarget) in boot menu mode
  * (Ps5OnceMode set, cleared afterwards); when it returns, the start screen runs again (here: its countdown ends) */
 TargetSet=OnceOn=OnceOff=0;assert(Run(0,5,SCAN_DELETE,&Windows)==EFI_SUCCESS && Boots==1 && TargetSet && OnceOn==1 && OnceOff==1 && CompareMem(&BootedFv,&UiAppG,sizeof(EFI_GUID))==0 && Tsc>=15*Hz);
 TargetSet=OnceOn=OnceOff=0;assert(Run(0,5,SCAN_DOWN,&Windows)==EFI_SUCCESS && Boots==1 && TargetSet && OnceOn==1 && OnceOff==1);
 /* Enter: boot now, no menu */
 assert(Run(0,5,0x10000|CHAR_CARRIAGE_RETURN,&Windows)==EFI_SUCCESS && Boots==0 && Adds==0 && Tsc<Hz);
 return 0;
}
'''


@unittest.skipUnless((EDK / 'MdePkg/Include/Uefi.h').exists(), 'EDK headers required')
class StartScreenTests(unittest.TestCase):
    """r5.17 start screen (PS5_QUIET_SCREEN, the production path): countdown, ESC / DEL / Down / Enter."""
    def test_start_screen_keys_and_countdown(self):
        with tempfile.TemporaryDirectory() as temp:
            p = Path(temp); (p / 'main.c').write_text(START_SCREEN)
            subprocess.run(['gcc', '-fshort-wchar', '-DMDEPKG_NDEBUG', '-I' + str(EDK / 'MdePkg/Include'),
                            '-I' + str(EDK / 'MdePkg/Include/X64'), '-I' + str(EDK / 'MdeModulePkg/Include'), '-I' + str(HERE),
                            '-I' + str(HERE / 'Ps5PlatformPkg/Include'), str(p / 'main.c'), '-o', str(p / 'test')], check=True)
            subprocess.run([str(p / 'test')], check=True)


if __name__ == '__main__':
    unittest.main()
