#ifndef PS5_PAINT
#ifdef PS5_DIAG_GRAPHICS
#define PS5_PAINT 1   /* build-uefi.py --diag-graphics: boxes, stripes, test images, colour probes */
#else
#define PS5_PAINT 0   /* v3.30 quiet boot (default) */
#endif
#endif
/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Experimental real GPU test generator, not a simulated GOP or BIOS screen.
 */
#ifndef PS5_TS
#ifdef PS5_BOOT_TS   /* build-uefi.py --boot-ts (16.11.): boot timing, ms since TSC reset */
#define PS5_TS(Tag) DEBUG((DEBUG_ERROR,"PS5 TS %a %Lu ms\n",(CHAR8 *)(Tag),(UINT64)(AsmReadTsc()/1596300ULL)))
#else
#define PS5_TS(Tag)
#endif
#endif
#ifndef PS5_WARM_LOGCELL
#define PS5_WARM_LOGCELL 0x111806380ULL   /* r5.25.11: warm-run cell (see Ps5BootWindows.c) */
#endif
#ifndef PS5_MMHUB_SAVE
#define PS5_MMHUB_SAVE 0x111806400ULL   /* r5.25.11: hand-over MMHUB VM values, kept across a warm re-entry */
#endif
#include "dcn201-stream.h"
#include "dcn201-pipe-replay.h"
#include "ps5-hdmi-video-protocol.h"
#include <Library/IoLib.h>                 /* r5.14.1: MmioRead32 of the frame counter before bootmgr */
#include <IndustryStandard/Acpi.h>        /* r5.14.1: BAR5 descriptor */
/* ---- display-modes draft (UNTESTED on hardware) ------------------------------------------------
 * Boot mode from the sink EDID (ICC) within a fixed whitelist of PS5-Linux-proven FLAVA3 recipes
 * (ps5-display-mode.c). PS5_DISPLAY_POLICY 0 keeps today's fixed 1080p60 path. A mode is only
 * eligible if its DCN replay (generated from a PS5 Linux capture at that mode) is compiled in. */
#include "dcn201-mode-replay.h"
#include "dcn201-dispclk.h"
#ifndef PS5_DISPCLK_RAISE
#define PS5_DISPCLK_RAISE 0   /* 0: plan + log only (today's behaviour) | 1: raise DISPCLK/DPPCLK to the replay's DENTIST value */
#endif
#include "ps5-display-mode.h"
#ifndef PS5_DISPLAY_POLICY
#define PS5_DISPLAY_POLICY 0   /* 0: 1080p60 only (today) | 1: best mode <= 60 Hz | 2: best mode | 3: largest offered area (Stage B test) */
#endif
#ifdef PS5_REPLAY_1080P120
#include "dcn201-pipe-replay-1080p120.h"
#endif
#ifdef PS5_REPLAY_1440P60
#include "dcn201-pipe-replay-1440p60.h"
#endif
#ifdef PS5_REPLAY_2160P60
#include "dcn201-pipe-replay-2160p60.h"
#endif
#ifdef PS5_REPLAY_3440P60
#include "dcn201-pipe-replay-3440p60.h"
#endif
#ifdef PS5_REPLAY_3440P100
#include "dcn201-pipe-replay-3440p100.h"
#endif
#ifdef PS5_REPLAY_UGREEN1440P60
#include "dcn201-pipe-replay-ugreen1440p60.h"
#endif
#ifdef PS5_REPLAY_2560X1440P120   /* stage F: generic-dcn replay (synthetic from the Dell timing) */
#include "dcn201-pipe-replay-2560x1440p120.h"
#endif
#ifdef PS5_REPLAY_720P144         /* r5.13: generic-dcn replay (synthetic from the UGREEN 720p144 DTD) */
#include "dcn201-pipe-replay-720p144.h"
#endif
/* KMD layout (wddm.c WddmMemoryLayout, bc250kmd.h:128): the carve-out tail starts 32 MiB below its
 * end, 0x46E000000. The scanout surface at 0x46C000000 must end before it. */
#define PS5_SCANOUT_LIMIT 0x46E000000ULL
STATIC CONST struct ps5_dcn201_mode_replay *Ps5ModeReplay(enum ps5_mode_id Id) {
 switch(Id) {
 case PS5_MODE_1080P60: return &ps5_dcn201_replay_1080p60_fw;
#ifdef PS5_REPLAY_1080P120
 case PS5_MODE_1080P120: return &ps5_dcn201_replay_1080p120;
#endif
#ifdef PS5_REPLAY_1440P60
 case PS5_MODE_1440P60: return &ps5_dcn201_replay_1440p60;
#endif
#ifdef PS5_REPLAY_2160P60
 case PS5_MODE_2160P60: return &ps5_dcn201_replay_2160p60;
#endif
#ifdef PS5_REPLAY_3440P60
 case PS5_MODE_3440P60: return &ps5_dcn201_replay_3440p60;
#endif
#ifdef PS5_REPLAY_3440P100
 case PS5_MODE_3440P100: return &ps5_dcn201_replay_3440p100;
#endif
#ifdef PS5_REPLAY_UGREEN1440P60
 case PS5_MODE_UGREEN1440P60: return &ps5_dcn201_replay_ugreen1440p60;
#endif
#ifdef PS5_REPLAY_2560X1440P120
 case PS5_MODE_2560P120: return &ps5_dcn201_replay_2560x1440p120;
#endif
#ifdef PS5_REPLAY_720P144
 case PS5_MODE_720P144: return &ps5_dcn201_replay_720p144;
#endif
 default: return NULL;
 }
}
STATIC enum ps5_mode_id mPs5ModeId=PS5_MODE_1080P60;
#if 0   /* r5.16.6: relink off */
STATIC VOID *mPs5RelinkVideo;          /* r5.16.4: converter protocol of a scrambled (>340 MHz) mode start, for the relink */
STATIC BOOLEAN mPs5RelinkGeometry;
#endif
STATIC UINT32 mPs5RefreshMax;   /* r5.16: settings refresh=max (peeked by the BDS before the display starts), policy 2 only */
STATIC UINT32 mPs5Resolution;   /* r5.27: settings resolution= (ps5_set_res index, 0 = auto), peeked like refresh */
STATIC UINT32 mPs5ChoiceMask;   /* r5.27: bit (1u<<choice) for every resolution the monitor offers and the firmware can start */
STATIC UINT32 mPs5ResResult;    /* r5.27: 0 = automatic choice, 1 = requested resolution applied, 2 = requested but not available */
/* r5.14: frame counter at the end of the mode start (Ps5NativeDpVideo) for the refresh measurement before bootmgr */
STATIC int VideoRead(void *Context,uint32_t Offset,uint32_t *Value);
STATIC EFI_PCI_IO_PROTOCOL *mPs5FramePci;
STATIC UINT64 mPs5FrameBar5;           /* r5.14.1: BAR5 base for the read before bootmgr (PciIo may be stopped by then) */
STATIC UINT32 mPs5FrameCount0;
STATIC UINT64 mPs5FrameTsc0;
#define PS5_FRAME_TSC_HZ 1596300000ULL   /* the measured TSC rate of HdmiProbeDxe TSC_HZ / PS5_TS */
STATIC VOID Ps5FrameRateLog(CONST CHAR8 *When) {
 UINT32 C=0;
 if(!mPs5FramePci){DEBUG((DEBUG_ERROR,"PS5 display: refresh %a: no mode start recorded\n",When));return;}
 if(mPs5FrameBar5)C=MmioRead32((UINTN)(mPs5FrameBar5+0x14030));
 else if(VideoRead(mPs5FramePci,0x14030,&C)){DEBUG((DEBUG_ERROR,"PS5 display: refresh %a: frame counter read failed\n",When));return;}
 UINT64 Dt=AsmReadTsc()-mPs5FrameTsc0;UINT32 Df=((C&0xFFFFFF)-mPs5FrameCount0)&0xFFFFFF;
 if(Dt<PS5_FRAME_TSC_HZ/4){DEBUG((DEBUG_ERROR,"PS5 display: refresh %a: only %Lu ms since mode start, not measured\n",When,Dt/1596300ULL));return;}
 UINT64 MilliHz=DivU64x64Remainder(MultU64x32((UINT64)Df,1000)*PS5_FRAME_TSC_HZ,Dt,NULL);
 DEBUG((DEBUG_ERROR,"PS5 display: measured refresh %Lu.%03Lu Hz (%u frames in %Lu ms, %a) for %a\n",MilliHz/1000,MilliHz%1000,Df,
   Dt/1596300ULL,When,ps5_display_modes[mPs5ModeId].name));
}
STATIC VOID Ps5NativeDisplaySelect(VOID) {
 PS5_HDMI_VIDEO_PROTOCOL *Video=NULL;
 if(EFI_ERROR(gBS->LocateProtocol(&mPs5HdmiVideoGuid,NULL,(VOID **)&Video)) || !Video)Video=NULL;
 STATIC UINT8 Edid[2048];
 UINTN Bytes=sizeof(Edid);UINT32 Flags=0,Updates=0,Mask=0;
 struct ps5_edid_info Info;
 mPs5ModeId=PS5_MODE_1080P60;
 for(UINTN Id=0;Id<PS5_MODE_COUNT;Id++) {
  CONST struct ps5_dcn201_mode_replay *R=Ps5ModeReplay((enum ps5_mode_id)Id);
  CONST struct ps5_timing *T=&ps5_display_modes[Id].t;
  /* Only a replay that was captured at exactly this mode, and a surface that fits below the tail. */
  if(R && R->width==T->h && R->height==T->v && R->pitch_pixels>=T->h &&
     PS5_DCN201_REPLAY_SURFACE+(UINT64)R->pitch_pixels*R->height*4<=PS5_SCANOUT_LIMIT)Mask|=1U<<Id;
 }
 EFI_STATUS S=(PS5_DISPLAY_POLICY && Video && Video->GetEdid)?Video->GetEdid(Edid,&Bytes,&Flags,&Updates):EFI_UNSUPPORTED;
 int Parsed=EFI_ERROR(S)?-2:ps5_edid_parse(Edid,(unsigned)Bytes,&Info);
 if(Parsed==0)mPs5ModeId=PS5_DISPLAY_POLICY==3?ps5_display_select_largest(&Info,0,Mask):
                         (PS5_DISPLAY_POLICY==2 && mPs5RefreshMax)?ps5_display_select_refresh_max(&Info,Mask):   /* r5.21: native first */
                         ps5_display_select(&Info,PS5_DISPLAY_POLICY==1?60:0,Mask);
/* r5.27: resolution= from the settings wins when the monitor offers it and its replay is compiled in */
 mPs5ChoiceMask=Parsed==0?ps5_display_choice_mask(&Info,Mask):0;
 mPs5ResResult=0;
 if(mPs5Resolution) {
  enum ps5_mode_id F=mPs5ModeId;
  if(Parsed==0 && ps5_display_select_choice(&Info,mPs5Resolution,Mask,&F)){mPs5ModeId=F;mPs5ResResult=1;}
  else mPs5ResResult=2;
  DEBUG((DEBUG_ERROR,"PS5 display: resolution setting %u %a -> %a (choices %x)\n",mPs5Resolution,
    mPs5ResResult==1?"applied":"not available, automatic choice",ps5_display_modes[mPs5ModeId].name,mPs5ChoiceMask));
 }
#ifdef PS5_DISPLAY_FORCE
 /* r5.13 test build (build-uefi.py --display-force): this mode wins when the sink offers it exactly and its replay is
  * compiled in; otherwise the policy's choice stays. */
 if(Parsed==0 && (Mask&(1U<<PS5_DISPLAY_FORCE)) && ps5_display_offered(&Info,(enum ps5_mode_id)PS5_DISPLAY_FORCE))
  mPs5ModeId=(enum ps5_mode_id)PS5_DISPLAY_FORCE;
 DEBUG((DEBUG_ERROR,"PS5 display: force mode %u (%a): offered %d replay %u -> %a\n",(UINT32)PS5_DISPLAY_FORCE,
   ps5_display_modes[PS5_DISPLAY_FORCE].name,Parsed==0?ps5_display_offered(&Info,(enum ps5_mode_id)PS5_DISPLAY_FORCE):-1,
   (Mask>>PS5_DISPLAY_FORCE)&1U,ps5_display_modes[mPs5ModeId].name));
#endif
 DEBUG((DEBUG_ERROR,"PS5 display: policy=%u refresh=%a edid=%r bytes=%u flags=%x updates=%u parse=%d replay-mask=%x -> %a\n",
   (UINT32)PS5_DISPLAY_POLICY,mPs5RefreshMax?"max":"native",S,(UINT32)Bytes,Flags,Updates,Parsed,Mask,ps5_display_modes[mPs5ModeId].name));
 if(Parsed==0 && Info.has_preferred)
  DEBUG((DEBUG_ERROR,"PS5 display: sink preferred %ux%u %u kHz, %u DTDs, %u VICs, TMDS %u/%u kHz scdc %u\n",
    Info.dtd[0].h,Info.dtd[0].v,Info.dtd[0].clock_khz,Info.dtd_count,Info.vic_count,Info.max_tmds_khz,Info.hf_max_tmds_khz,Info.scdc));
}
/* ------------------------------------------------------------------------------------------------ */
STATIC int VideoRead(void *Context,uint32_t Offset,uint32_t *Value) {
 EFI_PCI_IO_PROTOCOL *Pci=Context;
 return EFI_ERROR(Pci->Mem.Read(Pci,EfiPciIoWidthUint32,5,Offset,1,Value))?-1:0;
}
STATIC int VideoWrite(void *Context,uint32_t Offset,uint32_t Value) {
 EFI_PCI_IO_PROTOCOL *Pci=Context;
 return EFI_ERROR(Pci->Mem.Write(Pci,EfiPciIoWidthUint32,5,Offset,1,&Value))?-1:0;
}
STATIC void VideoDelay(void *Context,unsigned Us) {(VOID)Context;gBS->Stall(Us);}
STATIC VOID VideoState(EFI_PCI_IO_PROTOCOL *Pci) {
 CONST UINT32 Offsets[]={0x490,0x500,0x504,0x508,0x580,0x584,0x13530,0x13e3c,0x14118,0x14004,0x14008,0x14028,0x13400,0x13404,0x13414,0x13450,0x13454,0x13458,0x1345c,0x13460,0x13464,0x13e2c,0x13e30,0x154a0,0x155bc,0x155c0,0x15730,0x15740,0x15744,0x15748,0x15724,0x15728,0x15830,0x15834,0x15838,0x1583c,0x177A0,0x17800,0x1780c,0x17814,0x17818,0x1782C,0x1401C,0x14020,0x13E28,0x154B8,0x15760,0x15720,0x15734,0x1575c,0x154a4,0x154a8,0x140a0,0x140b8,0x140bc,0x15848,0x15870,0x15754,0x15868,0x157dc,0x157fc,0x157e8,0x15864,0x15770,0x15684,0x15738};
 for(UINTN I=0;I<sizeof(Offsets)/sizeof(Offsets[0]);I++) {
  UINT32 Value=0;int R=VideoRead(Pci,Offsets[I],&Value);
  DEBUG((DEBUG_INFO,"PS5 DP video state: offset=%x value=%x access=%d\n",Offsets[I],Value,R));
 }
}
STATIC EFI_STATUS NativeVideoStage(PS5_HDMI_VIDEO_PROTOCOL *Video,UINTN Stage) {
 DEBUG((DEBUG_INFO,"PS5 DP video: converter stage=%u intent\n",Stage));
 Ps5GpuSnapshot();return Video->Stage(Stage);
}
STATIC VOID Ps5NativeDpCrcProbe(EFI_PCI_IO_PROTOCOL *Pci) {
 /* v2.33 diagnostic only: optc1_configure_crc full-frame window A/B,
  * continuous; DIG_OUTPUT_CRC_EN keeps inherited DATA_SEL. DPG colour
  * changes R/G/B/R localise pixel loss between OTG and DIG output. */
 CONST UINT32 Extra[]={0x15738,0x1574c,0x15774,0x15778,0x157ac,0x157b0,0x1584c,0x15868,0x154b0,0x154ac,0x154c4,0x1572c,0x13e28};
 for(UINTN I=0;I<sizeof(Extra)/sizeof(Extra[0]);I++) {
  UINT32 Value=0;int R=VideoRead(Pci,Extra[I],&Value);
  DEBUG((DEBUG_INFO,"PS5 DP crc extra: offset=%x value=%x access=%d\n",Extra[I],Value,R));
 }
 CONST UINT32 Setup[][2]={{0x140a0,0},{0x140a8,1920U<<16},{0x140ac,1080U<<16},
  {0x140b0,1920U<<16},{0x140b4,1080U<<16},{0x140a0,0x11}};
 for(UINTN I=0;I<6;I++)if(VideoWrite(Pci,Setup[I][0],Setup[I][1]))return;
 UINT32 Dig=0;if(VideoRead(Pci,0x154a4,&Dig) || VideoWrite(Pci,0x154a4,Dig|1))return;
 CONST UINT32 Colour[4][3]={{0xffc0ffc0,0,0},{0,0xffc0ffc0,0},{0,0,0xffc0ffc0},{0xffc0ffc0,0,0}};
 for(UINTN C=0;C<4;C++) {
  if(VideoWrite(Pci,0x1345c,Colour[C][0]) || VideoWrite(Pci,0x13460,Colour[C][1]) || VideoWrite(Pci,0x13464,Colour[C][2]))return;
  gBS->Stall(3000000);
  CONST UINT32 Probe[]={0x140a0,0x140b8,0x140bc,0x154a4,0x154a8,0x1401c,0x14020,0x154b8,0x13e28,0x15730};
  for(UINTN I=0;I<sizeof(Probe)/sizeof(Probe[0]);I++) {
   UINT32 Value=0;int R=VideoRead(Pci,Probe[I],&Value);
   DEBUG((DEBUG_INFO,"PS5 DP crc colour=%u offset=%x value=%x access=%d\n",C,Probe[I],Value,R));
  }
 }
 /* v2.42: OTG-inserted colour. OTG_BLANK_DATA_EN replaces pixels at the OTG
  * output with OTG_BLACK_COLOR (here full green). Visible green => path
  * OTG->DIG->DP->FLAVA3->HDMI carries colour; black => loss after OTG. */
 UINT32 BlackBefore=0,BlankBefore=0;
 if(VideoRead(Pci,0x14080,&BlackBefore) || VideoRead(Pci,0x14008,&BlankBefore))return;
 DEBUG((DEBUG_INFO,"PS5 DP otg blank colour before black=%x blank=%x\n",BlackBefore,BlankBefore));
 if(VideoWrite(Pci,0x14080,0x000ffc00) || VideoWrite(Pci,0x14008,BlankBefore|0x100))return;
 gBS->Stall(5000000);
 CONST UINT32 Blank[]={0x14008,0x14080,0x140b8,0x140bc,0x154b8,0x15730};
 for(UINTN I=0;I<sizeof(Blank)/sizeof(Blank[0]);I++) {
  UINT32 Value=0;int R=VideoRead(Pci,Blank[I],&Value);
  DEBUG((DEBUG_INFO,"PS5 DP otg blank green offset=%x value=%x access=%d\n",Blank[I],Value,R));
 }
 VideoWrite(Pci,0x14008,BlankBefore&~0x100U);VideoWrite(Pci,0x14080,BlackBefore);
}
#ifdef PS5_NATIVE_DP_SCANOUT
#include "ps5-native-gop.h"
/* aperture-helper begin (VRAM-size draft): MMHUB system aperture from MMMC_VM_FB_LOCATION_BASE/TOP (16 MiB units),
 * mmhub_v2_0_init_system_aperture_regs: low = fb_start>>18, high = fb_end>>18. Plausible = base 0xF400 (the loader's
 * FB_BASE), 512 MiB..4 GiB; anything else keeps the former 512 MiB constants. */
STATIC VOID Ps5MmhubAperture(UINT32 FbBase,UINT32 FbTop,UINT32 *Low,UINT32 *High) {
 FbBase&=0xffffff;FbTop&=0xffffff;
 *Low=0x3d0000;*High=0x3d07ff;
 if(FbBase==0xf400 && FbTop>=0xf41f && FbTop<=0xf4ff){*Low=FbBase<<6;*High=(FbTop<<6)|0x3f;}
}
/* aperture-helper end */
STATIC BOOLEAN Ps5NativeScanout(EFI_PCI_IO_PROTOCOL *Pci) {
 /* v2.44: memory scanout. Replay the PS5-Linux 1080p pipe state (HUBP/DPP
  * 0+3 split, MPCC0->3, OPP0) onto a CPU-written linear ARGB8888 surface at
  * the same carve-out physical address, then hand OPP0 from DPG to MPC. */
 CONST struct ps5_dcn201_mode_replay *R=Ps5ModeReplay(mPs5ModeId);
 if(!R)return FALSE;
 CONST UINT64 Fb=PS5_DCN201_REPLAY_SURFACE;CONST UINTN W=R->width,H=R->height,Pitch=R->pitch_pixels;
 EFI_STATUS Page=Ps5NativeFramebufferPageCheck(Fb,(UINT64)Pitch*H*4);
 DEBUG((DEBUG_INFO,"PS5 scanout: fb=%Lx page check=%r\n",Fb,Page));Ps5GpuSnapshot();
 if(EFI_ERROR(Page))return FALSE;
 UINT32 *P=(UINT32 *)(UINTN)Fb;
 CONST UINT32 Bars[4]={0xffff0000,0xff00ff00,0xff0000ff,0xffffffff};
 for(UINTN Y=0;Y<H;Y++)for(UINTN X=0;X<Pitch;X++) {
  UINT32 C=X<W?Bars[X*4/W]:0xff000000;
  if(X<W && (X<16 || Y<16 || X>=W-16 || Y>=H-16))C=0xffffffff;
  if(X>=W/2-200 && X<W/2+200 && Y>=H/2-100 && Y<H/2+100)C=0xff000000;
  if(!PS5_PAINT)C=0xff000000; /* v3.30: black start surface instead of the colour-bar test image */
  P[Y*Pitch+X]=C;
 }
 AsmWbinvd();
 DEBUG((DEBUG_ERROR,"PS5 scanout fb readback: %x %x %x %x %x\n",P[H/2*Pitch+W/8],P[H/2*Pitch+W*3/8],P[H/2*Pitch+W*5/8],P[H/2*Pitch+W*7/8],P[H/2*Pitch+W/2]));
 DEBUG((DEBUG_INFO,"PS5 scanout: %ux%u pitch %u written, caches flushed; replay=%u regs\n",(UINT32)W,(UINT32)H,(UINT32)Pitch,R->count));
 Ps5GpuSnapshot();
 /* v2.46: MMHUB non-VM access windows as Linux mmhub_v2_0 programs them
  * (L1 TLB system access mode, system aperture, default page, AGP off). */
 CONST UINT32 Mmhub[]={0x69a00,0x69a04,0x69a08,0x69a20,0x69a24,0x69a30,0x69a3c,0x69a40,0x69a60,0x69a84,0x69b00,0x69b04,0x69cac,0x69cb0,0x69d2c,0x69d30,0x69dac,0x69db0,0x6a15c,0x6a160,0x6a164,0x6a180,0x6a184,0x6a188,0x6a1b0,0x6a1b4,0x6a1b8,0x6a1bc,0x6a1c0,0x6a1c4,0x6a1c8,0x6a1cc};
 for(UINTN I=0;I<sizeof(Mmhub)/sizeof(Mmhub[0]);I++){UINT32 Value=0;int R=VideoRead(Pci,Mmhub[I],&Value);
  DEBUG((DEBUG_ERROR,"PS5 scanout mmhub native: offset=%x value=%x access=%d\n",Mmhub[I],Value,R));}   /* r5.25.11: ERROR level for the cold vs. warm comparison */
 {/* r5.25.11: MMHUB VM context 0/1 after a warm re-entry. GameOS hands the GPU over with the contexts as its own POST
   * left them, the scanout surface address then passes straight through. bc250kmd enables context 0 for its GART, so
   * after a restart the same surface address is translated through the old Windows page tables and the screen keeps
   * showing the last Windows frame (HW 22./23.11.: firmware screen and menu invisible after a restart). A cold boot saves
   * the hand-over values in the reserved arena (0x1_1180_6400, kept across the re-entry); a warm run writes them back
   * before the replay. Registers the firmware already reads above (proven readable after a restart). */
  STATIC CONST UINT32 Vm[8]={0x69b00,0x69b04,0x69cac,0x69cb0,0x69d2c,0x69d30,0x69dac,0x69db0};
  volatile UINT64 *Sv=(volatile UINT64 *)(UINTN)PS5_MMHUB_SAVE;   /* [0] "MMHUBVM1", [1..8] hand-over values */
  BOOLEAN WarmRun=*(volatile UINT64 *)(UINTN)PS5_WARM_LOGCELL==0x31474F4C4D524157ULL;
  UINT32 Cur[8];
  for(UINTN I=0;I<8;I++){Cur[I]=0;(void)VideoRead(Pci,Vm[I],&Cur[I]);}
  DEBUG((DEBUG_ERROR,"PS5 mmhub vm (%a): cntl %x %x pt %x:%x start %x:%x end %x:%x\n",WarmRun?"warm":"cold",
         Cur[0],Cur[1],Cur[3],Cur[2],Cur[5],Cur[4],Cur[7],Cur[6]));
  if(!WarmRun){Sv[0]=0x314D564255484D4DULL;for(UINTN I=0;I<8;I++)Sv[1+I]=Cur[I];}
  else if(Sv[0]==0x314D564255484D4DULL){
   BOOLEAN Diff=FALSE;
   for(UINTN I=0;I<8;I++)if(Cur[I]!=(UINT32)Sv[1+I])Diff=TRUE;
   if(Diff){
    for(UINTN I=0;I<8;I++)(void)VideoWrite(Pci,Vm[I],(UINT32)Sv[1+I]);   /* context controls first (index 0/1) */
    for(UINTN I=0;I<8;I++){Cur[I]=0;(void)VideoRead(Pci,Vm[I],&Cur[I]);}
    DEBUG((DEBUG_ERROR,"PS5 mmhub vm restored: cntl %x %x pt %x:%x start %x:%x end %x:%x\n",
           Cur[0],Cur[1],Cur[3],Cur[2],Cur[5],Cur[4],Cur[7],Cur[6]));
   }
  }}
 /* VRAM-size draft (14.11.2026, UNTESTED): the system aperture covers the WHOLE framebuffer, as
  * mmhub_v2_0_init_system_aperture_regs does (low = fb_start>>18, high = fb_end>>18; AGP is empty). The loader sets
  * MMMC_VM_FB_LOCATION_BASE/TOP from vram.txt (configure_vram), the KMD never reprograms the aperture, and its
  * rings, PSP TMR and GART table sit at the top of the carve-out. 512 MiB (FB 0xF400..0xF41F) gives exactly the
  * former constants 0x3d0000/0x3d07ff; an implausible read keeps them. */
 UINT32 FbBase=0,FbTop=0,ApLow,ApHigh;
 if(VideoRead(Pci,0x6a1b0,&FbBase) || VideoRead(Pci,0x6a1b4,&FbTop))FbBase=FbTop=0;
 Ps5MmhubAperture(FbBase,FbTop,&ApLow,&ApHigh);
 DEBUG((DEBUG_ERROR,"PS5 scanout: FB location %x..%x -> MMHUB system aperture %x..%x\n",FbBase,FbTop,ApLow,ApHigh));
 CONST UINT32 MmhubLinux[][2]={
  {0x6a1c0,0x0}, /* MMMC_VM_AGP_BASE (Linux) */
  {0x6a1bc,0xffffff}, /* MMMC_VM_AGP_BOT (Linux) */
  {0x6a1b8,0x0}, /* MMMC_VM_AGP_TOP (Linux) */
  {0x6a1c4,ApLow}, /* MMMC_VM_SYSTEM_APERTURE_LOW_ADDR (Linux: fb_start>>18) */
  {0x6a1c8,ApHigh}, /* MMMC_VM_SYSTEM_APERTURE_HIGH_ADDR (Linux: fb_end>>18) */
  {0x6a160,0x46feff}, /* MMMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_LSB (Linux) */
  {0x6a164,0x0}, /* MMMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_MSB (Linux) */
  {0x6a1cc,0x1d59}, /* MMMC_VM_MX_L1_TLB_CNTL (Linux) */
  /* v2.47: mmhub_v2_0_init_cache_regs - native GameOS state had L2 disabled. */
  {0x69a08,0x80130009}, /* MMVM_L2_CNTL3 (Linux) */
  {0x69a60,0x1}, /* MMVM_L2_CNTL4 (Linux) */
  {0x69a24,0x60000}, /* MMVM_L2_PROTECTION_FAULT_CNTL2 (Linux) */
  {0x69a00,0x80e01}, /* MMVM_L2_CNTL (Linux, ENABLE_L2_CACHE) */
 };
 for(UINTN I=0;I<sizeof(MmhubLinux)/sizeof(MmhubLinux[0]);I++)if(VideoWrite(Pci,MmhubLinux[I][0],MmhubLinux[I][1]))return FALSE;
 {UINT32 L=0,Hh=0;int E=VideoRead(Pci,0x6a1c4,&L)|VideoRead(Pci,0x6a1c8,&Hh);   /* VRAM-size log, READ ONLY */
  DEBUG((DEBUG_ERROR,"PS5 vram: MM sys aperture written %x..%x readback %x..%x (%d)\n",ApLow,ApHigh,L,Hh,E));}
 Ps5GpuSnapshot();
 /* r5.25.12 (HW 23.11. fr1: after a restart the start screen was drawn but the screen kept the last Windows frame): the
  * surface address is double-buffered and only taken at a flip. HW fr2: after a restart the pipes still read the Windows
  * surface (EARLIEST_INUSE 3:B2FC0000) after the replay, flip controls were clean (0 / 0x40); the replay's flip is not
  * taken while the pipe restarts. A second flip once the pipe runs switches both to the firmware surface (4:6C000000).
  * Cold boot: in use = firmware surface right after the replay, no second flip. */
 STATIC CONST UINT32 FlipCtl[2]={0xeb6c,0xf5bc},FlipCtl2[2]={0xeb70,0xf5c0},InUse[2]={0xeb94,0xf5e4},AddrLo[2]={0xeb28,0xf578};
 BOOLEAN WarmFlip=*(volatile UINT64 *)(UINTN)PS5_WARM_LOGCELL==0x31474F4C4D524157ULL;
 for(UINTN I=0;I<2;I++){UINT32 A=0,B=0,U=0,Uh=0;VideoRead(Pci,FlipCtl[I],&A);VideoRead(Pci,FlipCtl2[I],&B);VideoRead(Pci,InUse[I],&U);VideoRead(Pci,InUse[I]+4,&Uh);
  DEBUG((DEBUG_ERROR,"PS5 scanout flip (%a, before replay): hubp%u flip %x flip2 %x in use %x:%08x\n",WarmFlip?"warm":"cold",I?3:0,A,B,Uh,U));
  if(WarmFlip){VideoWrite(Pci,FlipCtl[I],0);VideoWrite(Pci,FlipCtl2[I],0x40);}}
 UINTN Failed=0;
 for(UINTN I=0;I<R->count;I++)
  if(VideoWrite(Pci,R->rows[I].offset,R->rows[I].value))Failed++;
 DEBUG((DEBUG_INFO,"PS5 scanout: replay failed=%u\n",(UINT32)Failed));Ps5GpuSnapshot();
 if(Failed)return FALSE;
 gBS->Stall(50000);
 /* Unblank both HUBPs (DCHUBP_CNTL.HUBP_BLANK_EN), then release OPP0 from DPG. */
 UINT32 V=0;
 for(UINTN I=0;I<R->hubp_count && I<4;I++)
  if(VideoRead(Pci,R->hubp_cntl[I],&V) || VideoWrite(Pci,R->hubp_cntl[I],V&~1U))return FALSE;
 gBS->Stall(50000);
 if(VideoRead(Pci,0x13450,&V) || VideoWrite(Pci,0x13450,V&~1U))return FALSE;
 DEBUG((DEBUG_INFO,"PS5 scanout: HUBP unblanked, DPG disabled\n"));Ps5GpuSnapshot();
 gBS->Stall(PS5_PAINT?3000000:100000);   /* fast boot (15.11.): the 3 s only fed the diagnostic state read below */
 for(UINTN Try=0;Try<2;Try++) {   /* r5.25.12: the address the pipes read now; after a restart a second flip if it is not ours */
  BOOLEAN Ours=TRUE;
  for(UINTN I=0;I<2;I++){UINT32 A=0,U=0,Uh=0;VideoRead(Pci,FlipCtl[I],&A);VideoRead(Pci,InUse[I],&U);VideoRead(Pci,InUse[I]+4,&Uh);
   DEBUG((DEBUG_ERROR,"PS5 scanout flip (%a, after replay %u): hubp%u flip %x in use %x:%08x\n",WarmFlip?"warm":"cold",(UINT32)Try,I?3:0,A,Uh,U));
   if(((UINT64)(Uh&0xFFFF)<<32|U)!=Fb)Ours=FALSE;}
  if(Ours || !WarmFlip || Try)break;
  for(UINTN I=0;I<2;I++){VideoWrite(Pci,FlipCtl[I],0);VideoWrite(Pci,FlipCtl2[I],0x40);VideoWrite(Pci,AddrLo[I]+4,(UINT32)(Fb>>32));VideoWrite(Pci,AddrLo[I],(UINT32)Fb);}
  gBS->Stall(50000);
 }
 CONST UINT32 Check[]={0xeacc,0xf51c,0x13450,0x140b8,0x140bc,0x14020,0x1401c,0x154b8,0x15730,0x157ac,0x13e28};
 for(UINTN I=0;I<sizeof(Check)/sizeof(Check[0]);I++) {
  UINT32 Value=0;int R=VideoRead(Pci,Check[I],&Value);
  DEBUG((DEBUG_ERROR,"PS5 scanout state: offset=%x value=%x access=%d\n",Check[I],Value,R));
 }
 Ps5GpuSnapshot(); Ps5GpuSnapshot();
 return TRUE;
}
#endif
STATIC VOID Sample60f9(PS5_HDMI_VIDEO_PROTOCOL *Video,UINTN Stage) {
 if(!Video->Read)return;
 UINT8 Value=0;EFI_STATUS ReadStatus=Video->Read(0x60f9,1,&Value);
 DEBUG((DEBUG_INFO,"PS5 DP video: 60f9 after stage=%u value=%x status=%r\n",Stage,Value,ReadStatus));
}
STATIC EFI_STATUS Ps5NativeDpArmTiming(EFI_PCI_IO_PROTOCOL *Pci,CONST UINT8 *Rom) {
 /* setpixelclock v1.7: DP_DTO=11, UNIPHY=0x1e, DP mode=0, CRTC0,
  * 148.5MHz. This command writes the real pixel DTO phase/modulo. */
 PS5_TS("dp-arm-timing");
 Ps5NativeDisplaySelect();   /* display-modes draft: the mode is fixed here, before the timing is armed */
 {/* Stage B: the firmware does not program DISPCLK (RISKS.md #5); log what it inherits (PS5 Linux at the UGREEN
   * 1440p60: DENTIST_DISPCLK_CNTL 0x51193131, CLK4_CLK2_CURRENT_CNT 6000). Read only. */
  UINT32 Dentist=0,Clk2=0;int R1=VideoRead(Pci,0x490,&Dentist),R2=VideoRead(Pci,0x6c9fc,&Clk2);
  DEBUG((DEBUG_ERROR,"PS5 display: DENTIST_DISPCLK_CNTL=%x (%d) CLK4_CLK2_CURRENT_CNT=%u (%d) mode %a\n",Dentist,R1,Clk2,R2,ps5_display_modes[mPs5ModeId].name));
  /* DISPCLK draft: the presumed CLK4 CLK0/CLK1 counters (DISPCLK/DPPCLK?) next to CLK2 - offsets UNVERIFIED, read only. */
  UINT32 Clk0=0,Clk1=0;int R3=VideoRead(Pci,0x6c9f4,&Clk0),R4=VideoRead(Pci,0x6c9f8,&Clk1);
  DEBUG((DEBUG_ERROR,"PS5 display: CLK4 cnt 6c9f4=%u (%d) 6c9f8=%u (%d) [unverified: CLK0/CLK1_CURRENT_CNT?]\n",Clk0,R3,Clk1,R4));}
 {/* VRAM-size log (14.11.2026), READ ONLY. BAR5 byte offsets = GC 10.1 base idx 0 (0x4980) / MMHUB 2.0 + reg*4,
   * the same numbers as KMD regs.generated.h: GCMC_VM_FB_LOCATION_BASE 0xA600 TOP 0xA604, AGP_TOP 0xA608 BOT 0xA60C
   * BASE 0xA610, SYSTEM_APERTURE_LOW 0xA614 HIGH 0xA618; MMMC_VM_FB_LOCATION_BASE 0x6A1B0 TOP 0x6A1B4,
   * MMMC_VM_SYSTEM_APERTURE_LOW 0x6A1C4 HIGH 0x6A1C8. Units: FB location 16 MiB (>>24), aperture 256 KiB (>>18). */
  CONST UINT32 Off[11]={0xa614,0xa618,0xa60c,0xa608,0xa610,0xa600,0xa604,0x6a1b0,0x6a1b4,0x6a1c4,0x6a1c8};
  UINT32 V[11];int E=0;
  for(UINTN I=0;I<11;I++){V[I]=0;E|=VideoRead(Pci,Off[I],&V[I]);}
  DEBUG((DEBUG_ERROR,"PS5 vram: GC sys aperture %x..%x AGP bot %x top %x base %x (%d)\n",V[0],V[1],V[2],V[3],V[4],E));
  DEBUG((DEBUG_ERROR,"PS5 vram: FB GC %x..%x MM %x..%x, MM sys aperture inherited %x..%x\n",V[5],V[6],V[7],V[8],V[9],V[10]));}
 CONST struct ps5_mode_entry *M=&ps5_display_modes[mPs5ModeId];
 CONST struct ps5_dcn201_mode_replay *R=Ps5ModeReplay(mPs5ModeId);
 if(!R)return EFI_UNSUPPORTED;
 {/* DISPCLK draft (UNTESTED): Linux raises DISPCLK/DPPCLK before it programs the pipes (dcn201_update_clocks in
   * prepare_bandwidth). The firmware so far inherits GameOS 0x64190064 (DID 0x64 = 83 MHz if taken literally).
   * PS5_DISPCLK_RAISE 0 only logs the plan. A refused or failed raise keeps the inherited clocks and the boot goes on. */
  struct ps5_dcn201_mmio ClkIo={Pci,VideoRead,VideoWrite,VideoDelay};struct ps5_dcn201_dispclk_result Clk;
  int Raise=ps5_dcn201_dispclk_raise(&ClkIo,R->dentist,PS5_DISPCLK_RAISE,&Clk);
  DEBUG((DEBUG_ERROR,"PS5 display: dispclk rc=%d (0 done/planned, 1 nothing to do, <0 refused) write=%u want=%x pll=%x before=%x after=%x disp=%u dpp=%u\n",
    Raise,(UINT32)PS5_DISPCLK_RAISE,R->dentist,Clk.pll_req,Clk.before,Clk.after,Clk.disp_written,Clk.dpp_written));
  if(Clk.dpp_cnt) { /* r5.16.6: the DPPCLK counter must follow the divider (0x6C9F8, 0.1 MHz; 4*VCO/divider = 120000/div) */
   UINT32 Div=ps5_dentist_divider((Clk.after>>24)&0x7F),Want=Div?120000U/Div:0;
   DEBUG((DEBUG_ERROR,"PS5 display: dppclk %u.%u MHz (want %u.%u, %u ladder steps)%a\n",Clk.dpp_cnt/10,Clk.dpp_cnt%10,Want/10,Want%10,
     Clk.dpp_written,Want && Clk.dpp_cnt*100U<Want*95U?" - BELOW 95%, pipe will underflow":""));
  }
  Ps5GpuSnapshot();
  /* r5.16.2 (review 16.11. 20:48): a failed raise must not arm a high pixel clock on the old DISPCLK.
   * Fall back to 1080p60 (VIC16), which ran on the inherited GameOS clocks before the raise existed. The bridge and
   * the scanout read mPs5ModeId later, so they follow. */
  if(PS5_DISPCLK_RAISE && Raise<0 && mPs5ModeId!=PS5_MODE_1080P60) {
   DEBUG((DEBUG_ERROR,"PS5 display: dispclk raise failed (rc=%d) for %a -> fallback %a\n",Raise,M->name,ps5_display_modes[PS5_MODE_1080P60].name));
   mPs5ModeId=PS5_MODE_1080P60;M=&ps5_display_modes[mPs5ModeId];R=Ps5ModeReplay(mPs5ModeId);
   if(!R)return EFI_UNSUPPORTED;
  }}
 UINT32 Clock[4]={M->t.clock_khz*10U,0x1e0b,0,0};   /* 100 Hz units: 1485000 for CEA16 as before */
 DEBUG((DEBUG_ERROR,"PS5 DP video: pixel clock intent %ux100Hz DP_DTO (%a)\n",Clock[0],M->name));
 Ps5GpuSnapshot();
 EFI_STATUS Status=Ps5AtomExecute(Pci,Rom,65536,12,Clock,4);
 if(EFI_ERROR(Status))return Status;
 struct ps5_dcn201_mmio Io={Pci,VideoRead,VideoWrite,VideoDelay};
 /* PS5 Linux requires HBR3 x4 for the internal bridge. Retain hardware
  * automatic symbol-clock routing; direct force experiment was unsuccessful. */
 UINT32 ReferenceHz=0;
 int Dto=ps5_dcn201_pixel_dto_hz(&Io,M->t.clock_khz*1000U,&ReferenceHz);
 DEBUG((DEBUG_INFO,"PS5 DP video: measured-reference DTO=%d referenceHz=%u\n",Dto,ReferenceHz));
 Ps5GpuSnapshot();if(Dto)return EFI_DEVICE_ERROR;
 /* DPG bypasses memory/DPP fetch. These bounded blanking-window test
  * values are experimental, not a DML calculation for framebuffer scanout. */
 const struct ps5_dcn201_global_sync Sync={R->sync[0],R->sync[1],R->sync[2],R->sync[3]}; /* 1080p: {43,2,3,4} as before */
 struct ps5_dcn201_register_write Otg[10];
 int Result=ps5_dcn201_otg_sync(&M->t,Otg);
 if(!Result)Result=ps5_dcn201_start_solid_mode(&Io,Otg,M->t.h,M->t.v,M->t.vt,ps5_vtg0_vcount_init(&M->t),R->vtg_fp2,
                                             &Sync,PS5_PAINT?1023:0,0,0); /* v3.30: black unless diag graphics */
 DEBUG((DEBUG_INFO,"PS5 DP video: real DPG red/timing start=%d\n",Result));
 Ps5GpuSnapshot();if(Result)return EFI_DEVICE_ERROR;
 UINT32 Before=0,After=0,Readback=0;
 if(VideoRead(Pci,0x154a0,&Before))return EFI_DEVICE_ERROR;
 After=(Before&~0x7U);
 if(VideoWrite(Pci,0x154a0,After))return EFI_DEVICE_ERROR;
 if(VideoRead(Pci,0x154a0,&Readback))return EFI_DEVICE_ERROR;
 DEBUG((DEBUG_INFO,"PS5 DP video: DIG_SOURCE_SELECT OTG0 before=%x target=%x readback=%x\n",Before,After,Readback));
 Ps5GpuSnapshot();
 if((Readback&0x7U)!=0)return EFI_DEVICE_ERROR;
 VideoState(Pci);return EFI_SUCCESS;
}
STATIC EFI_STATUS Ps5NativeDpReset(EFI_PCI_IO_PROTOCOL *Pci,CONST UINT8 *Rom,
                                  CONST struct ps5_dcn201_link *Link) {
 (VOID)Rom;
 if(Link->phy!=0)return EFI_UNSUPPORTED;
 PS5_HDMI_VIDEO_PROTOCOL *Video=NULL;
 EFI_STATUS Status=gBS->LocateProtocol(&mPs5HdmiVideoGuid,NULL,(VOID **)&Video);
 if(EFI_ERROR(Status) || !Video || !Video->Stage)return EFI_NOT_READY;
 /* Stop converter output and place its DP receiver in the existing ISR
  * initialization state (6008=c0) while preserving the inherited HBR3 PHY. */
 const UINTN Stages[]={6,7,8};
 for(UINTN I=0;I<3;I++){Status=NativeVideoStage(Video,Stages[I]);if(EFI_ERROR(Status))return Status;}
 UINT32 Enabled=0;
 if(VideoRead(Pci,0x155c0,&Enabled))return EFI_DEVICE_ERROR;
 DEBUG((DEBUG_INFO,"PS5 DP source: inherited PHY preserved readback=%x\n",Enabled));
 Ps5GpuSnapshot();return (Enabled&1)?EFI_SUCCESS:EFI_NOT_READY;
}
/* hotplug proposal (UNTESTED, 14.11.2026): publish the mode handed to HdmiProbeDxe as the volatile runtime variable
 * "Ps5HdmiMode" (ps5-hdmi-mode-var.h), so ps5pwr.sys can replay the FLAVA3 stages of exactly this mode after an
 * HDMI replug in Windows. Diagnostic only for the firmware: the result does not change the boot path. */
#include "ps5-hdmi-mode-var.h"
STATIC VOID Ps5PublishHdmiMode(enum ps5_mode_id Id,CONST struct ps5_flava3_mode *F) {
 STATIC EFI_GUID Guid=PS5_HDMI_MODE_VENDOR_GUID_INIT;
 CONST struct ps5_mode_entry *M=&ps5_display_modes[Id];
 struct ps5_hdmi_mode_var V;
 ps5_hdmi_mode_pack(&V,(uint32_t)Id,F,M->t.h,M->t.v,M->refresh_hz,M->t.clock_khz);
 EFI_STATUS S=gRT->SetVariable(PS5_HDMI_MODE_VAR_NAME,&Guid,EFI_VARIABLE_BOOTSERVICE_ACCESS|EFI_VARIABLE_RUNTIME_ACCESS,sizeof(V),&V);
 DEBUG((DEBUG_ERROR,"PS5 display: Ps5HdmiMode=%r mode=%u crc=%08x %a\n",S,(UINT32)Id,V.crc32,M->name));
}
/* r5.16.4 (16.11.2026): Dell 3440x1440@100 showed the picture for about 1 s, then the converter's pastel "no
 * video" colour. PS5 Linux (ps5-hdmi-ultrawide-native.patch): "Without HPD the converter is only reprogrammed on a
 * modeset. When the sink reconnects (reported by the HDMI converter as a new EDID), force one" (500 ms after the EDID,
 * at most once per 3 s). The firmware turns HDMI events off after the first EDID and never reprograms. So: before
 * bootmgr, at least 3 s after a scrambled mode start, replay the converter stages once with the DCN running - the
 * ps5pwr hotplug REPLAY_BOOT list 6,7,8,9,10,0..5,(14),11,12,13 (the SetMode stays in HdmiProbeDxe). */
#if 0   /* r5.16.6: relink off, kept for reference */
STATIC VOID Ps5HdmiRelink(VOID) {
 PS5_HDMI_VIDEO_PROTOCOL *Video=(PS5_HDMI_VIDEO_PROTOCOL *)mPs5RelinkVideo;
 if(!Video || !Video->Stage)return;
 while(mPs5FrameTsc0 && AsmReadTsc()-mPs5FrameTsc0<3ULL*PS5_FRAME_TSC_HZ)gBS->Stall(10000);
 CONST UINT16 R[]={0x705f,0x7203,0x7011,0x6083,0x7019};UINT8 B[5]={0},A[5]={0};
 if(Video->Read)for(UINTN I=0;I<5;I++)(VOID)Video->Read(R[I],1,&B[I]);
 CONST UINTN St[]={6,7,8,9,10,0,1,2,3,4,5,14,11,12,13};EFI_STATUS Status=EFI_SUCCESS;UINTN Done=0;
 for(UINTN I=0;I<sizeof(St)/sizeof(St[0]);I++) {
  if(St[I]==14 && !mPs5RelinkGeometry)continue;
  Status=Video->Stage(St[I]);if(EFI_ERROR(Status))break;Done++;
 }
 if(Video->Read)for(UINTN I=0;I<5;I++)(VOID)Video->Read(R[I],1,&A[I]);
 DEBUG((DEBUG_ERROR,"PS5 display: relink replay %r (%u stages) 705f %x->%x 7203 %x->%x 7011 %x->%x 6083 %x->%x 7019 %x->%x\n",
   Status,(UINT32)Done,B[0],A[0],B[1],A[1],B[2],A[2],B[3],A[3],B[4],A[4]));
 mPs5RelinkVideo=NULL;
}
#endif
STATIC EFI_STATUS Ps5NativeDpVideo(EFI_PCI_IO_PROTOCOL *Pci,CONST UINT8 *Rom,unsigned Phy) {
 (VOID)Rom;
 if(Phy!=0)return EFI_UNSUPPORTED; /* fixed DP0/DIGA/OTG0 routing */
 PS5_HDMI_VIDEO_PROTOCOL *Video=NULL;
 EFI_STATUS Status=gBS->LocateProtocol(&mPs5HdmiVideoGuid,NULL,(VOID **)&Video);
 if(EFI_ERROR(Status) || !Video || !Video->Stage)return EFI_NOT_READY;
 const UINTN Before[]={9,10};
 for(UINTN I=0;I<sizeof(Before)/sizeof(Before[0]);I++) {
  Status=NativeVideoStage(Video,Before[I]);if(EFI_ERROR(Status))return Status;
 }
 CONST struct ps5_mode_entry *M=&ps5_display_modes[mPs5ModeId];
 struct ps5_flava3_mode F;BOOLEAN Geometry=FALSE;
 if(mPs5ModeId!=PS5_MODE_1080P60) {
  struct ps5_flava3_mode_x X;
  if(!Video->SetMode || ps5_flava3_modex_for(mPs5ModeId,&X))return EFI_UNSUPPORTED;
  F=X.m;
  /* r5.13: the manual-timing extension only for PS5_BRIDGE_MANUAL; every other mode keeps the 30-byte SetMode. */
  Status=X.manual?Video->SetMode(&X,sizeof(X)):Video->SetMode(&F,sizeof(F));if(EFI_ERROR(Status))return Status;
  if(X.manual)DEBUG((DEBUG_ERROR,"PS5 display: manual FLAVA3 timing 7042=%02x%02x/%02x%02x/%02x%02x/%02x%02x 7063=%02x%02x.. 7078=%02x%02x\n",
    X.m7042[0],X.m7042[1],X.m7042[2],X.m7042[3],X.m7042[4],X.m7042[5],X.m7042[6],X.m7042[7],X.m7063[0],X.m7063[1],X.m7078[0],X.m7078[1]));
  Geometry=F.uw_geometry!=0;
 } else if(Video->SetMode)Video->SetMode(NULL,0);
 Ps5PublishHdmiMode(mPs5ModeId,mPs5ModeId!=PS5_MODE_1080P60?&F:NULL);   /* hotplug proposal */
 struct ps5_dcn201_mmio Io={Pci,VideoRead,VideoWrite,VideoDelay};
 uint32_t Msa[4];ps5_dp_msa(&M->t,Msa);
 int Result=ps5_dcn201_stream_setup_msa(&Io,Msa);
 DEBUG((DEBUG_INFO,"PS5 DP video: stream attributes=%d\n",Result));
 if(Result)return EFI_DEVICE_ERROR;
 UINT32 FeDomain=0;
 if(VideoRead(Pci,0x15724,&FeDomain))return EFI_DEVICE_ERROR;
 DEBUG((DEBUG_INFO,"PS5 DP video: FE domain 15724=%x\n",FeDomain));
 if((FeDomain&0x07000007U)!=0x01000000U) {
  DEBUG((DEBUG_INFO,"PS5 DP video: FE domain dead after transmitter ENABLE\n"));
  return EFI_DEVICE_ERROR;
 }
 UINT32 DigFe=0,Backend=0,Sym=0;
 if(VideoRead(Pci,0x154a0,&DigFe))return EFI_DEVICE_ERROR;
 if(VideoRead(Pci,0x155c0,&Backend))return EFI_DEVICE_ERROR;
 DEBUG((DEBUG_INFO,"PS5 DP video: DIG_FE_CNTL=%x backend=%x DIG_ENABLE=%x\n",DigFe,Backend,Backend));
 unsigned FeOn=0;
 for(UINTN I=0;I<1000;I++) {
  if(VideoRead(Pci,0x154a0,&DigFe))return EFI_DEVICE_ERROR;
  if(DigFe&0x01000000U){FeOn=1;break;}
  gBS->Stall(1);
 }
 if(VideoRead(Pci,0x155c0,&Backend))return EFI_DEVICE_ERROR;
 if(VideoRead(Pci,0x177A0,&Sym))return EFI_DEVICE_ERROR;
 DEBUG((DEBUG_INFO,"PS5 DP video: DIG_FE_CNTL=%x backend=%x 177A0=%x FE_ON=%u\n",DigFe,Backend,Sym,FeOn));
 VideoState(Pci);Ps5GpuSnapshot();
 /* The converter's basic-video command must see an actual DP video stream.
  * Do not wait until HDMI output setup before enabling its input. */
 Result=ps5_dcn201_stream_unblank_m(&Io,1,ps5_dp_vid_m(M->t.clock_khz));
 DEBUG((DEBUG_INFO,"PS5 DP video: unblank=%d; capture required, GOP not published\n",Result));
 if(!Result) {
  UINT32 Stream=0;
  for(UINTN I=0;I<40;I++) {
   if(VideoRead(Pci,0x15730,&Stream)){Result=-2;break;}
   if(Stream&0x10000)break;
   gBS->Stall(1000);
  }
  DEBUG((DEBUG_INFO,"PS5 DP video: stream control/status=%x\n",Stream));
  VideoState(Pci);
  if(!Result && !(Stream&0x10000))Result=-3;
 }
 /* r5.16.4 (16.11.2026): above 340 MHz (scrambled HDMI behind the FLAVA3) PS5 Linux sends a VSC SDP and the
  * Dell at 3440x1440@100 showed a single colour without it. Linux state (dcn-dump diff 16.11.): VSC in generic slots 0+1
  * (HB 00 07 05 13, DB16 RGB/sRGB, DB17 8 bpc), DP_MSA_MISC 0x40 (MISC1 bit 6 = colorimetry in VSC SDP), FMT dither +
  * dynamic expansion, DIG_FE_CNTL bit 8, TMDS_CTL_BITS 1, DP_SEC_CNTL GSP0 + stream enable (audio packets stay off).
  * Order as Linux enc1_update_generic_info_packet / enc1_stream_encoder_update_dp_info_packets. */
 if(!Result && M->t.clock_khz>340000U) {
  UINT32 Afmt=0,Vbi=0,Fe=0,Sec=0,Upd=0;int E=0;
  E|=VideoRead(Pci,0x15698,&Afmt);E|=VideoWrite(Pci,0x15698,Afmt|1U);              /* AFMT_AUDIO_CLOCK_EN */
  for(UINT32 Slot=0;Slot<2 && !E;Slot++) {
   E|=VideoRead(Pci,0x155AC,&Vbi);E|=VideoWrite(Pci,0x155AC,Vbi|(1U<<17));         /* AFMT_GENERIC_CONFLICT_CLR */
   E|=VideoRead(Pci,0x155AC,&Vbi);E|=VideoWrite(Pci,0x155AC,(Vbi&~(7U<<28))|(Slot<<28)); /* AFMT_GENERIC_INDEX */
   E|=VideoWrite(Pci,0x15530,0x13050700U);                                        /* HB0 00 HB1 07 HB2 05 HB3 13 */
   for(UINT32 I=0;I<8;I++)E|=VideoWrite(Pci,0x15534+4*I,I==4?0x00000100U:0);      /* DB17 0x01 = 8 bpc */
   E|=VideoRead(Pci,0x1569C,&Upd);E|=VideoWrite(Pci,0x1569C,Upd|(Slot?(1U<<6):(1U<<2))); /* GENERICn_IMMEDIATE_UPDATE */
  }
  E|=VideoWrite(Pci,0x15738,0x40U);                                                /* DP_MSA_MISC */
  E|=VideoWrite(Pci,0x13404,0x0060C900U);E|=VideoWrite(Pci,0x133FC,0x11U);          /* FMT bit depth / dynamic exp */
  E|=VideoRead(Pci,0x154A0,&Fe);E|=VideoWrite(Pci,0x154A0,Fe|0x100U);E|=VideoWrite(Pci,0x15668,1U); /* DIG_FE, TMDS_CTL */
  E|=VideoRead(Pci,0x157AC,&Sec);E|=VideoWrite(Pci,0x157AC,Sec|0x00100001U);       /* DP_SEC_CNTL GSP0 + stream */
  UINT32 SecA=0,Misc=0;(VOID)VideoRead(Pci,0x157AC,&SecA);(VOID)VideoRead(Pci,0x15738,&Misc);
  DEBUG((DEBUG_ERROR,"PS5 display: vsc-sdp %a: err=%d sec %x->%x msa_misc %x fe %x\n",M->name,E,Sec,SecA,Misc,Fe|0x100U));
 }
 Ps5GpuSnapshot();if(Result)return Result==-3?EFI_TIMEOUT:EFI_DEVICE_ERROR;
 { /* r5.14 (16.11., after r5.13.1 measured 144.0 Hz): refresh measurement in every mode start without waiting (r5.14.2: recorded right after the DP stream runs, so fast boot still leaves ~3 s until bootmgr) -
    * OTG0_OTG_STATUS_FRAME_COUNT and the TSC now, again right before bootmgr (Ps5FrameRateLog). */
   UINT32 C=0;
   VOID *Res=NULL;
   if(!VideoRead(Pci,0x14030,&C)){mPs5FramePci=Pci;mPs5FrameCount0=C&0xFFFFFF;mPs5FrameTsc0=AsmReadTsc();}
   else DEBUG((DEBUG_ERROR,"PS5 display: refresh: frame counter read at mode start failed\n"));
   if(!EFI_ERROR(Pci->GetBarAttributes(Pci,5,NULL,&Res)) && Res) {
    EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *D=(EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *)Res;
    if(D->Desc==ACPI_ADDRESS_SPACE_DESCRIPTOR && D->ResType==ACPI_ADDRESS_SPACE_TYPE_MEM && D->AddrLen>0x14034)mPs5FrameBar5=D->AddrRangeMin;
    FreePool(Res);
   }
 }

 Status=EFI_SUCCESS;
 PS5_TS("flava3-stages-begin");
 for(UINTN I=0;I<=5;I++){Status=NativeVideoStage(Video,I);Sample60f9(Video,I);if(EFI_ERROR(Status))return Status;}
 /* 3440x1440: geometry + VIFUPD after the automatic profile updates, before unmute (hdmi.c:994-995). */
 if(Geometry){Status=NativeVideoStage(Video,14);Sample60f9(Video,14);if(EFI_ERROR(Status))return Status;}
 Status=NativeVideoStage(Video,11);Sample60f9(Video,11);if(EFI_ERROR(Status))return Status;
 /* v2.41: Linux amdgpu_dm continues with sceHdmiSetAudioConfig(2) and audio unmute. */
 for(UINTN I=12;I<=13;I++){Status=NativeVideoStage(Video,I);Sample60f9(Video,I);if(EFI_ERROR(Status))return Status;}
 PS5_TS("flava3-stages-end");
 /* r5.16.6: the r5.16.4 converter relink is off - the 3440p100 single colour was the DPPCLK (dcn201-dispclk.h ladder) */
 DEBUG((DEBUG_INFO,"PS5 DP video: final state after converter output start\n"));
 VideoState(Pci);Ps5GpuSnapshot();
 if(Video->Read) {
  CONST UINT16 Registers[]={0x60f8,0x60f9,0x6083,0x10e7,0x7011,0x7019,0x7021,0x705f,0x7c00,0x6004,0x6008,0x6224,0x7005,0x7040,0x7203,0x7227};
  for(UINTN I=0;I<sizeof(Registers)/sizeof(Registers[0]);I++) {
   UINT8 Value=0;EFI_STATUS ReadStatus=Video->Read(Registers[I],1,&Value);
   DEBUG((DEBUG_INFO,"PS5 converter read: register=%x status=%r value=%x\n",Registers[I],ReadStatus,Value));
   if(!EFI_ERROR(ReadStatus) && Registers[I]==0x705f && Value!=0) {
    DEBUG((DEBUG_INFO,"PS5 DP video: converter mute stuck\n"));
    return EFI_DEVICE_ERROR;
   }
   if(EFI_ERROR(ReadStatus))break; /* Stop on first failed query; no replay. */
  }
  Ps5GpuSnapshot();
 }
 if(PS5_PAINT)Ps5NativeDpCrcProbe(Pci); /* v3.30: 17 s R/G/B/R + green OTG colour test only with diag graphics */
 Ps5GpuSnapshot();
#ifdef PS5_NATIVE_DP_SCANOUT
 PS5_TS("scanout-begin");
 if(Ps5NativeScanout(Pci)) {
  CONST struct ps5_dcn201_mode_replay *R=Ps5ModeReplay(mPs5ModeId);
  Ps5NativeGopInstallMode(PS5_DCN201_REPLAY_SURFACE,R->width,R->height,R->pitch_pixels);
 }
#endif
 return EFI_SUCCESS;
}
