/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * ps5-diag-uefi.h v0.2 (17.11.2026, user: BIOS "System check"; v0.7 (18.11.2026): traffic light - memory/cores against the Windows 11
 *   minimums, no GOP = FAIL, no disk at all = FAIL (internal M.2 only SKIP when Windows is on USB), missing settings file = PASS,
 *   log always English and also published as the volatile variable Ps5DiagLog for the log viewer of the Diagnostics tab; v0.6 r5.20.7: PSWloader revision in the log, USB bridges without media; v0.5 r5.20.6: no FLAVA3 reads, Once only skips ICC when asked; v0.4 r5.20.5: Ps5DiagSetF EFIAPI (varargs); v0.3 r5.20.4: sub-step markers inside cpu/tctl/display (PS5_DIAG_SUB); v0.2 r5.20.3: Ps5DiagRunEx - progress line + log snapshot
 *   before every step, Once = no ICC at all (diag=once before bootmgr hung on the HW, cause not yet located)) - BDS part of the read-only compatibility check.
 * Include in Ps5BootWindows.c after Ps5PeekSettings()/mPs5ConfigEsp (needs ps5-native-dp-video.h, Ps5PresetRoot,
 * Ps5SetReadFile, Ps5SetCrc, Ps5SetDelete, Ps5HandleBehindUsb, mPs5Set). Only with PS5_NATIVE_SETUP + PS5_NATIVE_DP_VIDEO.
 * Every access is a read, through paths that are already in use: CPUID/MSR (as r5.11/r5.12), MP services, SMN Tctl through
 * the host bridge index/data pair 0x60/0x64 (index restored, as ps5pwr.sys and Linux k10temp), the HDMI video protocol
 * (DiagIcc = fan/temperature/FLAVA3 reads under mVideoBusy, GetEdid), DCN DENTIST 0x490 and CLK4 counters 0x6C9F4/0x6C9F8
 * through the recorded BAR5/PciIo, the UEFI memory map, GOP, BlockIo, PciIo config space, the settings file.
 * The only writes are \PS5DIAG.LOG on the config ESP and the volatile boot-services variable Ps5DiagLog (Ps5DiagSaveLog).
 * No SMU messages, no NBIO/RCC/BIF, no ICC SET. */
#ifndef PS5_DIAG_UEFI_H_
#define PS5_DIAG_UEFI_H_
#include "ps5-diag.h"
#include "Ps5Diag.h"   /* v0.7: PS5_DIAG_LOG_VARIABLE */
#include <Protocol/MpService.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/SimpleTextInEx.h>

typedef VOID (*PS5_DIAG_SUB_HOOK)(CONST CHAR8 *What);   /* v0.3: called before risky single accesses (diag=once only) */
STATIC PS5_DIAG_SUB_HOOK mPs5DiagSubHook;
#define PS5_DIAG_SUB(w) do { if(mPs5DiagSubHook)mPs5DiagSubHook(w); } while(0)
/* r5.20.5: EFIAPI is required - with GCC X64 the EDK2 VA_LIST is __builtin_ms_va_list and VA_START only works in an
 * ms_abi function. Without it every argument was read from the wrong registers (0 printed as 4294967295) and the first
 * %a dereferenced garbage: #PF, exception loop. This hung r5.20.1/.3/.4 with diag=once (and would hang the tab too). */
STATIC VOID EFIAPI Ps5DiagSetF(struct ps5_diag_report *R,unsigned Id,unsigned St,CONST CHAR8 *Fmt,...) {
  CHAR8 B[PS5_DIAG_DETAIL]; VA_LIST A;
  VA_START(A,Fmt); AsciiVSPrint(B,sizeof(B),Fmt,A); VA_END(A);
  ps5_diag_set(R,Id,St,B);
}
STATIC int Ps5DiagReg(UINT32 Off,UINT32 *V) {   /* DCN register read through the BAR5/PciIo recorded at the mode start */
  *V=0;
  if(mPs5FrameBar5){*V=MmioRead32((UINTN)(mPs5FrameBar5+Off));return 0;}
  if(mPs5FramePci)return VideoRead(mPs5FramePci,Off,V);
  return -1;
}
STATIC EFI_PCI_IO_PROTOCOL *Ps5DiagPci(UINTN WantBus,UINTN WantDev,UINTN WantFn,UINT16 WantVen,UINT16 WantId) {
  EFI_HANDLE *H=NULL;UINTN N=0,I;EFI_PCI_IO_PROTOCOL *Found=NULL;
  if(EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol,&gEfiPciIoProtocolGuid,NULL,&N,&H)))return NULL;
  for(I=0;I<N && !Found;I++){
    EFI_PCI_IO_PROTOCOL *P=NULL;UINTN Seg,Bus,Dev,Fn;UINT32 Id=0;
    if(EFI_ERROR(gBS->HandleProtocol(H[I],&gEfiPciIoProtocolGuid,(VOID **)&P))||!P)continue;
    if(WantVen){if(!EFI_ERROR(P->Pci.Read(P,EfiPciIoWidthUint32,0,1,&Id)) && (Id&0xFFFF)==WantVen && (Id>>16)==WantId)Found=P;continue;}
    if(!EFI_ERROR(P->GetLocation(P,&Seg,&Bus,&Dev,&Fn)) && Seg==0 && Bus==WantBus && Dev==WantDev && Fn==WantFn)Found=P;
  }
  FreePool(H);return Found;
}
STATIC VOID Ps5DiagCpu(struct ps5_diag_report *R) {
  UINT32 Eax=0,I,Count=0,P0=0;CHAR8 Name[49];CONST CHAR8 *N=Name;
  AsmCpuid(1,&Eax,NULL,NULL,NULL);
  unsigned St=ps5_diag_eval_apu(Eax);
  if(St!=PS5_DIAG_PASS){
    Ps5DiagSetF(R,PS5_DIAG_APU,St,"CPUID %08x: not AMD family 17h",Eax);
    ps5_diag_set(R,PS5_DIAG_PSTATES,PS5_DIAG_SKIP,"not family 17h");
  } else {
    PS5_DIAG_SUB("cpu: msr C0010030..35 name");
    for(I=0;I<6;I++){UINT64 Q=AsmReadMsr64(0xC0010030+I);CopyMem(Name+8*I,&Q,8);}   /* processor name string */
    PS5_DIAG_SUB("cpu: msr C0010064..6B pstates");
    Name[48]=0;while(*N==' ')N++;
    Ps5DiagSetF(R,PS5_DIAG_APU,PS5_DIAG_PASS,"%a (CPUID %08x)",N[0]?N:"unnamed",Eax);
    for(I=0;I<8;I++){UINT64 D=AsmReadMsr64(0xC0010064+I);UINT32 Fid=(UINT32)(D&0xFF),Did=(UINT32)((D>>8)&0x3F);
      if(D>>63){if(!Count)P0=Did?Fid*200/Did:0;Count++;}}
    Ps5DiagSetF(R,PS5_DIAG_PSTATES,Count>=2?PS5_DIAG_PASS:(Count?PS5_DIAG_WARN:PS5_DIAG_FAIL),"%u states, P0 %u MHz",Count,P0);
  }
  EFI_MP_SERVICES_PROTOCOL *Mp=NULL;UINTN Total=0,Enabled=0;
  PS5_DIAG_SUB("cpu: mp services");
  if(!EFI_ERROR(gBS->LocateProtocol(&gEfiMpServiceProtocolGuid,NULL,(VOID **)&Mp)) && Mp &&
     !EFI_ERROR(Mp->GetNumberOfProcessors(Mp,&Total,&Enabled)))
    Ps5DiagSetF(R,PS5_DIAG_CORES,ps5_diag_eval_cores((UINT32)Enabled),"%u of %u enabled (PS5: 16)",(UINT32)Enabled,(UINT32)Total);
  else ps5_diag_set(R,PS5_DIAG_CORES,PS5_DIAG_SKIP,"MP services not available");
}
STATIC VOID Ps5DiagTctl(struct ps5_diag_report *R) {
  EFI_PCI_IO_PROTOCOL *Hb=Ps5DiagPci(0,0,0,0,0);UINT32 Old=0,Raw=0,Idx=0x59800;   /* SMN THM_TCON_CUR_TMP */
  ps5_diag_set(R,PS5_DIAG_SMU,PS5_DIAG_SKIP,"not queried in firmware (checked by ps5pwr in Windows)");
  if(!Hb){ps5_diag_set(R,PS5_DIAG_TCTL,PS5_DIAG_SKIP,"host bridge 00:00.0 not found");return;}
  PS5_DIAG_SUB("tctl: smn 0x60/0x64");
  if(EFI_ERROR(Hb->Pci.Read(Hb,EfiPciIoWidthUint32,0x60,1,&Old)) || EFI_ERROR(Hb->Pci.Write(Hb,EfiPciIoWidthUint32,0x60,1,&Idx)) ||
     EFI_ERROR(Hb->Pci.Read(Hb,EfiPciIoWidthUint32,0x64,1,&Raw))){ps5_diag_set(R,PS5_DIAG_TCTL,PS5_DIAG_FAIL,"SMN read failed");}
  else {
    INT32 M=(INT32)((Raw>>21)*125u);if((Raw&(1u<<19))||((Raw&(3u<<16))==(3u<<16)))M-=49000;   /* ps5pwr_tctl_from_raw */
    Ps5DiagSetF(R,PS5_DIAG_TCTL,ps5_diag_eval_temp(M),"%d.%d C (raw %08x)",M/1000,(M>=0?M:-M)%1000/100,Raw);
  }
  (VOID)Hb->Pci.Write(Hb,EfiPciIoWidthUint32,0x60,1,&Old);   /* restore the index */
}
STATIC VOID Ps5DiagIcc(struct ps5_diag_report *R,PS5_HDMI_VIDEO_PROTOCOL *Video) {
  PS5_HDMI_DIAG_ICC D;EFI_STATUS S=EFI_UNSUPPORTED;
  if(Video && Video->DiagIcc)S=Video->DiagIcc(&D);
  if(EFI_ERROR(S)){
    unsigned St=S==EFI_ALREADY_STARTED?PS5_DIAG_WARN:PS5_DIAG_FAIL;
    Ps5DiagSetF(R,PS5_DIAG_ICC,St,"no ICC check (%r)",S);
    ps5_diag_set(R,PS5_DIAG_FAN,PS5_DIAG_SKIP,"no ICC");ps5_diag_set(R,PS5_DIAG_EMC_TEMP,PS5_DIAG_SKIP,"no ICC");
    ps5_diag_set(R,PS5_DIAG_BRIDGE,PS5_DIAG_SKIP,"no ICC");return;
  }
  BOOLEAN Any=!D.r_duty||!D.r_fan||!D.r_temp;   /* r5.20.6: no bridge reads */
  Ps5DiagSetF(R,PS5_DIAG_ICC,Any?PS5_DIAG_PASS:PS5_DIAG_FAIL,"%a (status duty %d, fan %d, temp %d; 0 = OK)",   /* r5.20.7 */
              Any?"answers":"no answer",D.r_duty,D.r_fan,D.r_temp);
  if(!D.r_duty && !D.r_fan)Ps5DiagSetF(R,PS5_DIAG_FAN,PS5_DIAG_PASS,"duty %u%% (%u/1024), target %u C",D.fan_duty*100/1024,D.fan_duty,D.fan_target_c);
  else Ps5DiagSetF(R,PS5_DIAG_FAN,PS5_DIAG_FAIL,"no answer (%d/%d)",D.r_duty,D.r_fan);
  if(!D.r_temp)Ps5DiagSetF(R,PS5_DIAG_EMC_TEMP,ps5_diag_eval_temp(D.temp_milli_c),"%d.%d C (MainSoC zone)",
                           D.temp_milli_c/1000,(D.temp_milli_c>=0?D.temp_milli_c:-D.temp_milli_c)%1000/100);
  else Ps5DiagSetF(R,PS5_DIAG_EMC_TEMP,PS5_DIAG_FAIL,"no answer (%d)",D.r_temp);
  ps5_diag_set(R,PS5_DIAG_BRIDGE,PS5_DIAG_SKIP,"not read in firmware");   /* r5.20.6 */
}
STATIC VOID Ps5DiagDisplay(struct ps5_diag_report *R,PS5_HDMI_VIDEO_PROTOCOL *Video) {
  STATIC UINT8 Edid[2048];UINTN Bytes=sizeof(Edid);UINT32 Flags=0,Updates=0,V=0;struct ps5_edid_info Info;BOOLEAN Have=FALSE;
  if(Video && Video->GetEdid && !EFI_ERROR(Video->GetEdid(Edid,&Bytes,&Flags,&Updates)) && Bytes>=128 &&
     ps5_edid_parse(Edid,(unsigned)Bytes,&Info)==0){
    UINT32 M=((UINT32)Edid[8]<<8)|Edid[9];Have=TRUE;
    Ps5DiagSetF(R,PS5_DIAG_EDID,PS5_DIAG_PASS,"%c%c%c %04x, %u bytes, preferred %ux%u",(CHAR8)('@'+((M>>10)&31)),
                (CHAR8)('@'+((M>>5)&31)),(CHAR8)('@'+(M&31)),(UINT32)Edid[10]|((UINT32)Edid[11]<<8),(UINT32)Bytes,
                Info.has_preferred?Info.dtd[0].h:0,Info.has_preferred?Info.dtd[0].v:0);
  } else ps5_diag_set(R,PS5_DIAG_EDID,PS5_DIAG_WARN,"none (display off, in standby or not answering at boot)");
  CONST struct ps5_mode_entry *Mode=&ps5_display_modes[mPs5ModeId];
  PS5_DIAG_SUB("display: dcn 0x14030 frame counter");
  Ps5DiagSetF(R,PS5_DIAG_MODE,(!Have && mPs5ModeId==PS5_MODE_1080P60)?PS5_DIAG_WARN:PS5_DIAG_PASS,"%a%a",Mode->name,
              (!Have && mPs5ModeId==PS5_MODE_1080P60)?" (fallback without display data)":"");
  if(mPs5FrameTsc0 && !Ps5DiagReg(0x14030,&V)){
    UINT64 Dt=AsmReadTsc()-mPs5FrameTsc0;UINT32 Df=((V&0xFFFFFF)-mPs5FrameCount0)&0xFFFFFF;
    if(Dt>=PS5_FRAME_TSC_HZ/4){
      UINT64 MilliHz=DivU64x64Remainder(MultU64x32((UINT64)Df,1000)*PS5_FRAME_TSC_HZ,Dt,NULL);
      Ps5DiagSetF(R,PS5_DIAG_REFRESH,ps5_diag_eval_refresh((UINT32)MilliHz,Mode->refresh_hz),"%u.%03u Hz (nominal %u)",
                  (UINT32)(MilliHz/1000),(UINT32)(MilliHz%1000),Mode->refresh_hz);
    } else ps5_diag_set(R,PS5_DIAG_REFRESH,PS5_DIAG_SKIP,"display started less than 0.25 s ago");
  } else ps5_diag_set(R,PS5_DIAG_REFRESH,PS5_DIAG_SKIP,"no display start recorded");
  UINT32 Dentist=0,Disp=0,Dpp=0;int Rd;
  PS5_DIAG_SUB("display: dccg 0x490 dentist");
  Rd=Ps5DiagReg(0x490,&Dentist);
  if(!Rd){PS5_DIAG_SUB("display: clk 0x6C9F4 dispclk counter");Rd=Ps5DiagReg(0x6C9F4,&Disp);}
  if(!Rd){PS5_DIAG_SUB("display: clk 0x6C9F8 dppclk counter");Rd=Ps5DiagReg(0x6C9F8,&Dpp);}
  if(!Rd){
    UINT32 T1=ps5_diag_dentist_khz(Dentist&0x7F),T2=ps5_diag_dentist_khz((Dentist>>24)&0x7F);
    Ps5DiagSetF(R,PS5_DIAG_DISPCLK,ps5_diag_eval_clock(Disp*100,T1),"%u.%u MHz (target %u.%u)",Disp/10,Disp%10,T1/1000,T1%1000/100);
    Ps5DiagSetF(R,PS5_DIAG_DPPCLK,ps5_diag_eval_clock(Dpp*100,T2),"%u.%u MHz (target %u.%u)",Dpp/10,Dpp%10,T2/1000,T2%1000/100);
  } else {ps5_diag_set(R,PS5_DIAG_DISPCLK,PS5_DIAG_SKIP,"display core not reachable");ps5_diag_set(R,PS5_DIAG_DPPCLK,PS5_DIAG_SKIP,"display core not reachable");}
}
STATIC VOID Ps5DiagMemory(struct ps5_diag_report *R) {
  UINTN Size=0,Key=0,DescSize=0;UINT32 Ver=0;EFI_MEMORY_DESCRIPTOR *Map=NULL;UINT64 Pages=0;
  EFI_STATUS S=gBS->GetMemoryMap(&Size,Map,&Key,&DescSize,&Ver);
  if(S==EFI_BUFFER_TOO_SMALL){Size+=4*DescSize;Map=AllocatePool(Size);if(Map)S=gBS->GetMemoryMap(&Size,Map,&Key,&DescSize,&Ver);}
  if(Map && !EFI_ERROR(S)){
    for(UINTN O=0;O+DescSize<=Size;O+=DescSize){EFI_MEMORY_DESCRIPTOR *D=(EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map+O);
      if(D->Type==EfiConventionalMemory||D->Type==EfiBootServicesCode||D->Type==EfiBootServicesData||
         D->Type==EfiLoaderCode||D->Type==EfiLoaderData)Pages+=D->NumberOfPages;}
    UINT32 Mib=(UINT32)(Pages/256);
    Ps5DiagSetF(R,PS5_DIAG_MEMORY,ps5_diag_eval_memory(Mib),"%u MiB usable (Windows 11 needs 4096)",Mib);
  } else Ps5DiagSetF(R,PS5_DIAG_MEMORY,PS5_DIAG_FAIL,"memory map not readable (%r)",S);
  if(Map)FreePool(Map);
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop=NULL;
  if(!EFI_ERROR(gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid,NULL,(VOID **)&Gop)) && Gop && Gop->Mode && Gop->Mode->Info){
    UINT64 Need=(UINT64)Gop->Mode->Info->HorizontalResolution*Gop->Mode->Info->VerticalResolution*4;
    Ps5DiagSetF(R,PS5_DIAG_VRAM,Gop->Mode->FrameBufferSize>=Need?PS5_DIAG_PASS:PS5_DIAG_WARN,"framebuffer %u MiB at 0x%lx for %ux%u",
                (UINT32)(Gop->Mode->FrameBufferSize>>20),Gop->Mode->FrameBufferBase,Gop->Mode->Info->HorizontalResolution,
                Gop->Mode->Info->VerticalResolution);
  } else ps5_diag_set(R,PS5_DIAG_VRAM,PS5_DIAG_FAIL,"no graphics output (GOP): no display for Windows");   /* v0.7: critical */
}
STATIC VOID Ps5DiagStorage(struct ps5_diag_report *R,EFI_HANDLE ConfigEsp) {
  EFI_HANDLE *H=NULL;UINTN N=0,I;UINT32 Internal=0,Usb=0,UsbNoMedia=0;UINT64 InternalMb=0;
  if(!EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol,&gEfiBlockIoProtocolGuid,NULL,&N,&H))){
    for(I=0;I<N;I++){EFI_BLOCK_IO_PROTOCOL *B=NULL;
      if(EFI_ERROR(gBS->HandleProtocol(H[I],&gEfiBlockIoProtocolGuid,(VOID **)&B))||!B||!B->Media||B->Media->LogicalPartition)continue;
      if(!B->Media->MediaPresent){if(Ps5HandleBehindUsb(H[I]))UsbNoMedia++;continue;}   /* r5.20.7: e.g. a USB bridge without a disk */
      if(Ps5HandleBehindUsb(H[I]))Usb++;
      else if(!B->Media->RemovableMedia){if(!Internal)InternalMb=MultU64x32(B->Media->LastBlock+1,B->Media->BlockSize)>>20;Internal++;}
    }
    FreePool(H);
  }
  if(Internal)Ps5DiagSetF(R,PS5_DIAG_M2,PS5_DIAG_PASS,"%u disk(s), first %Lu GB",Internal,InternalMb/1000);
  else if(Usb)ps5_diag_set(R,PS5_DIAG_M2,PS5_DIAG_SKIP,"none (Windows on a USB disk)");   /* v0.7: normal setup, not a warning */
  else ps5_diag_set(R,PS5_DIAG_M2,PS5_DIAG_FAIL,"no disk at all (internal or USB): nothing to boot");   /* v0.7: critical */
  if(UsbNoMedia)Ps5DiagSetF(R,PS5_DIAG_USB_DISK,PS5_DIAG_PASS,"%u USB disk(s), %u USB bridge(s) without media",Usb,UsbNoMedia);
  else Ps5DiagSetF(R,PS5_DIAG_USB_DISK,PS5_DIAG_PASS,"%u USB disk(s)",Usb);
  CHAR8 Buf[768];struct ps5_settings S;EFI_FILE_PROTOCOL *Root=ConfigEsp?Ps5PresetRoot(ConfigEsp):NULL;
  if(!Root){ps5_diag_set(R,PS5_DIAG_ESP_CFG,PS5_DIAG_WARN,"no config ESP (empty target)");return;}
  UINTN Got=Ps5SetReadFile(Root,L"\\PS5UEFI.CFG",Buf,sizeof(Buf));
  if(!Got)ps5_diag_set(R,PS5_DIAG_ESP_CFG,PS5_DIAG_PASS,"\\PS5UEFI.CFG not present, defaults used");   /* v0.7: first start, not a problem */
  else if(ps5_settings_parse(Buf,(unsigned)Got,Ps5SetCrc,&S))
    Ps5DiagSetF(R,PS5_DIAG_ESP_CFG,PS5_DIAG_PASS,"valid (preset %a, refresh %a)",ps5_set_preset[S.preset>2?1:S.preset],S.refresh?"max":"native");
  else ps5_diag_set(R,PS5_DIAG_ESP_CFG,PS5_DIAG_FAIL,"\\PS5UEFI.CFG damaged (CRC), defaults used");
  Root->Close(Root);
}
STATIC VOID Ps5DiagInput(struct ps5_diag_report *R) {
  EFI_HANDLE *H=NULL;UINTN N=0,I;UINT32 Kbd=0;
  if(!EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol,&gEfiSimpleTextInputExProtocolGuid,NULL,&N,&H))){
    for(I=0;I<N;I++)if(Ps5HandleBehindUsb(H[I]))Kbd++;
    FreePool(H);
  }
  if(Kbd)Ps5DiagSetF(R,PS5_DIAG_USB_INPUT,PS5_DIAG_PASS,"%u USB keyboard(s)",Kbd);
  else ps5_diag_set(R,PS5_DIAG_USB_INPUT,PS5_DIAG_WARN,"none (the power button still works in the menu)");
  PS5_DIAG_SUB("input: pci config scan");
  if(Ps5DiagPci(0,0,0,0x104D,0x9104))ps5_diag_set(R,PS5_DIAG_NETWORK,PS5_DIAG_PASS,"Ethernet 104D:9104 present (driver: ps5gbe)");
  else ps5_diag_set(R,PS5_DIAG_NETWORK,PS5_DIAG_WARN,"Ethernet 104D:9104 not found");
}
/* the whole check (the caller shows R itself) */
/* v0.2: Step is called before every step (progress in the UEFI log / on screen); Once skips the ICC step, so no step
 * waits for an answer or runs with interrupts off. */
typedef VOID (*PS5_DIAG_STEP)(UINT32 Index,UINT32 Count,CONST CHAR8 *Name);
STATIC VOID Ps5DiagRunEx(struct ps5_diag_report *R,BOOLEAN Once,PS5_DIAG_STEP Step) {
  PS5_HDMI_VIDEO_PROTOCOL *Video=NULL;
  if(EFI_ERROR(gBS->LocateProtocol(&mPs5HdmiVideoGuid,NULL,(VOID **)&Video)))Video=NULL;
  ps5_diag_init(R);
  if(Step)Step(1,7,"cpu");
  Ps5DiagCpu(R);
  if(Step)Step(1|0x80000000u,7,"cpu");
  if(Step)Step(2,7,"tctl");
  Ps5DiagTctl(R);
  if(Step)Step(2|0x80000000u,7,"tctl");
  if(Once){
    ps5_diag_set(R,PS5_DIAG_ICC,PS5_DIAG_SKIP,"not run before boot (diag=once)");
    ps5_diag_set(R,PS5_DIAG_FAN,PS5_DIAG_SKIP,"not run before boot (diag=once)");
    ps5_diag_set(R,PS5_DIAG_EMC_TEMP,PS5_DIAG_SKIP,"not run before boot (diag=once)");
    ps5_diag_set(R,PS5_DIAG_BRIDGE,PS5_DIAG_SKIP,"not run before boot (diag=once)");
  } else {
    if(Step)Step(3,7,"icc");
    Ps5DiagIcc(R,Video);
    if(Step)Step(3|0x80000000u,7,"icc");
  }
  if(Step)Step(4,7,"display");
  Ps5DiagDisplay(R,Video);
  if(Step)Step(4|0x80000000u,7,"display");
  if(Step)Step(5,7,"memory");
  Ps5DiagMemory(R);
  if(Step)Step(5|0x80000000u,7,"memory");
  if(Step)Step(6,7,"storage");
  Ps5DiagStorage(R,mPs5ConfigEsp);
  if(Step)Step(6|0x80000000u,7,"storage");
  if(Step)Step(7,7,"input");
  Ps5DiagInput(R);
  if(Step)Step(7|0x80000000u,7,"input");
  ps5_diag_tally(R);
  DEBUG((DEBUG_ERROR,"PS5 diag: %u PASS %u WARN %u FAIL %u SKIP\n",R->pass,R->warn,R->fail,R->skip));
}
STATIC VOID Ps5DiagRun(struct ps5_diag_report *R) { Ps5DiagRunEx(R,FALSE,NULL); }
STATIC UINTN Ps5DiagAppendPciInventory(CHAR8 *Buf,UINTN Cap,UINTN Len) {
  EFI_HANDLE *H=NULL;UINTN N=0,I;
  if(Len+64>=Cap)return Len;
  Len+=AsciiSPrint(Buf+Len,Cap-Len,"\r\n[PCI Inventory]\r\n");
  if(EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol,&gEfiPciIoProtocolGuid,NULL,&N,&H)))return Len;
  for(I=0;I<N&&Len+64<Cap;I++){
    EFI_PCI_IO_PROTOCOL *Io=NULL;UINTN Seg=0,Bus=0,Dev=0,Func=0;UINT32 Id=0,RevCls=0,Sub=0;
    if(EFI_ERROR(gBS->HandleProtocol(H[I],&gEfiPciIoProtocolGuid,(VOID **)&Io)))continue;
    if(EFI_ERROR(Io->GetLocation(Io,&Seg,&Bus,&Dev,&Func)))continue;
    if(EFI_ERROR(Io->Pci.Read(Io,EfiPciIoWidthUint32,0x00,1,&Id)))continue;
    (VOID)Io->Pci.Read(Io,EfiPciIoWidthUint32,0x08,1,&RevCls);
    (VOID)Io->Pci.Read(Io,EfiPciIoWidthUint32,0x2C,1,&Sub);
    Len+=AsciiSPrint(Buf+Len,Cap-Len,"  %02x:%02x.%x %04x:%04x sub=%04x:%04x rev=%02x cls=%02x/%02x\r\n",
      (UINT32)Bus,(UINT32)Dev,(UINT32)Func,(UINT32)(Id&0xFFFFu),(UINT32)(Id>>16),
      (UINT32)(Sub&0xFFFFu),(UINT32)(Sub>>16),(UINT32)(RevCls&0xFFu),
      (UINT32)((RevCls>>24)&0xFFu),(UINT32)((RevCls>>16)&0xFFu));
  }
  FreePool(H);
  return Len;
}
STATIC EFI_STATUS Ps5DiagWriteInDir(EFI_FILE_PROTOCOL *Root,CONST CHAR8 *Text,UINTN Len) {
  EFI_FILE_PROTOCOL *D1=NULL,*D2=NULL,*File=NULL;
  EFI_STATUS S=Root->Open(Root,&D1,L"PSWloader",EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,EFI_FILE_DIRECTORY);
  if(EFI_ERROR(S))return S;
  S=D1->Open(D1,&D2,L"logs",EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,EFI_FILE_DIRECTORY);
  D1->Close(D1);
  if(EFI_ERROR(S))return S;
  Ps5SetDelete(D2,L"PS5DIAG.LOG");
  S=D2->Open(D2,&File,L"PS5DIAG.LOG",EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,0);
  if(!EFI_ERROR(S)){
    UINTN W=Len;S=File->Write(File,&W,(VOID *)Text);
    EFI_STATUS F=File->Flush(File);File->Close(File);
    if(!EFI_ERROR(S))S=F;
  }
  D2->Close(D2);
  return S;
}
/* Save \PSWloader\logs\PS5DIAG.LOG: prefers any writable FAT32 USB drive (even with random data),
 * falls back to the Windows/config ESP (also writes \PS5DIAG.LOG there for backwards compatibility). */
STATIC EFI_STATUS Ps5DiagSaveLogEx(CONST struct ps5_diag_report *R,unsigned Lang,EFI_HANDLE Fallback,CHAR8 *SavedWhere,UINTN SavedWhereCap) {
  STATIC CHAR8 Text[PS5_DIAG_LOG_MAX];EFI_FILE_PROTOCOL *Root=NULL,*File=NULL;
  STATIC EFI_GUID LogGuid=PS5_DIAG_PROTOCOL_GUID;
  CHAR8 Fw[96];
  (VOID)Lang;
  if(SavedWhere&&SavedWhereCap)SavedWhere[0]='\0';
  AsciiSPrint(Fw,sizeof(Fw),"PSWloader v1.0.3 (%a), %s rev %x",PS5_FW_RELEASE,gST->FirmwareVendor?gST->FirmwareVendor:L"?",gST->FirmwareRevision);   /* r5.20.7 */
  UINTN Len=ps5_diag_format(R,0,Fw,Text,sizeof(Text));
  Len=Ps5DiagAppendPciInventory(Text,sizeof(Text),Len);
  EFI_STATUS V=gRT->SetVariable(PS5_DIAG_LOG_VARIABLE,&LogGuid,EFI_VARIABLE_BOOTSERVICE_ACCESS,Len,Text);   /* gone at ExitBootServices */
  DEBUG((DEBUG_ERROR,"PS5 diag: verdict %a, Ps5DiagLog %u bytes (%r)\n",ps5_diag_verdict_word(ps5_diag_verdict(R,NULL)),(UINT32)Len,V));
  (VOID)V;
  /* 1. Try any connected writable FAT32 USB drive first */
  EFI_HANDLE *H=NULL;UINTN N=0,I;
  if(!EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol,&gEfiSimpleFileSystemProtocolGuid,NULL,&N,&H))){
    for(I=0;I<N;I++){
      if(!Ps5HandleBehindUsb(H[I]))continue;
      Root=Ps5PresetRoot(H[I]);
      if(!Root)continue;
      EFI_STATUS Su=Ps5DiagWriteInDir(Root,Text,Len);
      Root->Close(Root);
      if(!EFI_ERROR(Su)){
        if(SavedWhere&&SavedWhereCap)AsciiSPrint(SavedWhere,SavedWhereCap,"USB:\\PSWloader\\logs\\PS5DIAG.LOG");
        DEBUG((DEBUG_ERROR,"PS5 diag: USB:\\PSWloader\\logs\\PS5DIAG.LOG %u bytes (OK)\n",(UINT32)Len));
        FreePool(H);
        return EFI_SUCCESS;
      }
    }
    FreePool(H);
  }
  /* 2. Fallback: config ESP / selected boot ESP */
  Root=Ps5PresetRoot(mPs5ConfigEsp?mPs5ConfigEsp:Fallback);
  if(!Root)return EFI_NOT_FOUND;
  EFI_STATUS S=Ps5DiagWriteInDir(Root,Text,Len);
  /* Also keep root \PS5DIAG.LOG on ESP for backwards compatibility */
  Ps5SetDelete(Root,L"\\PS5DIAG.LOG");
  if(!EFI_ERROR(Root->Open(Root,&File,L"\\PS5DIAG.LOG",EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,0))){
    UINTN W=Len;(VOID)File->Write(File,&W,Text);(VOID)File->Flush(File);File->Close(File);
  }
  Root->Close(Root);
  if(!EFI_ERROR(S)&&SavedWhere&&SavedWhereCap)AsciiSPrint(SavedWhere,SavedWhereCap,"ESP:\\PSWloader\\logs\\PS5DIAG.LOG");
  DEBUG((DEBUG_ERROR,"PS5 diag: ESP:\\PSWloader\\logs\\PS5DIAG.LOG %u bytes (%r)\n",(UINT32)Len,S));
  return S;
}
STATIC EFI_STATUS Ps5DiagSaveLog(CONST struct ps5_diag_report *R,unsigned Lang,EFI_HANDLE Fallback) {
  return Ps5DiagSaveLogEx(R,Lang,Fallback,NULL,0);
}
#endif
