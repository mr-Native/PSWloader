/** SPDX-License-Identifier: BSD-2-Clause-Patent
 * Opt-in boot-services-only ICC diagnostic. Does not publish GOP/EDID protocols,
 * set GPU modes, alter BARs, or run after entry returns. Explicit preparation
 * build additionally configures the FLAVA3 converter; no GOP is published.
 */
#if defined(PS5_HDMI_EDID_REQUEST) && !defined(PS5_ICC_LINUX_NOTIFY)
#define PS5_ICC_LINUX_NOTIFY 1 /* edid2-6: matches the define build-uefi.py puts into this module's icc-power.c */
#endif
#include <PiDxe.h>
#ifndef PS5_QUIET_SCREEN
#define PS5_QUIET_SCREEN 1   /* PSWloader v1.0: button status only in the log */
#endif
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/IoLib.h>
#include <Library/PciExpressLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/PrintLib.h>
#include <Library/BaseMemoryLib.h>
#include "icc-hdmi.h"
#include "icc-edid.h"
#ifdef PS5_HDMI_PREPARE
#include "flava3-prepare.h"
#include "flava3-video.h"
#include "flava3-edid.h"
#include "ps5-hdmi-video-protocol.h"
#include "ps5-fan.h"            /* r5.14 */
#include "button-keys.h"
#include <Protocol/SimpleTextIn.h>
#include <Protocol/SimpleTextInEx.h>
#include <Protocol/DevicePath.h>
#endif
#ifdef PS5_HDMI_EDID_REQUEST
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/PartitionInfo.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include "ps5-edid-info.h"
#if defined(PS5_WARM_PROBE)
#include "ps5-warm-probe.h"
#endif
#endif
#define MAILBOX 0x85400000U
#define DOORBELL 0x85308000U
#define TSC_HZ 1596300000ULL
STATIC struct ps5_edid_state mEdid;
STATIC uint32_t ReadIcc(void *Context,unsigned Region,unsigned Offset,unsigned Width) {
  UINTN Address=(Region?DOORBELL:MAILBOX)+Offset;
  if(Width==1) return MmioRead8(Address);
  if(Width==2) return MmioRead16(Address);
  return MmioRead32(Address);
}
STATIC void WriteIcc(void *Context,unsigned Region,unsigned Offset,unsigned Width,uint32_t Value) {
  UINTN Address=(Region?DOORBELL:MAILBOX)+Offset;
  if(Width==1) MmioWrite8(Address,(UINT8)Value);
  else if(Width==2) MmioWrite16(Address,(UINT16)Value);
  else MmioWrite32(Address,Value);
}
STATIC uint64_t Ticks(void *Context) { return AsmReadTsc(); }
STATIC void Relax(void *Context) { CpuPause(); }
STATIC BOOLEAN UncachedMmio(UINTN Address) {
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR D;
  EFI_STATUS Status=gDS->GetMemorySpaceDescriptor(Address,&D);
  return !EFI_ERROR(Status) && D.GcdMemoryType==EfiGcdMemoryTypeMemoryMappedIo &&
    D.BaseAddress<=Address && D.Length>=EFI_PAGE_SIZE &&
    Address-D.BaseAddress<=D.Length-EFI_PAGE_SIZE &&
    (D.Attributes&(EFI_MEMORY_UC|EFI_MEMORY_WC|EFI_MEMORY_WT|EFI_MEMORY_WB|EFI_MEMORY_UCE))==EFI_MEMORY_UC;
}
#ifdef PS5_HDMI_PREPARE
STATIC struct ps5_hdmi_config mVideoConfig;
STATIC BOOLEAN mVideoBusy;
/* display-modes draft (UNTESTED): FLAVA3 parameters chosen by the BDS; NULL = VIC16 as before. */
STATIC struct ps5_flava3_mode_x mVideoModeX;          /* r5.13: + manual timing extension */
#define mVideoMode (mVideoModeX.m)
STATIC CONST struct ps5_flava3_mode_x *mVideoModeSet;
#ifdef PS5_HDMI_EDID_REQUEST
STATIC BOOLEAN mVideoStarted;   /* edid4: the first BDS video stage runs the EDID phase once, before itself */
STATIC VOID Ps5EdidPhaseFromVideo(UINTN Stage);
#endif
STATIC EFI_STATUS EFIAPI VideoStage(UINTN Stage) {
#ifdef PS5_HDMI_EDID_REQUEST
  if(!mVideoStarted){mVideoStarted=TRUE;Ps5EdidPhaseFromVideo(Stage);}
#endif
  if(Stage>14)return EFI_INVALID_PARAMETER;
  if(mVideoBusy)return EFI_ALREADY_STARTED;
  if(!UncachedMmio(MAILBOX) || !UncachedMmio(DOORBELL))return EFI_NOT_READY;
  struct ps5_hdmi_commands Packet;
  int Result=(Stage<8 || Stage>=11) ? ps5_flava3_video_stage_modex(&mVideoConfig,(unsigned)Stage,mVideoModeSet,&Packet) :
                      ps5_flava3_prepare_stage(&mVideoConfig,(unsigned)Stage-8,&Packet);
  if(Result)return EFI_UNSUPPORTED;
  mVideoBusy=TRUE;
  /* PS5 Linux spcie.c ICC_TIMEOUT_MSECS=12000. Video batches contain
   * explicit 500/700ms delays and receiver register waits. No replay. */
  struct ps5_icc_io Io={NULL,ReadIcc,WriteIcc,Ticks,Relax,TSC_HZ*12};
  EFI_TPL OldTpl=gBS->RaiseTPL(TPL_HIGH_LEVEL);
  BOOLEAN Interrupts=SaveAndDisableInterrupts();
  int Stop=0;
  if(Stage==6)Stop=ps5_icc_query(&Io,2,0x1f,NULL,0,NULL,0,NULL);
  if(!(Stage==6&&Stop))Result=ps5_hdmi_commands_submit(&Io,&Packet,ps5_edid_notification,&mEdid);
  SetInterruptState(Interrupts);gBS->RestoreTPL(OldTpl);mVideoBusy=FALSE;
  if(Stage==6)DEBUG((DEBUG_ERROR,"PS5 HDMI: stopHdcpHw=%d\n",Stop));
  if(!(Stage==6&&Stop))DEBUG((DEBUG_ERROR,"PS5 HDMI video stage=%u result=%d\n",Stage,Result));
#ifdef PS5_ICC_LINUX_NOTIFY
  STATIC UINT32 SeenTolerated,SeenDropped;
  if(ps5_icc_tolerated.count!=SeenTolerated || ps5_icc_dropped.count!=SeenDropped) {
    SeenTolerated=ps5_icc_tolerated.count;SeenDropped=ps5_icc_dropped.count;
    DEBUG((DEBUG_ERROR,"PS5 ICC after stage %u: tolerated=%u dropped=%u\n",Stage,SeenTolerated,SeenDropped));
  }
#endif
  return ((Stage==6&&Stop)||Result)?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI VideoReadRegisters(UINT16 Register,UINTN Count,UINT8 *Out) {
  if(!Out || !Count || Count>32 || Count>65536U-Register)return EFI_INVALID_PARAMETER;
  if(mVideoBusy)return EFI_ALREADY_STARTED;
  if(!UncachedMmio(MAILBOX) || !UncachedMmio(DOORBELL))return EFI_NOT_READY;
  mVideoBusy=TRUE;
  struct ps5_icc_io Io={NULL,ReadIcc,WriteIcc,Ticks,Relax,TSC_HZ*12};
  EFI_TPL OldTpl=gBS->RaiseTPL(TPL_HIGH_LEVEL);
  BOOLEAN Interrupts=SaveAndDisableInterrupts();
  int Result=ps5_hdmi_register_read(&Io,Register,(unsigned)Count,Out,ps5_edid_notification,&mEdid);
  SetInterruptState(Interrupts);gBS->RestoreTPL(OldTpl);mVideoBusy=FALSE;
  return Result?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI VideoGetEdid(UINT8 *Buffer,UINTN *Bytes,UINT32 *Flags,UINT32 *Updates) {
  if(!Bytes)return EFI_INVALID_PARAMETER;
  if(mVideoBusy)return EFI_ALREADY_STARTED;
  if(Flags)*Flags=mEdid.flags;
  if(Updates)*Updates=mEdid.updates;
  if(!mEdid.bytes)return EFI_NOT_FOUND;
  if(!Buffer || *Bytes<mEdid.bytes){*Bytes=mEdid.bytes;return EFI_BUFFER_TOO_SMALL;}
  CopyMem(Buffer,mEdid.raw,mEdid.bytes);*Bytes=mEdid.bytes;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI VideoSetMode(CONST VOID *Mode,UINTN Size) {
  if(mVideoBusy)return EFI_ALREADY_STARTED;
  if(Mode==NULL){mVideoModeSet=NULL;return EFI_SUCCESS;}
  if(Size!=sizeof(mVideoMode) && Size!=sizeof(mVideoModeX))return EFI_INVALID_PARAMETER;
  ZeroMem(&mVideoModeX,sizeof(mVideoModeX));CopyMem(&mVideoModeX,Mode,Size);mVideoModeSet=&mVideoModeX;
  if(mVideoModeX.manual)DEBUG((DEBUG_ERROR,"PS5 HDMI: manual timing extension 7042 width %02x%02x height %02x%02x clock %02x%02x\n",
    mVideoModeX.m7042[5],mVideoModeX.m7042[4],mVideoModeX.m7042[7],mVideoModeX.m7042[6],mVideoModeX.m7078[1],mVideoModeX.m7078[0]));
  DEBUG((DEBUG_INFO,"PS5 HDMI: mode 7227=%02x 7070[5]=%02x high_tmds=%u vic97=%u geometry=%u\n",
    mVideoMode.reg7227[0],mVideoMode.reg7070[5],mVideoMode.high_tmds,mVideoMode.vic97_mask,mVideoMode.uw_geometry));
  return EFI_SUCCESS;
}
/* r5.14: fan target (ps5-fan.h). Same ICC discipline as VideoReadRegisters (busy flag, TPL_HIGH, 12 s Linux timeout). */
STATIC EFI_STATUS EFIAPI VideoFan(UINT32 TargetC,UINT32 *BeforeC,UINT32 *AfterC,UINT32 *DutyBefore,UINT32 *DutyAfter) {
  if(TargetC && (TargetC<PS5_FAN_TARGET_MIN || TargetC>PS5_FAN_TARGET_MAX))return EFI_INVALID_PARAMETER;
  if(mVideoBusy)return EFI_ALREADY_STARTED;
  if(!UncachedMmio(MAILBOX) || !UncachedMmio(DOORBELL))return EFI_NOT_READY;
  mVideoBusy=TRUE;
  struct ps5_icc_io Io={NULL,ReadIcc,WriteIcc,Ticks,Relax,TSC_HZ*12};
  UINT8 Get[PS5_FAN_GET_BYTES],Set[PS5_FAN_SET_BYTES],Reply[128];unsigned Bytes=0,Old[PS5_FAN_WORDS],Now[PS5_FAN_WORDS],Duty=0;
  UINT32 Before=0,After=0,D0=0xFFFF,D1=0xFFFF;int R;
  EFI_TPL OldTpl=gBS->RaiseTPL(TPL_HIGH_LEVEL);
  BOOLEAN Interrupts=SaveAndDisableInterrupts();
  if(!ps5_icc_query_events(&Io,PS5_FAN_SERVICE,1,NULL,0,Reply,sizeof(Reply),&Bytes,ps5_edid_notification,&mEdid) &&
     !ps5_fan_duty(Reply,Bytes,&Duty))D0=Duty;
  ps5_fan_get_request(Get);
  R=ps5_icc_query_events(&Io,PS5_FAN_SERVICE,7,Get,sizeof(Get),Reply,sizeof(Reply),&Bytes,ps5_edid_notification,&mEdid);
  if(!R)R=ps5_fan_decode(Reply,Bytes,Old);
  if(!R){Before=After=Old[0]>>8;}
  if(!R && TargetC && TargetC!=Before) {
    ps5_fan_set_request(Set,Old,TargetC);
    R=ps5_icc_query_events(&Io,PS5_FAN_SERVICE,6,Set,sizeof(Set),NULL,0,NULL,ps5_edid_notification,&mEdid);
    if(!R)R=ps5_icc_query_events(&Io,PS5_FAN_SERVICE,7,Get,sizeof(Get),Reply,sizeof(Reply),&Bytes,ps5_edid_notification,&mEdid);
    if(!R)R=ps5_fan_decode(Reply,Bytes,Now);
    if(!R && (Now[0]>>8)!=TargetC)R=-5;
    if(R){ps5_fan_set_request(Set,Old,Old[0]>>8);(void)ps5_icc_query_events(&Io,PS5_FAN_SERVICE,6,Set,sizeof(Set),NULL,0,NULL,ps5_edid_notification,&mEdid);}
    else After=Now[0]>>8;
  }
  if(!ps5_icc_query_events(&Io,PS5_FAN_SERVICE,1,NULL,0,Reply,sizeof(Reply),&Bytes,ps5_edid_notification,&mEdid) &&
     !ps5_fan_duty(Reply,Bytes,&Duty))D1=Duty;
  SetInterruptState(Interrupts);gBS->RestoreTPL(OldTpl);mVideoBusy=FALSE;
  if(BeforeC){*BeforeC=Before;}
  if(AfterC){*AfterC=After;}
  if(DutyBefore){*DutyBefore=D0;}
  if(DutyAfter){*DutyAfter=D1;}
  DEBUG((DEBUG_ERROR,"PS5 fan: curset %x %x %x %x %x %x, target %u C -> %u C (want %u), duty %u -> %u /1024, result %d\n",
    R?0:Old[0],R?0:Old[1],R?0:Old[2],R?0:Old[3],R?0:Old[4],R?0:Old[5],Before,After,TargetC,D0,D1,R));
  return R?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
/* r5.20: read-only ICC part of the BIOS "System check" (ps5-hdmi-video-protocol.h PS5_HDMI_DIAG_ICC) */
STATIC EFI_STATUS EFIAPI VideoDiagIcc(PS5_HDMI_DIAG_ICC *Out) {
  if(Out==NULL)return EFI_INVALID_PARAMETER;
  SetMem(Out,sizeof(*Out),0);
  Out->r_duty=Out->r_fan=Out->r_temp=Out->r_7227=Out->r_7056=1;
  if(mVideoBusy)return EFI_ALREADY_STARTED;
  if(!UncachedMmio(MAILBOX) || !UncachedMmio(DOORBELL))return EFI_NOT_READY;
  mVideoBusy=TRUE;
  struct ps5_icc_io Io={NULL,ReadIcc,WriteIcc,Ticks,Relax,TSC_HZ*12};
  UINT8 Get[PS5_FAN_GET_BYTES],Zone[4]={0,0,0,0},Reply[128];unsigned Bytes=0,W[PS5_FAN_WORDS],Duty=0;
  EFI_TPL OldTpl=gBS->RaiseTPL(TPL_HIGH_LEVEL);
  BOOLEAN Interrupts=SaveAndDisableInterrupts();
  Out->r_duty=ps5_icc_query_events(&Io,PS5_FAN_SERVICE,1,NULL,0,Reply,sizeof(Reply),&Bytes,ps5_edid_notification,&mEdid);
  if(!Out->r_duty){if(!ps5_fan_duty(Reply,Bytes,&Duty))Out->fan_duty=Duty;else Out->r_duty=-6;}
  ps5_fan_get_request(Get);
  Out->r_fan=ps5_icc_query_events(&Io,PS5_FAN_SERVICE,7,Get,sizeof(Get),Reply,sizeof(Reply),&Bytes,ps5_edid_notification,&mEdid);
  if(!Out->r_fan){if(!ps5_fan_decode(Reply,Bytes,W))Out->fan_target_c=W[0]>>8;else Out->r_fan=-6;}
  Out->r_temp=ps5_icc_query_events(&Io,0x0b,1,Zone,sizeof(Zone),Reply,sizeof(Reply),&Bytes,ps5_edid_notification,&mEdid);
  if(!Out->r_temp){if(Bytes>=4){INT32 Raw=(INT16)(UINT16)(Reply[2]|((UINT16)Reply[3]<<8));Out->temp_milli_c=(Raw*1000+128)/256;}else Out->r_temp=-6;}
  /* r5.20.6: no FLAVA3 bridge register reads here any more (never tested on the HW with the stream up); r_7227 and
   * r_7056 stay 1 = not read. Fan and zone temperature use the same ICC services as VideoFan. */
  SetInterruptState(Interrupts);gBS->RestoreTPL(OldTpl);mVideoBusy=FALSE;
  return EFI_SUCCESS;
}
STATIC PS5_HDMI_VIDEO_PROTOCOL mVideo={VideoStage,VideoReadRegisters,VideoGetEdid,VideoSetMode,VideoFan,VideoDiagIcc};
#endif
#ifdef PS5_HDMI_PREPARE
/* v2.51: one-button console input from the PS5 power button (ICC service 8).
 * Enabled like PS5 Linux icc_button_enable_notification(0,1); polled from a
 * 50 ms boot-services timer only while no video ICC transaction is active. */
STATIC struct ps5_button_keys mKeys;
STATIC int mButtonEnabled=-1;
STATIC BOOLEAN mButtonInit;
STATIC UINT32 mPolls,mPollKind[4],mNotes,mLastService,mLastType;
STATIC EFI_SIMPLE_TEXT_INPUT_PROTOCOL mButtonIn;
STATIC EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL mButtonInEx;   /* r5.1: LVGL/ConSplitter read keys only from TextInEx devices */
STATIC EFI_EVENT mButtonTimer;
STATIC struct {VENDOR_DEVICE_PATH V;EFI_DEVICE_PATH_PROTOCOL End;} mButtonPath={
  {{HARDWARE_DEVICE_PATH,HW_VENDOR_DP,{sizeof(VENDOR_DEVICE_PATH),0}},
   {0x5a4c2f2e,0x8d6b,0x4a1b,{0x9e,0x31,0x50,0x53,0x35,0x42,0x54,0x4e}}},
  {END_DEVICE_PATH_TYPE,END_ENTIRE_DEVICE_PATH_SUBTYPE,{sizeof(EFI_DEVICE_PATH_PROTOCOL),0}}};
STATIC void ButtonNotification(void *Context,uint8_t Service,uint16_t Type,const uint8_t *Payload,unsigned Bytes) {
  mNotes++;mLastService=Service;mLastType=Type;
  if(Service==PS5_BUTTON_SERVICE) {
    int Key=ps5_button_event(&mKeys,Service,Type,AsmReadTsc());
    DEBUG((DEBUG_INFO,"PS5 button: type=%x key=%d\n",Type,Key));
  } else ps5_edid_notification(Context,Service,Type,Payload,Bytes);
}
#ifdef PS5_HDMI_EDID_REQUEST
#define PS5_ICC_POLLER_MESSAGES 2048U   /* edid3: the poller stops for good after this many inbound messages */
STATIC BOOLEAN mPollerStopped;
#endif
STATIC BOOLEAN mButtonHandedOff;   /* r5.5: set at ExitBootServices - no more ICC access from the poller */
/* r5.25.5 (warm re-entry after Windows, HW 19.11.: hang between the IER opening and BDS probe 46 = the first
 * timer ticks): ps5pwr left ICC notifications armed and the mailbox possibly mid-message. The first poll drains it and
 * puts the transport back to idle exactly as ps5pwr icc_prepare does; the button enable is tried at most once, then given
 * up (each try waits up to 1 s per phase with interrupts off). Only with the trampoline's warm marker. */
STATIC BOOLEAN mPs5Warm, mPs5WarmPrepared, mPs5EnableRetried;
STATIC VOID Ps5IccWarmPrepare (CONST struct ps5_icc_io *Io)
{
  struct ps5_icc_io Short = *Io;
  UINT32 Sow, Sor;
  Short.timeout_ticks = TSC_HZ / 5;                                   /* 200 ms like icc_prepare */
  for (UINTN I = 0; I < 16 && ps5_icc_poll (&Short, ButtonNotification, &mEdid) > 0; I++) {}
  Sow = ReadIcc (NULL, 0, 0x7f0, 2); Sor = ReadIcc (NULL, 0, 0x7f4, 2);
  if (Sor != 1 || Sow != 0) {
    WriteIcc (NULL, 1, 0x14, 4, 3);                                   /* clear the latched transport status */
    WriteIcc (NULL, 0, 0x7f0, 2, 0);                                  /* SOW = 0: no host message pending */
    WriteIcc (NULL, 0, 0x7f4, 2, 1);                                  /* SOR = 1: idle */
  }
  DEBUG ((DEBUG_ERROR, "PS5 button: warm re-entry - ICC drained, SOW=%x SOR=%x%a\n", Sow, Sor,
          (Sor != 1 || Sow != 0) ? " -> reset to idle" : ""));
}
STATIC VOID EFIAPI ButtonPoll(EFI_EVENT Event,VOID *Context) {
#if defined(PS5_WARM_PROBE) && PS5_WARM_PROBE == 55
  Ps5WarmProbeOff();   /* lab: the ICC button poller runs after a warm re-entry */
#endif
  if(mButtonHandedOff)return;
#if defined(PS5_WARM_PROBE) && PS5_WARM_PROBE == 53
  UINT64 Ps5ProbeStart=AsmReadTsc();
#endif
  if(mButtonInit)ps5_button_tick(&mKeys,AsmReadTsc());   /* r5.2: queue-only, also while the ICC poll is paused */
  if(mVideoBusy)return;
#ifdef PS5_HDMI_EDID_REQUEST
  if(mPollerStopped)return;
  if(mPollKind[1]+mPollKind[2]+mPollKind[3]+ps5_icc_dropped.count>=PS5_ICC_POLLER_MESSAGES) {
    mPollerStopped=TRUE;
    if(mButtonTimer!=NULL)gBS->SetTimer(mButtonTimer,TimerCancel,0);
    DEBUG((DEBUG_ERROR,"PS5 ICC poller stopped: %u inbound messages (note=%u stale=%u err=%u dropped=%u tolerated=%u)\n",
      PS5_ICC_POLLER_MESSAGES,mPollKind[1],mPollKind[2],mPollKind[3],ps5_icc_dropped.count,ps5_icc_tolerated.count));
    return;
  }
#endif
  struct ps5_icc_io Io={NULL,ReadIcc,WriteIcc,Ticks,Relax,TSC_HZ};
  EFI_TPL Old=gBS->RaiseTPL(TPL_HIGH_LEVEL);
  BOOLEAN Ints=SaveAndDisableInterrupts();
  int R=0;
  if(mPs5Warm && !mPs5WarmPrepared){mPs5WarmPrepared=TRUE;Ps5IccWarmPrepare(&Io);}
  if(mButtonEnabled!=0 && (mPolls%40)==0 && !(mPs5Warm && mPs5EnableRetried)) {
    CONST UINT8 Enable[2]={0,1};
    mButtonEnabled=ps5_icc_query_events(&Io,PS5_BUTTON_SERVICE,1,Enable,sizeof(Enable),NULL,0,NULL,ButtonNotification,&mEdid);
    if(mPs5Warm){mPs5EnableRetried=TRUE;if(mButtonEnabled!=0){DEBUG((DEBUG_ERROR,"PS5 button: warm re-entry - enable failed %d, not retried\n",mButtonEnabled));mButtonEnabled=0;}}
  }
#ifdef PS5_HDMI_EDID_REQUEST
  /* edid3: at most 4 messages and 5 ms per 50 ms tick, so an inbound flood cannot starve the BDS. */
  UINT64 PollStart=AsmReadTsc();
  for(UINTN I=0;I<4;I++){R=ps5_icc_poll(&Io,ButtonNotification,&mEdid);mPollKind[R<0?3:(R>2?3:R)]++;if(R<=0||AsmReadTsc()-PollStart>TSC_HZ/200)break;}
#else
  for(UINTN I=0;I<4;I++){R=ps5_icc_poll(&Io,ButtonNotification,&mEdid);mPollKind[R<0?3:(R>2?3:R)]++;if(R<=0)break;}
#endif
  SetInterruptState(Ints);gBS->RestoreTPL(Old);
#if defined(PS5_WARM_PROBE) && PS5_WARM_PROBE == 53
  if(AsmReadTsc()-Ps5ProbeStart>TSC_HZ/2)Ps5WarmProbeOff();   /* lab: one poll took > 0.5 s after a warm re-entry (ICC timeouts) */
#endif
#if defined(PS5_WARM_PROBE) && PS5_WARM_PROBE == 56
  Ps5WarmProbeOff();   /* lab: one ICC poll came back */
#endif
  if((++mPolls%200)==1) {
    DEBUG((DEBUG_INFO,"PS5 button status: enabled=%d polls=%u none=%u note=%u stale=%u err=%u notes=%u last=%x/%x pending=%d\n",
      mButtonEnabled,mPolls,mPollKind[0],mPollKind[1],mPollKind[2],mPollKind[3],mNotes,mLastService,mLastType,ps5_button_pending(&mKeys)));
    /* v2.55: also on screen (TPL_CALLBACK console output is permitted). */
    if(!PS5_QUIET_SCREEN && gST!=NULL && gST->ConOut!=NULL) {   /* PSWloader: log line above is enough */
      CHAR16 Line[160];
      UnicodeSPrint(Line,sizeof(Line),L"[button enabled=%d polls=%u none=%u note=%u stale=%u err=%u notes=%u last=%x/%x]\r\n",
        mButtonEnabled,mPolls,mPollKind[0],mPollKind[1],mPollKind[2],mPollKind[3],mNotes,mLastService,mLastType);
      gST->ConOut->OutputString(gST->ConOut,Line);
    }
  }
}
STATIC EFI_STATUS EFIAPI ButtonReset(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,BOOLEAN Ext) {
  while(ps5_button_pop(&mKeys)){}
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI ButtonRead(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,EFI_INPUT_KEY *Key) {
  if(Key==NULL)return EFI_INVALID_PARAMETER;
  if(!ps5_button_pending(&mKeys))ButtonPoll(NULL,NULL); /* v2.56: drive ICC from the reader too */
  if(mButtonInit)ps5_button_tick(&mKeys,AsmReadTsc());  /* r5.2: release a single press after the double-press window */
  int K=ps5_button_pop(&mKeys);
  if(K==0)return EFI_NOT_READY;
  Key->ScanCode=K==PS5_KEY_DOWN?SCAN_DOWN:(K==PS5_KEY_ESC?SCAN_ESC:SCAN_NULL);   /* r5.2: double press = Esc */
  Key->UnicodeChar=K==PS5_KEY_ENTER?CHAR_CARRIAGE_RETURN:CHAR_NULL;
  return EFI_SUCCESS;
}
/* r5.1 (16.11.): SimpleTextInputEx for the power button. ConSplitter's ReadKeyStrokeEx (used by the LVGL setup,
 * lv_port_indev.c) only polls devices that expose TextInEx, so without this the graphical menu never saw the button.
 * No toggle/shift state; key notifications are matched and called when a key is read. */
typedef struct { BOOLEAN Used; EFI_KEY_DATA Key; EFI_KEY_NOTIFY_FUNCTION Fn; } PS5_BUTTON_NOTIFY;
STATIC PS5_BUTTON_NOTIFY mButtonNotify[8];
STATIC EFI_STATUS EFIAPI ButtonResetEx(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This,BOOLEAN Ext) { return ButtonReset(&mButtonIn,Ext); }
STATIC EFI_STATUS EFIAPI ButtonReadEx(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This,EFI_KEY_DATA *KeyData) {
  if(KeyData==NULL)return EFI_INVALID_PARAMETER;
  ZeroMem(KeyData,sizeof(*KeyData));
  EFI_STATUS S=ButtonRead(&mButtonIn,&KeyData->Key);
  if(!EFI_ERROR(S))
    for(UINTN I=0;I<8;I++)
      if(mButtonNotify[I].Used && mButtonNotify[I].Key.Key.ScanCode==KeyData->Key.ScanCode &&
         mButtonNotify[I].Key.Key.UnicodeChar==KeyData->Key.UnicodeChar)mButtonNotify[I].Fn(KeyData);
  return S;
}
STATIC EFI_STATUS EFIAPI ButtonSetState(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This,EFI_KEY_TOGGLE_STATE *State) {
  return State==NULL?EFI_INVALID_PARAMETER:EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI ButtonRegisterNotify(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This,EFI_KEY_DATA *KeyData,
                                              EFI_KEY_NOTIFY_FUNCTION Fn,VOID **Handle) {
  if(KeyData==NULL || Fn==NULL || Handle==NULL)return EFI_INVALID_PARAMETER;
  for(UINTN I=0;I<8;I++)if(!mButtonNotify[I].Used){mButtonNotify[I].Used=TRUE;mButtonNotify[I].Key=*KeyData;mButtonNotify[I].Fn=Fn;*Handle=&mButtonNotify[I];return EFI_SUCCESS;}
  return EFI_OUT_OF_RESOURCES;
}
STATIC EFI_STATUS EFIAPI ButtonUnregisterNotify(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This,VOID *Handle) {
  for(UINTN I=0;I<8;I++)if(Handle==&mButtonNotify[I] && mButtonNotify[I].Used){mButtonNotify[I].Used=FALSE;return EFI_SUCCESS;}
  return EFI_INVALID_PARAMETER;
}
STATIC VOID EFIAPI ButtonWait(EFI_EVENT Event,VOID *Context) {
  if(ps5_button_pending(&mKeys))gBS->SignalEvent(Event);
}
STATIC int Ps5ButtonEnable(struct ps5_icc_io *Io) {
  /* Caller holds ICC (TPL_HIGH, interrupts off). */
  if(!mButtonInit){ps5_button_init(&mKeys,TSC_HZ);mButtonInit=TRUE;}
  CONST UINT8 Enable[2]={0,1}; /* type 0 = power button, enable */
  return ps5_icc_query_events(Io,PS5_BUTTON_SERVICE,1,Enable,sizeof(Enable),NULL,0,NULL,ps5_edid_notification,&mEdid);
}
/* r5.5 (16.11., F20): leave the ICC as PS5 Linux leaves it on remove (icc_button_enable_all_notifications(0)).
 * The power-button notifications enabled for the setup menu stayed on across ExitBootServices; under Windows nobody
 * reads ICC notifications (ps5pwr drains only at S5), so a press during the Windows boot left 0x8010/0x8011 queued in
 * the mailbox, and that boot later hung ~6 min at shutdown without reaching S5. Now at EBS: power-button notification
 * off, pending inbound messages drained. MMIO + TSC only (no boot services), interrupts off around the packet. */
STATIC UINT32 mHandOffDrained;
STATIC void ButtonDrop(void *Context,uint8_t Service,uint16_t Type,const uint8_t *Payload,unsigned Bytes) { mHandOffDrained++; }
/* r5.6: the hand-off runs at ReadyToBoot (signalled by EfiBootManagerBoot only for real boot options, never for the
 * boot-manager menu), with full boot services, so the proof variable can be created; EBS repeats the ICC part as a
 * safety net without SetVariable (the variable driver is already in runtime mode there and rejects new volatile
 * variables: "Only variable have NV|RT attribute can be created in Runtime"). */
STATIC INT32 mHandOffResult=-99;
STATIC VOID Ps5ButtonHandOff(VOID) {
  mButtonHandedOff=TRUE;
  if(mVideoBusy)return;
  struct ps5_icc_io Io={NULL,ReadIcc,WriteIcc,Ticks,Relax,TSC_HZ};
  BOOLEAN Ints=SaveAndDisableInterrupts();
  CONST UINT8 Disable[2]={0,0};   /* type 0 = power button, disable */
  mHandOffResult=ps5_icc_query_events(&Io,PS5_BUTTON_SERVICE,1,Disable,sizeof(Disable),NULL,0,NULL,ButtonDrop,NULL);
  for(UINTN I=0;I<16 && ps5_icc_poll(&Io,ButtonDrop,NULL)>0;I++){}
  SetInterruptState(Ints);
}
STATIC VOID EFIAPI ButtonReadyToBoot(EFI_EVENT Event,VOID *Context) {
  Ps5ButtonHandOff();
  DEBUG((DEBUG_ERROR,"PS5 button: hand-off at ReadyToBoot - power notification off=%d drained=%u\n",mHandOffResult,mHandOffDrained));
#ifdef PS5_HDMI_EDID_REQUEST   /* gRT + vendor GUID come with the EDID publication */
  { STATIC EFI_GUID ProofGuid=PS5_HDMI_EDID_VENDOR_GUID_INIT; UINT32 Proof[3]={0x48414E44u,(UINT32)mHandOffResult,mHandOffDrained};
    EFI_STATUS Vs=EFI_UNSUPPORTED;
    if(gRT!=NULL && gRT->SetVariable!=NULL)
      Vs=gRT->SetVariable(L"Ps5IccHandoff",&ProofGuid,EFI_VARIABLE_BOOTSERVICE_ACCESS|EFI_VARIABLE_RUNTIME_ACCESS,sizeof(Proof),Proof);
    DEBUG((DEBUG_ERROR,"PS5 button: Ps5IccHandoff variable %r\n",Vs)); }
#endif
}
STATIC VOID EFIAPI ButtonExitBootServices(EFI_EVENT Event,VOID *Context) {
  if(!mButtonHandedOff)Ps5ButtonHandOff();   /* only if no boot option signalled ReadyToBoot */
}
STATIC VOID Ps5ButtonInputStart(VOID) {
  if(!mButtonInit){ps5_button_init(&mKeys,TSC_HZ);mButtonInit=TRUE;}
#ifndef PS5_NO_WARM_MARKER   /* host tests: no reserved arena */
  mPs5Warm = (*(volatile UINT64 *)(UINTN)0x111806020ULL == 0x544F4F424D524157ULL);   /* read only: dp-source clears it later */
#endif
  /* TPL_APPLICATION context: events and protocol installation are legal here. */
  mButtonIn.Reset=ButtonReset;mButtonIn.ReadKeyStroke=ButtonRead;
  if(EFI_ERROR(gBS->CreateEvent(EVT_NOTIFY_WAIT,TPL_NOTIFY,ButtonWait,NULL,&mButtonIn.WaitForKey)))return;
  mButtonInEx.Reset=ButtonResetEx;mButtonInEx.ReadKeyStrokeEx=ButtonReadEx;mButtonInEx.SetState=ButtonSetState;
  mButtonInEx.RegisterKeyNotify=ButtonRegisterNotify;mButtonInEx.UnregisterKeyNotify=ButtonUnregisterNotify;
  if(EFI_ERROR(gBS->CreateEvent(EVT_NOTIFY_WAIT,TPL_NOTIFY,ButtonWait,NULL,&mButtonInEx.WaitForKeyEx)))return;
  if(EFI_ERROR(gBS->CreateEvent(EVT_TIMER|EVT_NOTIFY_SIGNAL,TPL_CALLBACK,ButtonPoll,NULL,&mButtonTimer)))return;
#ifdef PS5_WARM_NO_BUTTON_POLL
  if(!mPs5Warm)   /* lab: no ICC button poller after a warm re-entry (does the BDS hang go away?) */
#endif
  gBS->SetTimer(mButtonTimer,TimerPeriodic,500000);
  { STATIC EFI_GUID EbsGroup={0x27abf055,0xb1b8,0x4c26,{0x80,0x48,0x74,0x8f,0x37,0xba,0xa2,0xdf}};   /* gEfiEventExitBootServicesGuid */
    EFI_EVENT Ebs=NULL;
    EFI_STATUS Es=gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_CALLBACK,ButtonExitBootServices,NULL,&EbsGroup,&Ebs);
    DEBUG((DEBUG_ERROR,"PS5 button: EBS hand-off registered %r\n",Es)); }
  { STATIC EFI_GUID RtbGroup={0x7ce88fb3,0x4bd7,0x4679,{0x87,0xa8,0xa8,0xd8,0xde,0xe5,0x0d,0x2b}};   /* gEfiEventReadyToBootGuid */
    EFI_EVENT Rtb=NULL;
    EFI_STATUS Rs=gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_CALLBACK,ButtonReadyToBoot,NULL,&RtbGroup,&Rtb);
    DEBUG((DEBUG_ERROR,"PS5 button: ReadyToBoot hand-off registered %r\n",Rs)); }
  STATIC EFI_GUID ConsoleInDevice={0xD3B36F2B,0xD551,0x11D4,{0x9A,0x46,0x00,0x90,0x27,0x3F,0xC1,0x4D}};
  EFI_HANDLE Handle=NULL;
  EFI_STATUS S=gBS->InstallMultipleProtocolInterfaces(&Handle,&gEfiDevicePathProtocolGuid,&mButtonPath,
    &gEfiSimpleTextInProtocolGuid,&mButtonIn,&gEfiSimpleTextInputExProtocolGuid,&mButtonInEx,&ConsoleInDevice,NULL,NULL);
  DEBUG((DEBUG_INFO,"PS5 button: SimpleTextIn install %r\n",S));
}
#endif
#ifdef PS5_HDMI_EDID_REQUEST
/* edid4 (UNTESTED). edid3 ran the phase inside a BDS event notify (TPL_CALLBACK) with TPL_HIGH and interrupts
 * off for the whole phase (up to ~5.5 s, while the USB stack was live), and a log export could only follow
 * after the handler had returned; the console stopped right there (14.11. 07:15). Now:
 * - the phase runs synchronously at TPL_APPLICATION, from the first BDS call of the video service (before
 *   that stage), only when the pinned ESP is mounted; at any other TPL it is skipped;
 * - interrupts are off only around a single ICC packet (<= 1 s) or a 2 ms poll burst, never for the window;
 * - after the start line, after every packet, every 250 ms of the window and at the end the BDS log group
 *   (...fa52) is signalled: at TPL_APPLICATION LogExportDxe exports the ring synchronously, so a hang is
 *   pinned to one step (at most 1 + 8 + 6 + 1 exports);
 * - all limits are TSC counts (no Stall, timer event, CheckEvent or WaitForEvent): request budget 3 s,
 *   window 1.5 s and 64 inbound messages.
 * edid5: every inbound message is announced (service/type/length) and exported BEFORE its body is read,
 * and logged with its first 8 data bytes and exported after; the HDMI events are switched off again after
 * the EDID (at the latest after 2 messages, else at the window end). */
#define PS5_EDID_REQUEST_BUDGET  (TSC_HZ*15)   /* b1440d: the Dell needs > 1 s for packet 7 */
#ifndef PS5_EDID_WINDOW_SECONDS
#define PS5_EDID_WINDOW_SECONDS  30U   /* r5.16.3: the Dell (MN864739 via EMC) delivers its EDID only after ~20-25 s
                                        * (nvme-boot1: one burst hung ~22 s, then 256 B); fast sinks still leave early */
#endif
#ifndef PS5_EDID_TIMESTAMPS
#define PS5_EDID_TIMESTAMPS 1          /* r5.16.3: always on */
#endif
#define PS5_EDID_RESEND_AFTER    (TSC_HZ*8)   /* r5.16.3: nothing at all after 8 s -> send the 8 packets once more */
#define PS5_EDID_OFF_GIVEUP      (TSC_HZ*4)   /* events off without an EDID: stop like before r5.16.3 */
#define PS5_EDID_WINDOW_BUDGET   (TSC_HZ*PS5_EDID_WINDOW_SECONDS)   /* r5.16.2: --edid-window-seconds (Dell test) */
#ifdef PS5_EDID_TIMESTAMPS
#define PS5_EDID_PROGRESS_MAX    (6U+PS5_EDID_WINDOW_SECONDS)       /* then one progress line per second */
#else
#define PS5_EDID_PROGRESS_MAX    6U
#endif
#define PS5_EDID_WINDOW_MESSAGES 64U
#define PS5_EDID_BURST           (TSC_HZ/500)
/* edid5: the payload's DXE DebugLib (PeiDxeDebugLibReportStatusCode) packs at most 12 arguments per DEBUG();
 * a 13th trips ASSERT (BaseListMarkerPointer <= FormatString) -> CpuDeadLoop (PcdDebugPropertyMask 0x2F).
 * edid2-4 printed the first tolerated ICC message with 20 arguments right after the window: that dead loop is
 * the hang of all three. Every DEBUG() here stays <= 8 arguments (check_debug_args.py, host test). */
#define PS5_EDID_RX_LOGGED       8U     /* messages logged + exported individually */
#define PS5_EDID_EVENTS_OFF_AFTER 2U    /* safety net: HDMI events off after the EDID, at the latest after 2 messages */
struct ps5_edid_rx { UINT32 svc,type,len; };
STATIC struct { UINT32 calls,svc,type,bytes,d0,d1; } mEdidRxNote;
STATIC void EdidRxNote(void *Context,uint8_t Service,uint16_t Type,const uint8_t *Payload,unsigned Bytes) {
  mEdidRxNote.calls++;mEdidRxNote.svc=Service;mEdidRxNote.type=Type;mEdidRxNote.bytes=Bytes;
  UINT8 D[8]={0};for(unsigned I=0;I<8 && I<Bytes;I++)D[I]=Payload[I];
  mEdidRxNote.d0=D[0]|(UINT32)D[1]<<8|(UINT32)D[2]<<16|(UINT32)D[3]<<24;
  mEdidRxNote.d1=D[4]|(UINT32)D[5]<<8|(UINT32)D[6]<<16|(UINT32)D[7]<<24;
  ps5_edid_notification(&mEdid,Service,Type,Payload,Bytes);
}
STATIC BOOLEAN EdidPeek(struct ps5_edid_rx *X) {
  /* Caller holds TPL_HIGH with interrupts off. Reads only what Linux icc_interrupt() reads first (status,
   * header) and acknowledges nothing: the following ps5_icc_poll() takes the message in Linux order
   * (status ack, length, body <= 0x7f0, EMW=0, EMR=1, doorbell ACK). */
  if(!(ReadIcc(NULL,1,0x14,4)&1))return FALSE;
  X->len=ReadIcc(NULL,0,0x808,2);X->svc=ReadIcc(NULL,0,0x801,1);X->type=ReadIcc(NULL,0,0x802,2);
  return TRUE;
}
STATIC int EdidEventsOff(struct ps5_icc_io *Io) {
  /* sceControlHdmiEvent(0): the same packet as the enable (cmd 5/4) with value 0. */
  struct ps5_hdmi_commands P;
  if(ps5_hdmi_commands_init(&P,4) || ps5_hdmi_commands_event(&P,4,0))return -1;
  return ps5_hdmi_commands_submit(Io,&P,EdidRxNote,NULL);
}
STATIC UINT32 mEdidOffUs;
STATIC int EdidEventsOffBounded(const struct ps5_icc_io *Io) {
  if(!Io)return -1;
  struct ps5_icc_io Bounded=*Io;
  if(Bounded.timeout_ticks>TSC_HZ)Bounded.timeout_ticks=TSC_HZ; /* cap the interrupt-off ICC wait at 1 s */
  UINT64 T0=AsmReadTsc();int R=EdidEventsOff(&Bounded);
  mEdidOffUs=(UINT32)((AsmReadTsc()-T0)/(TSC_HZ/1000000)); /* r5.16.2 logged after interrupts are back on */
  return R;
}
STATIC EFI_GUID mEdidLogGuid={0x873f7798,0xe801,0x4ec4,{0x86,0x97,0x30,0xb7,0xb3,0xa1,0xfa,0x52}};
STATIC EFI_GUID mEdidEspGuid={0xbd21c511,0x2781,0x4058,{0xb8,0x47,0xf6,0x25,0xa7,0xec,0xcb,0xb6}};
STATIC EFI_GUID mEdidEspNvmeGuid={0x53298651,0x3b33,0x4537,{0x90,0x18,0x3e,0x7d,0x4b,0xde,0x5a,0x13}};   /* r4-nvme */
STATIC UINT32 mEdidFlushes;
STATIC VOID EFIAPI EdidFlushNotify(EFI_EVENT Event,VOID *Context) {}
STATIC VOID EdidFlush(VOID) {
  EFI_EVENT E=NULL;
  if(EFI_ERROR(gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_CALLBACK,EdidFlushNotify,NULL,&mEdidLogGuid,&E)))return;
  gBS->SignalEvent(E);gBS->CloseEvent(E);mEdidFlushes++;
}
STATIC BOOLEAN EdidEspMounted(VOID) {
  /* Same test as LogExportDxe: a file system on the GPT system partition with the pinned GUID. */
  EFI_HANDLE *Handles=NULL;UINTN Count=0;BOOLEAN Found=FALSE;
  if(EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol,&gEfiSimpleFileSystemProtocolGuid,NULL,&Count,&Handles)))return FALSE;
  for(UINTN I=0;I<Count && !Found;I++) {
    EFI_PARTITION_INFO_PROTOCOL *P=NULL;
    if(!EFI_ERROR(gBS->HandleProtocol(Handles[I],&gEfiPartitionInfoProtocolGuid,(VOID **)&P)) && P!=NULL &&
       P->Type==PARTITION_TYPE_GPT && P->System && (CompareGuid(&P->Info.Gpt.UniquePartitionGUID,&mEdidEspGuid) ||
       CompareGuid(&P->Info.Gpt.UniquePartitionGUID,&mEdidEspNvmeGuid)))Found=TRUE;
  }
  if(Handles!=NULL)gBS->FreePool(Handles);
  return Found;
}
STATIC UINT32 EdidMs(UINT64 Ticks) { return (UINT32)DivU64x64Remainder(Ticks*1000,TSC_HZ,NULL); }
STATIC int EdidRequest(struct ps5_icc_io *Io,unsigned *StagesOut) {   /* r5.16.3: also used for the resend */
 UINT64 Start=AsmReadTsc();unsigned Stages=0;int Request=0;
 for(unsigned S=0;S<PS5_FLAVA3_EDID_STAGES;S++) {
   if(AsmReadTsc()-Start>=PS5_EDID_REQUEST_BUDGET){Request=-8;break;}
   struct ps5_hdmi_commands Packet;
   UINT64 T0=AsmReadTsc();
   int R=ps5_flava3_edid_stage(S,&Packet);
   if(!R) {
     EFI_TPL Old=gBS->RaiseTPL(TPL_HIGH_LEVEL);BOOLEAN Ints=SaveAndDisableInterrupts();
     R=ps5_hdmi_commands_submit(Io,&Packet,ps5_edid_notification,&mEdid);
     SetInterruptState(Ints);gBS->RestoreTPL(Old);
   }
   DEBUG((DEBUG_ERROR,"PS5 HDMI EDID packet %u/%u: result=%d ms=%u\n",S+1,PS5_FLAVA3_EDID_STAGES,R,EdidMs(AsmReadTsc()-T0)));
   EdidFlush();
   if(R){Request=R;break;}
   Stages=S+1;
 }
 *StagesOut=Stages;return Request;
}
/* edid6: EDID into the log (hex + parsed) and to Windows as volatile UEFI variables (BOOTSERVICE|RUNTIME,
 * never NV; this payload's variable store is RAM-only anyway, VARIABLE_SUPPORT=EMU). Names, GUID and the
 * info layout: ps5-edid-info.h. Windows reads them with GetFirmwareEnvironmentVariableExW (edidread.exe). */
STATIC EFI_GUID mPs5EdidVendorGuid=PS5_HDMI_EDID_VENDOR_GUID_INIT;
STATIC UINT8 mEdidPublished[PS5_EDID_MAX_BYTES],mEdidReadback[PS5_EDID_MAX_BYTES];
STATIC UINT32 EdidBe32(CONST UINT8 *P) { return (UINT32)P[0]<<24|(UINT32)P[1]<<16|(UINT32)P[2]<<8|P[3]; }
STATIC UINTN EdidAppendU(CHAR8 *Buf,UINTN Len,UINTN Cap,UINT32 V) {
  CHAR8 D[10];UINTN N=0;
  do{D[N++]=(CHAR8)('0'+V%10);V/=10;}while(V && N<sizeof(D));
  if(Len+N+2>Cap)return Len;
  while(N)Buf[Len++]=D[--N];
  Buf[Len++]=' ';Buf[Len]=0;return Len;
}
STATIC VOID EdidPublish(VOID) {
  UINT32 N=mEdid.bytes;
  if(N<128 || N>PS5_EDID_MAX_BYTES){DEBUG((DEBUG_ERROR,"PS5 EDID: nothing to publish (bytes=%u)\n",N));return;}
  for(UINT32 R=0;R<N;R+=16)
    DEBUG((DEBUG_ERROR,"PS5 EDID %03x: %08x %08x %08x %08x\n",R,EdidBe32(mEdid.raw+R),EdidBe32(mEdid.raw+R+4),EdidBe32(mEdid.raw+R+8),EdidBe32(mEdid.raw+R+12)));
  CopyMem(mEdidPublished,mEdid.raw,N);
  UINT8 RawByte0=mEdidPublished[0],RawSum=mEdidPublished[127];
  UINT8 FixedSum=ps5_edid_fix(mEdidPublished);
  struct ps5_edid_summary S;
  ps5_edid_summary(mEdidPublished,N,&S);
  DEBUG((DEBUG_ERROR,"PS5 EDID: vendor=%a product=%04x serial=%08x name=%a version=%u.%u extensions=%u\n",
    S.vendor,S.product,S.serial,S.name[0]?S.name:"-",S.version,S.revision,S.extensions));
  DEBUG((DEBUG_ERROR,"PS5 EDID: raw byte0=%02x (HDCP2.2=%u FRL=%u) checksum %02x->%02x header=%u block0=%u cta=%u\n",
    RawByte0,RawByte0&1,(RawByte0>>1)&7,RawSum,FixedSum,S.header_ok,S.block0_ok,S.cta));
  for(UINTN I=0;I<S.dtd_count;I++) {
    CONST struct ps5_edid_dtd *T=&S.dtd[I];
    DEBUG((DEBUG_ERROR,"PS5 EDID DTD %u%a: %ux%u%a @ %u.%03u Hz, pclk %u kHz, total %ux%u\n",(UINT32)I+1,I==0?" (preferred)":"",
      T->hact,T->vact,T->interlaced?"i":"",T->refresh_mhz/1000,T->refresh_mhz%1000,T->pclk_khz,T->htot,T->vtot));
  }
  for(UINTN I=0;I<S.vic_count;I+=16) {
    CHAR8 Line[80];UINTN L=0;Line[0]=0;
    for(UINTN J=I;J<S.vic_count && J<I+16;J++)L=EdidAppendU(Line,L,sizeof(Line),S.vic[J]);
    DEBUG((DEBUG_ERROR,"PS5 EDID VICs %u-%u of %u: %a\n",(UINT32)I+1,(UINT32)(I+16<S.vic_count?I+16:S.vic_count),S.vic_count,Line));
  }
  struct ps5_hdmi_edid_info Info;
  ZeroMem(&Info,sizeof(Info));
  Info.signature=PS5_HDMI_EDID_INFO_SIGNATURE;Info.version=1;Info.header_size=(UINT16)sizeof(Info);
  Info.edid_bytes=N;Info.collector_flags=mEdid.flags;Info.updates=mEdid.updates;
  Info.raw_byte0=RawByte0;Info.raw_checksum=RawSum;Info.fixed_checksum=FixedSum;Info.fixed=1;
  Info.tsc_at_capture=AsmReadTsc();Info.tsc_hz=TSC_HZ;
  EFI_TIME Time;ZeroMem(&Time,sizeof(Time));
  EFI_STATUS TimeStatus=(gRT!=NULL && gRT->GetTime!=NULL)?gRT->GetTime(&Time,NULL):EFI_UNSUPPORTED;
  CopyMem(Info.firmware_time,&Time,sizeof(Info.firmware_time));Info.time_status=(UINT32)TimeStatus;
  Info.crc32=ps5_edid_crc32(mEdidPublished,N);
  CONST UINT32 Attr=EFI_VARIABLE_BOOTSERVICE_ACCESS|EFI_VARIABLE_RUNTIME_ACCESS;   /* volatile: no NV bit */
  EFI_STATUS S1=EFI_UNSUPPORTED,S2=EFI_UNSUPPORTED,S3=EFI_UNSUPPORTED;UINT32 Crc=0;UINTN Size=sizeof(mEdidReadback);
  if(gRT!=NULL && gRT->SetVariable!=NULL) {
    S1=gRT->SetVariable(L"Ps5HdmiEdid",&mPs5EdidVendorGuid,Attr,N,mEdidPublished);
    S2=gRT->SetVariable(L"Ps5HdmiEdidInfo",&mPs5EdidVendorGuid,Attr,sizeof(Info),&Info);
    UINT32 A=0;
    S3=gRT->GetVariable(L"Ps5HdmiEdid",&mPs5EdidVendorGuid,&A,&Size,mEdidReadback);
    if(!EFI_ERROR(S3)){Crc=ps5_edid_crc32(mEdidReadback,(unsigned)Size);if(Crc!=Info.crc32 || A!=Attr)S3=EFI_CRC_ERROR;}
  }
  DEBUG((DEBUG_ERROR,"PS5 EDID published: Ps5HdmiEdid=%r Ps5HdmiEdidInfo=%r readback=%r bytes=%u crc=%08x\n",S1,S2,S3,N,Info.crc32));
#ifdef PS5_BOOT_TS
  DEBUG((DEBUG_ERROR,"PS5 TS edid-published %Lu ms\n",(UINT64)(AsmReadTsc()/1596300ULL)));
#endif
}
STATIC VOID Ps5EdidPhaseFromVideo(UINTN Stage) {
  EFI_TPL Tpl=gBS->RaiseTPL(TPL_HIGH_LEVEL);gBS->RestoreTPL(Tpl);
  if(Tpl!=TPL_APPLICATION){DEBUG((DEBUG_ERROR,"PS5 HDMI EDID phase: skipped, video stage %u called at TPL %u\n",(UINT32)Stage,(UINT32)Tpl));return;}
  if(mVideoConfig.converter!=2){DEBUG((DEBUG_ERROR,"PS5 HDMI EDID phase: skipped, converter %u\n",(UINT32)mVideoConfig.converter));return;}
  /* r2 (15.11.): never skip for a missing ESP - the policy needs the EDID (else 1080p). The log stays
   * in the 64 KiB ring and is exported by the next snapshot once the ESP is mounted. */
  BOOLEAN EspMounted=EdidEspMounted();
  if(!EspMounted)DEBUG((DEBUG_ERROR,"PS5 HDMI EDID phase: pinned ESP not mounted yet - continuing, log kept in memory\n"));
  if(!UncachedMmio(MAILBOX) || !UncachedMmio(DOORBELL))return;
  mVideoBusy=TRUE;   /* keeps the 50 ms button poller off the ICC for the whole phase */
#ifdef PS5_BOOT_TS
  DEBUG((DEBUG_ERROR,"PS5 TS edid-phase-begin %Lu ms\n",(UINT64)(AsmReadTsc()/1596300ULL)));
#endif
  DEBUG((DEBUG_ERROR,"PS5 HDMI EDID phase: start before video stage %u (TPL_APPLICATION, ESP %a)\n",(UINT32)Stage,EspMounted?"mounted":"not yet mounted"));
  EdidFlush();
  struct ps5_icc_io Io={NULL,ReadIcc,WriteIcc,Ticks,Relax,TSC_HZ*12}  /* b1440d: wie Videostufen/Linux ICC_TIMEOUT 12 s */;
  UINT64 Start=AsmReadTsc();unsigned Stages=0;int Request=EdidRequest(&Io,&Stages);
  DEBUG((DEBUG_ERROR,"PS5 HDMI EDID request: result=%d acknowledged=%u/%u ms=%u\n",Request,Stages,PS5_FLAVA3_EDID_STAGES,EdidMs(AsmReadTsc()-Start)));
  UINT64 Window=AsmReadTsc(),Flushed=Window;
  UINT32 Updates=mEdid.updates,Dropped=ps5_icc_dropped.count,Polls=0,Rx=0,Errors=0,Progress=0;int Last=0,OffResult=1;
  BOOLEAN Cap=FALSE,Off=FALSE;
#ifdef PS5_EDID_TIMESTAMPS
  UINT32 SeenUpdates=mEdid.updates;
#endif
  BOOLEAN Resent=FALSE;
  while(AsmReadTsc()-Window<PS5_EDID_WINDOW_BUDGET) {
    if(Off && mEdid.updates!=Updates)break;                 /* EDID in hand and events off: done */
    if(Off && AsmReadTsc()-Window>=PS5_EDID_OFF_GIVEUP)break; /* r5.16.3: events off without EDID - old 4 s end */
    if(!Resent && !Off && Rx==0 && mEdid.updates==Updates && AsmReadTsc()-Window>=PS5_EDID_RESEND_AFTER) {
      Resent=TRUE;unsigned St=0;int Rq=EdidRequest(&Io,&St);
      DEBUG((DEBUG_ERROR,"PS5 HDMI EDID resend at %u ms: result=%d acknowledged=%u/%u\n",EdidMs(AsmReadTsc()-Window),Rq,St,PS5_FLAVA3_EDID_STAGES));
      EdidFlush();
    }
    if(Rx>=PS5_EDID_WINDOW_MESSAGES){Cap=TRUE;break;}
    struct ps5_edid_rx X={0,0,0};BOOLEAN Pending=FALSE;
    EFI_TPL Old=gBS->RaiseTPL(TPL_HIGH_LEVEL);BOOLEAN Ints=SaveAndDisableInterrupts();
    UINT64 Burst=AsmReadTsc();
    do {
      if(EdidPeek(&X)){Pending=TRUE;break;}
      int R=ps5_icc_poll(&Io,EdidRxNote,NULL);Polls++;     /* nothing pending: clears transport ACK bits */
      if(R<0){Errors++;Last=R;}
    } while(AsmReadTsc()-Burst<PS5_EDID_BURST);
    SetInterruptState(Ints);gBS->RestoreTPL(Old);
#ifdef PS5_EDID_TIMESTAMPS
    { UINT64 Bd=AsmReadTsc()-Burst;                         /* nvme-boot1: one burst hung ~22 s, then the EDID was there */
      if(Bd>=TSC_HZ/2){DEBUG((DEBUG_ERROR,"PS5 HDMI EDID ts: long burst %u ms ending at %u ms polls=%u errors=%u last=%d bytes=%u updates=%u\n",
        EdidMs(Bd),EdidMs(AsmReadTsc()-Window),Polls,Errors,Last,mEdid.bytes,mEdid.updates));EdidFlush();} }
    if(mEdid.updates!=SeenUpdates){SeenUpdates=mEdid.updates;
      DEBUG((DEBUG_ERROR,"PS5 HDMI EDID ts: update at %u ms bytes=%u updates=%u rx=%u pending=%u\n",EdidMs(AsmReadTsc()-Window),mEdid.bytes,mEdid.updates,Rx,(UINT32)Pending));EdidFlush();}
    if(Pending){DEBUG((DEBUG_ERROR,"PS5 HDMI EDID ts: rx %u at %u ms svc=%02x type=%04x len=%u\n",Rx+1,EdidMs(AsmReadTsc()-Window),X.svc,X.type,X.len));EdidFlush();}
#endif
    if(Pending) {
      Rx++;
      if(Rx<=PS5_EDID_RX_LOGGED) {
        DEBUG((DEBUG_ERROR,"PS5 ICC rx %u: pending svc=%02x type=%04x len=%u (body read follows)\n",Rx,X.svc,X.type,X.len));
        EdidFlush();                                          /* exported BEFORE the body is read */
      }
      UINT32 Notes=mEdidRxNote.calls,Drops=ps5_icc_dropped.count;
      Old=gBS->RaiseTPL(TPL_HIGH_LEVEL);Ints=SaveAndDisableInterrupts();
      int R=ps5_icc_poll(&Io,EdidRxNote,NULL);Polls++;
      SetInterruptState(Ints);gBS->RestoreTPL(Old);
      if(R<0){Errors++;Last=R;}
      if(Rx<=PS5_EDID_RX_LOGGED) {
        DEBUG((DEBUG_ERROR,"PS5 ICC rx %u: result=%d delivered=%u dropped=%u svc=%02x type=%04x bytes=%u\n",
          Rx,R,mEdidRxNote.calls-Notes,ps5_icc_dropped.count-Drops,mEdidRxNote.svc,mEdidRxNote.type,mEdidRxNote.bytes));
        DEBUG((DEBUG_ERROR,"PS5 ICC rx %u: data=%08x %08x EDID bytes=%u updates=%u\n",Rx,mEdidRxNote.d0,mEdidRxNote.d1,mEdid.bytes,mEdid.updates));
        EdidFlush();
      }
      if(!Off && (mEdid.updates!=Updates || Rx>=PS5_EDID_EVENTS_OFF_AFTER)) {
        DEBUG((DEBUG_ERROR,"PS5 HDMI events off: after rx %u (EDID bytes=%u)\n",Rx,mEdid.bytes));EdidFlush();
        Old=gBS->RaiseTPL(TPL_HIGH_LEVEL);Ints=SaveAndDisableInterrupts();
        OffResult=EdidEventsOffBounded(&Io);
        SetInterruptState(Ints);gBS->RestoreTPL(Old);
        Off=TRUE;
        DEBUG((DEBUG_ERROR,"PS5 HDMI events off: result=%d us=%u\n",OffResult,mEdidOffUs));EdidFlush();
      }
    }
    if(!Off && mEdid.updates!=Updates && mEdid.bytes>=256) {   /* r5.15 (edid-window-early-exit): the EDID can land through
                                                               * the burst poll's notification without a pending rx (r5.14.2: 256 B
                                                               * at 754 ms, the loop ran to the 5 s budget) */
      DEBUG((DEBUG_ERROR,"PS5 HDMI events off: EDID via poll (rx %u, EDID bytes=%u)\n",Rx,mEdid.bytes));EdidFlush();
      EFI_TPL Old2=gBS->RaiseTPL(TPL_HIGH_LEVEL);BOOLEAN Ints2=SaveAndDisableInterrupts();
      OffResult=EdidEventsOffBounded(&Io);
      SetInterruptState(Ints2);gBS->RestoreTPL(Old2);
      Off=TRUE;
      DEBUG((DEBUG_ERROR,"PS5 HDMI events off: result=%d us=%u\n",OffResult,mEdidOffUs));EdidFlush();
      break;
    }
    if(AsmReadTsc()-Flushed>=(Progress<6?TSC_HZ/4:TSC_HZ) && Progress<PS5_EDID_PROGRESS_MAX) {
      Progress++;
      DEBUG((DEBUG_ERROR,"PS5 HDMI EDID window %u: ms=%u polls=%u rx=%u EDID bytes=%u\n",Progress,EdidMs(AsmReadTsc()-Window),Polls,Rx,mEdid.bytes));
      EdidFlush();Flushed=AsmReadTsc();
    }
    CpuPause();
  }
  if(!Off) {
    DEBUG((DEBUG_ERROR,"PS5 HDMI events off: at window end (rx %u)\n",Rx));EdidFlush();
    EFI_TPL Old=gBS->RaiseTPL(TPL_HIGH_LEVEL);BOOLEAN Ints=SaveAndDisableInterrupts();
    OffResult=EdidEventsOffBounded(&Io);
    SetInterruptState(Ints);gBS->RestoreTPL(Old);
    DEBUG((DEBUG_ERROR,"PS5 HDMI events off: result=%d us=%u\n",OffResult,mEdidOffUs));
  }
  DEBUG((DEBUG_ERROR,"PS5 HDMI EDID window: ms=%u polls=%u rx=%u cap-hit=%u errors=%u last=%d\n",
    EdidMs(AsmReadTsc()-Window),Polls,Rx,(UINT32)Cap,Errors,Last));
  DEBUG((DEBUG_ERROR,"PS5 HDMI EDID result: EDID bytes=%u flags=%x updates=%u rejected=%u icc-tolerated=%u icc-dropped=%u\n",
    mEdid.bytes,mEdid.flags,mEdid.updates,mEdid.rejected,ps5_icc_tolerated.count,ps5_icc_dropped.count-Dropped));
  for(UINTN K=0;K<2;K++) {
    CONST struct ps5_icc_wire_note *N=K?&ps5_icc_dropped:&ps5_icc_tolerated;
    if(!N->count)continue;
    UINT32 Hd[4];for(UINTN J=0;J<4;J++)Hd[J]=N->head[4*J]|(UINT32)N->head[4*J+1]<<8|(UINT32)N->head[4*J+2]<<16|(UINT32)N->head[4*J+3]<<24;
    DEBUG((DEBUG_ERROR,"PS5 ICC first %a: wire-len=%u sum=%04x want=%04x\n",K?"dropped":"tolerated",N->wire_length,N->checksum,N->expected));
    DEBUG((DEBUG_ERROR,"PS5 ICC first %a head: %08x %08x %08x %08x\n",K?"dropped":"tolerated",Hd[0],Hd[1],Hd[2],Hd[3]));
  }
  EdidPublish();
  mVideoBusy=FALSE;
  EdidFlush();
}
#endif
EFI_STATUS EFIAPI Ps5HdmiProbeEntry(EFI_HANDLE ImageHandle,EFI_SYSTEM_TABLE *SystemTable) {
  UINTN Device=0x04005000;
  if(PciExpressRead32(Device)!=0x9107104D || !(PciExpressRead16(Device+4)&BIT1) ||
     PciExpressRead32(Device+0x18)!=0x8520000C || PciExpressRead32(Device+0x1C)!=0 ||
     PciExpressRead32(Device+0x20)!=0x8540000C || PciExpressRead32(Device+0x24)!=0) {
    DEBUG((DEBUG_INFO,"PS5 HDMI probe: skipped (SPCIE identity/BAR mismatch)\n"));
    return EFI_UNSUPPORTED;
  }
  if(!UncachedMmio(MAILBOX) || !UncachedMmio(DOORBELL)) {
    DEBUG((DEBUG_ERROR,"PS5 HDMI probe: skipped (MMIO not UC)\n"));
    return EFI_NOT_READY;
  }
  struct ps5_icc_io Io={NULL,ReadIcc,WriteIcc,Ticks,Relax,TSC_HZ};
  struct ps5_hdmi_config Config;
  // One synchronous BSP transaction; suppress event/interrupt reentry. No
  // callbacks survive this function. Native interrupt ownership is unverified.
  EFI_TPL OldTpl=gBS->RaiseTPL(TPL_HIGH_LEVEL);
  BOOLEAN Interrupts=SaveAndDisableInterrupts();
  int Result=ps5_hdmi_read_config_events(&Io,&Config,ps5_edid_notification,&mEdid);
  int PollResult=0;
#ifdef PS5_HDMI_EDID_REQUEST
  UINT32 EntryPollErrors=0;
#endif
  /* v3.23: "Enable SuperSpeed-USB-C port" exactly like PS5 Linux spcie_icc_init():
   * icc_usbc_set_pdcon_op_mode(0,5) = ICC service 0x12 (USBC), type 0x10, {port 0, mode 5}.
   * Without it the front USB-C port (keyboard/mouse dongle behind a VIA hub) stays
   * dead in UEFI and in Windows (v3.21/v3.22: no VIA hub/HID device enumerated). */
  /* v3.29: power LED "running" (white, dim) exactly like PS5 Linux spcie_icc_init():
   * icc_indicator_set_led_white(0) = ICC service 0x09 (INDICATOR), type 0x20, indicator_white_dim.
   * Without it the console keeps the GameOS hand-off pattern (pulsing blue = "still booting"). */
  int LedResult=-1;
  if(Result==0) {
    STATIC CONST UINT8 WhiteDim[25]={0x03,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x02,0x00,0x11,
                                     0x01,0x00,0x20,0x00,0x01,0x00,0x12,0x01,0x00,0x00,0x00,0x02,0x00};
    LedResult=ps5_icc_query_events(&Io,0x09,0x20,WhiteDim,sizeof(WhiteDim),NULL,0,NULL,ps5_edid_notification,&mEdid);
  }
  int UsbcResult=-1;
  if(Result==0) {
    CONST UINT8 Usbc[2]={0,5};
    UsbcResult=ps5_icc_query_events(&Io,0x12,0x10,Usbc,sizeof(Usbc),NULL,0,NULL,ps5_edid_notification,&mEdid);
  }
#ifdef PS5_HDMI_PREPARE
  unsigned Completed=0;
  if(Result==0) {
    DEBUG((DEBUG_INFO,"PS5 HDMI config raw: %02x %02x %02x %02x %02x %02x %02x %02x converter=%u\n",
      Config.raw[0],Config.raw[1],Config.raw[2],Config.raw[3],Config.raw[4],Config.raw[5],Config.raw[6],Config.raw[7],Config.converter));
    Result=ps5_flava3_prepare(&Io,&Config,&Completed,ps5_edid_notification,&mEdid);
    DEBUG((DEBUG_ERROR,"PS5 HDMI prepare: result=%d acknowledged=%u/3; DCN/link/video timing not initialized\n",Result,Completed));
    if(Result==0)mButtonEnabled=Ps5ButtonEnable(&Io);
  }
#endif
  if(Result==0) {
    UINT64 Start=AsmReadTsc();
    while(AsmReadTsc()-Start<TSC_HZ/10) {
      PollResult=ps5_icc_poll(&Io,ps5_edid_notification,&mEdid);
#ifdef PS5_HDMI_EDID_REQUEST
      /* edid2/3: a poll error never unloads the driver (v1, 14.11. 05:17: poll=-4 -> no video service -> no GOP). */
      if(PollResult<0){EntryPollErrors++;PollResult=0;}
#endif
      if(PollResult<0) break;
      CpuPause();
    }
  }
  SetInterruptState(Interrupts);
  gBS->RestoreTPL(OldTpl);
  DEBUG((DEBUG_ERROR,"PS5 HDMI probe: query=%d poll=%d EDID bytes=%u flags=%x updates=%u rejected=%u\n",
    Result,PollResult,mEdid.bytes,mEdid.flags,mEdid.updates,mEdid.rejected));
#ifdef PS5_HDMI_EDID_REQUEST
  DEBUG((DEBUG_ERROR,"PS5 HDMI probe entry: poll-errors=%u icc-tolerated=%u icc-dropped=%u (EDID request deferred to the first BDS video stage)\n",
    EntryPollErrors,ps5_icc_tolerated.count,ps5_icc_dropped.count));
#endif
  DEBUG((DEBUG_INFO,"PS5 USB-C: pdcon op mode port0=5 result=%d\n",UsbcResult));
  DEBUG((DEBUG_INFO,"PS5 LED: white dim (service 9 type 0x20) result=%d\n",LedResult));
  if(Result==0) DEBUG((DEBUG_INFO,"PS5 HDMI probe: converter=%u TMDS polarity=%u swap=%u DP polarity=%u swap=%u raw-prefix=%02x/%02x/%02x\n",
    Config.converter,Config.tmds_polarity,Config.tmds_swap,Config.dp_polarity,Config.dp_swap,
    Config.raw[0],Config.raw[1],Config.raw[2]));
#ifdef PS5_HDMI_PREPARE
  if(Result==0 && PollResult>=0) {
    mVideoConfig=Config;
    EFI_HANDLE VideoHandle=NULL;
    EFI_STATUS Install=gBS->InstallProtocolInterface(&VideoHandle,&mPs5HdmiVideoGuid,EFI_NATIVE_INTERFACE,&mVideo);
    DEBUG((DEBUG_INFO,"PS5 HDMI fixed video service: %r\n",Install));
    DEBUG((DEBUG_INFO,"PS5 button: enable power notification=%d\n",mButtonEnabled));
    Ps5ButtonInputStart(); /* v2.53: always; the timer retries enabling */
  }
#endif
  return Result==0 && PollResult>=0 ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}
