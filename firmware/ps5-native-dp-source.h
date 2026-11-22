/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Experimental, explicitly selected native DP source bring-up. No GOP claim.
 * Platform ROM CRC pins the previously captured topology and command revision;
 * this is a consistency check, not authentication of an untrusted option ROM.
 */
#include "atom-native.h"
#include "dcn201-link.h"
#ifdef PS5_NATIVE_DP_VIDEO
#include "ps5-native-dp-video.h"
#endif
STATIC BOOLEAN Ps5IsOberonGpu(UINT32 Id) {
  return Id==0x13fc1002U || Id==0x13db1002U || Id==0x13dc1002U ||
         Id==0x13da1002U || Id==0x13f91002U || Id==0x13fa1002U || Id==0x13fb1002U;
}
STATIC EFI_STATUS Ps5NativeDpSource(VOID) {
  UINT8 *Rom=AllocateZeroPool(65536);
  if(!Rom)return EFI_OUT_OF_RESOURCES;
  volatile UINT8 *Source=(VOID *)(UINTN)0xc0000;
  /* Validate AMD ATOM VBIOS dynamically so all PS5 Fat / Slim / Pro revisions work. */
  if(Source[0]!=0x55 || Source[1]!=0xaa){FreePool(Rom);return EFI_COMPROMISED_DATA;}
  UINTN Length=(UINTN)Source[2]*512;
  if(Length<0x200 || Length>65536){FreePool(Rom);return EFI_COMPROMISED_DATA;}
  for(UINTN I=0;I<Length;I++)Rom[I]=Source[I];
  UINT16 Header=(UINT16)((UINT16)Rom[0x48]|((UINT16)Rom[0x49]<<8));
  if(Header>Length-36 || Rom[Header+4]!='A' || Rom[Header+5]!='T' ||
     Rom[Header+6]!='O' || Rom[Header+7]!='M'){FreePool(Rom);return EFI_COMPROMISED_DATA;}
  EFI_STATUS Status;
  EFI_HANDLE *Handles=NULL;UINTN Count=0;
  Status=gBS->LocateHandleBuffer(ByProtocol,&gEfiPciIoProtocolGuid,NULL,&Count,&Handles);
  if(EFI_ERROR(Status)){FreePool(Rom);return Status;}
  Status=EFI_NOT_FOUND;
  for(UINTN I=0;I<Count;I++) {
    EFI_PCI_IO_PROTOCOL *Pci=NULL;UINT32 Id=0;UINT16 Command=0;
    if(EFI_ERROR(gBS->HandleProtocol(Handles[I],&gEfiPciIoProtocolGuid,(VOID **)&Pci)) ||
       EFI_ERROR(Pci->Pci.Read(Pci,EfiPciIoWidthUint32,0,1,&Id)) || !Ps5IsOberonGpu(Id))continue;
    Status=Pci->Pci.Read(Pci,EfiPciIoWidthUint16,4,1,&Command);
    if(EFI_ERROR(Status) || !(Command&2)){Status=EFI_NOT_READY;break;}
    UINT32 O[2]={0},H[2]={0};
    for(UINTN N=0;N<2;N++) {
      Status=Pci->Mem.Read(Pci,EfiPciIoWidthUint32,5,0x14004+0x200*N,1,&O[N]);
      if(EFI_ERROR(Status))break;
      Status=Pci->Mem.Read(Pci,EfiPciIoWidthUint32,5,0x14f50+0x20*N,1,&H[N]);
      if(EFI_ERROR(Status))break;
    }
    if(EFI_ERROR(Status))break;
    DEBUG((DEBUG_INFO,"PS5 DP source: OTG=%x/%x HPD=%x/%x\n",O[0],O[1],H[0],H[1]));
    /* r5.25.4 warm re-entry (restart without GameOS): the previous firmware run left its timing generator running.
     * Only then (marker written by the re-entry trampoline, read once) it is switched off, so the pipeline is built
     * again exactly as after GameOS. */
    { volatile UINT64 *Warm=(volatile UINT64 *)(UINTN)0x111806020ULL;
      if(((O[0]|O[1])&1) && *Warm==0x544F4F424D524157ULL) {
        *Warm=0;
        for(UINTN N=0;N<2;N++) if(O[N]&1){UINT32 V=O[N]&~1U;Pci->Mem.Write(Pci,EfiPciIoWidthUint32,5,0x14004+0x200*N,1,&V);}
        gBS->Stall(50000);
        /* and the previous run's DP video stream (DP_VID_STREAM_CNTL 0x15730: bit 0 enable, bit 16 status) - the stream
         * setup refuses a live one (HW 19.11.: stream attributes=-4, no GOP) */
        { UINT32 V=0;
          if(!EFI_ERROR(Pci->Mem.Read(Pci,EfiPciIoWidthUint32,5,0x15730,1,&V)) && (V&0x10001)) {
            V&=~1U; Pci->Mem.Write(Pci,EfiPciIoWidthUint32,5,0x15730,1,&V);
            for(UINTN T=0;T<50;T++){gBS->Stall(1000);if(EFI_ERROR(Pci->Mem.Read(Pci,EfiPciIoWidthUint32,5,0x15730,1,&V)) || !(V&0x10000))break;}
            DEBUG((DEBUG_ERROR,"PS5 DP source: warm re-entry - previous DP video stream stopped, 15730=%x\n",V));
          } }
        for(UINTN N=0;N<2;N++) Pci->Mem.Read(Pci,EfiPciIoWidthUint32,5,0x14004+0x200*N,1,&O[N]);
        DEBUG((DEBUG_ERROR,"PS5 DP source: warm re-entry - previous timing generator stopped, OTG=%x/%x\n",O[0],O[1]));
      } }
    /* Never overwrite a live timing pipeline or guess between two sinks. */
    if((O[0]|O[1])&1){Status=EFI_ALREADY_STARTED;break;}
    if(((H[0]&2)!=0)==((H[1]&2)!=0)){Status=EFI_NOT_READY;break;}
    unsigned Pin=(H[0]&2)?1:2;
    struct ps5_dcn201_link Link;
    struct ps5_dcn201_register_write Plan[3];
    UINT32 Params[8]={0};
    if(ps5_dcn201_link_decode(0x3013+(Pin<<8),0x201eU+(Pin<<8),Pin,&Link) ||
       ps5_dcn201_link_plan(&Link,4,Plan) ||
       ps5_dcn201_transmitter_params(&Link,7,4,0,Params)){Status=EFI_INVALID_PARAMETER;break;}
#ifdef PS5_NATIVE_DP_VIDEO
    Status=Ps5NativeDpReset(Pci,Rom,&Link);
    if(EFI_ERROR(Status))break;
    Status=Ps5NativeDpArmTiming(Pci,Rom);
    if(EFI_ERROR(Status))break;
#endif
    DEBUG((DEBUG_INFO,"PS5 DP source: INIT PHY=%u HPD=%u; native ATOM, no VM\n",Link.phy,Link.hpd));
    Ps5GpuSnapshot();
    Status=Ps5AtomExecute(Pci,Rom,65536,76,Params,8);
    if(EFI_ERROR(Status))break;
    for(UINTN N=0;N<3;N++) {
      UINT32 Before=0,After=0;
      Status=Pci->Mem.Read(Pci,EfiPciIoWidthUint32,5,Plan[N].offset,1,&Before);
      if(EFI_ERROR(Status))break;
      After=(Before&~Plan[N].mask)|(Plan[N].value&Plan[N].mask);
      Status=Pci->Mem.Write(Pci,EfiPciIoWidthUint32,5,Plan[N].offset,1,&After);
      if(EFI_ERROR(Status))break;
      DEBUG((DEBUG_INFO,"PS5 DP source: offset=%x before=%x after=%x\n",Plan[N].offset,Before,After));
    }
    if(EFI_ERROR(Status))break;
    ZeroMem(Params,sizeof(Params));
    if(ps5_dcn201_transmitter_params(&Link,1,4,81000,Params)){Status=EFI_INVALID_PARAMETER;break;}
#ifdef PS5_NATIVE_DP_VIDEO
    DEBUG((DEBUG_INFO,"PS5 DP source: ENABLE HBR3 x4 intent; OTG timing already armed\n"));
    {
      UINT32 Phy=0,Ack=0,Dig=0;
      Status=Pci->Mem.Read(Pci,EfiPciIoWidthUint32,5,0x17800,1,&Phy);
      if(!EFI_ERROR(Status))Status=Pci->Mem.Read(Pci,EfiPciIoWidthUint32,5,0x1780c,1,&Ack);
      if(!EFI_ERROR(Status))Status=Pci->Mem.Read(Pci,EfiPciIoWidthUint32,5,0x155c0,1,&Dig);
      if(EFI_ERROR(Status))break;
      DEBUG((DEBUG_INFO,"PS5 DP source: PHY_CNTL0 17800=%x 1780C=%x 155c0=%x\n",Phy,Ack,Dig));
    }
#else
    DEBUG((DEBUG_INFO,"PS5 DP source: ENABLE HBR3 x4 intent; stream/timing not yet started\n"));
#endif
    Ps5GpuSnapshot();
    Status=Ps5AtomExecute(Pci,Rom,65536,76,Params,8);
#ifdef PS5_NATIVE_DP_VIDEO
    {
      UINT32 Phy=0,Ack=0,Sym=0,Clk=0,Dig=0;
      EFI_STATUS Log=Pci->Mem.Read(Pci,EfiPciIoWidthUint32,5,0x17800,1,&Phy);
      if(!EFI_ERROR(Log))Log=Pci->Mem.Read(Pci,EfiPciIoWidthUint32,5,0x1780c,1,&Ack);
      if(!EFI_ERROR(Log))Log=Pci->Mem.Read(Pci,EfiPciIoWidthUint32,5,0x177A0,1,&Sym);
      if(!EFI_ERROR(Log))Log=Pci->Mem.Read(Pci,EfiPciIoWidthUint32,5,0x580,1,&Clk);
      if(!EFI_ERROR(Log))Log=Pci->Mem.Read(Pci,EfiPciIoWidthUint32,5,0x155c0,1,&Dig);
      DEBUG((DEBUG_INFO,"PS5 DP source: after ENABLE 17800=%x 1780C=%x 177A0=%x 580=%x 155c0=%x DIG_ENABLE=%x\n",Phy,Ack,Sym,Clk,Dig,Dig));
      if(!EFI_ERROR(Status)&&EFI_ERROR(Log))Status=Log;
    }
#endif
    if(!EFI_ERROR(Status)) {
      /* The platform ATOM table clears DIG_FE_SOURCE_SELECT during ENABLE.
       * AMD connects BE to FE separately, after transmitter enable. Restore
       * the selected routing here, not only before the command. */
      UINT32 Before=0,After=0,Readback=0;
      Status=Pci->Mem.Read(Pci,EfiPciIoWidthUint32,5,Plan[0].offset,1,&Before);
      if(!EFI_ERROR(Status)) {
        After=(Before&~Plan[0].mask)|(Plan[0].value&Plan[0].mask);
        Status=Pci->Mem.Write(Pci,EfiPciIoWidthUint32,5,Plan[0].offset,1,&After);
      }
      if(!EFI_ERROR(Status))Status=Pci->Mem.Read(Pci,EfiPciIoWidthUint32,5,Plan[0].offset,1,&Readback);
      DEBUG((DEBUG_INFO,"PS5 DP source: route after ATOM before=%x target=%x readback=%x status=%r\n",Before,After,Readback,Status));
      if(!EFI_ERROR(Status) && (Readback&Plan[0].mask)!=(Plan[0].value&Plan[0].mask))Status=EFI_DEVICE_ERROR;
      Ps5GpuSnapshot();
    }
#ifdef PS5_NATIVE_DP_VIDEO
    if(!EFI_ERROR(Status))Status=Ps5NativeDpVideo(Pci,Rom,Link.phy);
#endif
    break; /* One attempt, no replay after a partially applied command. */
  }
  FreePool(Handles);FreePool(Rom);
  DEBUG((DEBUG_INFO,"PS5 DP source: finished %r; no GOP or visible output asserted\n",Status));
  Ps5GpuSnapshot();return Status;
}
