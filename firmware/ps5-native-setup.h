/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Actual firmware entry into EDK II UiApp, never a host-rendered UI.
 * Refuse the graphical path until a real GOP and console are available.
 */
#include <Protocol/GraphicsOutput.h>
/* v1 13.11.2026 (user request "fastboot 5 s"): how long the Esc / power-button window waits before the
 * UEFI boot continues, in whole 5 s ticks. build-uefi.py --setup-seconds N defines it; default unchanged (30 s). */
#ifndef PS5_QUIET_SCREEN   /* PSWloader: standalone (host test) = old screen output */
#define PS5_QUIET_SCREEN 0
#endif
#ifndef PS5_SCREEN
#define PS5_SCREEN(...) Print(__VA_ARGS__)
#endif
#ifndef PS5_SETUP_SECONDS
#define PS5_SETUP_SECONDS 30
#endif
/* r5 (16.11.): ticks of min(5, seconds) s, so --setup-seconds 1..4 really waits that long */
#define PS5_SETUP_TICK_S (PS5_SETUP_SECONDS < 5 ? PS5_SETUP_SECONDS : 5)
#define PS5_SETUP_TICKS ((PS5_SETUP_SECONDS + PS5_SETUP_TICK_S - 1) / PS5_SETUP_TICK_S)
#ifndef PS5_SETUP_TICK
#define PS5_SETUP_TICK()
#endif
#ifndef PS5_SETUP_DIAG
#define PS5_SETUP_DIAG()
#endif
/* r5.4 (16.11., menu test): the console must never stay in the menu. While UiApp runs, a pass-through on
 * the ConsoleInHandle TextInEx->ReadKeyStrokeEx (LVGL polls it every 10 ms at TPL_APPLICATION) notes the last key
 * (TSC) and after PS5_MENU_IDLE_S s without any key calls the BDS boot-now path, exactly like "Boot Windows".
 * build-uefi.py --menu-idle-seconds N (0 = off). Disarmed before any boot and when UiApp returns. */
#include <Protocol/SimpleTextInEx.h>
#include "Ps5BootNow.h"
#ifndef PS5_MENU_IDLE_S
#define PS5_MENU_IDLE_S 0   /* r5.25.3 (user, 18.11.): no automatic boot from the menu - who opened it stays until they choose */
#endif
STATIC EFI_STATUS (EFIAPI *mPs5MenuBootNow)(VOID);              /* set by the BDS before the setup window */
#if PS5_MENU_IDLE_S
STATIC EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *mPs5IdleTxt;
STATIC EFI_INPUT_READ_KEY_EX mPs5IdleOrig;
STATIC UINT64 mPs5IdleLast;
STATIC BOOLEAN mPs5IdleFired;
STATIC VOID Ps5MenuIdleDisarm(VOID) {
  if(mPs5IdleTxt!=NULL && mPs5IdleOrig!=NULL && mPs5IdleTxt->ReadKeyStrokeEx!=mPs5IdleOrig)mPs5IdleTxt->ReadKeyStrokeEx=mPs5IdleOrig;
  mPs5IdleTxt=NULL;mPs5IdleOrig=NULL;
}
STATIC EFI_STATUS EFIAPI Ps5MenuIdleRead(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This,EFI_KEY_DATA *KeyData) {
  EFI_INPUT_READ_KEY_EX Orig=mPs5IdleOrig;
  if(Orig==NULL)return EFI_NOT_READY;
  EFI_STATUS Status=Orig(This,KeyData);
  UINT64 Now=AsmReadTsc();
  if(!EFI_ERROR(Status)){mPs5IdleLast=Now;return Status;}
  if(!mPs5IdleFired && mPs5MenuBootNow!=NULL && Now-mPs5IdleLast>=(UINT64)PS5_MENU_IDLE_S*1596300000ULL) {
    mPs5IdleFired=TRUE;                                            /* once: a failed boot leaves the menu usable */
    DEBUG((DEBUG_ERROR,"PS5 boot menu: %u s without input - booting Windows\n",(UINT32)PS5_MENU_IDLE_S));
    mPs5MenuBootNow();                                             /* returns only if the boot failed */
  }
  return Status;
}
STATIC VOID Ps5MenuIdleArm(VOID) {
  STATIC EFI_GUID TxtInExGuid={0xdd9e7534,0x7762,0x4698,{0x8c,0x14,0xf5,0x85,0x17,0xa6,0x25,0xaa}};
  EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *Txt=NULL;
  if(mPs5IdleTxt!=NULL || mPs5MenuBootNow==NULL)return;
  if(EFI_ERROR(gBS->HandleProtocol(gST->ConsoleInHandle,&TxtInExGuid,(VOID **)&Txt)) || Txt==NULL || Txt->ReadKeyStrokeEx==NULL) {
    DEBUG((DEBUG_ERROR,"PS5 boot menu: no console TextInEx - idle auto-boot off\n"));
    return;
  }
  mPs5IdleTxt=Txt;mPs5IdleOrig=Txt->ReadKeyStrokeEx;Txt->ReadKeyStrokeEx=Ps5MenuIdleRead;
  mPs5IdleLast=AsmReadTsc();mPs5IdleFired=FALSE;
  DEBUG((DEBUG_ERROR,"PS5 boot menu: idle auto-boot armed (%u s)\n",(UINT32)PS5_MENU_IDLE_S));
}
#else
STATIC VOID Ps5MenuIdleDisarm(VOID) {}
STATIC VOID Ps5MenuIdleArm(VOID) {}
#endif
STATIC BOOLEAN mPs5ForceSetup;   /* r5.25.4: set by the warm re-entry hand-over */
STATIC EFI_STATUS Ps5NativeSetupEnter(EFI_BOOT_MANAGER_LOAD_OPTION *Windows) {
  EFI_STATUS Status;
  if(Windows!=NULL) {
    UINTN Count=0;
    EFI_BOOT_MANAGER_LOAD_OPTION *Options=EfiBootManagerGetLoadOptions(&Count,LoadOptionTypeBoot);
    INTN Existing=EfiBootManagerFindLoadOption(Windows,Options,Count);
    EfiBootManagerFreeLoadOptions(Options,Count);
    if(Existing<0) {
      Status=EfiBootManagerAddLoadOptionVariable(Windows,MAX_UINTN);
      if(EFI_ERROR(Status))return Status;
    }
  }
  EFI_BOOT_MANAGER_LOAD_OPTION Menu;
  Status=EfiBootManagerGetBootManagerMenu(&Menu);
  if(EFI_ERROR(Status))return Status;
  DEBUG((DEBUG_INFO,"PS5 native setup: entering actual EDK II UiApp\n"));
  Ps5MenuIdleArm();
  EfiBootManagerBoot(&Menu);
  Ps5MenuIdleDisarm();
  EfiBootManagerFreeLoadOption(&Menu);
  return EFI_SUCCESS;
}
STATIC UINT32 mPs5FastBoot;   /* r5.9: set by the BDS from \PS5UEFI.CFG (fastboot=1); r5.17: 3 s start screen, else 15 s */
STATIC UINT32 mPs5LangDe;     /* r5.17: lang=de in \PS5UEFI.CFG */
#include "ps5-boot-screen.h"
/* r5.17: DEL opens the boot menu app (BootManagerMenuApp) instead of the UiApp front page: same load option as the
 * boot manager menu (PcdBootManagerMenuFile = UiApp), FV file node switched to the BootManagerMenuApp FILE_GUID. */
STATIC EFI_STATUS Ps5NativeSetupWindow(EFI_BOOT_MANAGER_LOAD_OPTION *Windows) {
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop=NULL;
  EFI_STATUS Status=gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid,NULL,(VOID **)&Gop);
  if(EFI_ERROR(Status) || Gop==NULL || Gop->Mode==NULL || Gop->Mode->Info==NULL ||
     Gop->Mode->Info->HorizontalResolution==0 || Gop->Mode->Info->VerticalResolution==0 ||
     gST->ConOut==NULL || gST->ConIn==NULL || gST->ConIn->WaitForKey==NULL ||
     gST->ConIn->ReadKeyStroke==NULL) {
    DEBUG((DEBUG_ERROR,"PS5 native setup: graphical GOP/input prerequisite absent\n"));
    return EFI_NOT_READY;
  }
#if PS5_QUIET_SCREEN
  /* r5.17 (user 17.11.2026): PSWloader start screen, always shown. ESC (or a double power press) = UEFI BIOS (UiApp),
   * DEL (or a short power press) = boot menu, Enter = boot now; nothing pressed = default boot order after 3 s
   * (fastboot=1) or 15 s (fastboot=0). */
  for(;;) {   /* r5.25.4: the start screen comes back after the boot menu (DEL) when its boot did not start */
    BOOLEAN Again=FALSE;
    UINT32 Seconds=mPs5FastBoot?3:15;
    BOOLEAN Drawn=Ps5BootScreenDraw(Gop,mPs5LangDe!=0);
    DEBUG((DEBUG_ERROR,"PS5 start screen: %s %s, %u s (fastboot=%u, lang=%a, drawn=%u)\n",PS5_BRAND_TITLE,PS5_BRAND_VERSION,Seconds,
      mPs5FastBoot,mPs5LangDe?"de":"en",(UINT32)Drawn));
    UINT64 Hz=1596300000ULL,Start=AsmReadTsc(),Total=(UINT64)Seconds*Hz;
    UINT64 LastTick=0;
    for(;;) {
      EFI_INPUT_KEY Key;
      if(mPs5ForceSetup) {   /* r5.25.4: Windows asked for the firmware UI (shutdown /r /fw) - the Boot Manager opens like ESC */
        mPs5ForceSetup=FALSE; Ps5UsbMassAllow("OS asked for the firmware UI"); return Ps5NativeSetupEnter(Windows);
      }
      if(gST->ConIn->ReadKeyStroke(gST->ConIn,&Key)==EFI_SUCCESS) {
        DEBUG((DEBUG_ERROR,"PS5 start screen: key scan=%x char=%x\n",Key.ScanCode,Key.UnicodeChar));
        if(Key.ScanCode==SCAN_ESC){Ps5UsbMassAllow("ESC setup");return Ps5NativeSetupEnter(Windows);}   /* r5.21: list all disks */
        if(Key.ScanCode==SCAN_DELETE || Key.ScanCode==SCAN_DOWN){   /* r5.25.4 (user: DEL = a boot menu in the style of the Boot
                                                                       Manager: boot once from the chosen drive; if that boot does not
                                                                       start, back to this screen): the setup opens directly on its
                                                                       "Boot once" page (EDK2 Boot Manager formset, drawn by LVGL);
                                                                       Ps5OnceMode makes a failed boot or ESC leave the setup again */
          EFI_GUID G={0x5efc5ae0,0xb753,0x4f7a,{0xa6,0x62,0xe3,0x4d,0x15,0xa6,0xd8,0xe5}};       /* PS5_POWER_PRESET_GUID */
          EFI_GUID T={0x847bc3fe,0xb974,0x446d,{0x94,0x49,0x5a,0xd5,0x41,0x2e,0x99,0x3b}};       /* EDK2 Boot Manager formset */
          UINT8 One=1;
          gRT->SetVariable(L"Ps5SetupTarget",&G,EFI_VARIABLE_BOOTSERVICE_ACCESS,sizeof(T),&T);
          gRT->SetVariable(L"Ps5OnceMode",&G,EFI_VARIABLE_BOOTSERVICE_ACCESS,sizeof(One),&One);
          Ps5UsbMassAllow("DEL boot menu");(VOID)Ps5NativeSetupEnter(Windows);
          gRT->SetVariable(L"Ps5OnceMode",&G,EFI_VARIABLE_BOOTSERVICE_ACCESS,0,NULL);
          DEBUG((DEBUG_ERROR,"PS5 start screen: boot menu returned - start screen again\n"));
          Again=TRUE;break;
        }
        if(Key.UnicodeChar==CHAR_CARRIAGE_RETURN)return EFI_SUCCESS;
      }
      UINT64 Now=AsmReadTsc()-Start;
      if(Now>=Total)break;
      if(Drawn)Ps5BootScreenProgress(Gop,(UINTN)DivU64x64Remainder(Now*1000,Total,NULL));
      if(Now-LastTick>=Hz){PS5_SETUP_TICK();LastTick=Now;}   /* log ring persisted once per second */
      gBS->Stall(20000);
    }
    if(Again)continue;
    if(gST->ConOut!=NULL)gST->ConOut->ClearScreen(gST->ConOut);
    return EFI_SUCCESS;
  }
#endif
#if PS5_QUIET_SCREEN
  gST->ConOut->ClearScreen(gST->ConOut);
  Print(L"%s\r\n%s\r\n\r\nStarting uefi boot menu in %us...\r\n",PS5_BRAND_TITLE,PS5_BRAND_VERSION,(UINT32)(PS5_SETUP_SECONDS));
  DEBUG((DEBUG_INFO,"PS5 native setup: %s %s, Esc / power button = boot manager, UEFI boot continues in %u s; Firmware %s rev %u; UEFI %x; GOP %ux%u stride %u\n",
    PS5_BRAND_TITLE,PS5_BRAND_VERSION,(UINT32)(PS5_SETUP_SECONDS),gST->FirmwareVendor,gST->FirmwareRevision,gST->Hdr.Revision,
    Gop->Mode->Info->HorizontalResolution,Gop->Mode->Info->VerticalResolution,Gop->Mode->Info->PixelsPerScanLine));
#else
  Print(L"\r\nPSWloader\r\n");
  Print(L"Firmware: %s, revision %u; UEFI %x\r\n",gST->FirmwareVendor,gST->FirmwareRevision,gST->Hdr.Revision);
  UINT32 MaxLeaf=0;
  AsmCpuid(0x80000000,&MaxLeaf,NULL,NULL,NULL);
  if(MaxLeaf>=0x80000004) {
    UINT32 Brand[13]={0};
    for(UINT32 I=0;I<3;I++)AsmCpuid(0x80000002+I,&Brand[I*4],&Brand[I*4+1],&Brand[I*4+2],&Brand[I*4+3]);
    Print(L"CPU: %a\r\n",(CHAR8 *)Brand);
  }
  Print(L"Graphics: %u x %u; stride %u\r\n",Gop->Mode->Info->HorizontalResolution,
    Gop->Mode->Info->VerticalResolution,Gop->Mode->Info->PixelsPerScanLine);
#if PS5_SETUP_SECONDS == 30
  Print(L"Esc / power button: UEFI setup and boot manager | UEFI boot continues in 30 seconds\r\n");
#else
  Print(L"Esc / power button: UEFI setup and boot manager | UEFI boot continues in %u seconds\r\n",(UINT32)(PS5_SETUP_TICKS*5));
#endif
#endif /* PS5_QUIET_SCREEN */
  EFI_EVENT Timer=NULL;
  Status=gBS->CreateEvent(EVT_TIMER,TPL_CALLBACK,NULL,NULL,&Timer);
  if(EFI_ERROR(Status))return Status;
  Status=gBS->SetTimer(Timer,TimerPeriodic,(UINT64)PS5_SETUP_TICK_S*10000000ULL);
  if(EFI_ERROR(Status)){gBS->CloseEvent(Timer);return Status;}
  EFI_EVENT Events[2]={gST->ConIn->WaitForKey,Timer};
  UINTN Ticks=0;
#ifdef PS5_SETUP_POLL
  /* v2.56: native BDS timer events stall after the GPU path; wait by TSC and
   * poll ConIn directly instead of WaitForEvent (which would HLT forever). */
  PS5_SETUP_DIAG();
  {
    UINT64 Hz=1596300000ULL,Start=AsmReadTsc(),LastTick=Start;
    for(;;) {
      EFI_INPUT_KEY Key;
      if(gST->ConIn->ReadKeyStroke(gST->ConIn,&Key)==EFI_SUCCESS) {
        PS5_SCREEN(L"[key scan=%x char=%x]\r\n",Key.ScanCode,Key.UnicodeChar);
        if(Key.ScanCode==SCAN_ESC || Key.ScanCode==SCAN_DOWN || Key.UnicodeChar==CHAR_CARRIAGE_RETURN) {
          gBS->CloseEvent(Timer);
          return Ps5NativeSetupEnter(Windows);
        }
      }
      UINT64 Now=AsmReadTsc();
      if(Now-LastTick>(UINT64)PS5_SETUP_TICK_S*Hz){PS5_SETUP_TICK();LastTick=Now;if(++Ticks>=PS5_SETUP_TICKS)break;}
      gBS->Stall(20000);
    }
    gBS->CloseEvent(Timer);
    return EFI_SUCCESS;
  }
#endif
  for(;;) {
    UINTN Index=0;
    Status=gBS->WaitForEvent(2,Events,&Index);
    if(EFI_ERROR(Status))break;
    if(Index==1) {
      /* v2.54: 5 s tick - persist the log ring (button status lines), 30 s total. */
      PS5_SETUP_TICK();
      if(++Ticks>=PS5_SETUP_TICKS)break;
      continue;
    }
    EFI_INPUT_KEY Key;
    Status=gST->ConIn->ReadKeyStroke(gST->ConIn,&Key);
    if(Status==EFI_NOT_READY)continue;
    if(EFI_ERROR(Status))break;
    PS5_SCREEN(L"[key scan=%x char=%x]\r\n",Key.ScanCode,Key.UnicodeChar);
    if(Key.ScanCode!=SCAN_ESC && Key.ScanCode!=SCAN_DOWN && Key.UnicodeChar!=CHAR_CARRIAGE_RETURN)continue;
    gBS->CloseEvent(Timer);Timer=NULL;
    return Ps5NativeSetupEnter(Windows);
  }
  if(Timer!=NULL)gBS->CloseEvent(Timer);
  return Status;
}
