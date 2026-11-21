/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * PS5 native GOP (v2.49) on the hardware-proven DCN201 scanout surface:
 * linear BGRX8888 1920x1080, pitch 1920 pixels, physical 0x46C000000
 * (HUBP0/3 -> DPP -> MPCC -> OPP0 -> OTG0 -> DP HBR3x4 -> FLAVA3 -> HDMI).
 * Only installed after Ps5NativeScanout reported success.
 */
#ifndef PS5_NATIVE_GOP_H
#define PS5_NATIVE_GOP_H
/* Public copy: the code for the earlier ntoskrnl patches, which a PS5_NT_ZERO_TOUCH=1 payload does not build,
 * has been removed. Removed lines are left empty, so __LINE__ in DEBUG/ASSERT and the payload bytes are the
 * same as in the hardware-tested build. The only change to Microsoft code left in the boot is the winload
 * sti patch (Ps5PatchStiBySig below).
 * Quiet boot (v3.30): diagnostic paints stay out of the picture unless PS5_DIAG_GRAPHICS is defined.
 *
 *
 *
 */
#ifndef PS5_PAINT
#ifdef PS5_DIAG_GRAPHICS
#define PS5_PAINT 1   /* build-uefi.py --diag-graphics: boxes, stripes, test images, colour probes */
#else
#define PS5_PAINT 0   /* v3.30 quiet boot (default) */
#endif
#endif









#include <Protocol/GraphicsOutput.h>
#include <Protocol/DevicePath.h>
#include <Library/DevicePathLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/BaseLib.h>
/* display-modes draft (UNTESTED): the GOP mode is the one the BDS lit (ps5-native-dp-video.h).
 * Defaults = the proven 1920x1080 surface; pitch may exceed the width (Linux linear pitch
 * alignment, e.g. 3456 px for 3440). Every stride below uses PS5_GOP_PITCH. */
STATIC UINT32 mPs5GopW=1920U,mPs5GopH=1080U,mPs5GopPitch=1920U;
#define PS5_GOP_W mPs5GopW
#define PS5_GOP_H mPs5GopH
#define PS5_GOP_PITCH mPs5GopPitch
typedef struct {
  VENDOR_DEVICE_PATH Vendor;
  EFI_DEVICE_PATH_PROTOCOL End;
} PS5_GOP_DEVICE_PATH;
STATIC EFI_GRAPHICS_OUTPUT_MODE_INFORMATION mPs5GopInfo;
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE mPs5GopMode;
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL mPs5Gop;
STATIC EFI_HANDLE mPs5GopHandle;
STATIC PS5_GOP_DEVICE_PATH mPs5GopPath={
  {{HARDWARE_DEVICE_PATH,HW_VENDOR_DP,{sizeof(VENDOR_DEVICE_PATH),0}},
   {0x5a4c2f2e,0x8d6b,0x4a1b,{0x9e,0x31,0x50,0x53,0x35,0x47,0x4f,0x50}}},
  {END_DEVICE_PATH_TYPE,END_ENTIRE_DEVICE_PATH_SUBTYPE,{sizeof(EFI_DEVICE_PATH_PROTOCOL),0}}
};
STATIC UINT32 *Ps5GopPixel(UINTN X,UINTN Y) {
  return (UINT32 *)(UINTN)mPs5GopMode.FrameBufferBase+Y*PS5_GOP_PITCH+X;
}
STATIC EFI_STATUS EFIAPI Ps5GopQueryMode(EFI_GRAPHICS_OUTPUT_PROTOCOL *This,UINT32 ModeNumber,
                                         UINTN *SizeOfInfo,EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info) {
  if(This==NULL || SizeOfInfo==NULL || Info==NULL || ModeNumber!=0)return EFI_INVALID_PARAMETER;
  *Info=AllocateCopyPool(sizeof(mPs5GopInfo),&mPs5GopInfo);
  if(*Info==NULL)return EFI_OUT_OF_RESOURCES;
  *SizeOfInfo=sizeof(mPs5GopInfo);
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Ps5GopBlt(EFI_GRAPHICS_OUTPUT_PROTOCOL *This,EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Buffer,
                                   EFI_GRAPHICS_OUTPUT_BLT_OPERATION Op,UINTN Sx,UINTN Sy,UINTN Dx,UINTN Dy,
                                   UINTN W,UINTN H,UINTN Delta) {
  if(This==NULL || Buffer==NULL || Op>=EfiGraphicsOutputBltOperationMax)return EFI_INVALID_PARAMETER;
  if(W==0 || H==0)return EFI_INVALID_PARAMETER;
  if(Delta==0)Delta=W*sizeof(EFI_GRAPHICS_OUTPUT_BLT_PIXEL);
  /* Bounds on the video side of every operation. */
  if(Op==EfiBltVideoToBltBuffer || Op==EfiBltVideoToVideo) {
    if(Sx>=PS5_GOP_W || Sy>=PS5_GOP_H || W>PS5_GOP_W-Sx || H>PS5_GOP_H-Sy)return EFI_INVALID_PARAMETER;
  }
  if(Op!=EfiBltVideoToBltBuffer) {
    if(Dx>=PS5_GOP_W || Dy>=PS5_GOP_H || W>PS5_GOP_W-Dx || H>PS5_GOP_H-Dy)return EFI_INVALID_PARAMETER;
  }
  EFI_TPL Old=gBS->RaiseTPL(TPL_NOTIFY);
  switch(Op) {
  case EfiBltVideoFill: {
    UINT32 C=*(UINT32 *)Buffer&0x00ffffffU;
    for(UINTN Y=0;Y<H;Y++)SetMem32(Ps5GopPixel(Dx,Dy+Y),W*4,C);
    break;}
  case EfiBltVideoToBltBuffer:
    for(UINTN Y=0;Y<H;Y++)CopyMem((UINT8 *)Buffer+(Dy+Y)*Delta+Dx*4,Ps5GopPixel(Sx,Sy+Y),W*4);
    break;
  case EfiBltBufferToVideo:
    for(UINTN Y=0;Y<H;Y++)CopyMem(Ps5GopPixel(Dx,Dy+Y),(UINT8 *)Buffer+(Sy+Y)*Delta+Sx*4,W*4);
    break;
  case EfiBltVideoToVideo:
    if(Dy<Sy)for(UINTN Y=0;Y<H;Y++)CopyMem(Ps5GopPixel(Dx,Dy+Y),Ps5GopPixel(Sx,Sy+Y),W*4);
    else for(UINTN Y=H;Y>0;Y--)CopyMem(Ps5GopPixel(Dx,Dy+Y-1),Ps5GopPixel(Sx,Sy+Y-1),W*4);
    break;
  default:break;
  }
  /* The display engine reads DRAM directly; push CPU writes out of the caches. */
  if(Op!=EfiBltVideoToBltBuffer)AsmWbinvd();
  gBS->RestoreTPL(Old);
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Ps5GopSetMode(EFI_GRAPHICS_OUTPUT_PROTOCOL *This,UINT32 ModeNumber) {
  if(This==NULL)return EFI_INVALID_PARAMETER;
  if(ModeNumber!=0)return EFI_UNSUPPORTED;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Black={0,0,0,0};
  return Ps5GopBlt(This,&Black,EfiBltVideoFill,0,0,0,0,PS5_GOP_W,PS5_GOP_H,0);
}
/* v2.59: visible ExitBootServices marker. The DCN scanout keeps running after
 * EBS; draw a magenta 96x96 square top-left using only CPU stores. */
STATIC EFI_EVENT mPs5GopEbsEvent;
STATIC VOID EFIAPI Ps5GopEbsMarker(EFI_EVENT Event,VOID *Context) {
  if(PS5_PAINT)for(UINTN Y=0;Y<96;Y++)for(UINTN X=0;X<96;X++)*Ps5GopPixel(X,Y)=0x00ff00ffU;
#if defined(PS5_WARM_EBS_STEPS)   /* lab: 12 before / 13 after the WBINVD */
  { volatile UINT64 *C=(volatile UINT64 *)(UINTN)0x111806300ULL; if(C[0]==0x3142465153575350ULL&&C[1]!=0){volatile UINT32 *F=(volatile UINT32 *)(UINTN)C[1];for(UINT32 Y=0;Y<48;Y++)for(UINT32 X=0;X<48;X++)F[Y*C[2]+320+12*60+X]=0x00FF00FFU;} }
#endif
  AsmWbinvd();
#if defined(PS5_WARM_EBS_STEPS)
  { volatile UINT64 *C=(volatile UINT64 *)(UINTN)0x111806300ULL; if(C[0]==0x3142465153575350ULL&&C[1]!=0){volatile UINT32 *F=(volatile UINT32 *)(UINTN)C[1];for(UINT32 Y=0;Y<48;Y++)for(UINT32 X=0;X<48;X++)F[Y*C[2]+320+13*60+X]=0x00FFD700U;AsmWbinvd();} }
#endif
}
/* v2.60: SetVirtualAddressMap marker (yellow, right of the EBS square).
 * Runs after EBS in physical mode: CPU stores + WBINVD only, no services. */
/* v2.62: late EBS marker (cyan, x=224) at TPL_CALLBACK - runs after every
 * higher-priority EBS handler; present => all EBS notifies completed. */
STATIC EFI_EVENT mPs5GopEbsLateEvent;






















STATIC UINT32 mPs5WlState; /* 0 none, 1 patched, 2 base missing, 3 bytes mismatch */
/* v3.31 had a version-independent winload base finder (fixed fast path 0x44DEC000, else a stack scan); v3.36 replaced
 * it after the scan was proven to hang on winload 9711 - see Ps5WinloadBase below. */
/* v3.35 DIAG (only --diag-graphics paints): localise the post-EBS death of v334/v338 on winload 9711. Their frames
 * froze with magenta+cyan only, i.e. Ps5GopEbsLateMarker died between its cyan paint and the box paints, inside one
 * of ProbeV268/Instrument/SwitchBack/KernelHandoff - all of which start with Ps5WinloadBase(). Layout (clear of the
 * logo, the winload stripes 300..548, RT boxes y=120..160 and the bugcheck rows >=900):
 *   y=620..716  96-px checkpoints in the late callback (white = reached), x = 0,112,224,336,448,560
 *   y=740..780  Ps5WinloadBase per call c (0..3), 40-px fields at x=16+48*(c*4+s):
 *               s0 white entered | s1 green fast-path hit / yellow miss | s2 white stack scan started |
 *               s3 green scan found / red scan found nothing
 *   y=800..816  first resolved base: bits 12..31, MSB left, 10-px cells (green=1, red=0) */
STATIC VOID Ps5DiagRect(UINTN X0,UINTN Y0,UINTN W,UINTN H,UINT32 C) {
  if(PS5_PAINT)for(UINTN Y=Y0;Y<Y0+H;Y++)for(UINTN X=X0;X<X0+W;X++)*Ps5GopPixel(X,Y)=C;
  if(PS5_PAINT)AsmWbinvd();
}
STATIC UINT32 mPs5WbCall;
STATIC VOID Ps5DiagWb(UINT32 C,UINTN S,UINT32 Color){if(C<4)Ps5DiagRect(16+48*(C*4+S),740,40,40,Color);}
STATIC VOID Ps5DiagBaseBits(UINT32 C,UINT64 B) {
  if(C!=0)return;
  for(UINTN I=0;I<20;I++)Ps5DiagRect(16+I*10,800,8,16,((B>>(31-I))&1)?0x0000ff00U:0x00ff0000U);
}
/* v3.36 BOOT FIX (version-independent winload base). HW proof v339 (08.11.): on winload 26100.9711 the image is no
 * longer at the logged 0x44dec000 (fast path missed, s1 yellow) and the v3.31 stack scan (32 KiB above SP, then 4 MiB
 * down from every qword in 1..1.5 GiB) never returned (s3 missing) -> Ps5GopEbsLateMarker hung post-EBS, which is the
 * whole 9711 boot regression; on 9444 the fast path hit, so the scan had never run before.
 * Now: winload itself calls gBS->ExitBootServices, so we wrap that service and keep the caller's return address - a
 * pointer that is guaranteed to lie inside winload's .text. The base is found by walking DOWN from that page only;
 * every page between it and the image base belongs to the loaded image, so no foreign memory is ever read. A base is
 * accepted only if MZ/PE/SizeOfImage fit AND the return address lies inside [base, base+SizeOfImage). No return
 * address -> 0 (instrumentation skipped) instead of guessing. Result cached (4 callers per EBS). */
STATIC EFI_EXIT_BOOT_SERVICES mPs5OrigExitBootServices;
STATIC volatile UINT64 mPs5EbsRet;
STATIC UINT64 mPs5WlBaseCache;
STATIC EFI_STATUS EFIAPI Ps5ExitBootServicesWrap(EFI_HANDLE ImageHandle,UINTN MapKey) {
  mPs5EbsRet=(UINT64)(UINTN)__builtin_return_address(0);
  return mPs5OrigExitBootServices(ImageHandle,MapKey);
}
STATIC VOID Ps5HookExitBootServices(VOID) {
  if(mPs5OrigExitBootServices!=NULL || gBS->ExitBootServices==Ps5ExitBootServicesWrap)return;
  EFI_TPL Tpl=gBS->RaiseTPL(TPL_HIGH_LEVEL);
  mPs5OrigExitBootServices=gBS->ExitBootServices;
  gBS->ExitBootServices=Ps5ExitBootServicesWrap;
  gBS->Hdr.CRC32=0;gBS->Hdr.CRC32=CalculateCrc32(gBS,gBS->Hdr.HeaderSize);
  gBS->RestoreTPL(Tpl);
}
STATIC BOOLEAN Ps5IsWinloadPeAt(UINT64 B,UINT64 Ret) {
  if(B<0x100000ULL || (B&0xfffULL))return FALSE;
  if(*(volatile UINT16 *)(UINTN)B!=0x5a4d)return FALSE;                 /* MZ */
  UINT32 Lfa=*(volatile UINT32 *)(UINTN)(B+0x3c);
  if(Lfa<0x40 || Lfa>0x400)return FALSE;
  if(*(volatile UINT32 *)(UINTN)(B+Lfa)!=0x00004550U)return FALSE;      /* 'PE\0\0' */
  UINT32 Soi=*(volatile UINT32 *)(UINTN)(B+Lfa+24+56);                  /* SizeOfImage */
  return Soi>=0x300000U && Soi<0x500000U && Ret>=B && Ret<B+Soi;        /* winload-sized and contains the caller */
}
STATIC UINT64 Ps5WinloadBase(VOID) {
  UINT32 C=mPs5WbCall++;
  Ps5DiagWb(C,0,0x00ffffffU);
  if(mPs5WlBaseCache){Ps5DiagWb(C,3,0x0000ff00U);return mPs5WlBaseCache;}
  UINT64 Ret=mPs5EbsRet;
  Ps5DiagWb(C,1,Ret?0x0000ff00U:0x00ffff00U);                          /* green: EBS caller captured */
  if(!Ret){Ps5DiagWb(C,3,0x00ff0000U);return 0;}
  Ps5DiagWb(C,2,0x00ffffffU);
  UINT64 Page=Ret&~0xfffULL;
  for(UINTN K=0;K<0x500 && (UINT64)K*0x1000ULL<=Page;K++) {           /* <= 5 MiB down = max SizeOfImage */
    UINT64 B=Page-(UINT64)K*0x1000ULL;
    if(Ps5IsWinloadPeAt(B,Ret)){mPs5WlBaseCache=B;Ps5DiagWb(C,3,0x0000ff00U);Ps5DiagBaseBits(C,B);return B;}
  }
  Ps5DiagWb(C,3,0x00ff0000U);
  return 0;
}
STATIC UINT32 Ps5WinloadSizeOfImage(UINT64 B) {
  UINT32 Lfa=*(volatile UINT32 *)(UINTN)(B+0x3c);
  return *(volatile UINT32 *)(UINTN)(B+Lfa+24+56);
}
/* v3.31: patch Archpx64EnableInterruptsAsm (sti;ret -> ret;ret) by its version-stable signature instead of a fixed
 * RVA (PS5_WL_STI). Signature validated unique in winload 26100.9444 and .9711 (winload-locate.c). Returns 1 patched,
 * 0 not found / ambiguous (then nothing is written). */
STATIC CONST UINT8 mPs5StiSig[12]={0x66,0x66,0x0f,0x1f,0x84,0x00,0x00,0x00,0x00,0x00,0xfb,0xc3};
STATIC UINT32 Ps5PatchStiBySig(UINT64 B) {
  UINT32 Soi=Ps5WinloadSizeOfImage(B);
  if(Soi<0x2000U || Soi>0x500000U)return 0;
  volatile UINT8 *T=(volatile UINT8 *)(UINTN)B; UINT64 Hit=0; UINTN N=0;
  for(UINT64 p=0x1000;p+sizeof(mPs5StiSig)<=Soi;p++) {
    UINTN j=0; for(;j<sizeof(mPs5StiSig);j++) if(T[p+j]!=mPs5StiSig[j])break;
    if(j==sizeof(mPs5StiSig)){Hit=p;N++;}
  }
  if(N!=1)return 0;                                                     /* unique only, else leave winload untouched */
  volatile UINT8 *Fb=(volatile UINT8 *)(UINTN)(B+Hit+10);              /* the fb byte */
  UINTN Cr0=AsmReadCr0();AsmWriteCr0(Cr0&~(UINTN)BIT16);
  Fb[0]=0xc3;AsmWriteCr0(Cr0);AsmWbinvd();
  return 1;
}






























































































/* v2.68: v2.67 proved EBS returns into winload (stripe at 0x1046f) but
 * 0x10477 is never reached: the stall is inside BlpArchSwitchContext back to
 * winload's context (cli, lgdt/lidt/lldt, far return, mov cr3, sti).
 * (a) the sti helper (RVA 0x2a9220 `fb c3`) becomes `ret`, so winload keeps
 *     IF=0 after EBS (it needs no interrupts any more; the kernel sets its own);
 * (b) winload's app CR3 (context RVA 0x354d80, CR3 at +8) is walked to see if
 *     the framebuffer is identity-mapped there - only then the stripe stubs
 *     after the switch are meaningful, otherwise the BME stub is used;
 * (c) LAPIC ISR/IRR are sampled for a pending interrupt. */
#define PS5_WL_STI 0x2a9220U
#define PS5_WL_APP_CTX 0x354d80U
STATIC UINT32 mPs5WlMapState;  /* 1 FB identity-mapped, 2 not mapped, 3 CR3 implausible */
STATIC UINT32 mPs5WlApicState; /* 1 ISR/IRR empty, 2 something pending */
STATIC UINT32 mPs5WlStiState;  /* 1 sti removed, 3 bytes mismatch */












STATIC VOID Ps5WinloadProbeV268(VOID) {
  UINT64 B=Ps5WinloadBase();
  if(!B){mPs5WlMapState=3;mPs5WlStiState=3;}
  else {
    /* v3.33 BOOT-GATE FIX: the app-context CR3 probe was pinned to the winload 9444 RVA
     * PS5_WL_APP_CTX (0x354d80 = ApplicationExecutionContext). On 9711 that RVA is a different
     * function (BlockIopReadBlockBuffer), so reading +8 yields a GARBAGE qword; a garbage value in
     * (0, 64 GiB) passed the loose guard and Ps5PtIdentity then walked a bogus page table, reading
     * WILD physical memory -> winload died inside this probe, BEFORE the kernel handoff. That is the
     * real 9711 boot gate (HW-localised by d2). mPs5WlMapState is cosmetic (a paint box only) and the
     * CR3-swap stubs use the firmware CR3 (AsmReadCr3), never this app CR3, so the unsafe probe is
     * dropped entirely - safe for every winload build. (Re-add later only behind a winload-build gate.) */
    mPs5WlMapState=3;   /* app-CR3 map-state not probed (version-independent, no wild walk) */
    /* v3.31: version-independent sti patch by signature (not PS5_WL_STI) - survives cumulative updates */
    mPs5WlStiState=Ps5PatchStiBySig(B)?1:3;
  }
  Ps5DiagRect(112,620,96,96,mPs5WlStiState==1?0x00ffffffU:0x00ffff00U); /* v3.35 CP1: sti step done (white patched, yellow not) */
  UINT64 ApicMsr=AsmReadMsr64(0x1B);UINT32 Any=0;
  for(UINTN I=0;I<8;I++) {
    if(ApicMsr&BIT10)Any|=(UINT32)AsmReadMsr64(0x810+I)|(UINT32)AsmReadMsr64(0x820+I);
    else {
      volatile UINT32 *A=(volatile UINT32 *)(UINTN)(ApicMsr&0xFFFFF000ULL);
      Any|=A[(0x100+0x10*I)/4]|A[(0x200+0x10*I)/4];
    }
  }
  mPs5WlApicState=Any?2:1;
}













































































































































































































































































































































































































































































































































































































































































































































































































































































STATIC UINT32 mPs5WlKxState; /* 0 not done, 1 handoff installed, 2 site not found/ambiguous */














































STATIC volatile BOOLEAN mPs5RtTrace;
STATIC VOID EFIAPI Ps5GopEbsLateMarker(EFI_EVENT Event,VOID *Context) {
  if(PS5_PAINT)for(UINTN Y=0;Y<96;Y++)for(UINTN X=224;X<320;X++)*Ps5GopPixel(X,Y)=0x0000ffffU;
  AsmWbinvd();
  mPs5RtTrace=TRUE; /* v2.63: runtime-service trace between EBS and SVAM */
  Ps5DiagRect(0,620,96,96,0x00ffffffU);   /* v3.35 CP0: callback body entered */
  Ps5WinloadProbeV268();
  Ps5DiagRect(224,620,96,96,0x00ffffffU); /* v3.35 CP2: ProbeV268 returned (CP1 x=112 is inside it) */


  mPs5WlState=0; mPs5WlKxState=0;
  DEBUG((DEBUG_INFO,"PS5 EBS late: ntoskrnl patches installed: 0 (winload: sti_patch=%u, stripe_hooks=0, switchback_hook=0, kx_handoff_hook=0)\n",mPs5WlStiState));








  /* v2.68 boxes: x=448 map (green mapped/red not/yellow bad CR3),
   * x=560 LAPIC (green empty/red pending), x=672 sti (green removed/yellow). */
  UINT32 Cm=mPs5WlMapState==1?0x0000ff00U:(mPs5WlMapState==2?0x00ff0000U:0x00ffff00U);
  UINT32 Ca=mPs5WlApicState==1?0x0000ff00U:0x00ff0000U;
  UINT32 Cs=mPs5WlStiState==1?0x0000ff00U:0x00ffff00U;
  if(PS5_PAINT)for(UINTN Y=0;Y<96;Y++)for(UINTN X=0;X<96;X++){*Ps5GopPixel(448+X,Y)=Cm;*Ps5GopPixel(560+X,Y)=Ca;*Ps5GopPixel(672+X,Y)=Cs;}
  /* State box x=336: green both armed, white BME only, red base missing,
   * blue CF8 unusable, yellow bytes mismatch. */
  UINT32 C=mPs5WlState==5?0x0000ff00U:mPs5WlState==1?0x00ffffffU:(mPs5WlState==2?0x00ff0000U:(mPs5WlState==4?0x000000ffU:0x00ffff00U));
  if(PS5_PAINT)for(UINTN Y=0;Y<96;Y++)for(UINTN X=336;X<432;X++)*Ps5GopPixel(X,Y)=C;
  /* v3.32 KX handoff box x=96: green installed, red site not found (version-independent kernel handoff). */
  UINT32 Ck=mPs5WlKxState==1?0x0000ff00U:(mPs5WlKxState==2?0x00ff0000U:0x00ffff00U);
  if(PS5_PAINT)for(UINTN Y=0;Y<96;Y++)for(UINTN X=96;X<192;X++)*Ps5GopPixel(X,Y)=Ck;
  AsmWbinvd();
}
__attribute__((unused)) STATIC EFI_EVENT mPs5GopVaEvent;
__attribute__((unused)) STATIC VOID EFIAPI Ps5GopVaMarker(EFI_EVENT Event,VOID *Context) {
  mPs5RtTrace=FALSE;
  if(PS5_PAINT)for(UINTN Y=0;Y<96;Y++)for(UINTN X=112;X<208;X++)*Ps5GopPixel(X,Y)=0x00ffff00U;
  AsmWbinvd();
}

/* v2.63: runtime-service trace. Each service owns a 40x40 box at y=120,
 * x=16+48*index: white on entry, green on return (only between the late EBS
 * marker and SetVirtualAddressMap, CPU stores only). A box that stays white
 * names the service that never returned. */
STATIC VOID Ps5RtMark(UINTN Index,UINT32 Color) {
  if(!mPs5RtTrace)return;
  if(PS5_PAINT)for(UINTN Y=120;Y<160;Y++)for(UINTN X=16+48*Index;X<56+48*Index;X++)*Ps5GopPixel(X,Y)=Color;
  AsmWbinvd();
}
#define PS5_RT_IN(i) Ps5RtMark(i,0x00ffffffU)
#define PS5_RT_OUT(i) Ps5RtMark(i,0x0000ff00U)
STATIC EFI_RUNTIME_SERVICES mPs5RtOrig;
STATIC EFI_STATUS EFIAPI Ps5RtGetTime(EFI_TIME *T,EFI_TIME_CAPABILITIES *C){PS5_RT_IN(0);EFI_STATUS S=mPs5RtOrig.GetTime(T,C);PS5_RT_OUT(0);return S;}
STATIC EFI_STATUS EFIAPI Ps5RtSetTime(EFI_TIME *T){PS5_RT_IN(1);EFI_STATUS S=mPs5RtOrig.SetTime(T);PS5_RT_OUT(1);return S;}
STATIC EFI_STATUS EFIAPI Ps5RtGetWakeupTime(BOOLEAN *E,BOOLEAN *P,EFI_TIME *T){PS5_RT_IN(2);EFI_STATUS S=mPs5RtOrig.GetWakeupTime(E,P,T);PS5_RT_OUT(2);return S;}
STATIC EFI_STATUS EFIAPI Ps5RtSetWakeupTime(BOOLEAN E,EFI_TIME *T){PS5_RT_IN(3);EFI_STATUS S=mPs5RtOrig.SetWakeupTime(E,T);PS5_RT_OUT(3);return S;}
STATIC EFI_STATUS EFIAPI Ps5RtConvertPointer(UINTN D,VOID **A){PS5_RT_IN(5);EFI_STATUS S=mPs5RtOrig.ConvertPointer(D,A);PS5_RT_OUT(5);return S;}
STATIC EFI_STATUS EFIAPI Ps5RtGetVariable(CHAR16 *N,EFI_GUID *G,UINT32 *A,UINTN *Z,VOID *D){PS5_RT_IN(6);EFI_STATUS S=mPs5RtOrig.GetVariable(N,G,A,Z,D);PS5_RT_OUT(6);return S;}
STATIC EFI_STATUS EFIAPI Ps5RtGetNextVariableName(UINTN *Z,CHAR16 *N,EFI_GUID *G){PS5_RT_IN(7);EFI_STATUS S=mPs5RtOrig.GetNextVariableName(Z,N,G);PS5_RT_OUT(7);return S;}
STATIC EFI_STATUS EFIAPI Ps5RtSetVariable(CHAR16 *N,EFI_GUID *G,UINT32 A,UINTN Z,VOID *D){PS5_RT_IN(8);EFI_STATUS S=mPs5RtOrig.SetVariable(N,G,A,Z,D);PS5_RT_OUT(8);return S;}
STATIC EFI_STATUS EFIAPI Ps5RtGetNextHighMono(UINT32 *H){PS5_RT_IN(9);EFI_STATUS S=mPs5RtOrig.GetNextHighMonotonicCount(H);PS5_RT_OUT(9);return S;}
STATIC VOID EFIAPI Ps5RtResetSystem(EFI_RESET_TYPE T,EFI_STATUS St,UINTN Z,VOID *D){PS5_RT_IN(10);mPs5RtOrig.ResetSystem(T,St,Z,D);}
STATIC EFI_STATUS EFIAPI Ps5RtUpdateCapsule(EFI_CAPSULE_HEADER **C,UINTN N,EFI_PHYSICAL_ADDRESS L){PS5_RT_IN(11);EFI_STATUS S=mPs5RtOrig.UpdateCapsule(C,N,L);PS5_RT_OUT(11);return S;}
STATIC EFI_STATUS EFIAPI Ps5RtQueryCapsule(EFI_CAPSULE_HEADER **C,UINTN N,UINT64 *M,EFI_RESET_TYPE *R){PS5_RT_IN(12);EFI_STATUS S=mPs5RtOrig.QueryCapsuleCapabilities(C,N,M,R);PS5_RT_OUT(12);return S;}
STATIC EFI_STATUS EFIAPI Ps5RtQueryVariableInfo(UINT32 A,UINT64 *M,UINT64 *R,UINT64 *X){PS5_RT_IN(13);EFI_STATUS S=mPs5RtOrig.QueryVariableInfo(A,M,R,X);PS5_RT_OUT(13);return S;}
STATIC EFI_STATUS EFIAPI Ps5RtSetVirtualAddressMap(UINTN Size,UINTN DescSize,UINT32 Ver,EFI_MEMORY_DESCRIPTOR *Map) {
  PS5_RT_IN(4);
  /* Restore every original entry (and the table CRC) before the real call so
   * the runtime driver converts genuine runtime pointers only. */
  EFI_RUNTIME_SERVICES *Rt=gRT;
  CopyMem((UINT8 *)Rt+sizeof(EFI_TABLE_HEADER),(UINT8 *)&mPs5RtOrig+sizeof(EFI_TABLE_HEADER),sizeof(EFI_RUNTIME_SERVICES)-sizeof(EFI_TABLE_HEADER));
  Rt->Hdr.CRC32=0;Rt->Hdr.CRC32=CalculateCrc32(Rt,Rt->Hdr.HeaderSize);
  return mPs5RtOrig.SetVirtualAddressMap(Size,DescSize,Ver,Map);
}
__attribute__((unused)) STATIC VOID Ps5RtTraceInstall(VOID) {
  EFI_RUNTIME_SERVICES *Rt=gRT;
  CopyMem(&mPs5RtOrig,Rt,sizeof(mPs5RtOrig));
  Rt->GetTime=Ps5RtGetTime;Rt->SetTime=Ps5RtSetTime;Rt->GetWakeupTime=Ps5RtGetWakeupTime;Rt->SetWakeupTime=Ps5RtSetWakeupTime;
  Rt->SetVirtualAddressMap=Ps5RtSetVirtualAddressMap;Rt->ConvertPointer=Ps5RtConvertPointer;
  Rt->GetVariable=Ps5RtGetVariable;Rt->GetNextVariableName=Ps5RtGetNextVariableName;Rt->SetVariable=Ps5RtSetVariable;
  Rt->GetNextHighMonotonicCount=Ps5RtGetNextHighMono;Rt->ResetSystem=Ps5RtResetSystem;
  Rt->UpdateCapsule=Ps5RtUpdateCapsule;Rt->QueryCapsuleCapabilities=Ps5RtQueryCapsule;Rt->QueryVariableInfo=Ps5RtQueryVariableInfo;
  Rt->Hdr.CRC32=0;gBS->CalculateCrc32(Rt,Rt->Hdr.HeaderSize,&Rt->Hdr.CRC32);
}
STATIC EFI_STATUS Ps5NativeGopInstall(UINT64 FrameBuffer) {
  if(mPs5GopHandle!=NULL)return EFI_ALREADY_STARTED;
  mPs5GopInfo.Version=0;
  mPs5GopInfo.HorizontalResolution=PS5_GOP_W;
  mPs5GopInfo.VerticalResolution=PS5_GOP_H;
  mPs5GopInfo.PixelFormat=PixelBlueGreenRedReserved8BitPerColor;
  mPs5GopInfo.PixelsPerScanLine=PS5_GOP_PITCH;
  mPs5GopMode.MaxMode=1;
  mPs5GopMode.Mode=0;
  mPs5GopMode.Info=&mPs5GopInfo;
  mPs5GopMode.SizeOfInfo=sizeof(mPs5GopInfo);
  mPs5GopMode.FrameBufferBase=FrameBuffer;
  mPs5GopMode.FrameBufferSize=(UINTN)PS5_GOP_PITCH*PS5_GOP_H*4;
  mPs5Gop.QueryMode=Ps5GopQueryMode;
  mPs5Gop.SetMode=Ps5GopSetMode;
  mPs5Gop.Blt=Ps5GopBlt;
  mPs5Gop.Mode=&mPs5GopMode;
  EFI_STATUS Status=gBS->InstallMultipleProtocolInterfaces(&mPs5GopHandle,
    &gEfiDevicePathProtocolGuid,&mPs5GopPath,&gEfiGraphicsOutputProtocolGuid,&mPs5Gop,NULL);
  DEBUG((DEBUG_ERROR,"PS5 GOP: install %r fb=%Lx %ux%u pitch %u\n",Status,FrameBuffer,PS5_GOP_W,PS5_GOP_H,PS5_GOP_PITCH));
  if(EFI_ERROR(Status)){mPs5GopHandle=NULL;return Status;}
  EFI_STATUS Connect=gBS->ConnectController(mPs5GopHandle,NULL,NULL,TRUE);
  DEBUG((DEBUG_INFO,"PS5 GOP: console connect %r ConOut=%p\n",Connect,gST->ConOut));
  /* v2.50: ConPlatform tags a text output as console only if its device path is
   * in ConOut (or hot-plug). Register the GOP path like a platform BDS does and
   * tag the GraphicsConsole output so ConSplitter takes it. */
  EFI_STATUS Var=EfiBootManagerUpdateConsoleVariable(ConOut,(EFI_DEVICE_PATH_PROTOCOL *)&mPs5GopPath,NULL);
  STATIC EFI_GUID ConsoleOutDevice={0xD3B36F2C,0xD551,0x11D4,{0x9A,0x46,0x00,0x90,0x27,0x3F,0xC1,0x4D}};
  VOID *Tag=NULL;EFI_STATUS Tagged=EFI_ALREADY_STARTED;
  if(EFI_ERROR(gBS->HandleProtocol(mPs5GopHandle,&ConsoleOutDevice,&Tag)))
    Tagged=gBS->InstallProtocolInterface(&mPs5GopHandle,&ConsoleOutDevice,EFI_NATIVE_INTERFACE,NULL);
  Connect=gBS->ConnectController(mPs5GopHandle,NULL,NULL,TRUE);
  DEBUG((DEBUG_INFO,"PS5 GOP: ConOut var %r tag %r reconnect %r\n",Var,Tagged,Connect));
  /* v2.51: adopt the PS5 power-button SimpleTextIn (HdmiProbeDxe) as ConIn. */
  STATIC struct {VENDOR_DEVICE_PATH V;EFI_DEVICE_PATH_PROTOCOL End;} ButtonPath={
    {{HARDWARE_DEVICE_PATH,HW_VENDOR_DP,{sizeof(VENDOR_DEVICE_PATH),0}},
     {0x5a4c2f2e,0x8d6b,0x4a1b,{0x9e,0x31,0x50,0x53,0x35,0x42,0x54,0x4e}}},
    {END_DEVICE_PATH_TYPE,END_ENTIRE_DEVICE_PATH_SUBTYPE,{sizeof(EFI_DEVICE_PATH_PROTOCOL),0}}};
  EFI_STATUS InVar=EfiBootManagerUpdateConsoleVariable(ConIn,(EFI_DEVICE_PATH_PROTOCOL *)&ButtonPath,NULL);
  EFI_HANDLE *In=NULL;UINTN InCount=0;
  if(!EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol,&gEfiSimpleTextInProtocolGuid,NULL,&InCount,&In))) {
    for(UINTN I=0;I<InCount;I++)gBS->ConnectController(In[I],NULL,NULL,TRUE);
    FreePool(In);
  }
  DEBUG((DEBUG_INFO,"PS5 GOP: ConIn var %r, text-in handles %u\n",InVar,(UINT32)InCount));
  EFI_STATUS Ebs=gBS->CreateEvent(EVT_SIGNAL_EXIT_BOOT_SERVICES,TPL_NOTIFY,Ps5GopEbsMarker,NULL,&mPs5GopEbsEvent);
  DEBUG((DEBUG_INFO,"PS5 GOP: EBS marker event %r\n",Ebs));
  EFI_STATUS Late=gBS->CreateEvent(EVT_SIGNAL_EXIT_BOOT_SERVICES,TPL_CALLBACK,Ps5GopEbsLateMarker,NULL,&mPs5GopEbsLateEvent);
  DEBUG((DEBUG_INFO,"PS5 GOP: late EBS marker event %r\n",Late));
  Ps5HookExitBootServices(); /* v3.36: EBS caller (winload) return address -> version-independent winload base */
  DEBUG((DEBUG_INFO,"PS5 GOP: ExitBootServices wrapped (orig %p)\n",mPs5OrigExitBootServices));
  DEBUG((DEBUG_INFO,"PS5 GOP: ntoskrnl patches installed: %u (PS5_NT_ZERO_TOUCH=%u IER=%u IOAPIC=%u ROUTING=%u BUGCHECK_PAINTER=%u; winload: sti_patch=enabled stripe_hooks=%u kx_handoff_hook=%u)\n",
    (UINT32)0,
    (UINT32)1,(UINT32)0,(UINT32)0,(UINT32)0,(UINT32)0,
    (UINT32)0,(UINT32)0));
  /* v2.70: no VA-change marker and no runtime-service trace any more. winload
   * calls SetVirtualAddressMap from its own context (icall 0x106e0) where only
   * runtime memory is mapped; boot-services code (this BDS library) and the
   * physical framebuffer are not, so both instruments faulted there. */
  if(gST->ConOut!=NULL) {
    gST->ConOut->SetAttribute(gST->ConOut,EFI_TEXT_ATTR(EFI_WHITE,EFI_BLUE));
    gST->ConOut->ClearScreen(gST->ConOut);
#if PS5_QUIET_SCREEN
    Print(L"%s\r\n%s\r\n",PS5_BRAND_TITLE,PS5_BRAND_VERSION);
    DEBUG((DEBUG_INFO,"PS5 native UEFI - GOP %ux%u online (power button: short = Down, hold 3 s = Enter)\n",PS5_GOP_W,PS5_GOP_H));
#else
    Print(L"PSWloader - GOP %ux%u online\r\nPower button: short press = Down, hold 3 s = Enter\r\n",PS5_GOP_W,PS5_GOP_H);
#endif
  }
  return EFI_SUCCESS;
}
/* display-modes draft (UNTESTED): install the GOP for the mode the BDS actually lit. */
STATIC EFI_STATUS Ps5NativeGopInstallMode(UINT64 FrameBuffer,UINT32 Width,UINT32 Height,UINT32 Pitch) {
  if(!Width || !Height || Pitch<Width || Width>0x3fff || Height>0x3fff)return EFI_INVALID_PARAMETER;
  mPs5GopW=Width;mPs5GopH=Height;mPs5GopPitch=Pitch;
  return Ps5NativeGopInstall(FrameBuffer);
}
#endif
