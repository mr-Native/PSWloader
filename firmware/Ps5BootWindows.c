/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Wait for the pinned Windows ESP before boot selection, not after it.
 */
#ifndef PS5_WARM_LOGCELL
#define PS5_WARM_LOGCELL 0x111806380ULL   /* r5.25.11: "WARMLOG1" while a warm re-entry runs (host tests point it at a variable) */
#endif
#include <Uefi.h>
#include <Guid/EventGroup.h>
#include <Protocol/PartitionInfo.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/BlockIo.h>
#include <Protocol/BlockIo2.h>
#include <Protocol/DevicePath.h>
#include <Protocol/UsbIo.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/UefiLib.h>
#include <Protocol/SimpleTextOut.h>
#include "ps5-time-calendar.h"
#include <Guid/GlobalVariable.h>   /* r5.27.4: BootNext is deleted after a whole-disk boot */
#include "ps5-progress.h"    /* r5.27.2: progress cell for PSWloader.elf (GitHub issue #14); outside PS5_NATIVE_SETUP, the boot path uses it */
#ifndef PS5_FW_RELEASE   /* lab builds (--lab-rev) set their own */
#define PS5_FW_RELEASE "r5.28.14"   /* r5.20.7: the one firmware revision string (diag marker, PS5DIAG.LOG, r5.25.13: SMBIOS BIOS version); bump with every build */
#endif
#include "ps5-extapic.h"
#ifdef PS5_WARM_BS_TRACE
#include "ps5-bs-trace.h"
STATIC BOOLEAN mPs5BsTraceWarm;   /* lab: set by Ps5WarmReadRecord on a warm run */
#endif

/* Diagnostic: log the firmware device tree bootmgfw enumerates (DEVICE_PATH +
 * BLOCK_IO handles) right before the OS launch, to find why bootmgfw 28000's
 * BlFwEnumerateDevice returns 0xC0000225 on our UEFI while 26100 did not.
 * Read-only; comment out to drop it from the image. */
#include <Library/PrintLib.h>
/* PSWloader v1.0 (15.11.2026, user request): quiet screen - title, version and ONE countdown line only.
 * Diagnostic screen lines go to the log (DEBUG) instead; the log export itself is unchanged. 0 = old screen output. */
#ifndef PS5_QUIET_SCREEN
#define PS5_QUIET_SCREEN 1
#endif
#define PS5_BRAND_TITLE L"PSWloader"   /* r5.17: product name (user 17.11.2026) */
#define PS5_BRAND_VERSION L"v1.2"
#if PS5_QUIET_SCREEN
#define PS5_SCREEN(...) do{CHAR16 Ps5Q_[256];UnicodeSPrint(Ps5Q_,sizeof(Ps5Q_),__VA_ARGS__);DEBUG((DEBUG_INFO,"PS5 screen (quiet): %s",Ps5Q_));}while(0)
#else
#define PS5_SCREEN(...) Print(__VA_ARGS__)
#endif
/* #define PS5_FWENUM_DIAG 1   -- off since 16.11.: its ConnectAll before bootmgr cost 47 s with the flaky USB bridge attached; not needed for the NVMe boot */

#ifdef PS5_NATIVE_SETUP
#ifndef PS5_TS
#ifdef PS5_BOOT_TS   /* build-uefi.py --boot-ts (16.11.): boot timing, ms since TSC reset */
#define PS5_TS(Tag) DEBUG((DEBUG_ERROR,"PS5 TS %a %Lu ms\n",(CHAR8 *)(Tag),(UINT64)(AsmReadTsc()/1596300ULL)))
#else
#define PS5_TS(Tag)
#endif
#endif
STATIC VOID ConsoleSnapshot(VOID);
#define PS5_MMIO32(a) (*(volatile UINT32 *)(UINTN)(a))
#define PS5_MMIO64(a) (*(volatile UINT64 *)(UINTN)(a))
STATIC VOID Ps5TimerDiag(VOID) {
  EFI_TPL Tpl=gBS->RaiseTPL(TPL_HIGH_LEVEL);gBS->RestoreTPL(Tpl);
  BOOLEAN If=GetInterruptState();
  UINT64 ApicBase=AsmReadMsr64(0x1B)&0xFFFFF000ULL;
  UINT32 Lvt=PS5_MMIO32(ApicBase+0x320),C0=PS5_MMIO32(ApicBase+0x390);
  UINT64 H0=PS5_MMIO64(0xFED000F0ULL);UINT64 Hcfg=PS5_MMIO64(0xFED00010ULL);
  gBS->Stall(10000);
  UINT32 C1=PS5_MMIO32(ApicBase+0x390);UINT64 H1=PS5_MMIO64(0xFED000F0ULL);
  PS5_SCREEN(L"[diag tpl=%u if=%u apic=%lx lvt=%x cnt=%x->%x hpetcfg=%lx hpet=%lx->%lx]\r\n",Tpl,If,ApicBase,Lvt,C0,C1,Hcfg,H0,H1);
  PS5_SCREEN(L"[extapic applied=%u efeat=%x ectrl=%x ierC0=%x->%x ier20=%x]\r\n",mPs5ExtApic.Applied,mPs5ExtApic.Efeat,mPs5ExtApic.Ectrl,mPs5ExtApic.IerC0Before,mPs5ExtApic.IerC0After,mPs5ExtApic.Ier20After);
  /* v1.1: passive input proof - every SimpleTextIn handle with its device path (USB keyboard = ...USB(..)) */
  EFI_HANDLE *In=NULL;UINTN N=0;
  if(!EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol,&gEfiSimpleTextInProtocolGuid,NULL,&N,&In))) {
    for(UINTN I=0;I<N;I++) {
      CHAR16 *T=ConvertDevicePathToText(DevicePathFromHandle(In[I]),TRUE,TRUE);
      PS5_SCREEN(L"[textin %u/%u %s]\r\n",(UINT32)(I+1),(UINT32)N,T?T:L"(no path)");
      DEBUG((DEBUG_INFO,"PS5 text-in %u/%u: %s\n",(UINT32)(I+1),(UINT32)N,T?T:L"(no path)"));
      if(T)FreePool(T);
    }
    FreePool(In);
  }
  DEBUG((DEBUG_INFO,"PS5 timer diag: tpl=%u if=%u apic=%lx lvt=%x cnt=%x->%x hpetcfg=%lx hpet=%lx->%lx\n",Tpl,If,ApicBase,Lvt,C0,C1,Hcfg,H0,H1));
}
#define PS5_SETUP_TICK() ConsoleSnapshot()
#define PS5_SETUP_DIAG() Ps5TimerDiag()
#define PS5_SETUP_POLL 1
STATIC VOID Ps5UsbMassAllow(CONST CHAR8 *Why);   /* r5.21: before the boot menu / setup - all USB disks bound and listed */
#include "ps5-native-setup.h"
#include "ps5-native-gpu-probe.h"
#ifdef PS5_NATIVE_DP_SOURCE
#include "ps5-native-dp-source.h"
#endif
#endif

STATIC EFI_GUID mWindowsEsp={0xbd21c511,0x2781,0x4058,{0xb8,0x47,0xf6,0x25,0xa7,0xec,0xcb,0xb6}};
STATIC CHAR16 mWindowsPath[]=L"\\EFI\\Boot\\bootx64.efi";
#ifdef PS5_NATIVE_SETUP
#include "ps5-vbios-export.h"
#include "ps5-wifi-fw-export.h"
#endif

STATIC EFI_TEXT_STRING mConsoleOutput;
STATIC EFI_TEXT_SET_CURSOR_POSITION mConsoleCursor;
STATIC UINTN mConsoleRequests,mConsoleLineLength;
STATIC BOOLEAN mConsoleBusy,mConsoleBlankSaved;
STATIC CHAR16 mConsoleLine[193];
STATIC EFI_GUID mConsoleSnapshotGuid={0x873f7798,0xe801,0x4ec4,{0x86,0x97,0x30,0xb7,0xb3,0xa1,0xfa,0x52}};
STATIC VOID ConsoleSnapshot(VOID) {
  EFI_TPL Previous=gBS->RaiseTPL(TPL_HIGH_LEVEL);
  gBS->RestoreTPL(Previous);
  if(Previous<=TPL_CALLBACK) EfiEventGroupSignal(&mConsoleSnapshotGuid);
}
#include "ps5-post-wim.h"
STATIC VOID FlushConsoleLine(VOID) {
  if(mConsoleLineLength==0 || mConsoleRequests>=80) return;
  // The actual Microsoft text-mode progress bar consists only of lowercase
  // 'l' cells and padding. It is not a diagnostic line and must not consume
  // the budget reserved for a later loader/error message.
  BOOLEAN ProgressOnly=TRUE;
  for(UINTN I=0;I<mConsoleLineLength;I++) {
    CHAR16 Ch=mConsoleLine[I];
    if(Ch!='l' && Ch!=' ' && Ch!='\t') { ProgressOnly=FALSE;break; }
  }
  if(ProgressOnly) { mConsoleLineLength=0;return; }
  mConsoleLine[mConsoleLineLength]=0;mConsoleRequests++;
  DEBUG((DEBUG_ERROR,"PS5 Windows console line %u: %s\n",(UINT32)mConsoleRequests,mConsoleLine));
  mConsoleLineLength=0;ConsoleSnapshot();
}
STATIC EFI_STATUS EFIAPI WindowsConsoleOutput(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,CHAR16 *Text) {
  if(mConsoleOutput==NULL) return EFI_NOT_READY;
  if(mConsoleBusy || mConsoleRequests>=80) return mConsoleOutput(This,Text);
  mConsoleBusy=TRUE;
  EFI_STATUS Status=mConsoleOutput(This,Text);
  if(Text!=NULL) {
    for(UINTN I=0;I<4096 && Text[I]!=0 && mConsoleRequests<80;I++) {
      CHAR16 Ch=Text[I];
      if(Ch=='\r' || Ch=='\n') { FlushConsoleLine();continue; }
      if(mConsoleLineLength==0 && (Ch==' ' || Ch=='\t')) {
        // Keep one early progress snapshot, never one per printed character.
        if(!mConsoleBlankSaved && mConsoleRequests==0) {
          mConsoleBlankSaved=TRUE;mConsoleRequests++;
          DEBUG((DEBUG_INFO,"PS5 Windows console: initial progress output\n"));
          ConsoleSnapshot();
        }
        continue;
      }
      mConsoleLine[mConsoleLineLength++]=Ch;
      if(mConsoleLineLength==192) FlushConsoleLine();
    }
  }
  if(EFI_ERROR(Status)) {
    DEBUG((DEBUG_ERROR,"PS5 Windows console returned %r\n",Status));
    FlushConsoleLine();
  }
  mConsoleBusy=FALSE;return Status;
}
STATIC EFI_STATUS EFIAPI WindowsConsoleCursor(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,UINTN Column,UINTN Row) {
  if(!mConsoleBusy) { mConsoleBusy=TRUE;FlushConsoleLine();mConsoleBusy=FALSE; }
  return mConsoleCursor(This,Column,Row);
}
STATIC EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *InstallWindowsConsoleTap(VOID) {
  if(gST==NULL || gST->ConOut==NULL || gST->ConOut->OutputString==NULL ||
     gST->ConOut->OutputString==WindowsConsoleOutput) return NULL;
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *Console=gST->ConOut;
  mConsoleOutput=Console->OutputString;mConsoleCursor=Console->SetCursorPosition;
  mConsoleRequests=0;mConsoleLineLength=0;mConsoleBusy=FALSE;mConsoleBlankSaved=FALSE;
  Console->OutputString=WindowsConsoleOutput;
  if(mConsoleCursor!=NULL) Console->SetCursorPosition=WindowsConsoleCursor;
  return Console;
}
STATIC VOID RestoreWindowsConsoleTap(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *Console) {
  FlushConsoleLine();
  if(Console!=NULL && Console->OutputString==WindowsConsoleOutput)
    Console->OutputString=mConsoleOutput;
  if(Console!=NULL && Console->SetCursorPosition==WindowsConsoleCursor)
    Console->SetCursorPosition=mConsoleCursor;
}

STATIC UINT64 Read64(CONST UINT8 *P) {
  UINT64 V=0;for(UINTN I=0;I<8;I++) V|=(UINT64)P[I]<<(8*I);return V;
}
STATIC UINT32 Read32(CONST UINT8 *P) {
  UINT32 V=0;for(UINTN I=0;I<4;I++) V|=(UINT32)P[I]<<(8*I);return V;
}
STATIC EFI_STATUS SeedWindowsTime(EFI_HANDLE Handle) {
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;EFI_FILE_PROTOCOL *Root=NULL,*File=NULL;
  EFI_STATUS Status=gBS->HandleProtocol(Handle,&gEfiSimpleFileSystemProtocolGuid,(VOID **)&Fs);
  if(EFI_ERROR(Status)) return Status;
  Status=Fs->OpenVolume(Fs,&Root);if(EFI_ERROR(Status)) return Status;
  Status=Root->Open(Root,&File,L"\\PS5UTC.SEED",EFI_FILE_MODE_READ,0);
  if(EFI_ERROR(Status)) { Root->Close(Root);return Status; }
  UINT8 Data[49];UINTN Size=sizeof(Data);
  Status=File->Read(File,&Size,Data);File->Close(File);Root->Close(Root);
  if(EFI_ERROR(Status)) return Status;
  if(Size!=48 || CompareMem(Data,"PS5UTC01",8)!=0) return EFI_COMPROMISED_DATA;
  UINT32 Crc=0;Status=gBS->CalculateCrc32(Data,44,&Crc);
  if(EFI_ERROR(Status) || Crc!=Read32(Data+44)) return EFI_CRC_ERROR;
  UINT64 Epoch=Read64(Data+8),Anchor=Read64(Data+16),Hz=Read64(Data+24),Now=AsmReadTsc();
  UINT32 Nanos=Read32(Data+32),Uncertainty=Read32(Data+36),MaxAge=Read32(Data+40);
  if(Hz!=1596300000ULL || Nanos>=1000000000U || Uncertainty>10000 ||
     MaxAge==0 || MaxAge>900 || Now<Anchor || (Now-Anchor)/Hz>MaxAge ||
     Epoch<1704067200ULL || Epoch>4102444800ULL) return EFI_SECURITY_VIOLATION;
  UINT64 Delta=Now-Anchor;
  UINT64 Ns=(Delta%Hz)*1000000000ULL/Hz+Nanos;
  Epoch+=Delta/Hz+Ns/1000000000ULL;
  EFI_TIME Time;UINT64 Check;
  if(!Ps5EpochToTime(Epoch,(UINT32)(Ns%1000000000ULL),&Time) ||
     !Ps5TimeToEpoch(&Time,&Check) || Check!=Epoch) return EFI_COMPROMISED_DATA;
  Status=gRT->SetTime(&Time);
  DEBUG((DEBUG_INFO,"PS5 UTC seed: age=%Lu seconds uncertainty=%u ms status=%r\n",Delta/Hz,Uncertainty,Status));
  return Status;
}

STATIC EFI_STATUS WindowsTimeContract(VOID) {
  EFI_TIME Before,After;UINT64 First,Last;
  EFI_STATUS Status=gRT->GetTime(&Before,NULL);
  DEBUG((DEBUG_INFO,"PS5 Windows time contract: first GetTime=%r\n",Status));
  if(EFI_ERROR(Status)) return Status;
  if(!Ps5TimeToEpoch(&Before,&First)) return EFI_COMPROMISED_DATA;
  // Match the early Microsoft calibration call shape; always restore TPL.
  UINT64 TickStart=AsmReadTsc();
  EFI_TPL Previous=gBS->RaiseTPL(TPL_HIGH_LEVEL);
  Status=gBS->Stall(1);
  if(!EFI_ERROR(Status)) Status=gBS->Stall(1000);
  if(!EFI_ERROR(Status)) Status=gBS->Stall(9000);
  gBS->RestoreTPL(Previous);
  UINT64 TickEnd=AsmReadTsc();
  if(!EFI_ERROR(Status) && (TickEnd<TickStart || TickEnd-TickStart<14366700ULL ||
     TickEnd-TickStart>1596300000ULL)) Status=EFI_DEVICE_ERROR;
  DEBUG((DEBUG_INFO,"PS5 Windows time contract: high-TPL Stall=%r\n",Status));
  if(EFI_ERROR(Status)) return Status;
  Status=gRT->GetTime(&After,NULL);
  if(EFI_ERROR(Status)) return Status;
  if(!Ps5TimeToEpoch(&After,&Last) || Last<First || Last-First>2 ||
     (Last==First && After.Nanosecond<=Before.Nanosecond)) return EFI_DEVICE_ERROR;
  DEBUG((DEBUG_INFO,"PS5 Windows time contract: advancing GetTime verified\n"));
  return EFI_SUCCESS;
}

#ifdef PS5_FWENUM_DIAG
/* Replicate the firmware enumeration that bootmgfw's BlFwEnumerateDevice walks:
 *   LocateHandleBuffer(DEVICE_PATH) -> ConnectController(each) ->
 *   LocateHandleBuffer(BLOCK_IO) -> per-device identifier match.
 * bootmgfw 28000 returns STATUS_NOT_FOUND (0xC0000225) at this step on our
 * UEFI while 26100 did not, so log exactly which handles/protocols/paths exist
 * (and whether our pinned ESP block device is among them). Read-only. */
STATIC VOID Ps5LogFirmwareDeviceTree (EFI_HANDLE Esp)
{
  EFI_HANDLE *Dp=NULL,*Bio=NULL;
  UINTN NDp=0,NBio=0,Ok=0,Fail=0;
  /* REAL blocker: bootmgfw's BlInitializeLibrary AllocatePages at the winload
   * image-base region (~0x10000000-0x10339FFF) fails with 0xC0000225
   * ("ConvertPages: Incompatible memory types, the pages to allocate have been
   * allocated"). Dump our DXE memory map around that region so we see exactly
   * which Type/owner occupies it. */
  {
    UINTN MapSize=0,MapKey=0,DescSize=0; UINT32 DescVer=0;
    EFI_MEMORY_DESCRIPTOR *Map=NULL;
    gBS->GetMemoryMap(&MapSize,NULL,&MapKey,&DescSize,&DescVer);
    if(DescSize>0){
      MapSize+=16*DescSize; Map=AllocatePool(MapSize);
      if(Map!=NULL){
        EFI_STATUS Gm=gBS->GetMemoryMap(&MapSize,Map,&MapKey,&DescSize,&DescVer);
        DEBUG((DEBUG_INFO,"PS5 memmap: GetMemoryMap=%r entries=%u\n",Gm,(UINT32)(MapSize/DescSize)));
        PS5_SCREEN(L"[memmap %r n=%u]\r\n",Gm,(UINT32)(MapSize/DescSize));
        if(!EFI_ERROR(Gm)){
          for(UINTN Mo=0;Mo<MapSize;Mo+=DescSize){
            EFI_MEMORY_DESCRIPTOR *Md=(EFI_MEMORY_DESCRIPTOR*)((UINT8*)Map+Mo);
            UINT64 St=Md->PhysicalStart, En=St+(UINT64)Md->NumberOfPages*0x1000ULL;
            if(En>0x0F000000ULL && St<0x11000000ULL){
              DEBUG((DEBUG_INFO,"PS5 memmap: type=%u phys=0x%Lx pages=0x%Lx attr=0x%Lx\n",Md->Type,St,Md->NumberOfPages,Md->Attribute));
              PS5_SCREEN(L"[mm t=%u p=0x%Lx n=0x%Lx a=0x%Lx]\r\n",Md->Type,St,Md->NumberOfPages,Md->Attribute);
            }
          }
        }
        FreePool(Map);
      }
    }
  }
  EFI_STATUS S=gBS->LocateHandleBuffer(ByProtocol,&gEfiDevicePathProtocolGuid,NULL,&NDp,&Dp);
  DEBUG((DEBUG_INFO,"PS5 fwenum: DevicePath handles=%u status=%r\n",(UINT32)NDp,S));
  PS5_SCREEN(L"[fwenum dp=%u %r]\r\n",(UINT32)NDp,S);
  if(!EFI_ERROR(S) && Dp!=NULL){
    for(UINTN I=0;I<NDp;I++){
      if(EFI_ERROR(gBS->ConnectController(Dp[I],NULL,NULL,TRUE))) Fail++; else Ok++;
    }
    FreePool(Dp);
  }
  DEBUG((DEBUG_INFO,"PS5 fwenum: ConnectController ok=%u fail=%u\n",(UINT32)Ok,(UINT32)Fail));
  PS5_SCREEN(L"[fwenum connect ok=%u fail=%u]\r\n",(UINT32)Ok,(UINT32)Fail);
  S=gBS->LocateHandleBuffer(ByProtocol,&gEfiBlockIoProtocolGuid,NULL,&NBio,&Bio);
  DEBUG((DEBUG_INFO,"PS5 fwenum: BlockIo handles=%u status=%r\n",(UINT32)NBio,S));
  PS5_SCREEN(L"[fwenum blockio=%u %r]\r\n",(UINT32)NBio,S);
  if(!EFI_ERROR(S) && Bio!=NULL){
    for(UINTN I=0;I<NBio;I++){
      EFI_BLOCK_IO_PROTOCOL *B=NULL; VOID *Tmp=NULL;
      gBS->HandleProtocol(Bio[I],&gEfiBlockIoProtocolGuid,(VOID **)&B);
      BOOLEAN HasPi=!EFI_ERROR(gBS->HandleProtocol(Bio[I],&gEfiPartitionInfoProtocolGuid,&Tmp));
      BOOLEAN HasB2=!EFI_ERROR(gBS->HandleProtocol(Bio[I],&gEfiBlockIo2ProtocolGuid,&Tmp));
      BOOLEAN HasFs=!EFI_ERROR(gBS->HandleProtocol(Bio[I],&gEfiSimpleFileSystemProtocolGuid,&Tmp));
      CHAR16 *T=ConvertDevicePathToText(DevicePathFromHandle(Bio[I]),TRUE,TRUE);
      DEBUG((DEBUG_INFO,"PS5 fwenum: blk%u%a media=%x rmb=%u part=%u bsz=%u last=%Lu pi=%u bio2=%u fs=%u path=%s\n",
        (UINT32)I,(Bio[I]==Esp)?" ESP":"",
        B?B->Media->MediaId:0,B?(UINT32)B->Media->RemovableMedia:0,B?(UINT32)B->Media->LogicalPartition:0,
        B?B->Media->BlockSize:0,B?B->Media->LastBlock:0,(UINT32)HasPi,(UINT32)HasB2,(UINT32)HasFs,T?T:L"(none)"));
      PS5_SCREEN(L"[blk%u%s pi=%u bio2=%u fs=%u %s]\r\n",(UINT32)I,(Bio[I]==Esp)?L" ESP":L"",(UINT32)HasPi,(UINT32)HasB2,(UINT32)HasFs,T?T:L"?");
      if(T)FreePool(T);
    }
    FreePool(Bio);
  }
  ConsoleSnapshot();
}
#endif

/* v1.60 boot-device enumeration: boot ANY medium carrying \EFI\Boot\bootx64.efi, not only the
 * hard-pinned BIWIN ESP GUID. Lets the payload boot a fresh clean install (new internal ESP GUID)
 * and, via the 30 s menu, a USB installer stick. Classification: a volume is "removable/USB" if its
 * BlockIo media is removable OR its device path traverses a USB node (so a USB flash installer is
 * never auto-booted over an internal install). */
/* r5.25.4 (user: the installed USB NVMe was missing from the boot list although the firmware saw the disk): Windows Setup
 * writes \EFI\Microsoft\Boot\bootmgfw.efi; the removable-media fallback \EFI\Boot\bootx64.efi is not always there.
 * Either file makes a volume bootable; the boot uses the one that exists (fallback first, as before). */
STATIC CHAR16 mWindowsPathMs[]=L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi";
STATIC BOOLEAN Ps5VolHasPath (EFI_HANDLE Handle, CHAR16 *Path)
{
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs; EFI_FILE_PROTOCOL *Root=NULL,*File=NULL;
  if (EFI_ERROR(gBS->HandleProtocol(Handle,&gEfiSimpleFileSystemProtocolGuid,(VOID **)&Fs))) return FALSE;
  if (EFI_ERROR(Fs->OpenVolume(Fs,&Root)) || Root==NULL) return FALSE;
  EFI_STATUS Status=Root->Open(Root,&File,Path,EFI_FILE_MODE_READ,0);
  if (!EFI_ERROR(Status) && File!=NULL) File->Close(File);
  Root->Close(Root);
  return !EFI_ERROR(Status);
}
STATIC CHAR16 *Ps5VolumeBootFile (EFI_HANDLE Handle)
{
  if (Ps5VolHasPath(Handle,mWindowsPath)) return mWindowsPath;
  if (Ps5VolHasPath(Handle,mWindowsPathMs)) return mWindowsPathMs;
  return NULL;
}
STATIC BOOLEAN Ps5VolumeHasBootx64 (EFI_HANDLE Handle) { return Ps5VolumeBootFile(Handle)!=NULL; }
STATIC BOOLEAN Ps5HandleBehindUsb (EFI_HANDLE Handle)
{
  EFI_DEVICE_PATH_PROTOCOL *Dp=DevicePathFromHandle(Handle);
  while (Dp!=NULL && !IsDevicePathEnd(Dp)) {
    if (DevicePathType(Dp)==MESSAGING_DEVICE_PATH &&
        (DevicePathSubType(Dp)==MSG_USB_DP || DevicePathSubType(Dp)==MSG_USB_CLASS_DP ||
         DevicePathSubType(Dp)==MSG_USB_WWID_DP)) return TRUE;
    Dp=NextDevicePathNode(Dp);
  }
  return FALSE;
}
STATIC BOOLEAN Ps5HandleRemovable (EFI_HANDLE Handle)
{
  EFI_BLOCK_IO_PROTOCOL *Bio=NULL;
  if (!EFI_ERROR(gBS->HandleProtocol(Handle,&gEfiBlockIoProtocolGuid,(VOID **)&Bio)) &&
      Bio!=NULL && Bio->Media!=NULL && Bio->Media->RemovableMedia) return TRUE;
  return Ps5HandleBehindUsb(Handle);
}
/* v378: the search for a boot medium used to give up after 100 passes, and the display path ran only once a
 * medium had been found. A start whose Windows disk is missing from the USB bus therefore stayed dark and wrote
 * no log: the "blind start". Now the first 100 passes are unchanged (a medium found there boots exactly as
 * before); at pass 100 the display comes up anyway, and the search goes on for 120 s with a status line every
 * 5 s and a USB port power cycle every 25 s. */
#define PS5_BOOT_MEDIUM_QUIET_ATTEMPTS 30U     /* x 100 ms; r2: 3 s (was 10 s) - the ESP currently appears only after the xHCI port power cycle */
#define PS5_BOOT_MEDIUM_ATTEMPTS       1300U   /* x 100 ms */
#define PS5_BOOT_MEDIUM_PATIENT        600U    /* r4: 60 s after the display without power cycle (bridge cold start ~40-50 s) */
#ifdef PS5_NATIVE_SETUP
/* v380: what brings a missing Windows disk back. Measured on hardware 12.11.2026 00:00 (v378, log in
 * analysis/blind-boot-cause-20261111): the Realtek RTL9210B bridge of the Windows disk answers INQUIRY after the
 * wake but times out on READ CAPACITY, then drops off the bus (PORTSC 0x81603 -> 0x802A0). A warm reset of the
 * empty SuperSpeed port and a restart of the host controller did nothing. Port power off for two seconds plus a
 * controller restart brought it back at once, and it then enumerated at the first attempt.
 * XhciDxe ignores EfiUsbPortPower, hence the direct PORTSC access (BAR 0). Every call also prints the raw PORTSC
 * of all root ports. */
#define PS5_XHCI_PORT_NEUTRAL 0x4e00ffe9U   /* PORTSC without its write-1-to-clear bits */
#define PS5_XHCI_PORT_POWER   0x00000200U
/* r2: the first recovery round (display start + first power cycle) is the normal path while the ESP appears
 * only after the power cycle - keep it off the screen (the DEBUG lines remain); later rounds stay visible. */
STATIC BOOLEAN mRecoverQuiet;
STATIC BOOLEAN mRecoverSilent;   /* r5.25: early usbpowercycle run: log lines only, nothing on the screen */
STATIC UINT64 mPs5CycleTsc;        /* r5.25.3: TSC of the last power cycle that cut a 0-byte USB disk (0 = none) */
STATIC UINTN  mPs5CycleWant;       /* r5.25.3: working USB disks expected after it (working before + cycled 0-byte disks) */
STATIC BOOLEAN mPs5CycleOnlyZero;  /* r5.25.4: menu entry - cycle only ports with a 0-byte USB disk, nothing else */
STATIC BOOLEAN mPs5ForceCycle;     /* r5.25.23: warm USB target missing - cycle even a controller that carries a working disk */
STATIC BOOLEAN mPs5ChosenTop;      /* r5.27.3: the medium about to boot is the first entry of the boot list (not a fallback) */
STATIC BOOLEAN mPs5FullCycleRan;   /* r5.26.6: a full port power cycle ran in this start (the pre-boot hook then does nothing) */
STATIC UINT64 mPs5UsbCutTsc;       /* r5.28.0: TSC when the power came back to a USB disk port cut in this start (0 = none) */
#define PS5_USB_SETTLE_S 45ULL      /* r5.28.0: RTL9210B bridge cold start 40-50 s (see PS5_BOOT_MEDIUM_PATIENT) */
/* r5.28.14 (HW 26.11.: the Windows on the RTL9210B died with INACCESSIBLE_BOOT_DEVICE on every start from GameOS, its first cold
 * starts ever - all earlier starts were restarts): after a GameOS start the bridge is OFF the bus (PORTSC 2802A0, no BlockIo), so
 * its port counted as an empty port - power-cycled without the r5.28.0 settle, which only knew 0-byte bridges and the missing boot
 * disk of a restart. Now every root port cut in this start is remembered, and a USB boot disk on such a port gets the settle. */
STATIC EFI_HANDLE mPs5CutHc[8];     /* r5.28.14: controllers with ports cut in this start */
STATIC UINT32 mPs5CutPorts[8];      /* r5.28.14: bit (port-1) per controller */
STATIC UINTN mPs5CutN;
STATIC UINT64 mPs5PortCutTsc;       /* r5.28.14: TSC when the power came back to any cut root port (0 = none) */
STATIC UINTN Ps5UsbWorkingDisks(VOID)   /* whole-disk BlockIo behind USB with a size */
{
  EFI_HANDLE *H=NULL;UINTN N=0,C=0;
  if(EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol,&gEfiBlockIoProtocolGuid,NULL,&N,&H)))return 0;
  for(UINTN I=0;I<N;I++){EFI_BLOCK_IO_PROTOCOL *B=NULL;
    if(!EFI_ERROR(gBS->HandleProtocol(H[I],&gEfiBlockIoProtocolGuid,(VOID **)&B)) && B && B->Media && !B->Media->LogicalPartition &&
       B->Media->MediaPresent && B->Media->LastBlock && Ps5HandleBehindUsb(H[I]))C++;}
  FreePool(H);return C;
}
#define PS5_RECOVER_PRINT(...) do{ if(mRecoverSilent) break; if(PS5_QUIET_SCREEN && mRecoverQuiet) PS5_SCREEN(__VA_ARGS__); else Print(__VA_ARGS__); }while(0)
/* r5.25.2: Keep = the chosen boot volume (or NULL). Its root port stays powered during the power cycle: after 2 s off
 * the boot stick (JMicron) came back with its link stuck in Polling (PORTSC 6E1: connected, not enabled) and the firmware
 * never saw it again (lab log r5.25.1). Root port = first USB() node after the controller's device path (0-based). */
STATIC VOID Ps5DiskModel(EFI_HANDLE Handle,CHAR8 *Out,UINTN Cap);   /* r5.25.3: used by Ps5UsbRecover, defined with the boot names */
/* r5.25.3: 1-based root port of Dp on the controller HcDp (first USB() node after the controller path), 0 = not behind it */
STATIC UINT32 Ps5RootPortOf (EFI_DEVICE_PATH_PROTOCOL *Dp, EFI_DEVICE_PATH_PROTOCOL *HcDp, UINTN HcLen)
{
  if (Dp==NULL || HcDp==NULL || !HcLen || GetDevicePathSize(Dp)<=HcLen || CompareMem(Dp,HcDp,HcLen)!=0) return 0;
  EFI_DEVICE_PATH_PROTOCOL *Nx=(EFI_DEVICE_PATH_PROTOCOL *)((UINT8 *)Dp+HcLen);
  if (DevicePathType(Nx)!=MESSAGING_DEVICE_PATH || DevicePathSubType(Nx)!=MSG_USB_DP) return 0;
  return (UINT32)((USB_DEVICE_PATH *)Nx)->ParentPortNumber+1;
}
/* r5.25.20: every xHCI root port in the log - PORTSC (speed id 3 = 480 Mbit/s, 4 = 5 Gbit/s, 5 = 10 Gbit/s; link state)
 * and the link error count of USB 3 ports (PORTLI bits 0-15) - a weak USB 3 link shows here (HW 23./24.11.) */
STATIC VOID Ps5LogUsbPorts (CONST CHAR8 *When)
{
  EFI_HANDLE *Pci=NULL; UINTN N=0; UINT32 Hc=0;
  if (EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol,&gEfiPciIoProtocolGuid,NULL,&N,&Pci))) return;
  for (UINTN I=0; I<N; I++) {
    EFI_PCI_IO_PROTOCOL *Io=NULL; UINT8 Class[3]={0}; UINT32 Cap=0,Params=0;
    if (EFI_ERROR(gBS->HandleProtocol(Pci[I],&gEfiPciIoProtocolGuid,(VOID **)&Io)) ||
        EFI_ERROR(Io->Pci.Read(Io,EfiPciIoWidthUint8,0x09,3,Class)) ||
        Class[2]!=0x0c || Class[1]!=0x03 || Class[0]!=0x30) continue;
    UINT32 Id=Hc++;
    if (EFI_ERROR(Io->Mem.Read(Io,EfiPciIoWidthUint32,0,0,1,&Cap)) || Cap==0xffffffffU ||
        EFI_ERROR(Io->Mem.Read(Io,EfiPciIoWidthUint32,0,4,1,&Params))) continue;
    for (UINT32 P=0; P<(Params>>24) && P<32; P++) {
      UINT32 Reg=(Cap&0xffU)+0x400U+0x10U*P, V=0, Li=0;
      if (EFI_ERROR(Io->Mem.Read(Io,EfiPciIoWidthUint32,0,Reg,1,&V))) continue;
      Io->Mem.Read(Io,EfiPciIoWidthUint32,0,Reg+8,1,&Li);
      if (!(V&1U)) continue;   /* nothing connected */
      DEBUG((DEBUG_ERROR,"PS5 USB port (%a): xhci %u port %u PORTSC=%x speed=%u link=%u enabled=%u link-errors=%u\n",
        When,Id,P+1,V,(V>>10)&0xFU,(V>>5)&0xFU,(V>>1)&1U,Li&0xFFFFU));
    }
  }
  FreePool(Pci);
}
STATIC VOID Ps5UsbRecover (BOOLEAN PowerCycle, EFI_HANDLE Keep)
{
  if (PowerCycle && !mPs5CycleOnlyZero) mPs5FullCycleRan=TRUE;   /* r5.26.6 */
  EFI_HANDLE *Pci=NULL; UINTN N=0; UINT32 Hc=0;
  /* r5.25.11 (boot timing: 2 x 2 s of the 6.3 s before bootmgr were two sequential power-off pauses): the ports of every
   * controller are switched off first, then ONE pause of 2 s, then every controller is restarted */
  EFI_HANDLE CyH[8]; UINT32 CyId[8],CyCap[8],CyPar[8],CyOff[8],CyMask[8]; BOOLEAN CyLive[8], DiskCut=FALSE; UINTN Cy=0;
  /* r5.25.14 (HW 23.11., user: a direct restart into a Windows on a USB SSD ended in INACCESSIBLE_BOOT_DEVICE, Windows Setup
   * from the stick saw no disk after a direct restart; a start from GameOS worked): after a restart from Windows a USB disk
   * is NOT known good - the USB-NVMe bridge keeps the state the previous Windows left (UEFI can still read it, the next
   * Windows cannot). A warm run power-cycles every USB disk, the chosen boot disk included; the boot path then waits up to
   * 60 s for it (a USB stick is back in seconds, an RTL9210B bridge needs 40-50 s). */
  /* r5.25.17: r5.25.14 also cut the boot disk after a restart from Windows - it did not help (HW 23.11.: still
   * INACCESSIBLE_BOOT_DEVICE) and the bridge came back in an error state; the boot disk and working disks stay powered */
  BOOLEAN WarmAll=FALSE;
  EFI_DEVICE_PATH_PROTOCOL *KeepDp=Keep?DevicePathFromHandle(Keep):NULL;
  if (EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol,&gEfiPciIoProtocolGuid,NULL,&N,&Pci))) return;
  for (UINTN I=0; I<N; I++) {
    EFI_PCI_IO_PROTOCOL *Io=NULL; UINT8 Class[3]={0}; UINT32 Cap=0,Params=0,Off=0,Hcc=0;
    if (EFI_ERROR(gBS->HandleProtocol(Pci[I],&gEfiPciIoProtocolGuid,(VOID **)&Io)) ||
        EFI_ERROR(Io->Pci.Read(Io,EfiPciIoWidthUint8,0x09,3,Class)) ||
        Class[2]!=0x0c || Class[1]!=0x03 || Class[0]!=0x30) continue;       /* xHCI only */
    UINT32 Id=Hc++, CutMask=0;
    if (EFI_ERROR(Io->Mem.Read(Io,EfiPciIoWidthUint32,0,0,1,&Cap)) || Cap==0xffffffffU ||
        EFI_ERROR(Io->Mem.Read(Io,EfiPciIoWidthUint32,0,4,1,&Params))) {
      PS5_RECOVER_PRINT(L"[xhci%u: registers unreadable]\r\n",Id); continue;
    }
    /* r5.25: HCCPARAMS1 bit 3 PPC - without port power control the controller ignores PORTSC.PP (lab log: 0x260FFED, PPC=1) */
    if (EFI_ERROR(Io->Mem.Read(Io,EfiPciIoWidthUint32,0,0x10,1,&Hcc))) Hcc=0;
    DEBUG((DEBUG_ERROR,"PS5 xHCI %u HCCPARAMS1=%x PPC=%u ports=%u\n",Id,Hcc,(Hcc>>3)&1U,Params>>24));
    if (PowerCycle && !(Hcc&BIT3)) {
      DEBUG((DEBUG_ERROR,"PS5 xHCI %u: no port power control - power cycle skipped\n",Id)); continue;
    }
    UINT32 KeepPort=0, KeepMask=0, ZeroMask=0, ZeroCut=0, CardMask=0;   /* 1-based port / bit (port-1); r5.25.3: working USB disks kept, 0-byte disks cycled */
    { EFI_DEVICE_PATH_PROTOCOL *HcDp=DevicePathFromHandle(Pci[I]);
      UINTN HcLen=HcDp?GetDevicePathSize(HcDp)-END_DEVICE_PATH_LENGTH:0;
      KeepPort=Ps5RootPortOf(KeepDp,HcDp,HcLen);
      if (KeepPort && KeepPort<=32) KeepMask|=1U<<(KeepPort-1);
      if (PowerCycle && HcLen) {
        /* r5.25.3 (user: the USB NVMe must be listed at once): a USB disk that reports a size works - its port is not
         * cycled (after a restart from Windows the RTL9210B was cut off and needed 40-50 s). A stuck bridge reports
         * 0 bytes or has dropped off the bus; those ports are cycled as before. */
        EFI_HANDLE *Bh=NULL; UINTN Bn=0;
        if (!EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol,&gEfiBlockIoProtocolGuid,NULL,&Bn,&Bh))) {
          for (UINTN K=0; K<Bn; K++) {
            EFI_BLOCK_IO_PROTOCOL *Bk=NULL; UINT32 Pt;
            if (EFI_ERROR(gBS->HandleProtocol(Bh[K],&gEfiBlockIoProtocolGuid,(VOID **)&Bk)) || Bk==NULL || Bk->Media==NULL ||
                Bk->Media->LogicalPartition) continue;
            Pt=Ps5RootPortOf(DevicePathFromHandle(Bh[K]),HcDp,HcLen);
            if (Pt && Pt<=32 && (!Bk->Media->MediaPresent || Bk->Media->LastBlock==0)) {   /* stuck bridge: 0 bytes */
              CHAR8 Mdl[64];
              Ps5DiskModel(Bh[K],Mdl,sizeof(Mdl));
              if (AsciiStrStr(Mdl,"SD")||AsciiStrStr(Mdl,"MMC")||AsciiStrStr(Mdl,"Card")||AsciiStrStr(Mdl,"Reader")||AsciiStrStr(Mdl,"CF")) {
                CardMask|=1U<<(Pt-1);
                DEBUG((DEBUG_ERROR,"PS5 xHCI %u: port %u empty card reader '%a' - not a reason to cycle\n",Id,Pt,Mdl)); continue;
              }
              ZeroMask|=1U<<(Pt-1);
              DEBUG((DEBUG_ERROR,"PS5 xHCI %u: port %u has a USB disk with 0 bytes - will be cycled\n",Id,Pt));
              continue;
            }
            if (!WarmAll && Pt && Pt<=32 && !(KeepMask&(1U<<(Pt-1)))) {
              KeepMask|=1U<<(Pt-1);
              DEBUG((DEBUG_ERROR,"PS5 xHCI %u: port %u carries a working USB disk (%Lu blocks) - stays powered\n",Id,Pt,Bk->Media->LastBlock+1));
            }
          }
          FreePool(Bh);
        }
        /* r5.25.19 (HW 23.11. r19b: after a start from GameOS the RTL9210B with the installed Windows sat on a connected and
         * enabled port - INQUIRY answered, no disk: READ CAPACITY timed out, see v380 - so it was neither a working nor a
         * 0-byte disk and its port was left alone; the firmware booted the installer stick): a USB mass-storage interface
         * without a working disk on its root port is a stuck bridge and is cycled like a 0-byte disk (card readers excepted). */
        EFI_HANDLE *Uh=NULL; UINTN Un=0;
        if (!EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol,&gEfiUsbIoProtocolGuid,NULL,&Un,&Uh))) {
          for (UINTN K=0; K<Un; K++) {
            EFI_USB_IO_PROTOCOL *U=NULL; EFI_USB_INTERFACE_DESCRIPTOR D; UINT32 Pt;
            if (EFI_ERROR(gBS->HandleProtocol(Uh[K],&gEfiUsbIoProtocolGuid,(VOID **)&U)) || U==NULL ||
                EFI_ERROR(U->UsbGetInterfaceDescriptor(U,&D)) || D.InterfaceClass!=8) continue;
            Pt=Ps5RootPortOf(DevicePathFromHandle(Uh[K]),HcDp,HcLen);
            if (!Pt || Pt>32 || ((KeepMask|CardMask|ZeroMask)&(1U<<(Pt-1)))) continue;
            ZeroMask|=1U<<(Pt-1);
            DEBUG((DEBUG_ERROR,"PS5 xHCI %u: port %u has USB storage without a disk (bridge not ready) - will be cycled\n",Id,Pt));
          }
          FreePool(Uh);
        }
      } }
    if (PowerCycle && KeepPort) DEBUG((DEBUG_ERROR,"PS5 xHCI %u: port %u carries the chosen boot medium - stays powered\n",Id,KeepPort));
    /* r5.25.17 (HW 23.11. r15test/diag: a controller restart disturbed the RTL9210B kept on the same controller - BOT
     * timeouts, transaction errors, INACCESSIBLE_BOOT_DEVICE in the Windows on it; restoring only the port power without the
     * restart (r5.25.16) left stale devices in the USB stack and made every USB access slow): a controller that carries the
     * boot medium or a working USB disk is not touched at all, unless one of its disks is stuck at 0 bytes.
     * r5.25.19 (HW 23.11. install18: a start from GameOS did not see the RTL9210B with the Windows of the new install, the
     * installer stick on the same controller was "working", so the controller was left alone and the bridge - INQUIRY ok,
     * READ CAPACITY timeout, off the bus, see v380 - was never power-cycled; the firmware booted the stick again): only
     * after a restart from Windows. A start from GameOS cycles the other ports and restarts the controller as before. */
    /* r5.27.3 (HW 25.11.: the RTL9210B with the Windows of the first boot-list entry was found, then the start from GameOS
     * restarted its controller and the bridge did not come back within 60 s - the internal Windows started instead, on 3 of
     * 5 cold starts): a start from GameOS leaves the controller alone too when the medium about to boot is the FIRST
     * boot-list entry, its port is known and no disk is stuck at 0 bytes. The r5.25.19 case (stick chosen only because the
     * preferred RTL9210B was missing) still cycles: there the chosen medium is not the first entry. */
    /* r5.28.2 (HW 26.11. fin2 with the release r5.28.1, and b2 with r5.27.4: a start from GameOS hung for good after "ports off -
     * 2 s pause" on the controller that carried the boot disk (RTL9210B) and a working stick - only EMPTY ports of it had been
     * switched off, no device vanished; the stop of a controller with USB mass storage on it sometimes never returns): a controller
     * that carries the boot medium or a working USB disk and no stuck one is never stopped, on every start (was: only after a
     * restart from Windows or when the boot medium is the first boot-list entry). */
    if (PowerCycle && !mPs5ForceCycle &&
        (KeepMask|(KeepPort&&KeepPort<=32?(1U<<(KeepPort-1)):0)) && !ZeroMask) {
      DEBUG((DEBUG_ERROR,"PS5 xHCI %u: carries a working USB disk - controller and its ports left alone\n",Id));
      PS5_RECOVER_PRINT(L"[xhci%u: left alone]\r\n",Id); continue;
    }
    PS5_RECOVER_PRINT(L"[xhci%u:",Id);
    for (UINT32 P=0; P<(Params>>24) && P<32; P++) {
      UINT32 Reg=(Cap&0xffU)+0x400U+0x10U*P, V=0;
      if (EFI_ERROR(Io->Mem.Read(Io,EfiPciIoWidthUint32,0,Reg,1,&V))) continue;
      PS5_RECOVER_PRINT(L" %x",V);
      DEBUG((DEBUG_ERROR,"PS5 xHCI %u port %u PORTSC=%x power-cycle=%u\n",Id,P+1,V,(UINT32)PowerCycle));
      if (!PowerCycle || !(V&PS5_XHCI_PORT_POWER) || P+1==KeepPort || ((KeepMask&~ZeroMask)&(1U<<P))) continue;
      if (mPs5CycleOnlyZero && !(ZeroMask&(1U<<P))) continue;   /* r5.25.4: menu entry - only stuck bridges */
      /* r5.25.11 (HW 23.11. lab126/127): after a restart from Windows a connected device is NOT known good - Windows
       * suspended it at shutdown, and a self-powered USB-C hub (keyboard, mouse) then failed every port reset in Windows.
       * A warm run cycles such ports like a cold boot; the boot medium and working USB disks above stay powered. */
      /* r5.25.19 (HW 23.11. r19b/r19e): the r5.25.3 rule "connected + enabled = a working device, left alone" kept the stuck
       * RTL9210B (PORTSC A1603: on the bus, INQUIRY only, no disk) powered, and the controller restart then never returned.
       * A start from GameOS now cycles every port without a working disk, like a restart from Windows already did. */
      CutMask|=1U<<P; Off++; if (ZeroMask&(1U<<P)) ZeroCut++;   /* r5.27.7: switched off below (r5.27.8: see CyLive) */
      if (mPs5ForceCycle || (ZeroMask&(1U<<P))) DiskCut=TRUE;   /* r5.28.0: a USB disk (or the missing boot disk's port) loses power */
    }
    PS5_RECOVER_PRINT(L"]\r\n");
    if (!PowerCycle) continue;
    if (Off==0) { DEBUG((DEBUG_ERROR,"PS5 xHCI %u: no port needs a power cycle - controller left running\n",Id)); continue; }   /* r5.25.3 */
    if (ZeroCut) { if (!mPs5CycleTsc) mPs5CycleWant=Ps5UsbWorkingDisks(); mPs5CycleWant+=ZeroCut; mPs5CycleTsc=AsmReadTsc(); }   /* r5.25.3 */
    if (Cy<8) { CyH[Cy]=Pci[I]; CyId[Cy]=Id; CyCap[Cy]=Cap; CyPar[Cy]=Params; CyOff[Cy]=Off; CyMask[Cy]=CutMask;
                CyLive[Cy]=(KeepPort!=0 || (KeepMask&~ZeroMask)!=0); Cy++; }
  }
  /* r5.27.7 (HW 25.11.: the release firmware r5.27.4 hung for good after "ports off - 2 s pause" on a start from GameOS,
   * the next log line "stopped" never came; a good start logged 8 x "XhciDelAsyncIntTransfer: XhcDequeueTrbFromEndpoint
   * failed" at the same point; a mouse behind a USB hub came back dead after some restarts): the ports were switched off
   * while XhciDxe and the USB stack still ran, and the stop 2 s later sent Disable Slot / Stop Endpoint commands to devices
   * that were gone, each with a 10 s timeout, and could get stuck. Now the driver stop comes first, with every device still
   * powered (a clean unload, as at ExitBootServices), and the port power goes off on the halted controller.
   * r5.27.8 (HW 25.11. r5.27.7: after a clean stop and 2 s with the controller halted, the RTL9210B bridge with the boot disk on
   * a KEPT port answered no INQUIRY any more and never came back - with the old order it was back after 0.1 s): a controller
   * that carries a kept disk keeps the old order (its other ports off while the driver runs, stop after the pause); the
   * controllers without a kept disk are stopped first, so no device vanishes under a running driver during the pause. */
  EFI_STATUS CyStop[8];
  for (UINTN Live=0; Live<2; Live++)   /* first the controllers stopped before the pause, then the live ones */
  for (UINTN K=0; K<Cy; K++) {
    EFI_PCI_IO_PROTOCOL *Io=NULL;
    if ((UINTN)CyLive[K]!=Live) continue;
    if (!Live) {
      CyStop[K]=gBS->DisconnectController(CyH[K],NULL,NULL);
      DEBUG((DEBUG_ERROR,"PS5 xHCI %u power cycle: stopped %r (before the power-off)\n",CyId[K],CyStop[K])); ConsoleSnapshot();
    } else CyStop[K]=EFI_SUCCESS;
    if (EFI_ERROR(gBS->HandleProtocol(CyH[K],&gEfiPciIoProtocolGuid,(VOID **)&Io))) continue;
    if (!Live) Io->Attributes(Io,EfiPciIoAttributeOperationEnable,EFI_PCI_IO_ATTRIBUTE_MEMORY,NULL);   /* the stop restored the original attributes */
    for (UINT32 P=0; P<(CyPar[K]>>24) && P<32; P++) {
      UINT32 Reg=(CyCap[K]&0xffU)+0x400U+0x10U*P, V=0, A=0;
      if (!(CyMask[K]&(1U<<P)) || EFI_ERROR(Io->Mem.Read(Io,EfiPciIoWidthUint32,0,Reg,1,&V))) continue;
      V=(V&PS5_XHCI_PORT_NEUTRAL)&~PS5_XHCI_PORT_POWER;
      Io->Mem.Write(Io,EfiPciIoWidthUint32,0,Reg,1,&V);
      Io->Mem.Read(Io,EfiPciIoWidthUint32,0,Reg,1,&A);
      DEBUG((DEBUG_ERROR,"PS5 xHCI %u port %u power off (%a): PORTSC=%x\n",CyId[K],P+1,Live?"driver running":"controller halted",A));
    }
  }
  // The driver still owns the running controller. Its next start resets the controller, which powers the ports again.
  /* r5.25.19: a log snapshot around every step, so a start that stops in the power cycle leaves its log (HW 23.11.) */
  if (Cy) { DEBUG((DEBUG_ERROR,"PS5 usbpowercycle: %u controllers, ports off - 2 s pause\n",(UINT32)Cy)); ConsoleSnapshot(); gBS->Stall(2000000); }   /* 2 s as before (HW 23.11. lab133: with 1 s the mouse behind the USB-C hub failed once in 3 restarts) */
  for (UINTN K=0; K<Cy; K++) {
    EFI_PCI_IO_PROTOCOL *Io=NULL;
    EFI_STATUS Stop=CyStop[K];
    if (CyLive[K]) { Stop=gBS->DisconnectController(CyH[K],NULL,NULL); DEBUG((DEBUG_ERROR,"PS5 xHCI %u power cycle: stopped %r\n",CyId[K],Stop)); ConsoleSnapshot(); }
    EFI_STATUS Start=gBS->ConnectController(CyH[K],NULL,NULL,TRUE);
    DEBUG((DEBUG_ERROR,"PS5 xHCI %u power cycle: ports-off=%u stop=%r start=%r\n",CyId[K],CyOff[K],Stop,Start)); ConsoleSnapshot();
    if (!EFI_ERROR(gBS->HandleProtocol(CyH[K],&gEfiPciIoProtocolGuid,(VOID **)&Io)))
      for (UINT32 P=0; P<(CyPar[K]>>24) && P<32; P++) {   /* r5.25: PORTSC after the restart */
        UINT32 V=0;
        if (!EFI_ERROR(Io->Mem.Read(Io,EfiPciIoWidthUint32,0,(CyCap[K]&0xffU)+0x400U+0x10U*P,1,&V)))
          DEBUG((DEBUG_ERROR,"PS5 xHCI %u port %u PORTSC after=%x\n",CyId[K],P+1,V));
      }
    PS5_RECOVER_PRINT(L"[xhci%u: port power cycled on %u ports, stop %r, start %r]\r\n",CyId[K],CyOff[K],Stop,Start);
  }
  for (UINTN K=0; K<Cy; K++) {   /* r5.28.14: remember every cut root port */
    UINTN J=0;
    while (J<mPs5CutN && mPs5CutHc[J]!=CyH[K]) J++;
    if (J==mPs5CutN && mPs5CutN<8) { mPs5CutHc[J]=CyH[K]; mPs5CutPorts[J]=0; mPs5CutN++; }
    if (J<mPs5CutN) mPs5CutPorts[J]|=CyMask[K];
  }
  if (Cy) mPs5PortCutTsc=AsmReadTsc();
  if (DiskCut && Cy) { mPs5UsbCutTsc=AsmReadTsc(); DEBUG((DEBUG_ERROR,"PS5 usbpowercycle: a USB disk port was power-cycled - its bridge settles before Windows starts\n")); }
  FreePool(Pci);
}
/* r5.28.14: TRUE if the root port of Disk lost its power in this start */
STATIC BOOLEAN Ps5UsbPortWasCut (EFI_HANDLE Disk)
{
  EFI_DEVICE_PATH_PROTOCOL *Dp=Disk?DevicePathFromHandle(Disk):NULL;
  for (UINTN K=0; K<mPs5CutN; K++) {
    EFI_DEVICE_PATH_PROTOCOL *HcDp=DevicePathFromHandle(mPs5CutHc[K]);
    UINTN HcLen=HcDp?GetDevicePathSize(HcDp)-END_DEVICE_PATH_LENGTH:0;
    UINT32 Pt=Ps5RootPortOf(Dp,HcDp,HcLen);
    if (Pt && Pt<=32 && (mPs5CutPorts[K]&(1U<<(Pt-1)))) return TRUE;
  }
  return FALSE;
}
/* r5.28.0 settle, r5.28.14: one place for the boot path and starts from the Boot Manager. A USB boot disk whose root port was
 * power-cycled in this start (or after a cut 0-byte bridge, r5.28.0) gets 45 s from the power-on before Windows starts. */
STATIC VOID Ps5UsbSettle (EFI_HANDLE Boot, CONST CHAR8 *Where)
{
  UINT64 Cut=0;
  if (Boot==NULL || !Ps5HandleBehindUsb(Boot)) return;
  if (mPs5PortCutTsc!=0 && Ps5UsbPortWasCut(Boot)) Cut=mPs5PortCutTsc;
  else if (mPs5UsbCutTsc!=0) Cut=mPs5UsbCutTsc;
  if (Cut==0) return;
  UINT64 Hz=1596300000ULL, Since=AsmReadTsc()-Cut;
  if (Since<PS5_USB_SETTLE_S*Hz) {
    DEBUG((DEBUG_ERROR,"PS5 usb settle (%a): USB boot disk power-cycled %u s ago - waiting %u s before Windows starts\n",
      Where,(UINT32)(Since/Hz),(UINT32)(PS5_USB_SETTLE_S-Since/Hz)));
    ConsoleSnapshot();
    while (AsmReadTsc()-Cut<PS5_USB_SETTLE_S*Hz) {
      PS5_SCREEN(L"\rStarting the USB drive: %u s   ",(UINT32)(PS5_USB_SETTLE_S-(AsmReadTsc()-Cut)/Hz));
      gBS->Stall(1000000);
    }
    PS5_SCREEN(L"\r                                  \r");
    DEBUG((DEBUG_ERROR,"PS5 usb settle: done\n"));
  } else DEBUG((DEBUG_ERROR,"PS5 usb settle (%a): USB boot disk power-cycled %u s ago - settled\n",Where,(UINT32)(Since/Hz)));
}
/* Needs nothing from a boot medium: the VBIOS comes from the loader at 0xc0000. */
STATIC VOID Ps5StartDisplay (VOID)
{
  Ps5NativeGpuProbe();
#ifdef PS5_NATIVE_DP_SOURCE
  EFI_STATUS DpStatus=Ps5NativeDpSource();
  DEBUG((DEBUG_INFO,"PS5 DP source caller: %r\n",DpStatus));
  ConsoleSnapshot();
#endif
}
#endif
#ifdef PS5_NVME_PREINIT
/* Way (a) for the internal M.2 under Windows (v1.0, 13.11.2026): log the real (ARI/SFC) and the shadow (raw ECAM)
 * view of the SanDisk; with PS5_NVME_PREINIT_APPLY also enable MSI-X in the real config at ExitBootServices. */
#include "ps5-nvme-msix-preinit.h"
#endif
#ifdef PS5_M2_OVERLAY_N4
/* N4 (v1.0, 13.11.2026): internal PS5 M.2 with a nested GPT (PMBR@65536, header@65537, absolute LBAs, GameOS
 * areas [0,65536) and the last 34224 sectors untouched). The overlay hooks the namespace BlockIo/BlockIo2 so
 * PartitionDxe sees a standard GPT with the same absolute partitions; writes into the GameOS areas fail with
 * EFI_WRITE_PROTECTED. Without a valid nested GPT nothing is hooked and nothing is reconnected. */
#define PS5_M2_UEFI_BINDING 1
#include "ps5-m2-overlay.h"
#endif
#ifndef PS5_TS
#define PS5_TS(Tag)
#endif
/* r5.19.2: settings defaults and the config ESP are used with and without the setup menu (Ps5PeekSettings r5.16,
 * config ESP r5.15.2); declared here so a build without PS5_NATIVE_SETUP compiles. */
#include "ps5-settings.h"
STATIC EFI_HANDLE mPs5ConfigEsp;
STATIC EFI_HANDLE mPs5BootSelected;
/* r5.25.4 warm re-entry hand-over (record written by the reset trampoline at 0x1_1180_6040; last boot device at 0x1_1180_6060,
 * written below before every Windows start). Used only when the firmware was started again by a restart, never after GameOS. */
#ifndef PS5_WARM_RECORD
#define PS5_WARM_RECORD   0x111806040ULL
#define PS5_WARM_LASTBOOT 0x111806060ULL
#endif
STATIC BOOLEAN mPs5WarmDirect, mPs5WarmDirectHit, mPs5WarmNotified;
/* r5.25.12 (user: the restart must also work for Windows installed on USB): L[3] of the last-boot record marks a USB boot
 * device ("PSWUSB1"); a warm run then waits up to 60 s for its target (USB disks and bridges re-enumerate late) */
#define PS5_WARM_LASTUSB 0x0031425355575350ULL
#define PS5_WARM_USB_WAIT_ATTEMPTS 600U   /* x 100 ms */
STATIC BOOLEAN mPs5WarmUsb;
STATIC UINTN mPs5WarmForcedAt;   /* r5.28.1: attempt of the forced warm USB power cycle (0 = none) */
STATIC UINTN mPs5WarmFallbackAt;   /* r5.28.6: attempt at which a missing BootNext target fell back to the last boot device (0 = none) */
STATIC BOOLEAN mPs5UpdateRun;   /* r5.25.13: this firmware was started by an older one as its update (bit 2 of the record) */
/* r5.25.5: a warm re-entry that cannot boot its target restarts cold through GameOS (ICC) - no BDS fallback */
STATIC VOID Ps5WarmColdRestart(CONST CHAR8 *Why)
{
  DEBUG((DEBUG_ERROR,"PS5 warm re-entry: %a - cold restart through GameOS\n",Why));
  *(volatile UINT64 *)(UINTN)0x1118061A0ULL=0x31444C4F43575350ULL;   /* "PSWCOLD1": WarmReentry steps aside, ICC reboot */
  gRT->ResetSystem(EfiResetCold,EFI_SUCCESS,0,NULL);
}
STATIC EFI_GUID mPs5WarmGuid;
STATIC EFI_DEVICE_PATH_PROTOCOL *mPs5WarmNextDisk;
STATIC UINT32 mPs5WarmNextHash; STATIC BOOLEAN mPs5WarmNextHashOk, mPs5WarmNextPending;   /* r5.28.7: name hash from ResetSystem, a BootNext disk is awaited */
/* r5.28.7: FNV-1a over the UTF-16 code units of an entry name - the same hash as WarmOptionName in Ps5ResetSystemLib.c */
STATIC UINT32 Ps5NameHash(CONST CHAR16 *S) { UINT32 H=0x811C9DC5u; for(;S!=NULL && *S;S++){H^=(UINT8)*S;H*=16777619u;H^=(UINT8)(*S>>8);H*=16777619u;} return H; }
STATIC CHAR16 *mPs5WarmNextName;   /* r5.28.6: name of that entry ("USB: <model> <size>") - fallback when the path changed */   /* r5.27.4: BootNext named a whole disk - boot the ESP below this path */
STATIC UINT32 mPs5NewDiskId;
STATIC BOOLEAN Ps5VolGptGuidIs(EFI_HANDLE Volume, CONST EFI_GUID *Guid)
{
  EFI_PARTITION_INFO_PROTOCOL *Pi=NULL;
  return !EFI_ERROR(gBS->HandleProtocol(Volume,&gEfiPartitionInfoProtocolGuid,(VOID **)&Pi)) && Pi!=NULL &&
         Pi->Type==PARTITION_TYPE_GPT && CompareGuid(&Pi->Info.Gpt.UniquePartitionGUID,Guid);
}
/* r5.25.5: before EfiBootManagerConnectAll one log line per NVMe/xHCI function and the GPU - a controller
 * that reads all-ones after a restart from Windows costs NvmExpressDxe CAP.TO = 0xFF -> 127.5 s per wait; read only */
VOID Ps5WarmPciDump(VOID)
{
#ifdef PS5_NATIVE_SETUP
  EFI_HANDLE *H=NULL; UINTN N=0;
  if(EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol,&gEfiPciIoProtocolGuid,NULL,&N,&H)))return;
  for(UINTN I=0;I<N;I++) {
    EFI_PCI_IO_PROTOCOL *Io; UINT8 Cc[3]; UINT32 Id=0,B0=0,B1=0; UINT16 Cmd=0;
    if(EFI_ERROR(gBS->HandleProtocol(H[I],&gEfiPciIoProtocolGuid,(VOID **)&Io)))continue;
    Io->Pci.Read(Io,EfiPciIoWidthUint32,0,1,&Id);Io->Pci.Read(Io,EfiPciIoWidthUint8,9,3,Cc);Io->Pci.Read(Io,EfiPciIoWidthUint16,4,1,&Cmd);
    BOOLEAN IsGpu=(Id==0x13FC1002U || Id==0x13DB1002U || Id==0x13DC1002U ||
                   Id==0x13DA1002U || Id==0x13F91002U || Id==0x13FA1002U || Id==0x13FB1002U);
    if(!((Cc[2]==1&&Cc[1]==8)||(Cc[2]==0xC&&Cc[1]==3)||IsGpu))continue;
    Io->Mem.Read(Io,EfiPciIoWidthUint32,IsGpu?5:0,0,1,&B0);
    Io->Mem.Read(Io,EfiPciIoWidthUint32,IsGpu?5:0,4,1,&B1);
    DEBUG((DEBUG_ERROR,"PS5 warm pci: %08x class %02x%02x cmd %04x bar[0]=%08x [4]=%08x%a\n",Id,Cc[2],Cc[1],Cmd,B0,B1,
           (B0==0xFFFFFFFFU&&B1==0xFFFFFFFFU)?" ALL-ONES":""));
  }
  FreePool(H);
#endif
}
/* lab --warm-probe 46-49: called from PlatformBootManager.c; switches off at the chosen point after a warm re-entry */
VOID Ps5WarmProbePoint(UINTN N)
{
#if defined(PS5_WARM_WATCHDOG)
  Ps5WarmProgress((unsigned)N);   /* lab: 46..49 from PlatformBootManager.c */
#endif
#if defined(PS5_WARM_PROBE)
  if(N==PS5_WARM_PROBE)Ps5WarmProbeOff();
#if PS5_WARM_PROBE == 63
  if(N==46) {   /* lab: every function's config space read once (ECAM), then off - stays on = a config read stalls (CRS) */
    volatile UINT32 Sum=0;
    for(UINT32 F=0;F<0x8000;F++) Sum+=*(volatile UINT32 *)(UINTN)(0xF0000000ULL+((UINT64)F<<12));
    (VOID)Sum; Ps5WarmProbeOff();
  }
#endif
#else
  (VOID)N;
#endif
}
#if defined(PS5_WARM_EBS_MARK) && defined(PS5_NATIVE_SETUP)
/* lab: a white 96x96 square top left at ExitBootServices of a warm run - Windows does not redraw before its
 * spinner, so a visible square = winload reached EBS (hang in the kernel), none = hang in bootmgr/winload */
#if defined(PS5_WARM_EBS_STEPS)
/* lab --warm-ebs-steps: same 48x48 squares as Ps5WarmSq (ps5-warm-probe.h), cell 0x1_1180_6300 */
STATIC VOID Ps5WarmSqL(UINT32 Slot, UINT32 Color)
{
  volatile UINT64 *C=(volatile UINT64 *)(UINTN)0x111806300ULL;
  if(C[0]!=0x3142465153575350ULL||C[1]==0)return;
  volatile UINT32 *Fb=(volatile UINT32 *)(UINTN)C[1]; UINT64 S=C[2];
  for(UINT32 Y=0;Y<48;Y++)for(UINT32 X=0;X<48;X++)Fb[Y*S+320+Slot*60+X]=Color;
  AsmWbinvd();   /* WB-cached frame buffer */
}
STATIC VOID EFIAPI Ps5WarmSqFirst(IN EFI_EVENT Event, IN VOID *Context){(VOID)Event;(VOID)Context;Ps5WarmSqL(8,0x00FFFFFF);}   /* newest EBS notify: runs first */
STATIC VOID EFIAPI Ps5WarmSqFirstCb(IN EFI_EVENT Event, IN VOID *Context)   /* newest TPL_CALLBACK EBS notify: every TPL_NOTIFY one is done, IF was 1 in between */
{
  (VOID)Event;(VOID)Context;
  volatile UINT64 *C=(volatile UINT64 *)(UINTN)0x111806300ULL;
  if(C[0]!=0x3142465153575350ULL||C[1]==0)return;
  volatile UINT32 *Fb=(volatile UINT32 *)(UINTN)C[1]; UINT64 S=C[2];
  for(UINT32 Y=60;Y<108;Y++)for(UINT32 X=0;X<48;X++)Fb[Y*S+320+X]=0x00FFFFFF;
  AsmWbinvd();
}
STATIC VOID EFIAPI Ps5WarmSqAfterWim(IN EFI_EVENT Event, IN VOID *Context){(VOID)Event;(VOID)Context;Ps5WarmSqL(11,0x0080FF80);}   /* runs right after PostWimExit */
#endif
STATIC VOID EFIAPI Ps5WarmEbsMark(IN EFI_EVENT Event, IN VOID *Context)
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL *G=NULL;
#if defined(PS5_WARM_EBS_STEPS)
  Ps5WarmSqL(9,0x00FF0000);
#endif
#if defined(PS5_WARM_EBS_WATCH)
  Ps5WarmProgress(52);
#endif
  if(EFI_ERROR(gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid,NULL,(VOID **)&G))||G==NULL||G->Mode==NULL||G->Mode->Info==NULL)return;
  volatile UINT32 *Fb=(volatile UINT32 *)(UINTN)G->Mode->FrameBufferBase; UINT32 S=G->Mode->Info->PixelsPerScanLine;
#if defined(PS5_WARM_EBS_STEPS)
  Ps5WarmSqL(10,0x00FFA0A0);
#endif
  for(UINT32 Y=0;Y<96;Y++)for(UINT32 X=0;X<96;X++)Fb[Y*S+X]=0x00FFFFFF;
  AsmWbinvd();   /* lab: the frame buffer is WB-cached - unflushed, a later stall hides the square */
#if defined(PS5_WARM_EBS_WATCH)
  Ps5WarmProgress(53);
#endif
}
#endif
#if defined(PS5_WARM_EBS_WATCH) || (defined(PS5_WARM_EBS_MARK) && defined(PS5_NATIVE_SETUP))
STATIC EFI_GUID mPs5BeforeEbsGuid={0x8be0e274,0x3970,0x4b44,{0x80,0xc5,0x1a,0xb9,0x50,0x2f,0x3b,0xfc}};   /* gEfiEventBeforeExitBootServicesGuid */
/* lab: ExitBootServices called - with --warm-ebs-mark a RED 96x96 square at x 200 (BeforeExitBootServices runs before the
 * FAT flush, the timer stop and the memory-map termination; the white EBS square comes after them) */
STATIC VOID EFIAPI Ps5WarmBeforeEbs(IN EFI_EVENT Event, IN VOID *Context)
{
  (VOID)Event;(VOID)Context;
#if defined(PS5_WARM_EBS_WATCH)
  Ps5WarmProgress(51);
#endif
#if defined(PS5_WARM_EBS_MARK)
  EFI_GRAPHICS_OUTPUT_PROTOCOL *G=NULL;
  if(EFI_ERROR(gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid,NULL,(VOID **)&G))||G==NULL||G->Mode==NULL||G->Mode->Info==NULL)return;
  volatile UINT32 *Fb=(volatile UINT32 *)(UINTN)G->Mode->FrameBufferBase; UINT32 S=G->Mode->Info->PixelsPerScanLine;
  for(UINT32 Y=0;Y<96;Y++)for(UINT32 X=200;X<296;X++)Fb[Y*S+X]=0x00FF0000;
  AsmWbinvd();
#if defined(PS5_WARM_EBS_STEPS)
  { volatile UINT64 *C=(volatile UINT64 *)(UINTN)0x111806300ULL; C[1]=(UINT64)(UINTN)Fb; C[2]=S; C[0]=0x3142465153575350ULL; }
#endif
#endif
}
#endif
STATIC VOID Ps5WarmReadRecord(VOID)
{
  volatile UINT64 *R=(volatile UINT64 *)(UINTN)PS5_WARM_RECORD, *L=(volatile UINT64 *)(UINTN)PS5_WARM_LASTBOOT;
  *(volatile UINT64 *)(UINTN)PS5_WARM_LOGCELL=0;   /* r5.25.11: warm-log cell (LogExportDxe: a warm run's snapshots go to \PS5WARM.LOG) */
#if defined(PS5_WARM_EBS_STEPS)
  *(volatile UINT64 *)(UINTN)0x111806300ULL=0;   /* lab: only a warm run's BeforeExitBootServices publishes the frame buffer */
#endif
#if defined(PS5_WARM_PROBE) && (PS5_WARM_PROBE == 75 || PS5_WARM_PROBE == 76)
  *(volatile UINT64 *)(UINTN)0x1118061B0ULL=0;   /* lab: warm-run cell for the MpInitLib probes */
#endif
#if defined(PS5_WARM_WATCHDOG)
  Ps5WarmProgress(45);
#endif
  if(R[0]!=0x314D524157575350ULL)return;   /* not a warm re-entry */
#if defined(PS5_WARM_PROBE) && (PS5_WARM_PROBE == 75 || PS5_WARM_PROBE == 76)
  *(volatile UINT64 *)(UINTN)0x1118061B0ULL=0x314E555257575350ULL;   /* "PSWWRUN1" */
#endif
#ifdef PS5_WARM_BS_TRACE
  mPs5BsTraceWarm=TRUE;
#endif
#if defined(PS5_WARM_EBS_MARK) && defined(PS5_NATIVE_SETUP)
  { EFI_EVENT Ev; gBS->CreateEvent(EVT_SIGNAL_EXIT_BOOT_SERVICES,TPL_NOTIFY,Ps5WarmEbsMark,NULL,&Ev); }
#endif
#if defined(PS5_WARM_EBS_WATCH) || (defined(PS5_WARM_EBS_MARK) && defined(PS5_NATIVE_SETUP))
  { EFI_EVENT Eb; gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,Ps5WarmBeforeEbs,NULL,&mPs5BeforeEbsGuid,&Eb); }
#endif
#if defined(PS5_WARM_PROBE) && PS5_WARM_PROBE == 2
  gRT->ResetSystem(EfiResetShutdown,EFI_SUCCESS,0,NULL);   /* lab (HW 19.11.): off here = the restarted firmware reached BDS */
#endif
#if defined(PS5_WARM_PROBE) && PS5_WARM_PROBE == 45
  Ps5WarmProbeOff();   /* lab: like 2, but through the stand-alone ICC request (2 depends on the reset driver's device check) */
#endif
  *(volatile UINT64 *)(UINTN)PS5_WARM_LOGCELL=0x31474F4C4D524157ULL;   /* "WARMLOG1" */
  UINT64 Flags=R[1];
  R[0]=0;
  if(Flags&4) {   /* r5.25.13: started as a firmware update - behave like a start from GameOS (start screen, no direct boot) */
    mPs5UpdateRun=TRUE;
    DEBUG((DEBUG_ERROR,"PS5 firmware update: running %a, started by the built-in firmware\n",PS5_FW_RELEASE));
    return;
  }
  if(Flags&1) {
#ifdef PS5_NATIVE_SETUP
    mPs5ForceSetup=TRUE;
#endif
    DEBUG((DEBUG_ERROR,"PS5 warm re-entry: the OS asked for the firmware UI - opening the Boot Manager\n"));
    return;
  }
  mPs5WarmUsb=(L[0]==0x315453414C575350ULL && L[3]==PS5_WARM_LASTUSB);   /* r5.25.12 */
  if(Flags&2){CopyMem(&mPs5WarmGuid,(VOID *)&R[2],16);mPs5WarmDirect=TRUE;}
  else if(Flags&8) {   /* r5.27.4: BootNext = one of the firmware's disk entries ("USB: <model>", "M.2: <model>") */
    UINT8 *Gb=(UINT8 *)&R[2]; UINT16 Opt=(UINT16)(Gb[0]|(Gb[1]<<8)); CHAR16 Var[16]; EFI_BOOT_MANAGER_LOAD_OPTION O;
    EFI_DEVICE_PATH_PROTOCOL *Fp=NULL; CHAR16 *Ds=NULL;
    /* r5.28.7 (HW 26.11. switch3/4): this firmware numbers its entries anew, so the option number from Windows can name another
     * disk here; the name hash from ResetSystem decides, the number is only a first guess */
    mPs5WarmNextHashOk=(Gb[6]==0xA5);
    if(mPs5WarmNextHashOk) { mPs5WarmNextHash=(UINT32)Gb[2]|((UINT32)Gb[3]<<8)|((UINT32)Gb[4]<<16)|((UINT32)Gb[5]<<24); if(Gb[7]&1) mPs5WarmUsb=TRUE; }
    UnicodeSPrint(Var,sizeof(Var),L"Boot%04x",Opt);
    if(!EFI_ERROR(EfiBootManagerVariableToLoadOption(Var,&O))) {
      if(!mPs5WarmNextHashOk || Ps5NameHash(O.Description)==mPs5WarmNextHash) {
        Fp=DuplicateDevicePath(O.FilePath);
        if(O.Description!=NULL) Ds=AllocateCopyPool(StrSize(O.Description),O.Description);
      } else DEBUG((DEBUG_ERROR,"PS5 warm re-entry: %s is '%s' here, not the entry Windows chose - looking it up by its name\n",Var,O.Description));
      EfiBootManagerFreeLoadOption(&O);
    }
    if(Fp==NULL && mPs5WarmNextHashOk) {
      UINTN Cnt=0; EFI_BOOT_MANAGER_LOAD_OPTION *All=EfiBootManagerGetLoadOptions(&Cnt,LoadOptionTypeBoot);
      for(UINTN I=0;I<Cnt && Fp==NULL;I++)
        if(All[I].Description!=NULL && Ps5NameHash(All[I].Description)==mPs5WarmNextHash) {
          Fp=DuplicateDevicePath(All[I].FilePath); Ds=AllocateCopyPool(StrSize(All[I].Description),All[I].Description);
          DEBUG((DEBUG_ERROR,"PS5 warm re-entry: the entry Windows chose is Boot%04x '%s' here\n",(UINT32)All[I].OptionNumber,All[I].Description));
        }
      if(All!=NULL) EfiBootManagerFreeLoadOptions(All,Cnt);
    }
    mPs5WarmNextDisk=Fp; mPs5WarmNextName=Ds;   /* r5.28.6: the name is the fallback when the path changed */
    if(Fp!=NULL) {
      CHAR16 *Ps=ConvertDevicePathToText(Fp,FALSE,FALSE); DEBUG((DEBUG_ERROR,"PS5 warm re-entry: BootNext disk path %s\n",Ps?Ps:L"?")); if (Ps) FreePool(Ps);
      for(EFI_DEVICE_PATH_PROTOCOL *N=Fp;!IsDevicePathEnd(N);N=NextDevicePathNode(N))
        if(DevicePathType(N)==MESSAGING_DEVICE_PATH && DevicePathSubType(N)==MSG_USB_DP) mPs5WarmUsb=TRUE;   /* wait for a USB disk */
    }
    if(Fp!=NULL || mPs5WarmNextHashOk) {
      DEBUG((DEBUG_ERROR,"PS5 warm re-entry: BootNext %s '%s' (name hash %08x%a) names a whole disk - its ESP is booted directly\n",
        Var,Ds?Ds:L"(no entry yet)",mPs5WarmNextHash,mPs5WarmNextHashOk?"":", none"));
      mPs5WarmDirect=TRUE; mPs5WarmNextPending=TRUE;
      gRT->SetVariable(L"BootNext",&gEfiGlobalVariableGuid,0,0,NULL);   /* one-shot, as the UEFI boot manager does */
    } else DEBUG((DEBUG_ERROR,"PS5 warm re-entry: BootNext %s not readable\n",Var));
  }
  else if(L[0]==0x315453414C575350ULL){CopyMem(&mPs5WarmGuid,(VOID *)&L[1],16);mPs5WarmDirect=TRUE;}
  if(!(Flags&8)) DEBUG((DEBUG_ERROR,"PS5 warm re-entry: direct boot of %g (%a)\n",&mPs5WarmGuid,(Flags&2)?"the OS boot entry":"the last boot device"));   /* r5.27.4: bit 3 logs its own line */
  DEBUG((DEBUG_ERROR,"PS5 warm re-entry: OS request flags %Lx, last boot device %a\n",Flags,mPs5WarmUsb?"USB":"internal"));   /* r5.25.11: see WarmReadOsRequest */
}
#ifdef PS5_NATIVE_SETUP
/* r5.6 (16.11., user request): power preset persistence. \PS5POWER.CFG on the boot ESP, plain text
 * "preset=<efficiency|standard|power>" (readable from Windows via mountvol). Loaded before the menu into the volatile
 * variable Ps5PowerPreset (the menu edits it), written back before bootmgr only if it changed. Missing/garbled file
 * = standard. Windows (ThermalManager) applies the limits; the firmware sets no clocks. */
#include "Ps5PowerPreset.h"
#include <Guid/FileInfo.h>
#include <Guid/GlobalVariable.h>
/* r5.7 (16.11., user: settings persistent and OS independent): \PS5UEFI.CFG (ps5-settings.h, text + crc32).
 * Write = PS5UEFI.NEW -> flush -> read back + parse -> CFG renamed to BAK -> NEW renamed to CFG; read = CFG, else NEW,
 * else BAK, else migrate \PS5POWER.CFG (preset only), else defaults. Only written when something changed.
 * Applied by the firmware: lang -> PlatformLang (en-US/de-DE) before the menu; preset -> volatile Ps5PowerPreset. */
#include "ps5-settings.h"
STATIC struct ps5_settings mPs5Set, mPs5SetFile;
STATIC PS5_BOOT_DEVS mPs5Devs;   /* r5.21: disks published to the setup (display order); Count 0 = setup not entered */
STATIC BOOLEAN mPs5SetValid;
STATIC EFI_GUID mPs5PresetGuid=PS5_POWER_PRESET_GUID;
STATIC unsigned Ps5SetCrc(CONST char *Data,unsigned Len) { UINT32 C=0; gBS->CalculateCrc32((VOID *)Data,Len,&C); return C; }
STATIC EFI_FILE_PROTOCOL *Ps5PresetRoot(EFI_HANDLE Esp) {
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs=NULL;EFI_FILE_PROTOCOL *Root=NULL;
  if(EFI_ERROR(gBS->HandleProtocol(Esp,&gEfiSimpleFileSystemProtocolGuid,(VOID **)&Fs)) || Fs==NULL)return NULL;
  if(EFI_ERROR(Fs->OpenVolume(Fs,&Root)))return NULL;
  return Root;
}
STATIC UINTN Ps5SetReadFile(EFI_FILE_PROTOCOL *Root,CHAR16 *Name,CHAR8 *Buf,UINTN Cap) {
  EFI_FILE_PROTOCOL *F=NULL;UINTN Size=Cap-1;
  if(EFI_ERROR(Root->Open(Root,&F,Name,EFI_FILE_MODE_READ,0)))return 0;
  if(EFI_ERROR(F->Read(F,&Size,Buf)))Size=0;
  F->Close(F);Buf[Size]=0;return Size;
}
STATIC EFI_STATUS Ps5SetRename(EFI_FILE_PROTOCOL *Root,CHAR16 *From,CHAR16 *To) {
  EFI_FILE_PROTOCOL *F=NULL;EFI_STATUS S=Root->Open(Root,&F,From,EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE,0);
  if(EFI_ERROR(S))return S;
  UINTN Sz=0;EFI_FILE_INFO *Info=NULL;
  S=F->GetInfo(F,&gEfiFileInfoGuid,&Sz,NULL);
  if(S==EFI_BUFFER_TOO_SMALL && (Info=AllocateZeroPool(Sz+64))!=NULL) {
    S=F->GetInfo(F,&gEfiFileInfoGuid,&Sz,Info);
    if(!EFI_ERROR(S)){StrCpyS(Info->FileName,(Sz+64-OFFSET_OF(EFI_FILE_INFO,FileName))/sizeof(CHAR16),To+1);Info->Size=SIZE_OF_EFI_FILE_INFO+StrSize(To+1);S=F->SetInfo(F,&gEfiFileInfoGuid,Info->Size,Info);}
    FreePool(Info);
  }
  F->Close(F);return S;
}
STATIC VOID Ps5SetDelete(EFI_FILE_PROTOCOL *Root,CHAR16 *Name) {
  EFI_FILE_PROTOCOL *F=NULL;if(!EFI_ERROR(Root->Open(Root,&F,Name,EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE,0)))F->Delete(F);
}
/* r5.25.13 firmware self-update (user: PSWloader.elf stays, Windows updates only the firmware). The updater puts the new
 * \PSWloader\firmware\pswloader-upl.bin (PS5UPL01 container, the same file as on the USB stick) on the EFI partition. A
 * firmware that finds a NEWER revision there (marker "PSWFWREV:rA.B.C") copies it to the unused arena part 0x1_1110_0000
 * and restarts into it through the warm re-entry (Ps5ResetSystemLib, cell PSWUPDT1); later Windows restarts start it
 * directly. \PSWloader\firmware\update.txt guards against a bad file: "pending <crc> <n>" before the switch, "ok <crc>" when
 * the new firmware reaches the Windows start, "failed <crc>" after two starts that never got there - then the file is ignored
 * and the built-in firmware keeps running. */
#define PS5_UPD_CELL  0x111806480ULL
#define PS5_UPD_IMAGE 0x111100000ULL
#define PS5_UPD_MAX   0x700000U
#define PS5_UPD_FILE  L"\\PSWloader\\firmware\\pswloader-upl.bin"
#define PS5_UPD_STATE L"\\PSWloader\\firmware\\update.txt"
__attribute__((used)) STATIC CONST CHAR8 mPs5FwRevMarker[]="PSWFWREV:" PS5_FW_RELEASE;
STATIC UINT32 Ps5RevValue(CONST CHAR8 *R) {   /* "r5.25.13" -> 0x0005190D (major.minor.patch, 0 = unreadable) */
  UINT32 V[3]={0,0,0},I=0,D=0;
  if(*R!='r')return 0;
  for(R++;*R && I<3;R++){ if(*R>='0'&&*R<='9'){V[I]=V[I]*10+(UINT32)(*R-'0');D=1;} else if(*R=='.'&&D){I++;D=0;} else break; }
  return (V[0]<<16)|(V[1]<<8)|(V[2]&0xFF);
}
STATIC VOID Ps5UpdState(EFI_FILE_PROTOCOL *Root,CONST CHAR8 *Text) {
  EFI_FILE_PROTOCOL *F=NULL;
  Ps5SetDelete(Root,PS5_UPD_STATE);
  if(!EFI_ERROR(Root->Open(Root,&F,PS5_UPD_STATE,EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,0))){
    UINTN N=AsciiStrLen(Text);F->Write(F,&N,(VOID *)Text);F->Flush(F);F->Close(F);
  }
}
STATIC VOID Ps5FirmwareSelfUpdate(EFI_HANDLE Esp)
{
  volatile UINT64 *U=(volatile UINT64 *)(UINTN)PS5_UPD_CELL;
  if(Esp==NULL || mPs5UpdateRun)return;
  EFI_FILE_PROTOCOL *Root=Ps5PresetRoot(Esp),*F=NULL;
  if(Root==NULL)return;
  if(EFI_ERROR(Root->Open(Root,&F,PS5_UPD_FILE,EFI_FILE_MODE_READ,0))){Root->Close(Root);return;}
  UINTN Size=PS5_UPD_MAX;UINT8 *Img=(UINT8 *)(UINTN)PS5_UPD_IMAGE;
  EFI_STATUS S=F->Read(F,&Size,Img);F->Close(F);
  CHAR8 Why[64]="";UINT32 Crc=0,Have=Ps5RevValue(PS5_FW_RELEASE),Want=0;CHAR8 WantText[16]="?";
  if(EFI_ERROR(S) || Size<8192 || Size>=PS5_UPD_MAX)AsciiStrCpyS(Why,sizeof(Why),"unreadable or too large");
  else if(CompareMem(Img,"PS5UPL01",8)!=0 || *(UINT32 *)(Img+12)!=4096 || *(UINT64 *)(Img+16)!=0x41000000ULL ||
          *(UINT64 *)(Img+24)+4096>Size)AsciiStrCpyS(Why,sizeof(Why),"not a PSWloader firmware");
  else {
    UINT32 H=0,B=0;gBS->CalculateCrc32(Img,4092,&H);gBS->CalculateCrc32(Img+4096,(UINTN)*(UINT64 *)(Img+24),&B);
    if(H!=*(UINT32 *)(Img+4092) || B!=*(UINT32 *)(Img+44))AsciiStrCpyS(Why,sizeof(Why),"checksum mismatch");
    else {
      Crc=B;
      for(UINTN I=4096;I+24<Size;I++) if(Img[I]=='P' && CompareMem(Img+I,"PSWFWREV:r",10)==0 && Img[I+10]>='0' && Img[I+10]<='9') {   /* digit: not this search string */
        UINTN J=0;for(;J<15 && Img[I+9+J]>' ' && Img[I+9+J]<0x7F;J++)WantText[J]=(CHAR8)Img[I+9+J];
        WantText[J]=0;Want=Ps5RevValue(WantText);break;
      }
      if(!Want)AsciiStrCpyS(Why,sizeof(Why),"no revision marker");
      else if(Want<=Have)AsciiStrCpyS(Why,sizeof(Why),"not newer");
    }
  }
  if(Why[0]){
    DEBUG((DEBUG_ERROR,"PS5 firmware update: %s skipped (%a; file %a, running %a)\n",PS5_UPD_FILE,Why,WantText,mPs5FwRevMarker+9));
    Root->Close(Root);return;
  }
  CHAR8 St[64];UINTN N=Ps5SetReadFile(Root,PS5_UPD_STATE,St,sizeof(St));CHAR8 Tag[16];UINT32 Tries=0;
  AsciiSPrint(Tag,sizeof(Tag),"%08x",Crc);
  if(N && AsciiStrStr(St,"failed") && AsciiStrStr(St,Tag)){
    DEBUG((DEBUG_ERROR,"PS5 firmware update: %a (%a) failed before - keeping %a\n",WantText,Tag,PS5_FW_RELEASE));
    Root->Close(Root);return;
  }
  if(N && AsciiStrStr(St,"pending") && AsciiStrStr(St,Tag)){
    CHAR8 *P=AsciiStrStr(St,Tag)+8;while(*P==' ')P++;while(*P>='0'&&*P<='9')Tries=Tries*10+(UINT32)(*P++-'0');
  }
  if(Tries>=2){
    CHAR8 Line[48];AsciiSPrint(Line,sizeof(Line),"failed %a %a\r\n",Tag,WantText);Ps5UpdState(Root,Line);
    DEBUG((DEBUG_ERROR,"PS5 firmware update: %a never reached the Windows start (%u tries) - marked failed, keeping %a\n",WantText,Tries,PS5_FW_RELEASE));
    Root->Close(Root);return;
  }
  { CHAR8 Line[48];AsciiSPrint(Line,sizeof(Line),"pending %a %u %a\r\n",Tag,Tries+1,WantText);Ps5UpdState(Root,Line); }
  Root->Close(Root);
  { EFI_BLOCK_IO_PROTOCOL *Bio=NULL;
    if(!EFI_ERROR(gBS->HandleProtocol(Esp,&gEfiBlockIoProtocolGuid,(VOID **)&Bio)) && Bio!=NULL && Bio->FlushBlocks!=NULL)Bio->FlushBlocks(Bio); }
  U[1]=PS5_UPD_IMAGE;U[2]=Size;U[3]=1;U[0]=0x3154445055575350ULL;   /* "PSWUPDT1" */
  AsmWbinvd();
  DEBUG((DEBUG_ERROR,"PS5 firmware update: starting %a from the EFI partition (running %a, %u bytes, crc %a)\n",WantText,PS5_FW_RELEASE,(UINT32)Size,Tag));
  gRT->ResetSystem(EfiResetCold,EFI_SUCCESS,0,NULL);
  U[0]=0;   /* only if the re-entry was not prepared (then ResetSystem would have switched off already) */
}
/* r5.25.13: the updated firmware reached the Windows start - confirm it */
STATIC VOID Ps5FirmwareUpdateConfirm(EFI_HANDLE Esp)
{
  volatile UINT64 *U=(volatile UINT64 *)(UINTN)PS5_UPD_CELL;
  if(!mPs5UpdateRun || Esp==NULL || U[0]!=0x3154445055575350ULL)return;
  EFI_FILE_PROTOCOL *Root=Ps5PresetRoot(Esp);if(Root==NULL)return;
  CHAR8 Line[48];AsciiSPrint(Line,sizeof(Line),"ok %08x %a\r\n",*(UINT32 *)(UINTN)(PS5_UPD_IMAGE+44),PS5_FW_RELEASE);
  Ps5UpdState(Root,Line);Root->Close(Root);
  DEBUG((DEBUG_ERROR,"PS5 firmware update: %a confirmed (reached the Windows start)\n",PS5_FW_RELEASE));
}
/* r5.15.2: the ESP that owns \PS5UEFI.CFG = the internal one that would boot without bootorder=usb (unpinned internal,
 * else the unique non-removable pinned ESP). NULL = no internal Windows (empty target): settings live on the selected medium.
 * (declared above the PS5_NATIVE_SETUP block) */
STATIC VOID Ps5PowerPresetLoad(EFI_HANDLE Esp) {
  STATIC CHAR16 *Names[3]={L"\\PS5UEFI.CFG",L"\\PS5UEFI.NEW",L"\\PS5UEFI.BAK"};
  CHAR8 Buf[768];CONST CHAR8 *From="defaults";
  EFI_FILE_PROTOCOL *Root=Ps5PresetRoot(Esp);
  ps5_settings_default(&mPs5Set);mPs5SetValid=FALSE;
  if(Root!=NULL) {
    for(UINTN I=0;I<3 && !mPs5SetValid;I++) {
      UINTN N=Ps5SetReadFile(Root,Names[I],Buf,sizeof(Buf));
      if(N && ps5_settings_parse(Buf,(unsigned)N,Ps5SetCrc,&mPs5Set)){mPs5SetValid=TRUE;From=I==0?"PS5UEFI.CFG":(I==1?"PS5UEFI.NEW":"PS5UEFI.BAK");}
    }
    if(!mPs5SetValid) {                                            /* migration from r5.6 */
      UINTN N=Ps5SetReadFile(Root,L"\\PS5POWER.CFG",Buf,sizeof(Buf));
      int P=N?ps5_settings_legacy_preset(Buf,(unsigned)N):-1;
      if(P>=0){mPs5Set.preset=(unsigned)P;From="PS5POWER.CFG (migrated)";}
    }
    Root->Close(Root);
  }
  mPs5SetFile=mPs5Set;
  if(!mPs5SetValid)mPs5SetFile.preset=99;                          /* forces one write of the new file */
  PS5_POWER_PRESET V;V.Preset=mPs5Set.preset;
  EFI_STATUS S=gRT->SetVariable(L"Ps5PowerPreset",&mPs5PresetGuid,EFI_VARIABLE_BOOTSERVICE_ACCESS|EFI_VARIABLE_RUNTIME_ACCESS,sizeof(V),&V);
  CHAR8 *Lang=mPs5Set.lang==1?"de-DE":"en-US";
  mPs5LangDe=mPs5Set.lang==1;   /* r5.17: start screen language */
  { UINT32 Fb=mPs5Set.fastboot; mPs5FastBoot=Fb; gRT->SetVariable(L"Ps5FastBoot",&mPs5PresetGuid,EFI_VARIABLE_BOOTSERVICE_ACCESS|EFI_VARIABLE_RUNTIME_ACCESS,sizeof(Fb),&Fb); }
  { UINT32 Fr=mPs5Set.fastrestart; gRT->SetVariable(L"Ps5FastRestart",&mPs5PresetGuid,EFI_VARIABLE_BOOTSERVICE_ACCESS|EFI_VARIABLE_RUNTIME_ACCESS,sizeof(Fr),&Fr); }   /* r5.25.12 */
  { UINT32 Uc=mPs5Set.usbcycle; gRT->SetVariable(L"Ps5UsbCycle",&mPs5PresetGuid,EFI_VARIABLE_BOOTSERVICE_ACCESS|EFI_VARIABLE_RUNTIME_ACCESS,sizeof(Uc),&Uc); }   /* r5.25.1 */
  { UINT32 Bo=mPs5Set.bootorder; gRT->SetVariable(L"Ps5BootOrder",&mPs5PresetGuid,EFI_VARIABLE_BOOTSERVICE_ACCESS|EFI_VARIABLE_RUNTIME_ACCESS,sizeof(Bo),&Bo); }   /* r5.15 */
  { UINT32 Rv=mPs5Set.refresh; gRT->SetVariable(L"Ps5Refresh",&mPs5PresetGuid,EFI_VARIABLE_BOOTSERVICE_ACCESS|EFI_VARIABLE_RUNTIME_ACCESS,sizeof(Rv),&Rv); }   /* r5.16 */
  { UINT32 Res=mPs5Set.resolution; gRT->SetVariable(L"Ps5Resolution",&mPs5PresetGuid,EFI_VARIABLE_BOOTSERVICE_ACCESS|EFI_VARIABLE_RUNTIME_ACCESS,sizeof(Res),&Res); }   /* r5.27 */
  { UINT32 Pv=mPs5Set.pstates; gRT->SetVariable(L"Ps5PStates",&mPs5PresetGuid,EFI_VARIABLE_BOOTSERVICE_ACCESS|EFI_VARIABLE_RUNTIME_ACCESS,sizeof(Pv),&Pv); }   /* r5.12 */
  { PS5_LANG Lv; Lv.Lang=mPs5Set.lang; gRT->SetVariable(L"Ps5Lang",&mPs5PresetGuid,EFI_VARIABLE_BOOTSERVICE_ACCESS|EFI_VARIABLE_RUNTIME_ACCESS,sizeof(Lv),&Lv); }
  EFI_STATUS L=gRT->SetVariable(L"PlatformLang",&gEfiGlobalVariableGuid,EFI_VARIABLE_BOOTSERVICE_ACCESS|EFI_VARIABLE_RUNTIME_ACCESS|EFI_VARIABLE_NON_VOLATILE,AsciiStrSize(Lang),Lang);
  DEBUG((DEBUG_ERROR,"PS5 settings: %a preset=%a lang=%a fastboot=%u fastrestart=%u pstates=%u (variable %r, PlatformLang %r)\n",From,ps5_set_preset[mPs5Set.preset],
    ps5_set_lang[mPs5Set.lang],mPs5Set.fastboot,mPs5Set.fastrestart,mPs5Set.pstates,S,L));
}
#ifdef PS5_NATIVE_DP_VIDEO
/* r5.27: volatile runtime variable "Ps5DisplayInfo" (GUID PS5_POWER_PRESET_GUID, BS|RT, 40 bytes) for the Windows tool
 * PSWDisplay: Version 1; ChoiceMask = bit (1<<choice) per resolution the monitor offers and the firmware can start
 * (choice = index of ps5_set_res, ps5-settings.h); Current = choice of the started mode (0 = not a user choice);
 * Requested = resolution= of the settings (0 = auto); Result 0 automatic / 1 applied / 2 requested but not available;
 * ConfigEsp = GPT unique partition GUID of the ESP holding \PS5UEFI.CFG (zero = none). */
typedef struct { UINT32 Version, ChoiceMask, Current, Requested, Result, Reserved; EFI_GUID ConfigEsp; } PS5_DISPLAY_INFO;
STATIC VOID Ps5PublishDisplayInfo(EFI_HANDLE ConfigEsp) {
  PS5_DISPLAY_INFO I; EFI_PARTITION_INFO_PROTOCOL *Pi=NULL;
  ZeroMem(&I,sizeof(I));
  I.Version=1; I.ChoiceMask=mPs5ChoiceMask; I.Current=ps5_display_choice_of(mPs5ModeId); I.Requested=mPs5Resolution; I.Result=mPs5ResResult;
  if(ConfigEsp && !EFI_ERROR(gBS->HandleProtocol(ConfigEsp,&gEfiPartitionInfoProtocolGuid,(VOID **)&Pi)) && Pi!=NULL && Pi->Type==PARTITION_TYPE_GPT)
    CopyGuid(&I.ConfigEsp,&Pi->Info.Gpt.UniquePartitionGUID);
  EFI_STATUS S=gRT->SetVariable(L"Ps5DisplayInfo",&mPs5PresetGuid,EFI_VARIABLE_BOOTSERVICE_ACCESS|EFI_VARIABLE_RUNTIME_ACCESS,sizeof(I),&I);
  DEBUG((DEBUG_ERROR,"PS5 display info: choices %x current %u requested %u result %u config ESP %g (%r)\n",I.ChoiceMask,I.Current,I.Requested,I.Result,&I.ConfigEsp,S));
}
#endif
STATIC EFI_STATUS Ps5PowerPresetSave(EFI_HANDLE Esp) {
  PS5_POWER_PRESET V;UINTN VSize=sizeof(V);CHAR8 Lang[8];UINTN LSize=sizeof(Lang);
  if(!EFI_ERROR(gRT->GetVariable(L"Ps5PowerPreset",&mPs5PresetGuid,NULL,&VSize,&V)) && VSize==sizeof(V) && V.Preset<=2)mPs5Set.preset=V.Preset;
  if(!EFI_ERROR(gRT->GetVariable(L"PlatformLang",&gEfiGlobalVariableGuid,NULL,&LSize,Lang)) && LSize>=3)mPs5Set.lang=(Lang[0]=='d' && Lang[1]=='e')?1:0;
  { UINT32 Fb=0;UINTN Fs=sizeof(Fb); if(!EFI_ERROR(gRT->GetVariable(L"Ps5FastBoot",&mPs5PresetGuid,NULL,&Fs,&Fb)) && Fs==sizeof(Fb))mPs5Set.fastboot=Fb?1:0; }
  { UINT32 Fr=0;UINTN Frs=sizeof(Fr); if(!EFI_ERROR(gRT->GetVariable(L"Ps5FastRestart",&mPs5PresetGuid,NULL,&Frs,&Fr)) && Frs==sizeof(Fr))mPs5Set.fastrestart=Fr?1:0; }   /* r5.25.12 */
  { UINT32 Uc=0;UINTN Us=sizeof(Uc); if(!EFI_ERROR(gRT->GetVariable(L"Ps5UsbCycle",&mPs5PresetGuid,NULL,&Us,&Uc)) && Us==sizeof(Uc))mPs5Set.usbcycle=Uc?1:0; }   /* r5.25.1 */
  { UINT32 Bo=0;UINTN Bs=sizeof(Bo); if(!EFI_ERROR(gRT->GetVariable(L"Ps5BootOrder",&mPs5PresetGuid,NULL,&Bs,&Bo)) && Bs==sizeof(Bo))mPs5Set.bootorder=Bo?1:0; }   /* r5.15 */
  { UINT32 Rv=0;UINTN Rs=sizeof(Rv); if(!EFI_ERROR(gRT->GetVariable(L"Ps5Refresh",&mPs5PresetGuid,NULL,&Rs,&Rv)) && Rs==sizeof(Rv))mPs5Set.refresh=Rv?1:0; }   /* r5.16 */
  { UINT32 Res=0;UINTN Ress=sizeof(Res); if(!EFI_ERROR(gRT->GetVariable(L"Ps5Resolution",&mPs5PresetGuid,NULL,&Ress,&Res)) && Ress==sizeof(Res))mPs5Set.resolution=Res<PS5_SET_RES_COUNT?Res:0; }   /* r5.27 */
  { UINT32 Pv=0;UINTN Ps=sizeof(Pv); if(!EFI_ERROR(gRT->GetVariable(L"Ps5PStates",&mPs5PresetGuid,NULL,&Ps,&Pv)) && Ps==sizeof(Pv))mPs5Set.pstates=Pv?1:0; }   /* r5.12 */
  if(mPs5Devs.Count) {   /* r5.21: order arranged in the Boot tab -> bootlist (unplugged disks keep their place) */
    PS5_BOOT_LIST Sel;UINTN Ss=sizeof(Sel);
    EFI_STATUS Gs=gRT->GetVariable(L"Ps5BootListSel",&mPs5PresetGuid,NULL,&Ss,&Sel);
    if(EFI_ERROR(Gs) || Ss!=sizeof(Sel)) DEBUG((DEBUG_ERROR,"PS5 boot list: save check - no Ps5BootListSel (%r, %u bytes)\n",Gs,(UINT32)Ss));   /* r5.25.4 */
    if(!EFI_ERROR(Gs) && Ss==sizeof(Sel)) {
      unsigned NewOrd[PS5_BOOTLIST_MAX],Nn=0,Changed=0,Merged[PS5_BOOTLIST_MAX];
      for(UINTN I=0;I<PS5_BOOTDEV_MAX && Sel.Order[I];I++) {
        UINTN Ix=(UINTN)Sel.Order[I]-1;BOOLEAN Dup=FALSE;
        if(Ix>=mPs5Devs.Count)continue;
        for(UINTN J=0;J<Nn;J++)if(NewOrd[J]==mPs5Devs.Id[Ix])Dup=TRUE;
        if(!Dup){if(Ix!=Nn)Changed=1;NewOrd[Nn++]=mPs5Devs.Id[Ix];}
      }
      DEBUG((DEBUG_ERROR,"PS5 boot list: save check - Ps5BootListSel %02x %02x %02x, %u of %u disks, changed %u, stored list %u\n",
        Sel.Order[0],Sel.Order[1],Sel.Order[2],(UINT32)Nn,(UINT32)mPs5Devs.Count,(UINT32)Changed,(UINT32)mPs5Set.bootlist_n));   /* r5.25.4: diagnosis */
      if(Nn==mPs5Devs.Count && (Changed || (mPs5Set.bootlist_n==0 && mPs5Set.bootorder==1))) {
        unsigned M=ps5_bootlist_merge(mPs5Set.bootlist,mPs5Set.bootlist_n,NewOrd,Nn,Merged);
        CopyMem(mPs5Set.bootlist,Merged,M*sizeof(unsigned));mPs5Set.bootlist_n=M;
        DEBUG((DEBUG_ERROR,"PS5 boot list: %u disks arranged, list now %u ids\n",(UINT32)Nn,(UINT32)M));
      }
    }
  }
  if(mPs5Set.bootlist_n==mPs5SetFile.bootlist_n && CompareMem(mPs5Set.bootlist,mPs5SetFile.bootlist,mPs5Set.bootlist_n*sizeof(unsigned))==0 &&
     mPs5Set.preset==mPs5SetFile.preset && mPs5Set.lang==mPs5SetFile.lang && mPs5Set.fastboot==mPs5SetFile.fastboot && mPs5Set.pstates==mPs5SetFile.pstates &&
     mPs5Set.bootorder==mPs5SetFile.bootorder && mPs5Set.refresh==mPs5SetFile.refresh && mPs5Set.resolution==mPs5SetFile.resolution && mPs5Set.diag==mPs5SetFile.diag && mPs5Set.usbcycle==mPs5SetFile.usbcycle && mPs5Set.fastrestart==mPs5SetFile.fastrestart)return EFI_SUCCESS;              /* unchanged: no ESP write */
  EFI_FILE_PROTOCOL *Root=Ps5PresetRoot(Esp),*File=NULL;
  if(Root==NULL)return EFI_NOT_FOUND;
  CHAR8 Text[640],Back[768];UINTN Size=ps5_settings_format(&mPs5Set,Ps5SetCrc,Text,sizeof(Text));   /* r5.20.2: + PS5_SET_EXTRA; r5.21: + bootlist */
  struct ps5_settings Check;
  Ps5SetDelete(Root,L"\\PS5UEFI.NEW");
  EFI_STATUS S=Root->Open(Root,&File,L"\\PS5UEFI.NEW",EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,0);
  if(!EFI_ERROR(S)) {
    UINTN W=Size;S=File->Write(File,&W,Text);
    EFI_STATUS F=File->Flush(File);File->Close(File);
    if(!EFI_ERROR(S))S=F;
  }
  UINTN N=EFI_ERROR(S)?0:Ps5SetReadFile(Root,L"\\PS5UEFI.NEW",Back,sizeof(Back));
  if(!EFI_ERROR(S) && !(N==Size && ps5_settings_parse(Back,(unsigned)N,Ps5SetCrc,&Check)))S=EFI_VOLUME_CORRUPTED;
  if(!EFI_ERROR(S)) {
    Ps5SetDelete(Root,L"\\PS5UEFI.BAK");
    Ps5SetRename(Root,L"\\PS5UEFI.CFG",L"\\PS5UEFI.BAK");          /* may fail on the first write: no CFG yet */
    S=Ps5SetRename(Root,L"\\PS5UEFI.NEW",L"\\PS5UEFI.CFG");
    if(!EFI_ERROR(S)){Root->Flush(Root);mPs5SetFile=mPs5Set;}
  }
  Root->Close(Root);
  { EFI_BLOCK_IO_PROTOCOL *Bio=NULL;
    if(!EFI_ERROR(S) && !EFI_ERROR(gBS->HandleProtocol(Esp,&gEfiBlockIoProtocolGuid,(VOID **)&Bio)) && Bio!=NULL && Bio->FlushBlocks!=NULL)
      Bio->FlushBlocks(Bio); }
  DEBUG((DEBUG_ERROR,"PS5 settings: saved preset=%a lang=%a fastboot=%u refresh=%u resolution=%a pstates=%u usbcycle=%u to \\PS5UEFI.CFG (%r)\n",
    ps5_set_preset[mPs5Set.preset],ps5_set_lang[mPs5Set.lang],mPs5Set.fastboot,mPs5Set.refresh,ps5_set_res[mPs5Set.resolution<PS5_SET_RES_COUNT?mPs5Set.resolution:0],mPs5Set.pstates,mPs5Set.usbcycle,S));
  return S;
}
/* r5.22 (17.11., plan install-testsigning A): testsigning in the BCD of every started Windows ESP (stick and installed
 * disk), so our PSWloader-signed boot drivers load on the first start after the stock setup GUI. Only the element
 * 16000049 of the default loader is added (ps5-bcd.h); BCD.PSWNEW -> read back -> BCD.PSWBAK -> BCD, attributes kept.
 * Off with testsign=off in \PS5UEFI.CFG. Called before bootmgr (Ps5LaunchWindows) and at ReadyToBoot for boots from the
 * boot menu (BootCurrent). */
#include "ps5-bcd.h"
STATIC BOOLEAN Ps5VolumeHasFile (EFI_HANDLE Handle, CHAR16 *Path);
STATIC EFI_STATUS Ps5BcdRename(EFI_FILE_PROTOCOL *Dir,CHAR16 *From,CHAR16 *To,UINT64 Attr)
{
  EFI_FILE_PROTOCOL *F=NULL;UINTN Sz=0;EFI_FILE_INFO *Info=NULL;
  EFI_STATUS S=Dir->Open(Dir,&F,From,EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE,0);
  if(EFI_ERROR(S))return S;
  S=F->GetInfo(F,&gEfiFileInfoGuid,&Sz,NULL);
  if(S==EFI_BUFFER_TOO_SMALL && (Info=AllocateZeroPool(Sz+64))!=NULL) {
    S=F->GetInfo(F,&gEfiFileInfoGuid,&Sz,Info);
    if(!EFI_ERROR(S)){StrCpyS(Info->FileName,(Sz+64-OFFSET_OF(EFI_FILE_INFO,FileName))/sizeof(CHAR16),To);
      if(Attr!=(UINT64)-1)Info->Attribute=Attr;
      Info->Size=SIZE_OF_EFI_FILE_INFO+StrSize(To);S=F->SetInfo(F,&gEfiFileInfoGuid,Info->Size,Info);}
    FreePool(Info);
  }
  F->Close(F);return S;
}
#ifdef PS5_LAB_SANDISK
/* lab: start the internal SanDisk's own Windows (its BCD default pointed at an interrupted install on another disk) */
STATIC VOID Ps5LabSanDiskDefault(EFI_HANDLE Esp)
{
  EFI_FILE_PROTOCOL *Root,*Dir=NULL,*F=NULL;UINT8 *Buf=NULL;UINTN Size=0,Sz=0,N;EFI_FILE_INFO *Info=NULL;EFI_STATUS S;char Og[40],Ng[40];int R;
  if(Esp==NULL || (Root=Ps5PresetRoot(Esp))==NULL)return;
  if(EFI_ERROR(Root->Open(Root,&Dir,L"\\EFI\\Microsoft\\Boot",EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE,0))){Root->Close(Root);return;}
  if(!EFI_ERROR(Dir->Open(Dir,&F,L"BCD.PSWLAB",EFI_FILE_MODE_READ,0))){F->Close(F);DEBUG((DEBUG_ERROR,"PS5 lab SanDisk BCD: already switched\n"));goto Done;}
  if(EFI_ERROR(Dir->Open(Dir,&F,L"BCD",EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE,0)))goto Done;
  if(F->GetInfo(F,&gEfiFileInfoGuid,&Sz,NULL)==EFI_BUFFER_TOO_SMALL && (Info=AllocateZeroPool(Sz))!=NULL && !EFI_ERROR(F->GetInfo(F,&gEfiFileInfoGuid,&Sz,Info)))Size=(UINTN)Info->FileSize;
  if(Info)FreePool(Info);
  if(Size<0x2000 || Size>0x400000 || (Buf=AllocateZeroPool(Size))==NULL){F->Close(F);goto Done;}
  N=Size;S=F->Read(F,&N,Buf);
  if(EFI_ERROR(S) || N!=Size){F->Close(F);goto Done;}
  { EFI_FILE_PROTOCOL *B=NULL; UINTN Nb=Size;
    if(EFI_ERROR(Dir->Open(Dir,&B,L"BCD.PSWLAB",EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,0))){F->Close(F);goto Done;}
    S=B->Write(B,&Nb,Buf);B->Flush(B);B->Close(B);
    if(EFI_ERROR(S)){F->Close(F);goto Done;} }
  R=ps5_bcd_default_to_other(Buf,Size,Og,Ng);
  if(R==1){F->SetPosition(F,0);N=Size;S=F->Write(F,&N,Buf);F->Flush(F);}
  F->Close(F);
  DEBUG((DEBUG_ERROR,"PS5 lab SanDisk BCD: default %a -> %a (%d, write %r)\n",Og,R==1?Ng:"(unchanged)",R,R==1?S:EFI_SUCCESS));
Done:
  if(Buf)FreePool(Buf);
  Dir->Close(Dir);Root->Close(Root);
}
#endif
STATIC VOID Ps5BcdTestsign(EFI_HANDLE Esp,CONST CHAR8 *Why)
{
  EFI_FILE_PROTOCOL *Root,*Dir=NULL,*F=NULL;UINT8 *Buf=NULL,*Back=NULL;UINTN Size=0,Sz=0,N;size_t Out=0,Chk=0;EFI_FILE_INFO *Info=NULL;
  UINT64 Attr=(UINT64)-1;int R;EFI_STATUS S;
  if(Esp==NULL || mPs5Set.testsign==1)return;
  if((Root=Ps5PresetRoot(Esp))==NULL)return;
  if(EFI_ERROR(Root->Open(Root,&Dir,L"\\EFI\\Microsoft\\Boot",EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE,0))){Root->Close(Root);return;}
  if(EFI_ERROR(Dir->Open(Dir,&F,L"BCD",EFI_FILE_MODE_READ,0)))goto Done;
  if(F->GetInfo(F,&gEfiFileInfoGuid,&Sz,NULL)==EFI_BUFFER_TOO_SMALL && (Info=AllocateZeroPool(Sz))!=NULL &&
     !EFI_ERROR(F->GetInfo(F,&gEfiFileInfoGuid,&Sz,Info))){Size=(UINTN)Info->FileSize;Attr=Info->Attribute;}
  if(Info)FreePool(Info);
  if(Size<0x2000 || Size>0x400000 || (Buf=AllocateZeroPool(Size+0x1000))==NULL){F->Close(F);goto Done;}
  N=Size;S=F->Read(F,&N,Buf);F->Close(F);F=NULL;
  if(EFI_ERROR(S) || N!=Size)goto Done;
  BOOLEAN Lab=FALSE;
#ifdef PS5_WARM_RESET_TEST
  { size_t O1=0; int R1=ps5_bcd_enable_element(Buf,Size,Size+0x1000,&O1,"26000060");   /* lab: onecpu (warm re-entry AP test) */
    DEBUG((DEBUG_ERROR,"PS5 BCD lab onecpu: %d\n",R1)); if(R1==1){Buf=ReallocatePool(Size+0x1000,O1+0x1000,Buf);Size=O1;Lab=TRUE;if(Buf==NULL)goto Done;} }
#endif
  R=ps5_bcd_enable_testsigning(Buf,Size,Size+0x1000,&Out);
  if(R==0 && Lab){R=1;Out=Size;}
  if(R!=1){DEBUG((DEBUG_ERROR,"PS5 BCD testsigning: %a: %a (%d)\n",Why,R==0?"already set":"not changed",R));goto Done;}
  Ps5SetDelete(Dir,L"BCD.PSWNEW");
  S=Dir->Open(Dir,&F,L"BCD.PSWNEW",EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,0);
  if(!EFI_ERROR(S)){N=Out;S=F->Write(F,&N,Buf);EFI_STATUS Fl=F->Flush(F);F->Close(F);F=NULL;if(!EFI_ERROR(S))S=Fl;}
  if(!EFI_ERROR(S) && (Back=AllocateZeroPool(Out+0x1000))!=NULL && !EFI_ERROR(Dir->Open(Dir,&F,L"BCD.PSWNEW",EFI_FILE_MODE_READ,0))) {
    N=Out;S=F->Read(F,&N,Back);F->Close(F);F=NULL;
    if(!EFI_ERROR(S) && (N!=Out || CompareMem(Back,Buf,Out)!=0 || ps5_bcd_enable_testsigning(Back,Out,Out+0x1000,&Chk)!=0))S=EFI_VOLUME_CORRUPTED;
  } else if(!EFI_ERROR(S))S=EFI_DEVICE_ERROR;
  if(!EFI_ERROR(S)) {
    Ps5SetDelete(Dir,L"BCD.PSWBAK");
    S=Ps5BcdRename(Dir,L"BCD",L"BCD.PSWBAK",(UINT64)-1);
    if(!EFI_ERROR(S))S=Ps5BcdRename(Dir,L"BCD.PSWNEW",L"BCD",Attr);
  } else Ps5SetDelete(Dir,L"BCD.PSWNEW");
  DEBUG((DEBUG_ERROR,"PS5 BCD testsigning: %a: element 16000049 added (%u -> %u bytes): %r\n",Why,(UINT32)Size,(UINT32)Out,S));
Done:
  if(Buf)FreePool(Buf);
  if(Back)FreePool(Back);
  Dir->Close(Dir);Root->Close(Root);
}
/* boot option device path (up to the file path node) matches a volume: prefix of its full path, or the same HD node */
STATIC BOOLEAN Ps5BootPathMatches(EFI_DEVICE_PATH_PROTOCOL *Opt,EFI_DEVICE_PATH_PROTOCOL *Vol)
{
  UINTN L=0;EFI_DEVICE_PATH_PROTOCOL *N;
  if(Opt==NULL || Vol==NULL)return FALSE;
  for(N=Opt;!IsDevicePathEnd(N) && !(DevicePathType(N)==MEDIA_DEVICE_PATH && DevicePathSubType(N)==MEDIA_FILEPATH_DP);N=NextDevicePathNode(N))
    L+=DevicePathNodeLength(N);
  if(L==0)return FALSE;
  if(DevicePathType(Opt)==MEDIA_DEVICE_PATH && DevicePathSubType(Opt)==MEDIA_HARDDRIVE_DP) {   /* short form */
    for(N=Vol;!IsDevicePathEnd(N);N=NextDevicePathNode(N))
      if(DevicePathNodeLength(N)==DevicePathNodeLength(Opt) && CompareMem(N,Opt,DevicePathNodeLength(N))==0)return TRUE;
    return FALSE;
  }
  return GetDevicePathSize(Vol)>=L+sizeof(EFI_DEVICE_PATH_PROTOCOL) && CompareMem(Opt,Vol,L)==0;
}
STATIC VOID Ps5WarmMsixUnmask(VOID);   /* r5.27.0: also for starts from the Boot Manager, see below */
STATIC VOID Ps5ApplyFanTarget(CONST CHAR8 *When,EFI_HANDLE Esp);   /* r5.28.7: quiet fan table at ReadyToBoot */
STATIC VOID EFIAPI Ps5BcdReadyToBoot(EFI_EVENT Event,VOID *Context)
{
  UINT16 Cur=0;UINTN S=sizeof(Cur),Count=0;CHAR16 Name[16];EFI_BOOT_MANAGER_LOAD_OPTION Opt;EFI_HANDLE *H=NULL;
  (VOID)Event;(VOID)Context;
  /* r5.27.0 (known issue v1.1: "Install driver to show hardware", no keyboard/mouse in Windows Setup after "Boot once from
   * ..." following the restart an NTFS installer drive asks for): that restart is a warm re-entry, whose PCI replay leaves the
   * MSI-X function mask set (F34). Only Ps5LaunchWindows cleared it - a start from the Boot Manager went to bootmgr with
   * every xHCI vector masked. ReadyToBoot runs for every start; a second call (normal path) finds nothing to do. */
#ifndef PS5_LAB_NO_RTB_UNMASK   /* lab (--lab-no-rtb-unmask): the state before this fix */
  Ps5WarmMsixUnmask();
#endif
  if(mPs5ConfigEsp!=NULL || mPs5BootSelected!=NULL) Ps5PowerPresetSave(mPs5ConfigEsp?mPs5ConfigEsp:mPs5BootSelected);
  if(mPs5ConfigEsp!=NULL || mPs5BootSelected!=NULL) Ps5ApplyFanTarget("boot menu",mPs5ConfigEsp?mPs5ConfigEsp:mPs5BootSelected);   /* r5.28.7: only if not up yet */
  if(mPs5Set.testsign==1 || EFI_ERROR(gRT->GetVariable(L"BootCurrent",&gEfiGlobalVariableGuid,NULL,&S,&Cur)))return;
  UnicodeSPrint(Name,sizeof(Name),L"Boot%04X",Cur);
  if(EFI_ERROR(EfiBootManagerVariableToLoadOption(Name,&Opt)))return;
  if(!EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol,&gEfiSimpleFileSystemProtocolGuid,NULL,&Count,&H))) {
    for(UINTN I=0;I<Count;I++)
      if(Ps5VolumeHasFile(H[I],L"\\EFI\\Microsoft\\Boot\\BCD") && Ps5BootPathMatches(Opt.FilePath,DevicePathFromHandle(H[I])))
        Ps5BcdTestsign(H[I],"boot menu");
    FreePool(H);
  }
  EfiBootManagerFreeLoadOption(&Opt);
}
/* r5.12 (16.11., telemetry plan step 2): ACPI processor performance SSDT (ps5-pstate-ssdt.h) - only with
 * pstates=1 (Advanced -> CPU P-states). _PSS = exactly the enabled PStateDef MSRs of the BSP (read-only here); the OS
 * then selects P-states via C0010062 (FFixedHW). Installed once, right before bootmgr, via EFI_ACPI_TABLE_PROTOCOL
 * (AcpiTableDxe re-publishes the bootstrap tables, the SSDT is appended to the XSDT). */
#include <Protocol/AcpiTable.h>
#include "ps5-pstate-ssdt.h"
/* r5.14 (16.11., user: heat/fan): the MainSoC fan target from the power preset as early as the settings are
 * known (before the menu), and again before bootmgr if the menu changed the preset. HdmiProbeDxe owns ICC and does
 * GET/SET/verify (ps5-fan.h); it also logs the curset and duty it found (the EMC state before any Windows task). */
#include "ps5-hdmi-video-protocol.h"
#include "ps5-fan.h"
#include "ps5-fan-quiet.h"   /* r5.26.4 */
STATIC VOID Ps5ApplyFanTarget(CONST CHAR8 *When,EFI_HANDLE Esp) {
  STATIC UINT32 Applied=0;
  STATIC UINT8 Original[0x6ae];   /* PS5_FC_SERVO_SIZE */
  PS5_HDMI_VIDEO_PROTOCOL *Video=NULL;
  /* r5.26.2 (user 24.11.: the fan still spun up at every Windows start): r5.26.1 left the EMC at the GameOS table (target
   * 91 C), so the fan stayed at ~10 % through the firmware and Windows start, the MainSoC reached ~76 C and the PS5 Linux
   * curve in ps5pwr then had to ramp the fan up hard. PS5 Linux reaches its fan driver seconds after GameOS; we take
   * minutes. Now the EMC gets the quiet backup servo table of PS5 Linux fan.c as early as the settings are known and
   * regulates by itself (auto) until ps5pwr drives the curve - also in Windows Setup and WinPE, where no ps5pwr runs. The
   * GameOS table goes to ps5pwr in the volatile variable PswFanOriginal (restored at shutdown). The presets still set the
   * clock limits. */
  UINT32 Flags=0,D0=0,D1=0;INT32 T=-1;UINT32 Target=ps5_fan_target_for_preset(mPs5Set.preset);   /* r5.28.10: 72/70/66 C */
  if(Applied)return;
  if(EFI_ERROR(gBS->LocateProtocol(&mPs5HdmiVideoGuid,NULL,(VOID **)&Video)) || !Video || !Video->FanQuiet) {
    DEBUG((DEBUG_ERROR,"PS5 fan: %a: no HDMI/ICC protocol, EMC table unchanged\n",When));return; }
  Flags=Target<<24;   /* r5.28.10: the servo target goes to HdmiProbeDxe in bits 24-31 */
  EFI_STATUS S=Video->FanQuiet(Original,sizeof(Original),&Flags,&D0,&D1,&T);
  if(!EFI_ERROR(S) || (Flags & 2))Applied=1;
  /* r5.26.4 (HW 24.11.: the quiet table survived a hard power-off AND a GameOS start - GameOS does not put its own table
   * back, so after a crash or a hold-15 s power-off nobody knew the original): the GameOS table is kept in
   * \PSWloader\fan-gameos.bin on the ESP the first time it is seen; a later start that finds the quiet table takes it
   * from there, so ps5pwr can still restore it at shutdown. */
  if((Flags & 2) && Esp) {
    EFI_FILE_PROTOCOL *Root=Ps5PresetRoot(Esp),*F=NULL;UINTN N=sizeof(Original);
    if(Root && !EFI_ERROR(Root->Open(Root,&F,L"\\PSWloader\\fan-gameos.bin",EFI_FILE_MODE_READ,0))) {
      if(!EFI_ERROR(F->Read(F,&N,Original)) && N==sizeof(Original) && !ps5_fq_is_quiet(Original))Flags|=0x101;   /* 0x100: from the ESP */
      F->Close(F);
    }
    if(Root)Root->Close(Root);
  } else if((Flags & 1) && Esp) {
    EFI_FILE_PROTOCOL *Root=Ps5PresetRoot(Esp),*F=NULL,*D=NULL;STATIC UINT8 Have[0x6ae];UINTN N=sizeof(Have);BOOLEAN Same=FALSE;
    if(Root && !EFI_ERROR(Root->Open(Root,&F,L"\\PSWloader\\fan-gameos.bin",EFI_FILE_MODE_READ,0))) {
      Same=!EFI_ERROR(F->Read(F,&N,Have)) && N==sizeof(Have) && CompareMem(Have,Original,sizeof(Have))==0;F->Close(F);F=NULL; }
    if(Root && !Same) {
      if(!EFI_ERROR(Root->Open(Root,&D,L"\\PSWloader",EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,EFI_FILE_DIRECTORY)))D->Close(D);
      Ps5SetDelete(Root,L"\\PSWloader\\fan-gameos.bin");
      if(!EFI_ERROR(Root->Open(Root,&F,L"\\PSWloader\\fan-gameos.bin",EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,0))) {
        N=sizeof(Original);EFI_STATUS W=F->Write(F,&N,Original);F->Flush(F);F->Close(F);
        DEBUG((DEBUG_ERROR,"PS5 fan: GameOS table kept on the ESP: %r\n",W)); }
    }
    if(Root)Root->Close(Root);
  }
  if(Flags & 1) {
    STATIC EFI_GUID PswGuid={0x5efc5ae0,0xb753,0x4f7a,{0xa6,0x62,0xe3,0x4d,0x15,0xa6,0xd8,0xe5}};   /* ps5pwr g_hp_guid */
    EFI_STATUS V=gRT->SetVariable(L"PswFanOriginal",&PswGuid,EFI_VARIABLE_BOOTSERVICE_ACCESS|EFI_VARIABLE_RUNTIME_ACCESS,sizeof(Original),Original);
    DEBUG((DEBUG_ERROR,"PS5 fan: PswFanOriginal published (%a): %r\n",(Flags & 0x100)?"from the ESP":"snapshot",V));
  }
  DEBUG((DEBUG_ERROR,"PS5 fan: %a: quiet backup table, target %u C: %r (flags %x, MainSoC %d mC, duty %u.%u%% -> %u.%u%%)\n",When,Target,S,Flags,T,
    D0<=1024?D0*100/1024:0,D0<=1024?(D0*1000/1024)%10:0,D1<=1024?D1*100/1024:0,D1<=1024?(D1*1000/1024)%10:0));
}
STATIC VOID Ps5InstallPStateSsdt(VOID) {
  STATIC BOOLEAN Done=FALSE; STATIC UINT8 Aml[4096];
  if(Done || !mPs5Set.pstates)return;
  Done=TRUE;
  UINT32 Eax=0; AsmCpuid(1,&Eax,NULL,NULL,NULL);
  if(((Eax >> 8) & 0xF) + ((Eax >> 20) & 0xFF) != 0x17){DEBUG((DEBUG_ERROR,"PS5 pstates: not family 17h, no SSDT\n"));return;}
  unsigned long long Def[PS5_PSS_MAX]; struct ps5_pstate Ps[PS5_PSS_MAX];
  for(UINT32 I=0;I<PS5_PSS_MAX;I++)Def[I]=AsmReadMsr64(0xC0010064+I);
  unsigned N=ps5_pstate_decode(Def,AsmReadMsr64(0xC0010061),Ps);
  for(unsigned I=0;I<N;I++)DEBUG((DEBUG_ERROR,"PS5 pstates: P%u %u MHz %u mW\n",I,Ps[I].mhz,Ps[I].mw));
  unsigned Len=N>=2?ps5_pstate_ssdt(Ps,N,Aml,sizeof(Aml)):0;     /* one state only = nothing to control */
  if(Len==0){DEBUG((DEBUG_ERROR,"PS5 pstates: %u states, no SSDT\n",N));return;}
  EFI_ACPI_TABLE_PROTOCOL *At=NULL; UINTN Key=0;
  EFI_STATUS S=gBS->LocateProtocol(&gEfiAcpiTableProtocolGuid,NULL,(VOID **)&At);
  if(!EFI_ERROR(S))S=At->InstallAcpiTable(At,Aml,Len,&Key);
  DEBUG((DEBUG_ERROR,"PS5 pstates: SSDT %u bytes, %u states, 16 CPUs: %r\n",Len,N,S));
}
#endif
#if defined(PS5_NATIVE_SETUP) && defined(PS5_NATIVE_DP_VIDEO)
STATIC VOID Ps5DiagOnce(EFI_HANDLE Selected);   /* r5.20.1, defined with the system-check protocol below */
STATIC VOID EFIAPI Ps5DiagMark(CONST CHAR8 *Fmt,...);   /* r5.20.4: \PS5DSTEP.TXT progress marker; r5.20.5: EFIAPI (VA_LIST = ms_va_list) */
#endif
/* r5.4 (16.11.): post-menu Windows launch, shared by the normal BDS path and the setup-browser
 * boot-now protocol ("Boot Windows" / menu idle timeout). Returns only if bootx64.efi returned. */
/* r5.25.11 (HW 23.11. lab128: after a restart the console's xHCI never completed a port reset in Windows, keyboard and
 * mouse missing): the warm PCI replay sets the MSI-X function mask on every MSI-X function (no stray interrupts during
 * the firmware run), and Windows' pci.sys keeps that bit when it enables MSI-X - every vector stays masked. Before the OS
 * starts, a warm run puts such functions back to the fresh state (MSI-X off, mask clear); Windows enables them itself.
 * The firmware drivers poll and do not use MSI-X. Raw ECAM, like the replay; bounded capability walk. */
__attribute__((unused)) STATIC VOID Ps5WarmMsixUnmask(VOID)   /* unused only in the lab build --lab-bm-boot --lab-no-rtb-unmask */
{
  if(*(volatile UINT64 *)(UINTN)PS5_WARM_LOGCELL!=0x31474F4C4D524157ULL)return;   /* cold boot: the hand-over state as is */
  UINTN Fixed=0;
  for(UINT32 I=0;I<0x8000;I++) {
    volatile UINT8 *C=(volatile UINT8 *)(UINTN)(0xF0000000ULL+((UINT64)I<<12));
    if(*(volatile UINT32 *)C==0xFFFFFFFFU || !(*(volatile UINT16 *)(C+6)&BIT4))continue;   /* absent or no capability list */
    UINT8 Cap=C[0x34]&0xFC;
    for(UINTN K=0;K<48 && Cap>=0x40;K++) {
      if(C[Cap]==0x11) {
        UINT16 Ctl=*(volatile UINT16 *)(C+Cap+2);
        if(Ctl!=0xFFFF && (Ctl&BIT14)) { *(volatile UINT16 *)(C+Cap+2)=(UINT16)(Ctl&~(BIT15|BIT14)); Fixed++;
          DEBUG((DEBUG_ERROR,"PS5 warm MSI-X: %02x:%02x.%x %08x ctl %04x -> %04x\n",I>>8,(I>>3)&0x1F,I&7,*(volatile UINT32 *)C,Ctl,*(volatile UINT16 *)(C+Cap+2))); }
        break;
      }
      Cap=C[Cap+1]&0xFC;
    }
  }
  DEBUG((DEBUG_ERROR,"PS5 warm MSI-X: %u function(s) back to the fresh state\n",(UINT32)Fixed));
}
STATIC EFI_STATUS Ps5LaunchWindows(EFI_HANDLE Selected)
{
  EFI_STATUS Status;
  ps5_progress(PS5_PROG_LAUNCH);
#ifndef PS5_LAB_BM_BOOT   /* lab (--lab-bm-boot): behave like a start from the Boot Manager, which never had this call */
  Ps5WarmMsixUnmask();   /* r5.25.11 */
#else
  DEBUG((DEBUG_ERROR,"PS5 lab: Boot Manager path simulated - no MSI-X unmask in Ps5LaunchWindows\n"));
#endif
  { /* r5.25.11: memory type of the GOP frame buffer, cold vs. warm (after a restart the firmware's writes stay invisible
     * while the CPU reads them back - suspected write-back caching): variable MTRRs, read only */
    UINT64 Cap=AsmReadMsr64(0xFE),Def=AsmReadMsr64(0x2FF); UINTN Vc=(UINTN)(Cap&0xFF);
    DEBUG((DEBUG_ERROR,"PS5 mtrr: cap %Lx def %Lx\n",Cap,Def));
    for(UINTN V=0;V<Vc && V<16;V++){UINT64 B=AsmReadMsr64(0x200+2*V),M=AsmReadMsr64(0x201+2*V);
      if(M&BIT11)DEBUG((DEBUG_ERROR,"PS5 mtrr %u: base %Lx mask %Lx\n",(UINT32)V,B,M));}
  }
#if defined(PS5_WARM_WATCHDOG)
  Ps5WarmProgress(50);   /* lab: ends the warm watchdog before bootmgr */
#endif
  { /* r5.25.4: CPU/APIC state of the BSP right before the OS start (cold vs. warm re-entry comparison) */
    UINT64 Ab=AsmReadMsr64(0x1B); volatile UINT32 *A=(volatile UINT32 *)(UINTN)(Ab&0xFFFFF000ULL);
    DEBUG((DEBUG_ERROR,"PS5 cpu state: APICBASE=%Lx HWCR=%Lx EFER=%Lx PAT=%Lx MTRRdef=%Lx CR0=%Lx CR4=%Lx\n",Ab,AsmReadMsr64(0xC0010015),
      AsmReadMsr64(0xC0000080),AsmReadMsr64(0x277),AsmReadMsr64(0x2FF),(UINT64)AsmReadCr0(),(UINT64)AsmReadCr4()));
    if(!(Ab&BIT10)) {
      DEBUG((DEBUG_ERROR,"PS5 apic state: ID=%x TPR=%x SVR=%x LVT tmr=%x therm=%x perf=%x lint0=%x lint1=%x err=%x TMICT=%x TMCCT=%x TDCR=%x\n",
        A[0x20/4],A[0x80/4],A[0xF0/4],A[0x320/4],A[0x330/4],A[0x340/4],A[0x350/4],A[0x360/4],A[0x370/4],A[0x380/4],A[0x390/4],A[0x3E0/4]));
      DEBUG((DEBUG_ERROR,"PS5 apic ext: EFEAT=%x ECTRL=%x IER=%x %x %x %x %x %x %x %x ISR7=%x IRR7=%x\n",A[0x400/4],A[0x410/4],
        A[0x480/4],A[0x490/4],A[0x4A0/4],A[0x4B0/4],A[0x4C0/4],A[0x4D0/4],A[0x4E0/4],A[0x4F0/4],A[0x170/4],A[0x270/4]));
    }
  }
#if defined(PS5_DCN_DUMP) && defined(PS5_NATIVE_DP_VIDEO)
  { /* lab --dcn-dump (22.11., v2): display/MMHUB registers right before bootmgr, read only, cold vs. warm comparison.
     * v1 read whole DCN blocks and stalled the bus (HW lab117: hang at the console clear) - v2 reads only registers the
     * firmware itself programs (the active mode's pipe replay, the MMHUB windows) plus MMHUB VM context 0. */
    STATIC CONST UINT32 Mm[]={0x69a00,0x69a04,0x69a08,0x69a20,0x69a24,0x69a30,0x69a3c,0x69a40,0x69a60,0x69a84,0x69b00,0x69b04,
      0x69cac,0x69cb0,0x69d2c,0x69d30,0x69dac,0x69db0,0x6a15c,0x6a160,0x6a164,0x6a180,0x6a184,0x6a188,0x6a1b0,0x6a1b4,0x6a1b8,
      0x6a1bc,0x6a1c0,0x6a1c4,0x6a1c8,0x6a1cc,0xa600,0xa604,0xa608,0xa60c,0xa610,0xa614,0xa618};
    CONST struct ps5_dcn201_mode_replay *Rp=Ps5ModeReplay(mPs5ModeId);
    EFI_HANDLE *Hs=NULL;UINTN Hn=0;
    if(Rp!=NULL && !EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol,&gEfiPciIoProtocolGuid,NULL,&Hn,&Hs))) {
      for(UINTN I=0;I<Hn;I++) {
        EFI_PCI_IO_PROTOCOL *Pci=NULL;UINT32 Id=0;
        if(EFI_ERROR(gBS->HandleProtocol(Hs[I],&gEfiPciIoProtocolGuid,(VOID **)&Pci)) ||
           EFI_ERROR(Pci->Pci.Read(Pci,EfiPciIoWidthUint32,0,1,&Id)) || (Id&0xFFFF)!=0x1002 || (Id>>16)!=0x13FC)continue;
        DEBUG((DEBUG_ERROR,"PS5 dcn dump: start (%a) replay %u + mmhub %u\n",mPs5WarmDirectHit?"warm":"cold",Rp->count,(UINT32)(sizeof(Mm)/sizeof(Mm[0]))));
        UINTN N=Rp->count+sizeof(Mm)/sizeof(Mm[0]);
        for(UINTN K0=0;K0<N;K0+=8) {
          UINT32 O[8],V[8];
          for(UINTN K=0;K<8;K++){UINTN X=K0+K;O[K]=X<N?(X<Rp->count?Rp->rows[X].offset:Mm[X-Rp->count]):0;V[K]=0;
            if(O[K] && EFI_ERROR(Pci->Mem.Read(Pci,EfiPciIoWidthUint32,5,O[K],1,&V[K])))V[K]=0xDEADDEAD;}
          DEBUG((DEBUG_ERROR,"PS5 dcn %x=%x %x=%x %x=%x %x=%x %x=%x %x=%x %x=%x %x=%x\n",O[0],V[0],O[1],V[1],O[2],V[2],O[3],V[3],
                 O[4],V[4],O[5],V[5],O[6],V[6],O[7],V[7]));
        }
        DEBUG((DEBUG_ERROR,"PS5 dcn dump: end\n"));
        break;
      }
      FreePool(Hs);
    }
  }
#endif
#ifdef PS5_WARM_RESET_TEST
  { /* lab build (--warm-reset-test): the first boot restarts itself once through ResetSystem; the warm re-entry must bring
     * back the start screen and this boot then continues. Marker in the loader's reserved arena (kept by the re-entry). */
    volatile UINT64 *Mark=(volatile UINT64 *)(UINTN)0x111000300ULL;
    if(*Mark!=0x5453455452415757ULL){*Mark=0x5453455452415757ULL;AsmWbinvd();
      DEBUG((DEBUG_ERROR,"PS5 warm re-entry test: ResetSystem(cold) now\n"));
      gRT->ResetSystem(EfiResetCold,EFI_SUCCESS,0,NULL);}
    *Mark=0;DEBUG((DEBUG_ERROR,"PS5 warm re-entry test: second start reached the boot path - re-entry works\n"));
  }
#endif
#ifdef PS5_NATIVE_SETUP
#ifdef PS5_NATIVE_DP_VIDEO
  Ps5DiagOnce(Selected);                                         /* r5.20.1: diag=once -> system check, \PS5DIAG.LOG, diag=off */
#endif
  if(mPs5NewDiskId!=0) {   /* r5.25.4: a new bootable disk goes to place 1 of the saved list */
    unsigned N=0,L[PS5_BOOTLIST_MAX]; L[N++]=mPs5NewDiskId;
    for(unsigned I=0;I<mPs5Set.bootlist_n && N<PS5_BOOTLIST_MAX;I++) if(mPs5Set.bootlist[I]!=mPs5NewDiskId) L[N++]=mPs5Set.bootlist[I];
    CopyMem(mPs5Set.bootlist,L,N*sizeof(unsigned)); mPs5Set.bootlist_n=N; mPs5NewDiskId=0;
  }
  { EFI_PARTITION_INFO_PROTOCOL *Pi=NULL;   /* r5.25.4: last boot device for a direct start after a restart */
    if(!EFI_ERROR(gBS->HandleProtocol(Selected,&gEfiPartitionInfoProtocolGuid,(VOID **)&Pi)) && Pi!=NULL && Pi->Type==PARTITION_TYPE_GPT) {
      volatile UINT64 *L=(volatile UINT64 *)(UINTN)PS5_WARM_LASTBOOT;
      CopyMem((VOID *)&L[1],&Pi->Info.Gpt.UniquePartitionGUID,16); L[3]=Ps5HandleBehindUsb(Selected)?PS5_WARM_LASTUSB:0;   /* r5.25.12 */
      L[0]=0x315453414C575350ULL;
    } }
  Ps5PowerPresetSave(mPs5ConfigEsp?mPs5ConfigEsp:Selected);      /* r5.6: only if changed in the menu; r5.15.2: config ESP */
  Ps5FirmwareUpdateConfirm(mPs5ConfigEsp?mPs5ConfigEsp:Selected); /* r5.25.13 */
  Ps5InstallPStateSsdt();                                        /* r5.12: only with pstates=1 */
#ifdef PS5_LAB_SANDISK
  Ps5LabSanDiskDefault(Selected);
#endif
  Ps5BcdTestsign(Selected,"before bootmgr");                     /* r5.22: testsigning for our boot drivers */
  Ps5ApplyFanTarget("before bootmgr",mPs5ConfigEsp?mPs5ConfigEsp:Selected);                           /* r5.28.10: only if not up yet */
#ifdef PS5_NATIVE_DP_VIDEO
  Ps5FrameRateLog("before bootmgr");                             /* r5.14: OTG frame counter since the mode start */
#endif
#endif
  Status=SeedWindowsTime(Selected);
  if(EFI_ERROR(Status)) {
    // v1.57: non-fatal. The software RTC (Ps5RtcRuntimeDxe) is default-initialised to a
    // build-time UTC, so Windows still boots with a reasonable clock (NTP-corrected later)
    // even without a valid external PS5UTC.SEED (a USB installer stick has none).
    DEBUG((DEBUG_ERROR,"PS5 UTC seed rejected: %r; continuing with built-in default clock\n",Status));
  }
  Status=WindowsTimeContract();
  if(EFI_ERROR(Status)) {
    DEBUG((DEBUG_ERROR,"PS5 Windows time contract rejected: %r\n",Status));
    return Status;
  }
  EFI_BOOT_MANAGER_LOAD_OPTION Option;
  CHAR16 *BootFile=Ps5VolumeBootFile(Selected);   /* r5.25.4: fallback or Microsoft path */
  DEBUG((DEBUG_ERROR,"PS5 Windows boot file: %s\n",BootFile?BootFile:mWindowsPath));
  EFI_DEVICE_PATH_PROTOCOL *Path=FileDevicePath(Selected,BootFile?BootFile:mWindowsPath);
  if (Path==NULL) return EFI_OUT_OF_RESOURCES;
  Status=EfiBootManagerInitializeLoadOption(&Option,LoadOptionNumberUnassigned,
      LoadOptionTypeBoot,LOAD_OPTION_ACTIVE,L"Windows Boot Manager (PS5)",Path,NULL,0);
  FreePool(Path);
  if (EFI_ERROR(Status)) return Status;
  DEBUG((DEBUG_INFO,"PS5 Windows boot: launching selected bootx64.efi\n"));
#ifdef PS5_FWENUM_DIAG
  Ps5LogFirmwareDeviceTree(Selected);
#endif
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *Console=InstallWindowsConsoleTap();
#if defined(PS5_WARM_EBS_STEPS) && defined(PS5_WARM_EBS_MARK) && defined(PS5_NATIVE_SETUP)
  { EFI_EVENT E1; gBS->CreateEvent(EVT_SIGNAL_EXIT_BOOT_SERVICES,TPL_NOTIFY,Ps5WarmSqAfterWim,NULL,&E1); }
#endif
  BOOLEAN PostWimInstalled=InstallPostWimTap();
#if defined(PS5_NATIVE_SETUP) && defined(PS5_NATIVE_DP_VIDEO)
  Ps5DiagMark("L bootmgr launch");                 /* r5.20.4: last marker before bootx64.efi */
#endif
  PS5_TS("bootmgr-launch");
#if defined(PS5_WARM_EBS_STEPS) && defined(PS5_WARM_EBS_MARK) && defined(PS5_NATIVE_SETUP)
  { EFI_EVENT E2; gBS->CreateEvent(EVT_SIGNAL_EXIT_BOOT_SERVICES,TPL_NOTIFY,Ps5WarmSqFirst,NULL,&E2); }
  { EFI_EVENT E3; gBS->CreateEvent(EVT_SIGNAL_EXIT_BOOT_SERVICES,TPL_CALLBACK,Ps5WarmSqFirstCb,NULL,&E3); }
#endif
#ifdef PS5_WARM_BS_TRACE
  if (mPs5BsTraceWarm) Ps5BsTraceStart();   /* lab: from here every boot service is counted, an AP paints it */
#endif
  EfiBootManagerBoot(&Option);
  RestorePostWimTap(PostWimInstalled);
  RestoreWindowsConsoleTap(Console);
  Status=Option.Status;
  DEBUG((DEBUG_ERROR,"PS5 Windows boot: bootx64.efi returned %r\n",Status));
  EfiBootManagerFreeLoadOption(&Option);
  return Status;
}
#ifdef PS5_NATIVE_SETUP
/* mPs5BootSelected declared alongside mPs5ConfigEsp above */
/* r5.25.4 (user: "Save and boot" with the stick on place 1 started the SanDisk again): the boot from the menu used the
 * disk chosen BEFORE the menu. It now takes the first drive of the order arranged in the Boot tab (Ps5BootListSel ->
 * Ps5BootDevs ids) that has a bootable volume; only if none is found, the earlier choice. */
STATIC EFI_HANDLE Ps5DiskOf(EFI_HANDLE Volume);
STATIC UINT32 Ps5DiskId(EFI_HANDLE Disk);
STATIC EFI_HANDLE Ps5MenuOrderFirst(VOID)
{
  PS5_BOOT_LIST Sel;UINTN Ss=sizeof(Sel);EFI_HANDLE *H=NULL;UINTN N=0;EFI_HANDLE Found=NULL;
  if(!mPs5Devs.Count || EFI_ERROR(gRT->GetVariable(L"Ps5BootListSel",&mPs5PresetGuid,NULL,&Ss,&Sel)) || Ss!=sizeof(Sel))return NULL;
  if(EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol,&gEfiSimpleFileSystemProtocolGuid,NULL,&N,&H)))return NULL;
  for(UINTN I=0;I<PS5_BOOTDEV_MAX && Sel.Order[I] && Found==NULL;I++) {
    UINTN Ix=(UINTN)Sel.Order[I]-1;
    if(Ix>=mPs5Devs.Count)continue;
    for(UINTN V=0;V<N;V++) if(Ps5VolumeHasBootx64(H[V]) && Ps5DiskId(Ps5DiskOf(H[V]))==mPs5Devs.Id[Ix]) {
      Found=H[V];
      DEBUG((DEBUG_ERROR,"PS5 boot menu: boot order place %u = %s\n",(UINT32)I+1,mPs5Devs.Name[Ix]));
      break;
    }
  }
  FreePool(H);
  return Found;
}
STATIC EFI_STATUS EFIAPI Ps5BootNow(VOID)
{
  { EFI_HANDLE First=Ps5MenuOrderFirst(); if(First!=NULL)mPs5BootSelected=First; }   /* r5.25.4 */
  if (mPs5BootSelected==NULL) return EFI_NOT_READY;
  EFI_TPL Tpl=gBS->RaiseTPL(TPL_HIGH_LEVEL); gBS->RestoreTPL(Tpl);
  if (Tpl!=TPL_APPLICATION) { DEBUG((DEBUG_ERROR,"PS5 boot menu: boot-now refused at TPL %u\n",(UINT32)Tpl)); return EFI_ACCESS_DENIED; }
  Ps5MenuIdleDisarm();
  DEBUG((DEBUG_ERROR,"PS5 boot menu: booting Windows from the setup browser\n"));
  gST->ConOut->SetAttribute(gST->ConOut,EFI_TEXT_ATTR(EFI_LIGHTGRAY,EFI_BLACK));
  gST->ConOut->ClearScreen(gST->ConOut);
  EFI_STATUS Status=Ps5LaunchWindows(mPs5BootSelected);
  DEBUG((DEBUG_ERROR,"PS5 boot menu: Windows launch returned %r - back to the menu\n",Status));
  return Status;
}
STATIC EFI_STATUS EFIAPI Ps5SaveSettingsNow(VOID)   /* r5.21: restart / power off / immediate save from the menu keeps the settings */
{
  return Ps5PowerPresetSave(mPs5ConfigEsp?mPs5ConfigEsp:mPs5BootSelected);
}
STATIC PS5_BOOT_NOW_PROTOCOL mPs5BootNowProtocol={2,Ps5BootNow,Ps5SaveSettingsNow};
STATIC EFI_GUID mPs5BootNowGuid=PS5_BOOT_NOW_PROTOCOL_GUID;
#endif
/* r5.15 (16.11., user goal: Windows 11 from a stock ISO stick onto a USB NVMe): boot order and USB classes.
 * A USB volume with \EFI\Microsoft\Boot\BCD and without \sources\boot.wim is an installed Windows (the target disk
 * after Setup's first copy); one with \sources\boot.wim is an installer stick. Installed beats installer, so Setup's
 * reboots continue from the target disk while the stick stays plugged in. bootorder=usb (\PS5UEFI.CFG of the internal
 * ESP, or Advanced -> Boot order) puts USB before the internal disk; without an internal ESP USB is used anyway. */
STATIC BOOLEAN Ps5VolumeHasFile (EFI_HANDLE Handle, CHAR16 *Path)
{
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs; EFI_FILE_PROTOCOL *Root=NULL,*File=NULL;
  if (EFI_ERROR(gBS->HandleProtocol(Handle,&gEfiSimpleFileSystemProtocolGuid,(VOID **)&Fs))) return FALSE;
  if (EFI_ERROR(Fs->OpenVolume(Fs,&Root)) || Root==NULL) return FALSE;
  EFI_STATUS Status=Root->Open(Root,&File,Path,EFI_FILE_MODE_READ,0);
  if (!EFI_ERROR(Status) && File!=NULL) File->Close(File);
  Root->Close(Root);
  return !EFI_ERROR(Status);
}
/* r5.16: one early read of \PS5UEFI.CFG (bootorder for the selection, refresh before the display starts) */
STATIC VOID Ps5PeekSettings (EFI_HANDLE Esp, struct ps5_settings *Out)
{
  ps5_settings_default(Out);
#ifdef PS5_NATIVE_SETUP
  STATIC CHAR16 *Names[3]={L"\\PS5UEFI.CFG",L"\\PS5UEFI.NEW",L"\\PS5UEFI.BAK"};
  CHAR8 Buf[768]; EFI_FILE_PROTOCOL *Root=Esp?Ps5PresetRoot(Esp):NULL;
  if (Root==NULL) return;
  for (UINTN I=0;I<3;I++) {
    UINTN N=Ps5SetReadFile(Root,Names[I],Buf,sizeof(Buf));
    if (N && ps5_settings_parse(Buf,(unsigned)N,Ps5SetCrc,Out)) break;
  }
  Root->Close(Root);
#else
  (VOID)Esp;
#endif
}
#if defined(PS5_NATIVE_SETUP) && defined(PS5_NATIVE_DP_VIDEO)
/* r5.20: BIOS "System check" (read-only, ps5-diag.h / ps5-diag-uefi.h) offered to Ps5InfoDxe through PS5_DIAG_PROTOCOL */
#include "ps5-diag-uefi.h"
#include "Ps5Diag.h"
STATIC EFI_HANDLE mPs5DiagFallbackEsp;   /* the selected boot ESP, for the log when there is no config ESP */
STATIC EFI_STATUS EFIAPI Ps5DiagProtocolRun(PS5_DIAG_RESULT *Out)
{
  STATIC struct ps5_diag_report R;
  unsigned Lang=mPs5Set.lang==1;
  if(Out==NULL)return EFI_INVALID_PARAMETER;
  ZeroMem(Out,sizeof(*Out));
  Ps5DiagRun(&R);
  CHAR8 Where[80];Where[0]='\0';
  Out->LogStatus=(INT32)Ps5DiagSaveLogEx(&R,Lang,mPs5DiagFallbackEsp,Where,sizeof(Where));
  Out->Pass=R.pass;Out->Warn=R.warn;Out->Fail=R.fail;Out->Skip=R.skip;
  for(UINTN I=0;I<PS5_DIAG_COUNT && I<PS5_DIAG_LINES;I++)
    UnicodeSPrint(Out->Line[I],sizeof(Out->Line[I]),L"%a  %a: %a",ps5_diag_status_word(R.item[I].status),ps5_diag_title((unsigned)I,Lang),R.item[I].detail);
  Out->Lines=PS5_DIAG_COUNT<PS5_DIAG_LINES?PS5_DIAG_COUNT:PS5_DIAG_LINES;
  if(Out->LogStatus==0 && Where[0]!='\0')
    UnicodeSPrint(Out->Summary,sizeof(Out->Summary),Lang?L"%u PASS  %u WARN  %u FAIL  %u SKIP - gespeichert: %a":L"%u PASS  %u WARN  %u FAIL  %u SKIP - saved: %a",R.pass,R.warn,R.fail,R.skip,Where);
  else
    UnicodeSPrint(Out->Summary,sizeof(Out->Summary),Lang?L"%u PASS  %u WARN  %u FAIL  %u SKIP - nicht gespeichert (bitte FAT32-USB-Stick anstecken)":L"%u PASS  %u WARN  %u FAIL  %u SKIP - please plug in a FAT32 USB drive to save the log",R.pass,R.warn,R.fail,R.skip);
  return EFI_SUCCESS;
}
/* r5.20.1 (17.11., HW driver: non-interactive trigger): diag=once in \PS5UEFI.CFG runs the read-only system check
 * once right before bootmgr and writes \PS5DIAG.LOG.
 * r5.20.3 (17.11.): r5.20.1 hung on the HW in this path with diag=once still in the file (every boot would hang again).
 * Now the key is consumed FIRST (diag=off written and read back through the usual NEW -> BAK -> CFG sequence), and
 * the check only runs if that write succeeded: a hang can happen at most once. The check runs without the ICC step,
 * and before every step a progress line goes to the screen and the UEFI log is exported (\PS5UEFI.LOG snapshot), so
 * the last line there names the step that hung. */
/* r5.20.4 (17.11., r5.20.3 with diag=once hung as well; no picture, no PS5 Linux, \PS5UEFI.LOG overwritten by the
 * recovery boot): isolation build. \PS5DSTEP.TXT on the config ESP gets one line per step, appended, flushed and closed
 * every time (LogExportDxe and r5.19 do not touch it), so Windows can read how far the firmware came.
 * PS5_DIAG_ONCE_VARIANT 1 (A): diag=once only consumes the key (diag=off written + read back), no check.
 * PS5_DIAG_ONCE_VARIANT 2 (B): diag=once runs the check steps WITHOUT any CFG write (the key stays once; reset it in
 *                               Windows), then writes \PS5DIAG.LOG.
 * PS5_DIAG_ONCE_VARIANT 0     : r5.20.3 behaviour (consume first, then the check). */
#ifndef PS5_DIAG_ONCE_VARIANT
#define PS5_DIAG_ONCE_VARIANT 0
#endif
STATIC BOOLEAN mPs5DiagMarkOn;
STATIC EFI_HANDLE mPs5DiagMarkEsp;
STATIC VOID EFIAPI Ps5DiagMark(CONST CHAR8 *Fmt,...)
{
  CHAR8 Line[120];VA_LIST A;UINTN Len,Size=0;EFI_FILE_PROTOCOL *Root,*File=NULL;EFI_FILE_INFO *Info=NULL;
  if(!mPs5DiagMarkOn || !mPs5DiagMarkEsp)return;
  VA_START(A,Fmt);Len=AsciiVSPrint(Line,sizeof(Line)-3,Fmt,A);VA_END(A);
  Line[Len++]='\r';Line[Len++]='\n';Line[Len]=0;
  DEBUG((DEBUG_ERROR,"PS5 diag mark: %a",Line));
  Root=Ps5PresetRoot(mPs5DiagMarkEsp);
  if(!Root)return;
  if(!EFI_ERROR(Root->Open(Root,&File,L"\\PS5DSTEP.TXT",EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,0))){
    if(File->GetInfo(File,&gEfiFileInfoGuid,&Size,NULL)==EFI_BUFFER_TOO_SMALL && (Info=AllocateZeroPool(Size))!=NULL){
      if(!EFI_ERROR(File->GetInfo(File,&gEfiFileInfoGuid,&Size,Info)))File->SetPosition(File,Info->FileSize);
      FreePool(Info);
    }
    File->Write(File,&Len,Line);File->Flush(File);File->Close(File);
  }
  Root->Close(Root);
}
STATIC VOID Ps5DiagOnceStep(UINT32 Index,UINT32 Count,CONST CHAR8 *Name)
{
  CHAR16 Line[80];BOOLEAN Done=(Index&0x80000000u)!=0;Index&=0x7FFFFFFFu;
  DEBUG((DEBUG_ERROR,"PS5 diag once: step %u/%u %a %a\n",Index,Count,Name,Done?"done":"begin"));
  Ps5DiagMark("step %u/%u %a %a",Index,Count,Name,Done?"done":"begin");
  if(Done)return;
  UnicodeSPrint(Line,sizeof(Line),L"System check %u/%u: %a\r\n",Index,Count,Name);
  if(gST->ConOut)gST->ConOut->OutputString(gST->ConOut,Line);
  ConsoleSnapshot();
}
STATIC VOID Ps5DiagOnceSub(CONST CHAR8 *What) { Ps5DiagMark("  sub %a",What); }   /* r5.20.4: before single risky reads */
STATIC VOID Ps5DiagAutoStep(UINT32 Index,UINT32 Count,CONST CHAR8 *Name)
{
  DEBUG((DEBUG_ERROR,"PS5 diag: step %u/%u %a %a\n",Index&0x7FFFFFFFu,Count,Name,(Index&0x80000000u)?"done":"begin"));
}
STATIC VOID Ps5DiagOnce(EFI_HANDLE Selected)
{
  STATIC struct ps5_diag_report R;
  BOOLEAN Full=mPs5Set.diag==3;                     /* r5.20.6: diag=full = with the ICC fan/temperature step */
  if(mPs5Set.diag!=1 && !Full) {
    /* r5.25.12: \PS5DIAG.LOG is written at every start, so a support request always has a current one: the quick check
     * (no ICC step, nothing on the screen), log on the config ESP (or the boot ESP), never on other USB drives */
    PS5_TS("diag-begin");
    Ps5DiagRunEx(&R,TRUE,Ps5DiagAutoStep);
    mPs5DiagEspOnly=TRUE;
    EFI_STATUS S=Ps5DiagSaveLog(&R,mPs5Set.lang==1,Selected);
    mPs5DiagEspOnly=FALSE;
    DEBUG((DEBUG_ERROR,"PS5 diag: start check %u PASS %u WARN %u FAIL %u SKIP, \\PS5DIAG.LOG %r\n",R.pass,R.warn,R.fail,R.skip,S));
    PS5_TS("diag-end");
    return;
  }
  mPs5DiagMarkEsp=mPs5ConfigEsp?mPs5ConfigEsp:Selected;
  mPs5DiagMarkOn=TRUE;
  Ps5DiagMark("0 diag=%a seen, variant %u, firmware %a",Full?"full":"once",(UINT32)PS5_DIAG_ONCE_VARIANT,PS5_FW_RELEASE);
#if PS5_DIAG_ONCE_VARIANT != 2
  mPs5Set.diag=2;
  Ps5DiagMark("C1 consume: writing diag=off");
  Ps5PowerPresetSave(mPs5DiagMarkEsp);
  Ps5DiagMark("C2 consume: file has diag=%u (2 = off)",mPs5SetFile.diag);
  if(mPs5SetFile.diag!=2){
    DEBUG((DEBUG_ERROR,"PS5 diag once: diag=off not written, check skipped (would repeat on every boot)\n"));
    ConsoleSnapshot();return;
  }
#endif
#if PS5_DIAG_ONCE_VARIANT == 1
  Ps5DiagMark("A done (no check in variant A)");
  (VOID)R;(VOID)Ps5DiagOnceStep;(VOID)Ps5DiagOnceSub;(VOID)Full;   /* unused in variant A */
  ConsoleSnapshot();return;
#else
  Ps5DiagOnceStep(0,7,"start");
  mPs5DiagSubHook=Ps5DiagOnceSub;
  Ps5DiagRunEx(&R,!Full,Ps5DiagOnceStep);
  mPs5DiagSubHook=NULL;
  Ps5DiagMark("S8 writing \\PS5DIAG.LOG");
  EFI_STATUS S=Ps5DiagSaveLog(&R,mPs5Set.lang==1,Selected);
  Ps5DiagMark("S9 log %r, %u PASS %u WARN %u FAIL %u SKIP",S,R.pass,R.warn,R.fail,R.skip);
  DEBUG((DEBUG_ERROR,"PS5 diag once: %u PASS %u WARN %u FAIL %u SKIP, \\PS5DIAG.LOG %r\n",R.pass,R.warn,R.fail,R.skip,S));
  ConsoleSnapshot();
#endif
}
STATIC PS5_DIAG_PROTOCOL mPs5DiagProtocol={1,Ps5DiagProtocolRun};
STATIC EFI_GUID mPs5DiagGuid=PS5_DIAG_PROTOCOL_GUID;
#endif

/* r5.21 (17.11., user at the console: the boot menu showed only the SanDisk): the USB mass-storage stop marker
 * (--usb-mass-quiet, installed once the internal ESP is selected) kept every USB stick/SSD unbound, so
 * EfiBootManagerRefreshAllBootOption never saw them. The marker stays for the normal fast boot; entering the boot menu
 * (DEL) or the setup (ESC) removes it, connects everything and refreshes the boot options first. Boot options get
 * readable names: "USB: <vendor product> <size> GB" / "M.2: <model> <size> GB" (DiskInfo INQUIRY / NVMe Identify,
 * BlockIo size), instead of the generic EDK2 description. */
#include <Protocol/DiskInfo.h>
STATIC EFI_HANDLE mPs5UsbMassStopHandle;
STATIC EFI_GUID mPs5UsbMassStopGuid={0x9c1e5f2a,0x1360,0x4e1b,{0xb0,0x5e,0x13,0x60,0x0e,0xf1,0x00,0x05}};
#define PS5_BOOTLIST_WAIT_ATTEMPTS 100U   /* r5.25.4: 10 s for a better-placed boot-list disk (USB appears late) */
STATIC VOID Ps5PublishBootDevs(VOID);
/* r5.25.4 (user: the boot-once list should show real storage only, no mouse or other USB device): after every refresh,
 * visible boot options whose device is no storage with a medium (no file system and no block device with a size, e.g.
 * network boot, an empty card reader, a USB device without media) are deleted. Hidden options and firmware-volume apps
 * (setup, boot menu) are never touched; EDK2 creates the options again at the next refresh if a medium appears. */
STATIC BOOLEAN Ps5OptionIsStorage(EFI_DEVICE_PATH_PROTOCOL *Fp)
{
  EFI_DEVICE_PATH_PROTOCOL *Dp=Fp;EFI_HANDLE H=NULL;EFI_BLOCK_IO_PROTOCOL *B=NULL;
  if(!EFI_ERROR(gBS->LocateDevicePath(&gEfiSimpleFileSystemProtocolGuid,&Dp,&H)))return TRUE;
  Dp=Fp;
  if(EFI_ERROR(gBS->LocateDevicePath(&gEfiBlockIoProtocolGuid,&Dp,&H)))return FALSE;
  if(EFI_ERROR(gBS->HandleProtocol(H,&gEfiBlockIoProtocolGuid,(VOID **)&B)) || B==NULL || B->Media==NULL)return FALSE;
  return B->Media->MediaPresent && B->Media->LastBlock>0;
}
STATIC VOID Ps5PruneBootOptions(VOID)
{
  UINTN N=0;EFI_BOOT_MANAGER_LOAD_OPTION *O=EfiBootManagerGetLoadOptions(&N,LoadOptionTypeBoot);
  for(UINTN I=0;I<N;I++) {
    EFI_DEVICE_PATH_PROTOCOL *Fp=O[I].FilePath;BOOLEAN Fv=FALSE;
    if((O[I].Attributes&LOAD_OPTION_HIDDEN)!=0 || Fp==NULL)continue;
    for(EFI_DEVICE_PATH_PROTOCOL *D=Fp;!IsDevicePathEnd(D);D=NextDevicePathNode(D))
      if(DevicePathType(D)==MEDIA_DEVICE_PATH && (DevicePathSubType(D)==MEDIA_PIWG_FW_FILE_DP || DevicePathSubType(D)==MEDIA_PIWG_FW_VOL_DP))Fv=TRUE;
    if(DevicePathType(Fp)==MEDIA_DEVICE_PATH)Fv=TRUE;   /* short-form path (HD(...)\\EFI\\...): written by an OS, kept */
    if(Fv || Ps5OptionIsStorage(Fp))continue;
    EFI_STATUS S=EfiBootManagerDeleteLoadOptionVariable(O[I].OptionNumber,LoadOptionTypeBoot);
    DEBUG((DEBUG_ERROR,"PS5 boot menu: option Boot%04x '%s' is no storage with a medium - removed (%r)\n",(UINT32)O[I].OptionNumber,O[I].Description,S));
  }
  if(O!=NULL)EfiBootManagerFreeLoadOptions(O,N);
}
/* r5.28.12 (user 26.11.: "USB: Generic- SD/MMC 0 GB" in the boot list and in Windows' firmware list): the refresh of the boot
 * path created options for empty card-reader slots and nothing pruned them there (the full prune above would also drop USB
 * disks the boot path did not look for). This one only removes visible options whose device IS there - the whole-disk
 * BlockIo is found for the complete path - and has no medium (no media or 0 blocks); also called after the menu refresh.
 * r5.28.13: only for card readers (the model test Ps5UsbRecover uses). A USB SSD whose bridge hangs after a restart also
 * reports 0 bytes for a while - its "USB: ..." option must stay, or a "Boot once" by name (BootNext hash) would be lost. */
STATIC VOID Ps5DiskModel(EFI_HANDLE Handle,CHAR8 *Out,UINTN Cap);
STATIC BOOLEAN Ps5IsCardReader(EFI_HANDLE Handle)
{
  CHAR8 Mdl[64];
  Mdl[0]=0; Ps5DiskModel(Handle,Mdl,sizeof(Mdl));
  return AsciiStrStr(Mdl,"SD")||AsciiStrStr(Mdl,"MMC")||AsciiStrStr(Mdl,"Card")||AsciiStrStr(Mdl,"Reader")||AsciiStrStr(Mdl,"CF");
}
STATIC VOID Ps5PruneEmptyMedia(VOID)
{
  UINTN N=0;EFI_BOOT_MANAGER_LOAD_OPTION *O=EfiBootManagerGetLoadOptions(&N,LoadOptionTypeBoot);
  for(UINTN I=0;I<N;I++) {
    EFI_DEVICE_PATH_PROTOCOL *Dp=O[I].FilePath;EFI_HANDLE H=NULL;EFI_BLOCK_IO_PROTOCOL *B=NULL;
    if((O[I].Attributes&LOAD_OPTION_HIDDEN)!=0 || Dp==NULL || DevicePathType(Dp)==MEDIA_DEVICE_PATH)continue;
    if(EFI_ERROR(gBS->LocateDevicePath(&gEfiBlockIoProtocolGuid,&Dp,&H)) || !IsDevicePathEnd(Dp))continue;   /* device not there: kept */
    if(EFI_ERROR(gBS->HandleProtocol(H,&gEfiBlockIoProtocolGuid,(VOID **)&B)) || B==NULL || B->Media==NULL)continue;
    if(B->Media->MediaPresent && B->Media->LastBlock>0)continue;
    if(!Ps5IsCardReader(H)) {   /* r5.28.13: a disk without a size that is no card reader (stuck USB bridge) keeps its option */
      DEBUG((DEBUG_ERROR,"PS5 boot options: Boot%04x '%s' reports no medium but is no card reader - kept\n",(UINT32)O[I].OptionNumber,O[I].Description));
      continue;
    }
    EFI_STATUS S=EfiBootManagerDeleteLoadOptionVariable(O[I].OptionNumber,LoadOptionTypeBoot);
    DEBUG((DEBUG_ERROR,"PS5 boot options: Boot%04x '%s' has no medium (empty card reader) - removed (%r)\n",(UINT32)O[I].OptionNumber,O[I].Description,S));
  }
  if(O!=NULL)EfiBootManagerFreeLoadOptions(O,N);
}
STATIC BOOLEAN Ps5DescNoMedium(CONST CHAR16 *D)   /* r5.28.12: "USB: <model> 0 GB" = written while the slot was empty; r5.28.13: card readers only */
{
  UINTN L=(D!=NULL)?StrLen(D):0;
  if(L<5 || StrCmp(D+L-5,L" 0 GB")!=0)return FALSE;
  return StrStr(D,L"SD")!=NULL||StrStr(D,L"MMC")!=NULL||StrStr(D,L"Card")!=NULL||StrStr(D,L"Reader")!=NULL||StrStr(D,L"CF")!=NULL;
}
/* r5.28.5 (HW 26.11.: in the Windows on the internal M.2 the firmware entry "USB: Realtek ..." was missing, so "Boot once" into
 * the Windows on the USB SSD was impossible): once the internal ESP is selected, the USB-MSD stop marker keeps late USB disks
 * unbound, and EfiBootManagerRefreshAllBootOption deletes every auto-created option whose device it does not see. The boot
 * options are copied before the first refresh of this start; after the boot path's refresh, "USB: ..." entries that vanished
 * while the marker was set (the disks were never looked for) are written back with their old number. A refresh with USB
 * enumerated (boot menu, setup) still removes entries of disks that are really gone. */
STATIC EFI_BOOT_MANAGER_LOAD_OPTION *mPs5BootSnap;
STATIC UINTN mPs5BootSnapN;
VOID Ps5SnapshotBootOptions(VOID)
{
  if (mPs5BootSnap==NULL) mPs5BootSnap=EfiBootManagerGetLoadOptions(&mPs5BootSnapN,LoadOptionTypeBoot);
}
STATIC VOID Ps5RestoreUsbBootOptions(VOID)
{
  UINTN N=0; EFI_BOOT_MANAGER_LOAD_OPTION *O;
  if (mPs5BootSnap==NULL || mPs5UsbMassStopHandle==NULL) return;
  O=EfiBootManagerGetLoadOptions(&N,LoadOptionTypeBoot);
  for (UINTN I=0; I<mPs5BootSnapN; I++) {
    EFI_BOOT_MANAGER_LOAD_OPTION *E=&mPs5BootSnap[I]; BOOLEAN Have=FALSE;
    if (E->Description==NULL || StrnCmp(E->Description,L"USB: ",5)!=0 || (E->Attributes&LOAD_OPTION_HIDDEN)!=0 || Ps5DescNoMedium(E->Description)) continue;
    for (UINTN J=0; J<N; J++) if (O[J].OptionNumber==E->OptionNumber) Have=TRUE;
    if (Have) continue;
    EFI_STATUS S=EfiBootManagerAddLoadOptionVariable(E,(UINTN)-1);
    DEBUG((DEBUG_ERROR,"PS5 boot options: '%s' (Boot%04x) kept - USB disks were not looked for in this start (%r)\n",E->Description,(UINT32)E->OptionNumber,S));
  }
  if (O!=NULL) EfiBootManagerFreeLoadOptions(O,N);
}
STATIC VOID Ps5UsbMassAllow(CONST CHAR8 *Why)
{
  if(mPs5UsbMassStopHandle!=NULL) {
    EFI_STATUS S=gBS->UninstallProtocolInterface(mPs5UsbMassStopHandle,&mPs5UsbMassStopGuid,NULL);
    DEBUG((DEBUG_ERROR,"PS5 USB-MSD: %a - USB mass-storage binding allowed again (%r)\n",Why,S));
    if(!EFI_ERROR(S))mPs5UsbMassStopHandle=NULL;
  }
  EfiBootManagerConnectAll();
#ifdef PS5_NATIVE_SETUP
  /* r5.25.4 (user: the installed USB NVMe was still missing; the system check counted it as a USB disk): the boot path cycles
   * a stuck bridge (0 bytes after the cold start) only after it has chosen a medium - entering the menu first skipped that.
   * Now the menu entry cycles the ports of 0-byte USB disks itself (card readers excluded, nothing else touched). */
  if (mPs5CycleTsc==0) {
    mPs5CycleOnlyZero=TRUE; mRecoverSilent=TRUE;
    Ps5UsbRecover(TRUE,NULL);
    mRecoverSilent=FALSE; mPs5CycleOnlyZero=FALSE;
    if (mPs5CycleTsc!=0) { DEBUG((DEBUG_ERROR,"PS5 menu: power-cycled a stuck USB bridge before listing the drives\n")); EfiBootManagerConnectAll(); }
  }
  /* r5.25.3 (user: the USB NVMe must be in the list at once, a missing drive is confusing): a USB bridge that hung after the
   * cold start was power-cycled moments ago and needs 40-50 s - wait for it before the menu lists the drives (at most 60 s
   * after the cycle). Without such a cycle (normal restart) the menu opens at once. */
  if (mPs5CycleTsc!=0) {
    UINT64 Hz=1596300000ULL; UINTN Shown=(UINTN)-1;
    while (Ps5UsbWorkingDisks()<mPs5CycleWant && AsmReadTsc()-mPs5CycleTsc<60ULL*Hz) {
      UINTN Left=(UINTN)(60-(AsmReadTsc()-mPs5CycleTsc)/Hz);
      if (Left!=Shown) {
        Shown=Left;
        if (mPs5Set.lang==1) Print(L"\rUSB-Laufwerk wird gestartet ... noch h\x00f6chstens %2u s   ",Left);
        else Print(L"\rStarting USB drive ... at most %2u s left   ",Left);
        if (Left%5==0) EfiBootManagerConnectAll();
      }
      gBS->Stall(200000);
    }
    DEBUG((DEBUG_ERROR,"PS5 menu: USB drives after the power cycle: %u working, %u expected\n",(UINT32)Ps5UsbWorkingDisks(),(UINT32)mPs5CycleWant));
    mPs5CycleTsc=0; Print(L"\r                                                        \r");
    EfiBootManagerConnectAll();
  }
#endif
  EfiBootManagerRefreshAllBootOption();
  Ps5PruneBootOptions();                             /* r5.25.4 */
  Ps5PruneEmptyMedia();                              /* r5.28.12: also options of a device that is there without a medium */
  Ps5PublishBootDevs();                              /* r5.21: list for the Boot tab (defined below) */
}
STATIC EFI_GUID mPs5DiskInfoGuid=EFI_DISK_INFO_PROTOCOL_GUID;
STATIC EFI_GUID mPs5DiUsbGuid=EFI_DISK_INFO_USB_INTERFACE_GUID;
STATIC EFI_GUID mPs5DiScsiGuid=EFI_DISK_INFO_SCSI_INTERFACE_GUID;
STATIC EFI_GUID mPs5DiNvmeGuid=EFI_DISK_INFO_NVME_INTERFACE_GUID;
#include <Protocol/NvmExpressPassthru.h>
STATIC EFI_GUID mPs5NvmePtGuid={0x52c78312,0x8edc,0x4233,{0x98,0xf2,0x1a,0x1a,0xa5,0xe3,0x88,0xa5}};   /* r5.25.3: EFI_NVM_EXPRESS_PASS_THRU_PROTOCOL */
STATIC VOID Ps5CopyText(CHAR8 *Out,UINTN Cap,UINTN *At,CONST UINT8 *In,UINTN Len)   /* printable ASCII, runs of spaces folded */
{
  for(UINTN I=0;I<Len && *At+1<Cap;I++) {
    CHAR8 C=(CHAR8)In[I];
    if(C<0x20 || C>0x7E)C=' ';
    if(C==' ' && (*At==0 || Out[*At-1]==' '))continue;
    Out[(*At)++]=C;
  }
  Out[*At]=0;
}
STATIC VOID Ps5DiskModelEx(EFI_HANDLE Handle,CHAR8 *Out,UINTN Cap,BOOLEAN IdKey)   /* r5.25.3: IdKey = r5.21 bytes for the stable disk id */
{
  EFI_DISK_INFO_PROTOCOL *Di=NULL;UINTN At=0;
  Out[0]=0;
  if(EFI_ERROR(gBS->HandleProtocol(Handle,&mPs5DiskInfoGuid,(VOID **)&Di)) || Di==NULL)return;
  if(CompareGuid(&Di->Interface,&mPs5DiUsbGuid) || CompareGuid(&Di->Interface,&mPs5DiScsiGuid)) {
    UINT8 Inq[96];UINT32 Size=sizeof(Inq);
    if(!EFI_ERROR(Di->Inquiry(Di,Inq,&Size)) && Size>=32){Ps5CopyText(Out,Cap,&At,Inq+8,8);Ps5CopyText(Out,Cap,&At,(CONST UINT8 *)" ",1);Ps5CopyText(Out,Cap,&At,Inq+16,16);}
  } else if(IdKey && CompareGuid(&Di->Interface,&mPs5DiNvmeGuid)) {   /* unchanged r5.21 key, so saved bootlist ids stay valid */
    STATIC UINT8 Ns[4096];UINT32 Size=sizeof(Ns);
    if(!EFI_ERROR(Di->Identify(Di,Ns,&Size)) && Size>=64)Ps5CopyText(Out,Cap,&At,Ns+24,40);
  } else if(CompareGuid(&Di->Interface,&mPs5DiNvmeGuid)) {
    /* r5.25.3: DiskInfo->Identify of NvmExpressDxe returns the NAMESPACE data (user saw "M.2: `pt 500 GB"); the model
     * number is in the CONTROLLER data -> Identify CNS=1 through the controller's NVM Express pass-thru, as EDK2's
     * BmGetNvmeDescription does. */
    STATIC UINT8 Id[4096];
    EFI_DEVICE_PATH_PROTOCOL *Dp=NULL;EFI_HANDLE Ctrl=NULL;EFI_NVM_EXPRESS_PASS_THRU_PROTOCOL *Pt=NULL;
    if(!EFI_ERROR(gBS->HandleProtocol(Handle,&gEfiDevicePathProtocolGuid,(VOID **)&Dp)) &&
       !EFI_ERROR(gBS->LocateDevicePath(&mPs5NvmePtGuid,&Dp,&Ctrl)) &&
       !EFI_ERROR(gBS->HandleProtocol(Ctrl,&mPs5NvmePtGuid,(VOID **)&Pt)) && Pt!=NULL) {
      EFI_NVM_EXPRESS_PASS_THRU_COMMAND_PACKET Pk;EFI_NVM_EXPRESS_COMMAND Cmd;EFI_NVM_EXPRESS_COMPLETION Cpl;
      ZeroMem(&Pk,sizeof(Pk));ZeroMem(&Cmd,sizeof(Cmd));ZeroMem(&Cpl,sizeof(Cpl));ZeroMem(Id,sizeof(Id));
      Cmd.Cdw0.Opcode=0x06;Cmd.Nsid=0;Cmd.Cdw10=1;Cmd.Flags=CDW10_VALID;          /* Identify controller */
      Pk.NvmeCmd=&Cmd;Pk.NvmeCompletion=&Cpl;Pk.TransferBuffer=Id;Pk.TransferLength=sizeof(Id);
      Pk.CommandTimeout=EFI_TIMER_PERIOD_SECONDS(5);Pk.QueueType=0;               /* admin queue */
      if(!EFI_ERROR(Pt->PassThru(Pt,0,&Pk,NULL)))Ps5CopyText(Out,Cap,&At,Id+24,40);   /* controller data: Mn at 24..63 */
    }
  }
  while(At && Out[At-1]==' ')Out[--At]=0;
}
STATIC VOID Ps5DiskModel(EFI_HANDLE Handle,CHAR8 *Out,UINTN Cap) { Ps5DiskModelEx(Handle,Out,Cap,FALSE); }
STATIC CHAR16 *EFIAPI Ps5BootDescription(EFI_HANDLE Handle,CONST CHAR16 *DefaultDescription)
{
  EFI_BLOCK_IO_PROTOCOL *B=NULL;CHAR8 Model[64];UINT64 Gb=0;CHAR16 *D;
  (VOID)DefaultDescription;
  if(!EFI_ERROR(gBS->HandleProtocol(Handle,&gEfiBlockIoProtocolGuid,(VOID **)&B)) && B && B->Media && B->Media->MediaPresent)
    Gb=DivU64x32(MultU64x32(B->Media->LastBlock+1,B->Media->BlockSize),1000000000u);
  Ps5DiskModel(Handle,Model,sizeof(Model));
  if(!Model[0] && !Gb)return NULL;                   /* nothing better known: EDK2 default */
  D=AllocateZeroPool(160*sizeof(CHAR16));
  if(D==NULL)return NULL;
  /* r5.25.3: the model of many SSDs already ends with its size ("SANDISK OPTIMUS 5100 500GB") - no second size then */
  if(Model[0] && (AsciiStrStr(Model,"GB")||AsciiStrStr(Model,"TB")||AsciiStrStr(Model,"Gb")))UnicodeSPrint(D,160*sizeof(CHAR16),L"%s: %a",Ps5HandleBehindUsb(Handle)?L"USB":L"M.2",Model);
  else if(Model[0])UnicodeSPrint(D,160*sizeof(CHAR16),L"%s: %a %Lu GB",Ps5HandleBehindUsb(Handle)?L"USB":L"M.2",Model,Gb);
  else UnicodeSPrint(D,160*sizeof(CHAR16),L"%s: %Lu GB disk",Ps5HandleBehindUsb(Handle)?L"USB":L"M.2",Gb);
  return D;
}

/* r5.21: stable disk id for bootlist= (no port numbers): CRC-32 of "U|model|gb" (USB) or "M|model|serial|gb" (NVMe
 * serial from Identify). The disk is the whole-disk BlockIo above the volume's last HD node. */
STATIC EFI_HANDLE Ps5DiskOf(EFI_HANDLE Volume)
{
  EFI_DEVICE_PATH_PROTOCOL *Dp=DevicePathFromHandle(Volume),*Copy,*Node,*Last=NULL,*Walk;EFI_HANDLE Disk=NULL;
  if(Dp==NULL || (Copy=DuplicateDevicePath(Dp))==NULL)return NULL;
  for(Node=Copy;!IsDevicePathEnd(Node);Node=NextDevicePathNode(Node))
    if(DevicePathType(Node)==MEDIA_DEVICE_PATH && DevicePathSubType(Node)==MEDIA_HARDDRIVE_DP)Last=Node;
  if(Last!=NULL) {
    SetDevicePathEndNode(Last);Walk=Copy;
    if(EFI_ERROR(gBS->LocateDevicePath(&gEfiBlockIoProtocolGuid,&Walk,&Disk)) || !IsDevicePathEnd(Walk))Disk=NULL;
  } else Disk=Volume;                                /* file system directly on the disk (superfloppy stick) */
  FreePool(Copy);
  return Disk;
}
STATIC UINT32 Ps5DiskId(EFI_HANDLE Disk)
{
  EFI_BLOCK_IO_PROTOCOL *B=NULL;EFI_DISK_INFO_PROTOCOL *Di=NULL;CHAR8 Model[64],Serial[24],Key[128];UINT64 Gb=0;UINTN At=0;UINT32 C=0;
  if(Disk==NULL)return 0;
  Serial[0]=0;
  Ps5DiskModelEx(Disk,Model,sizeof(Model),TRUE);   /* r5.25.3: id key unchanged */
  if(!EFI_ERROR(gBS->HandleProtocol(Disk,&mPs5DiskInfoGuid,(VOID **)&Di)) && Di && CompareGuid(&Di->Interface,&mPs5DiNvmeGuid)) {
    STATIC UINT8 Id[4096];UINT32 Size=sizeof(Id);
    if(!EFI_ERROR(Di->Identify(Di,Id,&Size)) && Size>=24)Ps5CopyText(Serial,sizeof(Serial),&At,Id+4,20);
  }
  if(!EFI_ERROR(gBS->HandleProtocol(Disk,&gEfiBlockIoProtocolGuid,(VOID **)&B)) && B && B->Media)
    Gb=DivU64x32(MultU64x32(B->Media->LastBlock+1,B->Media->BlockSize),1000000000u);
  AsciiSPrint(Key,sizeof(Key),"%a|%a|%a|%Lu",Ps5HandleBehindUsb(Disk)?"U":"M",Model,Serial,Gb);
  gBS->CalculateCrc32(Key,AsciiStrLen(Key),&C);
  return C?C:1;
}
/* r5.25.2: every whole-disk BlockIo behind USB with its disk id (diagnosis of the wait after usbpowercycle) */
STATIC VOID Ps5LogUsbDisks(CONST CHAR8 *Why)
{
  EFI_HANDLE *H=NULL;UINTN N=0,Usb=0;
  if(EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol,&gEfiBlockIoProtocolGuid,NULL,&N,&H)))N=0;
  for(UINTN I=0;I<N;I++) {
    EFI_BLOCK_IO_PROTOCOL *B=NULL;CHAR8 Model[64];
    if(EFI_ERROR(gBS->HandleProtocol(H[I],&gEfiBlockIoProtocolGuid,(VOID **)&B)) || B==NULL || B->Media==NULL ||
       B->Media->LogicalPartition || !Ps5HandleBehindUsb(H[I]))continue;
    Usb++;Ps5DiskModel(H[I],Model,sizeof(Model));
    CHAR16 *T=ConvertDevicePathToText(DevicePathFromHandle(H[I]),TRUE,TRUE);
    DEBUG((DEBUG_ERROR,"PS5 USB disk (%a): id %08x model '%a' media=%u blocks=%Lu x %u path=%s\n",Why,Ps5DiskId(H[I]),Model,
      (UINT32)B->Media->MediaPresent,B->Media->LastBlock+1,B->Media->BlockSize,T?T:L"?"));
    if(T)FreePool(T);
  }
  DEBUG((DEBUG_ERROR,"PS5 USB disks (%a): %u of %u BlockIo\n",Why,(UINT32)Usb,(UINT32)N));
  if(H)FreePool(H);
}
/* r5.21: publish the bootable disks (Ps5BootDevs) and the current order (Ps5BootListSel) for the Boot tab */
STATIC VOID Ps5PublishBootDevs(VOID)
{
#ifdef PS5_NATIVE_SETUP
  EFI_HANDLE *H=NULL;UINTN N=0;unsigned Present[PS5_BOOTDEV_MAX],Order[PS5_BOOTDEV_MAX],Pn=0,On;UINT8 Usb[PS5_BOOTDEV_MAX];
  EFI_HANDLE Disk[PS5_BOOTDEV_MAX];PS5_BOOT_LIST Sel;
  if(!EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol,&gEfiSimpleFileSystemProtocolGuid,NULL,&N,&H))) {
    for(UINTN I=0;I<N && Pn<PS5_BOOTDEV_MAX;I++) {
      if(!Ps5VolumeHasBootx64(H[I]))continue;
      EFI_HANDLE D=Ps5DiskOf(H[I]);UINT32 Id=Ps5DiskId(D);BOOLEAN Dup=FALSE;
      if(!Id)continue;
      for(unsigned J=0;J<Pn;J++)if(Present[J]==Id)Dup=TRUE;
      if(Dup)continue;
      Disk[Pn]=D;Usb[Pn]=Ps5HandleBehindUsb(D)?1:0;Present[Pn++]=Id;
    }
    FreePool(H);
  }
  On=ps5_bootlist_order(mPs5Set.bootlist,mPs5Set.bootlist_n,Present,Usb,Pn,mPs5Set.bootorder==1,Order);
  ZeroMem(&mPs5Devs,sizeof(mPs5Devs));ZeroMem(&Sel,sizeof(Sel));
  for(unsigned I=0;I<On;I++) {
    unsigned J=Order[I];CHAR16 *Name=Ps5BootDescription(Disk[J],NULL);
    mPs5Devs.Id[I]=Present[J];mPs5Devs.Usb[I]=Usb[J];
    UnicodeSPrint(mPs5Devs.Name[I],sizeof(mPs5Devs.Name[I]),L"%s",Name?Name:(Usb[J]?L"USB disk":L"Internal disk"));
    if(Name)FreePool(Name);
    Sel.Order[I]=(UINT8)(I+1);
    DEBUG((DEBUG_ERROR,"PS5 boot list: %u. %s (id %08x)\n",I+1,mPs5Devs.Name[I],Present[J]));
  }
  mPs5Devs.Count=On;
  EFI_STATUS S1=gRT->SetVariable(L"Ps5BootDevs",&mPs5PresetGuid,EFI_VARIABLE_BOOTSERVICE_ACCESS|EFI_VARIABLE_RUNTIME_ACCESS,sizeof(mPs5Devs),&mPs5Devs);
  EFI_STATUS S2=gRT->SetVariable(L"Ps5BootListSel",&mPs5PresetGuid,EFI_VARIABLE_BOOTSERVICE_ACCESS|EFI_VARIABLE_RUNTIME_ACCESS,sizeof(Sel),&Sel);
  DEBUG((DEBUG_ERROR,"PS5 boot list: %u disks published (%r/%r, stored list %u)\n",(UINT32)On,S1,S2,(UINT32)mPs5Set.bootlist_n));
#endif
}

#ifdef PS5_NATIVE_SETUP
/* r5.26.6 (user 25.11.: existing Windows on a USB SSD + a freshly written NTFS installer drive; after the "restart" the
 * installer drive asks for, "Boot once from ..." in the Boot Manager started Windows Setup without keyboard, mouse and
 * installer drive - Setup asked for a driver): a start from the EDK2 Boot Manager skipped the port power cycle of the
 * normal boot path, so the USB devices kept the state the previous Windows (here: Windows Setup) left - the firmware
 * can still use them, Windows cannot (r5.25.11, r5.25.14). EfiBootManagerBoot (BmBoot.c, ps5_boot_manager.py) calls
 * this hook before ReadyToBoot, at the caller's TPL: if no full cycle ran in this start (and usbpowercycle is on), the
 * ports are cycled as in the boot path, the chosen drive stays powered, and the start waits up to 60 s for it. */
#define PS5_PRE_BOOT_GUID {0x96d4b2b8,0xc972,0x487b,{0x85,0x84,0x7a,0x2e,0x2c,0xe0,0x89,0x7b}}
typedef struct { UINT32 Revision; VOID (EFIAPI *PreBoot)(EFI_DEVICE_PATH_PROTOCOL *FilePath); } PS5_PRE_BOOT_PROTOCOL;
STATIC EFI_GUID mPs5PreBootGuid=PS5_PRE_BOOT_GUID;
STATIC UINT32 Ps5VolumeDiskPresent(UINT32 Want)
{
  EFI_HANDLE *H=NULL; UINTN N=0; UINT32 Found=0;
  if (EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol,&gEfiSimpleFileSystemProtocolGuid,NULL,&N,&H))) return 0;
  for (UINTN I=0; I<N && !Found; I++) if (Ps5DiskId(Ps5DiskOf(H[I]))==Want) Found=1;
  if (H!=NULL) FreePool(H);
  return Found;
}
STATIC VOID EFIAPI Ps5PreBootCycle(EFI_DEVICE_PATH_PROTOCOL *FilePath)
{
  UINT32 Uc=1; UINTN Us=sizeof(Uc);
  if (FilePath!=NULL && mPs5FullCycleRan) {   /* r5.28.14: the drive picked in the Boot Manager may sit on a port cut earlier in this start */
    EFI_DEVICE_PATH_PROTOCOL *Sd=FilePath; EFI_HANDLE Sh=NULL;
    EfiBootManagerConnectDevicePath(FilePath,NULL);
    if (!EFI_ERROR(gBS->LocateDevicePath(&gEfiBlockIoProtocolGuid,&Sd,&Sh))) Ps5UsbSettle(Sh,"boot menu");
  }
  if (mPs5FullCycleRan || FilePath==NULL) return;
  if (!EFI_ERROR(gRT->GetVariable(L"Ps5UsbCycle",&mPs5PresetGuid,NULL,&Us,&Uc)) && Us==sizeof(Uc) && !Uc) return;
  EfiBootManagerConnectDevicePath(FilePath,NULL);
  EFI_DEVICE_PATH_PROTOCOL *Dp=FilePath; EFI_HANDLE H=NULL;
  if (EFI_ERROR(gBS->LocateDevicePath(&gEfiSimpleFileSystemProtocolGuid,&Dp,&H))) H=NULL;
  UINT32 Want=H!=NULL?Ps5DiskId(Ps5DiskOf(H)):0;
  DEBUG((DEBUG_ERROR,"PS5 pre-boot: start from the Boot Manager without a port power cycle - cycling now (chosen disk %08x)\n",Want));
  if (mPs5UsbMassStopHandle!=NULL && !EFI_ERROR(gBS->UninstallProtocolInterface(mPs5UsbMassStopHandle,&mPs5UsbMassStopGuid,NULL)))
    mPs5UsbMassStopHandle=NULL;   /* as in the boot path: never a USB-MSD stop marker across the controller restart */
  mRecoverSilent=TRUE; Ps5UsbRecover(TRUE,H); mRecoverSilent=FALSE;
  EfiBootManagerConnectAll();
  if (Want==0) return;
  for (UINTN A=0; A<PS5_WARM_USB_WAIT_ATTEMPTS; A++) {
    if (Ps5VolumeDiskPresent(Want)) { DEBUG((DEBUG_ERROR,"PS5 pre-boot: chosen disk %08x back after %u.%u s\n",Want,(UINT32)(A/10),(UINT32)(A%10)));
      Ps5UsbSettle(H,"boot menu"); return; }   /* r5.28.14: the menu start just cut the ports itself (chosen disk kept: no wait) */
    if (A%20==10) EfiBootManagerConnectAll();
    gBS->Stall(100000);
  }
  DEBUG((DEBUG_ERROR,"PS5 pre-boot: chosen disk %08x not back after 60 s\n",Want));
}
STATIC PS5_PRE_BOOT_PROTOCOL mPs5PreBoot={1,Ps5PreBootCycle};
#endif

EFI_STATUS Ps5BootPinnedWindows (VOID)
{
  { STATIC BOOLEAN Registered=FALSE;   /* r5.21: readable boot option names */
    if(!Registered){EFI_STATUS Rs=EfiBootManagerRegisterBootDescriptionHandler(Ps5BootDescription);Registered=TRUE;
      DEBUG((DEBUG_ERROR,"PS5 boot names: description handler %r\n",Rs));
#ifdef PS5_NATIVE_SETUP
      EFI_EVENT Rtb=NULL;   /* r5.22: testsigning for boots from the boot menu */
      EFI_STATUS Rh=EfiCreateEventReadyToBootEx(TPL_CALLBACK,Ps5BcdReadyToBoot,NULL,&Rtb);
      DEBUG((DEBUG_ERROR,"PS5 BCD testsigning: ReadyToBoot hook %r\n",Rh));(VOID)Rh;
      { EFI_HANDLE Ph=NULL;   /* r5.26.6: USB power cycle for starts from the Boot Manager */
        EFI_STATUS Pp=gBS->InstallMultipleProtocolInterfaces(&Ph,&mPs5PreBootGuid,&mPs5PreBoot,NULL);
        DEBUG((DEBUG_ERROR,"PS5 pre-boot hook %r\n",Pp));(VOID)Pp; }
#endif
    } }
  Ps5WarmReadRecord();   /* r5.25.4 */
  UINT32 BootOrder=0; EFI_HANDLE BootOrderEsp=NULL;    /* r5.15.3: keep selection order bound to its settings owner */
  unsigned BootList[PS5_BOOTLIST_MAX],BootListN=0;     /* r5.21: bootlist= from the config ESP (disk ids, preferred first) */
  UINTN FirstUsb=(UINTN)-1;                            /* r5.15.1: attempt of the first USB boot volume */
#ifdef PS5_NATIVE_SETUP
  UINT32 UsbCycle=1, CycleWantId=0; BOOLEAN CycleDone=FALSE, CycleBack=FALSE; UINTN CycleAt=0;   /* r5.25.1: usbpowercycle */
#endif
  PS5_TS("bds-boot-windows-entry");
#ifdef PS5_NATIVE_SETUP
  BOOLEAN DisplayStarted=FALSE, NoMediumMenuEntered=FALSE;
#endif
  BOOLEAN M2Expect=FALSE;
#ifdef PS5_NVME_PREINIT
  Ps5RegisterNvmePreinit();
#endif
#ifdef PS5_M2_OVERLAY_N4
  {
    EFI_STATUS M2Status=Ps5InstallM2BlockIoOverlay();
    DEBUG((DEBUG_ERROR,"PS5 M2 overlay: %r\n",M2Status));
    M2Expect=!EFI_ERROR(M2Status);   /* r4-nvme: the internal ESP is expected - give it 2 s before USB may win */
  }
#endif
  for (UINTN Attempt=0; Attempt<PS5_BOOT_MEDIUM_ATTEMPTS; Attempt++) {
    EFI_HANDLE *Handles=NULL; UINTN Count=0;
    EFI_HANDLE Internal=NULL, Pinned=NULL, Removable=NULL; UINTN PinnedCount=0;
    EFI_HANDLE UsbInstalled=NULL, UsbInstaller=NULL;    /* r5.15 */
    EFI_HANDLE VolH[PS5_BOOTLIST_MAX]; UINTN VolN=0;     /* r5.21: every volume with bootx64.efi */
#ifdef PS5_NATIVE_SETUP
    BOOLEAN CycleQuiet=CycleDone && !CycleBack && CycleWantId!=0 && (Attempt-CycleAt)%50!=0;   /* r5.25.2: no per-pass lines while waiting */
#else
    BOOLEAN CycleQuiet=FALSE;
#endif
    EFI_STATUS Status=gBS->LocateHandleBuffer(ByProtocol,&gEfiSimpleFileSystemProtocolGuid,
                                             NULL,&Count,&Handles);
    if (!EFI_ERROR(Status)) {
      for (UINTN Index=0; Index<Count; Index++) {
        if (!Ps5VolumeHasBootx64(Handles[Index])) continue;
        if (VolN<PS5_BOOTLIST_MAX) VolH[VolN++]=Handles[Index];
        EFI_PARTITION_INFO_PROTOCOL *Partition=NULL; BOOLEAN IsPinned=FALSE;
        if (!EFI_ERROR(gBS->HandleProtocol(Handles[Index],&gEfiPartitionInfoProtocolGuid,(VOID **)&Partition)) &&
            Partition!=NULL && Partition->Type==PARTITION_TYPE_GPT &&
            CompareGuid(&Partition->Info.Gpt.UniquePartitionGUID,&mWindowsEsp)) { IsPinned=TRUE; PinnedCount++; }
        BOOLEAN Usb=Ps5HandleRemovable(Handles[Index]);
        CHAR16 *T=ConvertDevicePathToText(DevicePathFromHandle(Handles[Index]),TRUE,TRUE);
        if (!CycleQuiet) DEBUG((DEBUG_ERROR,"PS5 boot enum: idx=%u bootx64=1 pinned=%u usb=%u path=%s\n",
          (UINT32)Index,(UINT32)IsPinned,(UINT32)Usb,T?T:L"?"));
        if (T) FreePool(T);
        if (IsPinned) { if (Pinned==NULL) Pinned=Handles[Index]; }
        else if (!Usb) { if (Internal==NULL) Internal=Handles[Index]; }
        else {
          if (Removable==NULL) Removable=Handles[Index];
          if (Ps5VolumeHasFile(Handles[Index],L"\\sources\\boot.wim")) { if (UsbInstaller==NULL) UsbInstaller=Handles[Index]; }
          else if (Ps5VolumeHasFile(Handles[Index],L"\\EFI\\Microsoft\\Boot\\BCD")) { if (UsbInstalled==NULL) UsbInstalled=Handles[Index]; }
        }
      }
    }
    if (Handles!=NULL) FreePool(Handles);
    // Ambiguous pinned ESP (two volumes carry the same unique GUID) is not trustworthy as a fallback.
    if (PinnedCount>1) { DEBUG((DEBUG_ERROR,"PS5 Windows boot: duplicate pinned ESP, ignoring pinned fallback\n")); Pinned=NULL; }
    // Default without user input: prefer an installed internal Windows (continues setup reboots),
    // then the legacy pinned ESP, then a removable/USB medium (= installer stick on an empty target).
    if (UsbInstalled!=NULL) Removable=UsbInstalled;      /* r5.15: an installed Windows on USB before an installer stick */
    else if (UsbInstaller!=NULL) Removable=UsbInstaller;
    // r5.15.2 (review 19:19): bootorder (and all settings) come from the config ESP = the internal ESP
    // that boots without bootorder=usb. The fixed-GUID Windows ESP is classified Pinned and excluded from Internal, so
    // r5.15.1 never read bootorder in the common "pinned internal ESP only" case. Duplicate/removable pinned = ambiguous.
    EFI_HANDLE ConfigEsp=Internal;
    if (ConfigEsp==NULL && PinnedCount==1 && Pinned!=NULL && !Ps5HandleRemovable(Pinned)) ConfigEsp=Pinned;
    mPs5ConfigEsp=ConfigEsp;
    if (ConfigEsp!=BootOrderEsp) {
      BootOrderEsp=ConfigEsp;
      { struct ps5_settings Pk; Ps5PeekSettings(ConfigEsp,&Pk); BootOrder=ConfigEsp?Pk.bootorder:0;   /* r5.21: + bootlist */
        BootListN=ConfigEsp?Pk.bootlist_n:0; CopyMem(BootList,Pk.bootlist,sizeof(BootList));
#ifdef PS5_NATIVE_SETUP
        UsbCycle=Pk.usbcycle;   /* r5.25.1: default 1, also without a config ESP */
#endif
        if (BootListN) DEBUG((DEBUG_ERROR,"PS5 boot list: %u ids from \\PS5UEFI.CFG, first %08x\n",BootListN,BootList[0])); }
      if (ConfigEsp!=NULL)
        DEBUG((DEBUG_ERROR,"PS5 boot order: %a (config ESP \\PS5UEFI.CFG; owner changed internal=%u pinned=%u)\n",BootOrder?"usb":"internal",
          (UINT32)(ConfigEsp==Internal),(UINT32)(ConfigEsp==Pinned)));
      else
        DEBUG((DEBUG_ERROR,"PS5 boot order: config ESP disappeared; default internal\n"));
    }
    EFI_HANDLE Selected = Internal ? Internal : (Pinned ? Pinned : Removable);
    if (BootOrder==1 && Removable==NULL && Attempt<200) {  /* r5.15.1: bootorder=usb but no USB volume yet (hub devices bind
                                                              * late): up to 20 s, ConnectAll every 2 s, before internal */
      if (Attempt%20==0) {
        DEBUG((DEBUG_ERROR,"PS5 boot order usb: wait %u s: fs=%u usb-bootx64=0 internal=%u\n",(UINT32)(Attempt/10),(UINT32)Count,(UINT32)(Internal!=NULL)));
        if (Attempt) EfiBootManagerConnectAll();
      }
      Selected=NULL;
    }
    if (Removable!=NULL && FirstUsb==(UINTN)-1) {
      FirstUsb=Attempt;
      if (BootOrder==1) DEBUG((DEBUG_ERROR,"PS5 boot order usb: wait %u s: fs=%u usb-bootx64=1 (installed=%u installer=%u)\n",
        (UINT32)(Attempt/10),(UINT32)Count,(UINT32)(UsbInstalled!=NULL),(UINT32)(UsbInstaller!=NULL)));
    }
    if (BootOrder==1 && Removable!=NULL) {                /* r5.15: bootorder=usb */
      Selected=Removable;
      if (UsbInstalled==NULL && Attempt<FirstUsb+30) {               /* only a stick so far: 3 s for the (slower) USB NVMe's ESP */
        if (Attempt==0 || Attempt==29) DEBUG((DEBUG_ERROR,"PS5 boot order usb: installer only, waiting for an installed USB ESP\n"));
        Selected=NULL;
      }
    }
#ifdef PS5_NATIVE_SETUP
    BOOLEAN CycleHold=FALSE;   /* r5.25.1: after usbpowercycle wait up to 60 s for the disk chosen before it */
    if (CycleDone && !CycleBack && CycleWantId!=0) {
      BOOLEAN Back=FALSE;
      for (UINTN V=0; V<VolN; V++) if (Ps5DiskId(Ps5DiskOf(VolH[V]))==CycleWantId) Back=TRUE;
      /* r5.25.19: a stuck bridge (RTL9210B ~40-50 s) was cut too - wait for it as well, then choose again (an installed
       * Windows on it goes before the installer stick that was chosen without it) */
      if (Back && mPs5CycleTsc!=0 && Ps5UsbWorkingDisks()<mPs5CycleWant && Attempt<CycleAt+600) {
        if ((Attempt-CycleAt)%50==0) { DEBUG((DEBUG_ERROR,"PS5 usbpowercycle: waiting for the cycled USB bridge (%u of %u disks): %u s\n",
          (UINT32)Ps5UsbWorkingDisks(),(UINT32)mPs5CycleWant,(UINT32)((Attempt-CycleAt)/10))); if (Attempt>CycleAt) EfiBootManagerConnectAll(); }
        Back=FALSE;
      }
      if (Back || Attempt>=CycleAt+600) {
        if (mPs5CycleTsc!=0) { DEBUG((DEBUG_ERROR,"PS5 usbpowercycle: USB disks after the cycle: %u of %u\n",(UINT32)Ps5UsbWorkingDisks(),(UINT32)mPs5CycleWant)); mPs5CycleTsc=0; }
        CycleBack=TRUE;
        DEBUG((DEBUG_ERROR,"PS5 usbpowercycle: chosen disk %08x %a after %u.%u s\n",CycleWantId,Back?"back":"not back, giving up",
          (UINT32)((Attempt-CycleAt)/10),(UINT32)((Attempt-CycleAt)%10)));
        if (!Back) Ps5LogUsbDisks("given up");
      } else {
        if ((Attempt-CycleAt)%50==0) {
          DEBUG((DEBUG_ERROR,"PS5 usbpowercycle: waiting for disk %08x: %u s\n",CycleWantId,(UINT32)((Attempt-CycleAt)/10)));
          Ps5LogUsbDisks("waiting");
          if (Attempt>CycleAt) EfiBootManagerConnectAll();
        }
        CycleHold=TRUE;
      }
    }
#else
    BOOLEAN CycleHold=FALSE;
#endif
    BOOLEAN ListDecided=FALSE;
    if (BootListN) {   /* r5.21: the first disk of bootlist that is present wins; 3 s for a better-placed disk to appear */
      EFI_HANDLE Best=NULL; UINTN BestPos=BootListN;
      for (UINTN V=0; V<VolN; V++) {
        UINT32 Id=Ps5DiskId(Ps5DiskOf(VolH[V]));
        for (UINTN P=0; P<BestPos; P++) if (BootList[P]==Id) { Best=VolH[V]; BestPos=P; break; }
      }
      if (Best!=NULL) {
        ListDecided=TRUE; Selected=Best;
        /* r5.25.4 (HW 18.11.: stick saved on place 1, the SanDisk on place 2 started): USB disks appear late on the console
         * (hub, bridge); 3 s was too short for the stick. A better-placed disk that is missing is now waited for up to
         * 10 s (ConnectAll every 2 s); a list whose first entry is present boots at once. */
        if (BestPos>0 && Attempt<PS5_BOOTLIST_WAIT_ATTEMPTS) {
          if (Attempt%20==10) EfiBootManagerConnectAll();
          if (Attempt==0 || Attempt==50) DEBUG((DEBUG_ERROR,"PS5 boot list: entry %u present, waiting up to 10 s for a better-placed disk\n",(UINT32)BestPos+1));
          Selected=NULL;
        } else if (!CycleHold) DEBUG((DEBUG_ERROR,"PS5 boot list: entry %u present - booting it\n",(UINT32)BestPos+1));
      }
    }
#ifdef PS5_LAB_SANDISK
    if (Internal!=NULL) { Selected=Internal; ListDecided=TRUE; }   /* lab: the internal SanDisk first */
#endif
    /* r5.25.4 (user): after a Windows restart start the disk Windows asked for (or the last one) directly, without the
     * start screen; a bootable disk that is not in the saved list yet (e.g. a just installed Windows) goes to place 1. */
    if (mPs5WarmDirect) {
      EFI_HANDLE T=NULL;
      for (UINTN V=0; V<VolN && T==NULL; V++) if (Ps5VolGptGuidIs(VolH[V],&mPs5WarmGuid)) T=VolH[V];
      if (mPs5WarmNextPending) {
        if (mPs5WarmNextDisk!=NULL) {   /* r5.27.4: the ESP whose device path starts with the BootNext disk path */
          UINTN Pl=GetDevicePathSize(mPs5WarmNextDisk)-END_DEVICE_PATH_LENGTH;
          for (UINTN V=0; V<VolN && T==NULL; V++) {
            EFI_DEVICE_PATH_PROTOCOL *Vp=DevicePathFromHandle(VolH[V]);
            if (Vp!=NULL && GetDevicePathSize(Vp)>Pl && CompareMem(Vp,mPs5WarmNextDisk,Pl)==0 && Ps5VolumeHasFile(VolH[V],L"\\EFI\\Boot\\bootx64.efi")) T=VolH[V];
          }
        }
        /* r5.28.6 (HW 26.11.: "Boot once" from the internal Windows into the Windows on the RTL9210B ended in GameOS - the disk and
         * its ESP were there, but the entry's device path no longer matched: the bridge came back on another path, or the entry
         * was an older copy): the disk whose firmware name equals the entry's name ("USB: <model> <size>") is taken instead */
        for (UINTN V=0; V<VolN && T==NULL && (mPs5WarmNextName!=NULL || mPs5WarmNextHashOk); V++) {   /* r5.28.7: or by the name hash */
          EFI_HANDLE Dk; CHAR16 *Nm;
          if (!Ps5VolumeHasFile(VolH[V],L"\\EFI\\Boot\\bootx64.efi") || (Dk=Ps5DiskOf(VolH[V]))==NULL) continue;
          Nm=Ps5BootDescription(Dk,NULL);
          if (Nm!=NULL && (mPs5WarmNextHashOk ? Ps5NameHash(Nm)==mPs5WarmNextHash : StrCmp(Nm,mPs5WarmNextName)==0)) {
            T=VolH[V];
            if (!mPs5WarmNotified) DEBUG((DEBUG_ERROR,"PS5 warm re-entry: BootNext disk found by its name '%s' (the device path changed)\n",Nm));
          }
          if (Nm!=NULL) FreePool(Nm);
        }
      }
      /* r5.25.22 (HW 24.11. autoinst3: the restart at the end of Windows Setup from a USB stick came back with no BootNext and
       * no Windows entry in BootOrder, so the last boot device - the installer stick - was started again and Windows Setup began
       * anew): a restart from a medium without an installed Windows (no \EFI\Microsoft\Boot\BCD) goes to an installed Windows
       * on USB if there is one, like a start from GameOS; up to 5 s for the USB SSD's ESP to appear. */
      BOOLEAN WarmWaitInstalled=FALSE;
      if (T!=NULL && !Ps5VolumeHasFile(T,L"\\EFI\\Microsoft\\Boot\\BCD")) {
        if (UsbInstalled!=NULL && UsbInstalled!=T) {
          if (!mPs5WarmNotified) DEBUG((DEBUG_ERROR,"PS5 warm re-entry: the last boot device has no installed Windows - booting the installed Windows on USB instead\n"));
          T=UsbInstalled;
        } else if (Attempt<50) {
          WarmWaitInstalled=TRUE;
          if (Attempt%10==0) DEBUG((DEBUG_ERROR,"PS5 warm re-entry: last boot device without an installed Windows - looking for one (%u s)\n",(UINT32)(Attempt/10)));
          if (Attempt%20==10) EfiBootManagerConnectAll();
        }
      }
      if (WarmWaitInstalled) {
        Selected=NULL;
      } else if (T!=NULL) {
        Selected=T; ListDecided=TRUE; mPs5WarmDirectHit=TRUE;
        if (!mPs5WarmNotified) { mPs5WarmNotified=TRUE; DEBUG((DEBUG_ERROR,"PS5 warm re-entry: target found - booting it directly\n")); }
      } else {
        /* r5.25.12: 10 s for an internal target, 60 s for a USB one; after usbpowercycle (it restarts the controller, the
         * chosen USB disk re-enumerates) the time counts from the cycle, not from the start of the search */
        UINTN Limit=mPs5WarmUsb?PS5_WARM_USB_WAIT_ATTEMPTS:PS5_BOOTLIST_WAIT_ATTEMPTS;
#ifdef PS5_NATIVE_SETUP
        if (CycleDone && CycleAt+PS5_WARM_USB_WAIT_ATTEMPTS>Limit) Limit=CycleAt+PS5_WARM_USB_WAIT_ATTEMPTS;
        if (mPs5WarmForcedAt && mPs5WarmForcedAt+PS5_WARM_USB_WAIT_ATTEMPTS>Limit) Limit=mPs5WarmForcedAt+PS5_WARM_USB_WAIT_ATTEMPTS;   /* r5.28.1 */
        if (mPs5WarmFallbackAt) { UINTN Fl=mPs5WarmFallbackAt+(mPs5WarmUsb?PS5_WARM_USB_WAIT_ATTEMPTS:PS5_BOOTLIST_WAIT_ATTEMPTS); if (Fl>Limit) Limit=Fl; }   /* r5.28.6 */
        /* r5.25.23 (HW 24.11. warmloop restart 3: the RTL9210B came back from Windows at 480 Mbit/s without a disk, its controller
         * also carried the setup stick and was "left alone"): after 5 s every port without a working disk is power-cycled, the
         * controller restarted even if it carries a working disk (the boot medium is gone anyway) */
        /* r5.28.1 (HW/user 26.11.: after unclean power-offs the USB SSDs ran their internal recovery and their bridges stayed
         * off the bus meanwhile; a cycle after 5 s restarted that every time): the bridge gets 30 s to come back by itself */
        if (mPs5WarmUsb && Attempt==300) {
          DEBUG((DEBUG_ERROR,"PS5 warm re-entry: USB target missing after 30 s - power cycle of the USB ports without a working disk\n"));
          mPs5ForceCycle=TRUE; mRecoverSilent=TRUE; Ps5UsbRecover(TRUE,NULL); mRecoverSilent=FALSE; mPs5ForceCycle=FALSE;
          mPs5WarmForcedAt=Attempt; ConsoleSnapshot();   /* r5.28.1: 60 s more from the cycle */
        }
#endif
        if (Attempt<Limit) {
          if (Attempt%20==10) EfiBootManagerConnectAll();
          Selected=NULL;
        } else {
          /* r5.28.6 (user 26.11.: a restart into a disk that does not come back must not end in GameOS; generic, for every disk): a
           * missing BootNext target falls back to the disk the restart came from (last-boot record); a missing target after that,
           * or without such a record, goes to the normal boot-list start instead of a cold restart through GameOS (r5.25.5 used the
           * cold restart while the warm path was still fragile; a direct boot that fails to load still restarts cold) */
          volatile UINT64 *Lb=(volatile UINT64 *)(UINTN)PS5_WARM_LASTBOOT;
          if (mPs5WarmNextPending && Lb[0]==0x315453414C575350ULL && !mPs5WarmFallbackAt) {
            if (mPs5WarmNextDisk!=NULL) { FreePool(mPs5WarmNextDisk); mPs5WarmNextDisk=NULL; }
            mPs5WarmNextPending=FALSE; mPs5WarmNextHashOk=FALSE;
            if (mPs5WarmNextName!=NULL) { FreePool(mPs5WarmNextName); mPs5WarmNextName=NULL; }
            CopyMem(&mPs5WarmGuid,(VOID *)&Lb[1],16); mPs5WarmUsb=(Lb[3]==PS5_WARM_LASTUSB); mPs5WarmFallbackAt=Attempt;
            DEBUG((DEBUG_ERROR,"PS5 warm re-entry: BootNext target not found after %u s - falling back to the last boot device %g (%a)\n",
              (UINT32)(Attempt/10),&mPs5WarmGuid,mPs5WarmUsb?"USB":"internal"));
            Selected=NULL;
          } else {
            mPs5WarmDirect=FALSE; DEBUG((DEBUG_ERROR,"PS5 warm re-entry: target not found after %u s - normal start from the boot list\n",(UINT32)(Attempt/10)));
            Selected=NULL;
          }
        }
      }
    } else if (BootListN) {
      for (UINTN V=0; V<VolN; V++) {
        UINT32 Id=Ps5DiskId(Ps5DiskOf(VolH[V])); BOOLEAN Known=FALSE;
        if (!Id) continue;
        for (UINTN P=0; P<BootListN; P++) if (BootList[P]==Id) Known=TRUE;
        if (!Known) {
          Selected=VolH[V]; ListDecided=TRUE;
          if (mPs5NewDiskId!=Id) DEBUG((DEBUG_ERROR,"PS5 boot list: new bootable disk %08x - place 1\n",Id));
          mPs5NewDiskId=Id; break;
        }
      }
    }
    if (!ListDecided && Selected!=NULL && Internal==NULL && M2Expect && Attempt<20 && BootOrder!=1) {   /* r4-nvme: internal preferred, USB only as fallback */
      if (Attempt==0) DEBUG((DEBUG_ERROR,"PS5 Windows boot: USB medium found first, waiting up to 2 s for the internal M.2 ESP\n"));
      Selected=NULL;
    }
#ifdef PS5_NATIVE_SETUP
    /* r5.25.1: usbpowercycle (default on): USB disks whose bridge stays in its fallback mode after a cold start (RTL9210:
     * 0 bytes, only a replug helps) get the port power cycle that brings a missing boot disk back (Ps5UsbRecover: PP=0
     * on every powered root port, 2 s, controller restart). It runs once, when a medium has been chosen and before it
     * is used: in r5.25 it ran after the choice and the controller restart took the chosen stick away, so the internal
     * Windows started. Afterwards the search starts again and waits up to 60 s for the disk chosen before the cycle (a
     * USB stick comes back in a few seconds, an RTL9210B needs ~40-50 s); a disk that was not there before is never
     * waited for, so an unplugged disk in the boot list costs nothing. */
    if (Selected!=NULL && UsbCycle && !CycleDone) {
      CycleDone=TRUE; CycleAt=Attempt; CycleWantId=Ps5DiskId(Ps5DiskOf(Selected));
      DEBUG((DEBUG_ERROR,"PS5 usbpowercycle: port power cycle of the xHCI root ports before booting (chosen disk %08x, pass %u)\n",
        CycleWantId,(UINT32)Attempt));
      Ps5LogUsbDisks("before the cycle");
      Ps5LogUsbPorts("before the cycle");
      /* r5.28.4: the r5.28.3 stop marker for internal targets is gone again - it hid USB disks that appear late (an RTL9210B after
       * a Windows restart), so their firmware boot entries ("USB: ...") vanished and "Boot once" into them failed; the endless
       * wait it avoided is bounded in UsbMassStorageDxe now (build-uefi.py prepare_usb_mass_boot) */
      if (mPs5UsbMassStopHandle!=NULL && !EFI_ERROR(gBS->UninstallProtocolInterface(mPs5UsbMassStopHandle,&mPs5UsbMassStopGuid,NULL))) {
        mPs5UsbMassStopHandle=NULL;   /* r5.25.2: never a USB-MSD stop marker across the controller restart */
        DEBUG((DEBUG_ERROR,"PS5 USB-MSD: stop marker removed before usbpowercycle\n"));
      }
      mPs5ChosenTop=(mPs5Set.bootlist_n>0 && CycleWantId==mPs5Set.bootlist[0]);   /* r5.27.3 */
      if (mPs5ChosenTop) DEBUG((DEBUG_ERROR,"PS5 usbpowercycle: chosen disk %08x is the first boot-list entry - its controller stays running if it works\n",CycleWantId));
      mRecoverSilent=TRUE; Ps5UsbRecover(TRUE,Selected); mRecoverSilent=FALSE;
      mPs5ChosenTop=FALSE;
      DEBUG((DEBUG_ERROR,"PS5 usbpowercycle: ConnectAll after the cycle\n")); ConsoleSnapshot();   /* r5.28.3: log point */
      EfiBootManagerConnectAll();
      DEBUG((DEBUG_ERROR,"PS5 usbpowercycle: ConnectAll done\n")); ConsoleSnapshot();
      continue;
    }
    if (CycleHold) Selected=NULL;   /* r5.25.2: decided before the boot list (no "booting it" line per pass) */
#endif
    if (Selected!=NULL) {
      PS5_TS("boot-medium-found");
#ifdef PS5_NATIVE_SETUP
      Ps5FirmwareSelfUpdate(mPs5ConfigEsp?mPs5ConfigEsp:Selected);   /* r5.25.13: returns only if nothing newer to start */
#endif
#ifdef PS5_USB_MASS_QUIET
      /* r5 (16.11.): booting from the internal NVMe ESP - stop UsbMassStorageDxe from binding further USB disks
       * (flaky bridges/card readers ran INQUIRY timeouts for 40+ s each in the EDID window and before bootmgr). USB HID
       * (keyboard, power-button ConIn) is a different driver and unaffected; a USB fallback boot never sets the marker. */
      if (Selected==Internal) {
        EFI_STATUS MarkStatus=EFI_SUCCESS;
        if(mPs5UsbMassStopHandle==NULL)   /* r5.21: handle kept, so DEL/ESC can remove the marker again */
          MarkStatus=gBS->InstallProtocolInterface(&mPs5UsbMassStopHandle,&mPs5UsbMassStopGuid,EFI_NATIVE_INTERFACE,NULL);
        DEBUG((DEBUG_ERROR,"PS5 USB-MSD: internal ESP selected - no further USB mass-storage binding (%r)\n",MarkStatus));
      }
#endif
      DEBUG((DEBUG_ERROR,"PS5 boot select: %a (bootorder=%a usb-installed=%u usb-installer=%u)\n",
        Selected==Internal?"internal":(Selected==Pinned?"pinned-ESP":(Selected==UsbInstalled?"usb-installed":(Selected==UsbInstaller?"usb-installer":"usb"))),
        BootOrder?"usb":"internal",(UINT32)(UsbInstalled!=NULL),(UINT32)(UsbInstaller!=NULL)));
      DEBUG((DEBUG_INFO,"PS5 Windows boot: target=%a (internal=%u pinned=%u removable=%u)\n",
        Selected==Internal?"internal":(Selected==Pinned?"pinned-ESP":"removable/USB"),
        (UINT32)(Internal!=NULL),(UINT32)(Pinned!=NULL),(UINT32)(Removable!=NULL)));
#ifdef PS5_NATIVE_SETUP
      ConsoleSnapshot();
      PS5_TS("vbios-export-begin");
      EFI_STATUS VbiosStatus=Ps5ExportVbios(Selected);
      (VOID)Ps5ExportWifiFw(Selected);
      PS5_TS("vbios-export-end");
      DEBUG((DEBUG_ERROR,"PS5 native VBIOS export: %r; external ESP copy only\n",VbiosStatus));
      ConsoleSnapshot();
      PS5_TS("display-begin");
#ifdef PS5_NATIVE_DP_VIDEO
      if (!DisplayStarted) {   /* r5.16: refresh=max from the config ESP decides the mode before the display starts */
        struct ps5_settings Early; Ps5PeekSettings(mPs5ConfigEsp?mPs5ConfigEsp:Selected,&Early); mPs5RefreshMax=Early.refresh==1;
        mPs5Resolution=Early.resolution<PS5_SET_RES_COUNT?Early.resolution:0;   /* r5.27 */
      }
#endif
      if (!DisplayStarted) { Ps5StartDisplay(); DisplayStarted=TRUE; ps5_progress(PS5_PROG_DISPLAY);
#ifdef PS5_NATIVE_DP_VIDEO
        Ps5PublishDisplayInfo(mPs5ConfigEsp?mPs5ConfigEsp:Selected);   /* r5.27: for PSWDisplay in Windows */
#endif
      }
      PS5_TS("display-end");
      /* r5.28.0 (user 25.11.: "0x7B has to be fixed, many people reported it"; HW 25.11. fin1: after a Windows restart the
       * RTL9210B with the installed Windows was gone from the bus, the warm path power-cycled its port after 5 s, the disk
       * was back after 0.1 s and bootmgr loaded - and Windows then hung before its event log started, black screen; the
       * same Windows booted on every start where its bridge had not been power-cycled): a USB bridge answers UEFI's
       * Bulk-Only reads right after power-on but finishes its own start only after 40-50 s, and Windows' UAS driver gives
       * up on it in that window. A USB boot disk whose port was power-cycled in this start therefore gets 45 s from the
       * power-on before Windows starts. Starts without such a cycle (all normal starts and restarts) do not wait. */
      Ps5UsbSettle(Selected,"boot path");   /* r5.28.14: also when the port was cut as an empty one (bridge off the bus) */
      // Auto-create a Boot#### entry for EVERY medium with \EFI\Boot\bootx64.efi (USB installer +
      // internal NVMe/SSD) so the 30 s boot-manager menu lists them for manual selection.
      EfiBootManagerRefreshAllBootOption();
      Ps5RestoreUsbBootOptions();   /* r5.28.5 */
      Ps5PruneEmptyMedia();         /* r5.28.12 */
      /* r5.28.9: the r5.28.8 list of USB entries on the config ESP (usbdisks.bin) is gone again - every start into the
       * internal Windows hung on the blue screen once the file existed (HW 26.11., reproduced on a cold start) */
      Ps5PowerPresetLoad(mPs5ConfigEsp?mPs5ConfigEsp:Selected);   /* r5.7: \PS5UEFI.CFG -> PlatformLang, Ps5PowerPreset before the menu;
                                                       r5.15.2: always the internal config ESP, also when USB boots */
      /* r5.28.10 (user 26.11.: the fan spun up like a helicopter on the PSWloader screen, and with r5.28.7 at the Windows start):
       * the quiet table's servo target was PS5 Linux' 50 C fallback - at the 60-75 C after a restart or a long wait it drove the
       * fan up hard, wherever it went up (r5.28.7 only moved that to right before bootmgr). It now carries the preset's target
       * (72/70/66 C, ThermalManager's FanTargetC) and goes up here again, so the fan regulates gently from the settings on. */
      Ps5ApplyFanTarget("settings loaded",mPs5ConfigEsp?mPs5ConfigEsp:Selected);
      { /* r5.11 (16.11., telemetry plan step 1): read-only P-state MSRs of the BSP (AMD family 17h, architectural):
         * PStateCurLim C0010061, PStateCtl C0010062, PStateStat C0010063, PStateDef0..7 C0010064..6B, HwPstateStatus C0010293.
         * Basis for an ACPI _PSS with exactly the P-states the PS5 defines (Zen: MHz = FID*25*8/DID for enabled defs, bit 63). */
        UINT32 Eax=0; AsmCpuid(1,&Eax,NULL,NULL,NULL);
        if (((Eax >> 8) & 0xF) + ((Eax >> 20) & 0xFF) == 0x17) {
          DEBUG((DEBUG_ERROR,"PS5 msr: CurLim %Lx Ctl %Lx Stat %Lx HwStat %Lx\n",AsmReadMsr64(0xC0010061),AsmReadMsr64(0xC0010062),
            AsmReadMsr64(0xC0010063),AsmReadMsr64(0xC0010293)));
          for (UINT32 Ps=0; Ps<8; Ps++) {
            UINT64 D=AsmReadMsr64(0xC0010064+Ps); UINT32 Fid=(UINT32)(D & 0xFF), Did=(UINT32)((D >> 8) & 0x3F);
            DEBUG((DEBUG_ERROR,"PS5 msr: PStateDef%u %Lx en=%u fid=%u did=%u -> %u MHz\n",Ps,D,(UINT32)(D >> 63),Fid,Did,Did?Fid*200/Did:0));
          }
        }
      }
      { /* r5.7: seed the clock before the menu, so the setup shows a real time or "Time not set" (Ps5TimeValid) */
        EFI_STATUS Ts=SeedWindowsTime(Selected); UINT32 Tv=EFI_ERROR(Ts)?0:1;
        gRT->SetVariable(L"Ps5TimeValid",&mPs5PresetGuid,EFI_VARIABLE_BOOTSERVICE_ACCESS|EFI_VARIABLE_RUNTIME_ACCESS,sizeof(Tv),&Tv);
        DEBUG((DEBUG_ERROR,"PS5 time before menu: seed %r -> Ps5TimeValid=%u\n",Ts,Tv)); }
      mPs5BootSelected=Selected;                       /* r5.4: "Boot Windows" / idle timeout from the menu */
      mPs5MenuBootNow=Ps5BootNow;
      {
        STATIC EFI_HANDLE BootNowHandle=NULL;
        if (BootNowHandle==NULL) {
          EFI_STATUS BnStatus=gBS->InstallProtocolInterface(&BootNowHandle,&mPs5BootNowGuid,EFI_NATIVE_INTERFACE,&mPs5BootNowProtocol);
          DEBUG((DEBUG_ERROR,"PS5 boot menu: boot-now protocol %r\n",BnStatus));
        }
      }
#if defined(PS5_NATIVE_SETUP) && defined(PS5_NATIVE_DP_VIDEO)
      {   /* r5.20: BIOS "System check" for the Diagnostics tab */
        STATIC EFI_HANDLE DiagHandle=NULL;
        mPs5DiagFallbackEsp=Selected;
        if (DiagHandle==NULL) {
          EFI_STATUS DgStatus=gBS->InstallProtocolInterface(&DiagHandle,&mPs5DiagGuid,EFI_NATIVE_INTERFACE,&mPs5DiagProtocol);
          DEBUG((DEBUG_ERROR,"PS5 boot menu: system-check protocol %r\n",DgStatus));
        }
      }
#endif
#ifdef PS5_NATIVE_DP_VIDEO
      /* r5.16.5 (user request 16.11.): the Dell needs ~1-2 s after the mode start before it shows a picture, so
       * the 3 s countdown starts at the earliest 1.5 s after the mode start (only when the countdown is shown). */
      /* r5.17: the start screen is always shown now (fastboot only shortens it); the Dell at 3440x1440@100 (scrambled
       * TMDS + sink re-sync) needs clearly longer than 1.5 s, so 5 s lead above 340 MHz. */
      /* r5.25.12 (user): after a Windows restart the start screen is shown like at power-on (3 or 15 s); with
       * fastrestart=1 ("Fast restart" under Fast boot) it is skipped, and so is the monitor lead before it */
      BOOLEAN SkipScreen=mPs5WarmDirectHit && mPs5Set.fastrestart;
      if(mPs5FrameTsc0 && !SkipScreen) {
        UINT64 Lead=ps5_display_modes[mPs5ModeId].t.clock_khz>340000U?5ULL*PS5_FRAME_TSC_HZ:(3ULL*PS5_FRAME_TSC_HZ)/2;
        while(AsmReadTsc()-mPs5FrameTsc0<Lead)gBS->Stall(10000);
      }
#endif
      PS5_TS("setup-window-begin");
#ifndef PS5_NATIVE_DP_VIDEO
      BOOLEAN SkipScreen=mPs5WarmDirectHit && mPs5Set.fastrestart;
#endif
      if(mPs5WarmDirectHit) DEBUG((DEBUG_ERROR,"PS5 warm re-entry: start screen %a (fastrestart=%u)\n",SkipScreen?"skipped":"shown",mPs5Set.fastrestart));
      EFI_STATUS SetupStatus=SkipScreen?EFI_SUCCESS:Ps5NativeSetupWindow(NULL);   /* r5.25.12: was always skipped after a restart (r5.25.4) */
      PS5_TS("setup-window-end");
      if(EFI_ERROR(SetupStatus)) {
        DEBUG((DEBUG_ERROR,"PS5 native setup: prerequisites failed %r; stopping before OS boot\n",SetupStatus));
        ConsoleSnapshot();CpuDeadLoop();
        return SetupStatus;
      }
#endif
      { EFI_STATUS Ls=Ps5LaunchWindows(Selected);
        if (mPs5WarmDirectHit && EFI_ERROR(Ls)) Ps5WarmColdRestart("direct boot failed");
        return Ls; }
    }
    // USB root-hub enumeration and its deferred callbacks run while waiting.
    gBS->Stall(100000);
#ifdef PS5_NATIVE_SETUP
    if (Attempt+1<PS5_BOOT_MEDIUM_QUIET_ATTEMPTS) continue;
    if (!DisplayStarted) {
      DEBUG((DEBUG_ERROR,"PS5 Windows boot: no bootx64.efi medium after the first %u passes, starting the display anyway\n",
        PS5_BOOT_MEDIUM_QUIET_ATTEMPTS));
      ConsoleSnapshot();
      PS5_TS("display-begin-no-medium");
      Ps5StartDisplay(); DisplayStarted=TRUE;
      PS5_TS("display-end-no-medium");
    }
    if (CycleHold) continue;   /* r5.25.2: waiting after usbpowercycle - no second power cycle, no "no medium" lines */
    UINTN Late=Attempt+1-PS5_BOOT_MEDIUM_QUIET_ATTEMPTS;       /* passes since the display came up */
    if (!NoMediumMenuEntered && Late==100 && !(mPs5WarmDirect && mPs5WarmUsb)) {   /* r5.25.10: open the graphical UEFI menu even without any bootable medium; r5.25.23: not while a restart waits for its USB disk */
      NoMediumMenuEntered=TRUE;
      DEBUG((DEBUG_ERROR,"PS5 Windows boot: no bootx64.efi medium after 13 s - opening UEFI Boot Manager\n"));
      EfiBootManagerRefreshAllBootOption();
      Ps5PruneEmptyMedia();         /* r5.28.12 */
      Ps5PowerPresetLoad(mPs5ConfigEsp);
      Ps5ApplyFanTarget("no medium setup",mPs5ConfigEsp);   /* r5.28.10: back (preset target) */
      mPs5BootSelected=NULL;
      mPs5MenuBootNow=Ps5BootNow;
      { STATIC EFI_HANDLE NoMedBootNow=NULL;
        if (NoMedBootNow==NULL) gBS->InstallProtocolInterface(&NoMedBootNow,&mPs5BootNowGuid,EFI_NATIVE_INTERFACE,&mPs5BootNowProtocol); }
#if defined(PS5_NATIVE_DP_VIDEO)
      { STATIC EFI_HANDLE NoMedDiag=NULL; mPs5DiagFallbackEsp=NULL;
        if (NoMedDiag==NULL) gBS->InstallProtocolInterface(&NoMedDiag,&mPs5DiagGuid,EFI_NATIVE_INTERFACE,&mPs5DiagProtocol); }
#endif
      Ps5UsbMassAllow("no medium setup");
      Ps5NativeSetupEnter(NULL);
    }
    if (Late%50!=0) continue;
    // The console does not scroll: start a fresh page before the screen is full.
    /* r4 (15.11., dmesg 21:49): the RTL9210B bridge needs ~40-50 s after a cold start and then
     * re-attaches by itself; an early port power cycle restarts its NVMe init. So: wait patiently (ConnectAll every
     * 5 s) for PS5_BOOT_MEDIUM_PATIENT passes after the display, quietly; only then power-cycle as before. */
    BOOLEAN Patient=Late<PS5_BOOT_MEDIUM_PATIENT;
    UINTN Cyc=Patient?0:Late-PS5_BOOT_MEDIUM_PATIENT;
    if (!Patient && Cyc%250==0 && Cyc!=0 && gST->ConOut!=NULL) gST->ConOut->ClearScreen(gST->ConOut);
    DEBUG((DEBUG_ERROR,"PS5 Windows boot: no bootx64.efi medium, %u s after the display (filesystems=%u)%a\n",
      (UINT32)(Late/10),(UINT32)Count,Patient?" - waiting for the bridge, no power cycle yet":""));
    mRecoverQuiet=Patient;
    if(PS5_QUIET_SCREEN && mRecoverQuiet) PS5_SCREEN(L"[boot medium: none, %u s, %u filesystems]\r\n",(UINT32)(Late/10),(UINT32)Count);
    else Print(L"[boot medium: none, %u s, %u filesystems]\r\n",(UINT32)(Late/10),(UINT32)Count);
    // USB port power cycle at once, then again at 25 s, 50 s, 75 s and 100 s; in between only the port status.
    Ps5UsbRecover(!Patient && Cyc%250==0,NULL);
    mRecoverQuiet=FALSE;
    EfiBootManagerConnectAll();
#else
    if (Attempt+1>=PS5_BOOT_MEDIUM_QUIET_ATTEMPTS) break;
#endif
  }
  DEBUG((DEBUG_ERROR,"PS5 Windows boot: no bootx64.efi medium after bounded wait\n"));
  Print(L"No medium with \\EFI\\Boot\\bootx64.efi. Check the USB disk.\r\n");
  return EFI_NOT_FOUND;
}
