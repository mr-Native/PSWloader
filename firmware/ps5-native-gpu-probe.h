/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Read-only DCN201 state through the real UEFI PCI I/O protocol.
 * Never publish GOP from an unverified address or initialize guessed registers.
 */
#include <Protocol/PciIo.h>
#include "dcn201-scanout.h"
#include "dcn201-memory.h"
#include "ps5-native-framebuffer.h"
#include "ps5-gpu-aperture.h"
#include "ps5-native-page-map.h"
STATIC EFI_PHYSICAL_ADDRESS mPs5NativeFramebuffer;
STATIC VOID Ps5GpuSnapshot(VOID) {
  EFI_GUID Snapshot={0x873f7798,0xe801,0x4ec4,{0x86,0x97,0x30,0xb7,0xb3,0xa1,0xfa,0x52}};
  EfiEventGroupSignal(&Snapshot);
}
STATIC VOID Ps5FramebufferCheckpoint(UINT32 Phase,EFI_PHYSICAL_ADDRESS Address) {
  DEBUG((DEBUG_INFO,"PS5 native framebuffer checkpoint=%u CPU=%Lx\n",Phase,Address));
  Ps5GpuSnapshot();
}
STATIC VOID Ps5NativeGpuProbe(VOID) {
  EFI_HANDLE *Handles=NULL;UINTN Count=0;
  EFI_STATUS Status=gBS->LocateHandleBuffer(ByProtocol,&gEfiPciIoProtocolGuid,NULL,&Count,&Handles);
  if(EFI_ERROR(Status))return;
  for(UINTN I=0;I<Count;I++) {
    EFI_PCI_IO_PROTOCOL *Pci=NULL;UINT32 Id=0;UINT16 Command=0;
    Status=gBS->HandleProtocol(Handles[I],&gEfiPciIoProtocolGuid,(VOID **)&Pci);
    if(EFI_ERROR(Status))continue;
    Status=Pci->Pci.Read(Pci,EfiPciIoWidthUint32,0,1,&Id);
    if(EFI_ERROR(Status) || Id!=0x13fc1002U)continue;
    Status=Pci->Pci.Read(Pci,EfiPciIoWidthUint16,4,1,&Command);
    if(EFI_ERROR(Status) || !(Command&2U))continue;
    // MMHUB segment0 0x1a000 + mmMMMC_VM_* from Linux DCN201 resource.
    CONST UINT32 MemoryOffsets[3]={0x6a1b0,0x6a1b4,0x6a15c};
    UINT32 MemoryValues[3]={0};BOOLEAN MemoryValid=TRUE;
    for(UINTN N=0;N<3;N++) {
      DEBUG((DEBUG_INFO,"PS5 native MC read intent: index=%u offset=%x\n",N,MemoryOffsets[N]));
      Ps5GpuSnapshot();
      Status=Pci->Mem.Read(Pci,EfiPciIoWidthUint32,5,MemoryOffsets[N],1,&MemoryValues[N]);
      DEBUG((DEBUG_INFO,"PS5 native MC: index=%u offset=%x status=%r value=%x\n",N,MemoryOffsets[N],Status,EFI_ERROR(Status)?0:MemoryValues[N]));
      if(EFI_ERROR(Status))MemoryValid=FALSE;
    }
    if(MemoryValid) {
      struct ps5_dcn201_memory Memory;
      int Result=ps5_dcn201_memory_decode(MemoryValues[0],MemoryValues[1],MemoryValues[2],&Memory);
      DEBUG((DEBUG_INFO,"PS5 native MC decode=%d\n",Result));
      if(Result==0)DEBUG((DEBUG_INFO,"PS5 native MC: FB=[%Lx,%Lx) UMA=[%Lx,%Lx); CPU mapping NOT verified\n",
        Memory.fb_base,Memory.fb_end,Memory.uma_base,Memory.uma_end));
      if(Result==0) {
        UINT32 GfxOffset=0;
        DEBUG((DEBUG_INFO,"PS5 native GFXHUB read intent: offset=A5AC\n"));
        Ps5GpuSnapshot();
        Status=Pci->Mem.Read(Pci,EfiPciIoWidthUint32,5,0xa5ac,1,&GfxOffset);
        DEBUG((DEBUG_INFO,"PS5 native GFXHUB: status=%r FB_OFFSET=%x; APU CPU aperture candidate=%Lx\n",Status,EFI_ERROR(Status)?0:GfxOffset,EFI_ERROR(Status)?0:((UINT64)GfxOffset<<24)));
        if(!EFI_ERROR(Status) && GfxOffset<=0xffffff && ((UINT64)GfxOffset<<24)==Memory.uma_base &&
           Memory.uma_end-Memory.uma_base>=PS5_FRAMEBUFFER_BYTES) {
          UINT64 Candidate=(Memory.uma_end-PS5_FRAMEBUFFER_BYTES)&~65535ULL;
          DEBUG((DEBUG_INFO,"PS5 native APU page check intent: CPU=%Lx bytes=%u\n",Candidate,PS5_FRAMEBUFFER_BYTES));
          Ps5GpuSnapshot();
          EFI_STATUS PageStatus=Ps5NativeFramebufferPageCheck(Candidate,PS5_FRAMEBUFFER_BYTES);
          DEBUG((DEBUG_INFO,"PS5 native APU page check=%r; identity/RW only, ownership/cache/scanout NOT verified\n",PageStatus));
          Ps5GpuSnapshot();
        }
        struct ps5_gpu_cpu_aperture Aperture;
        Status=Ps5GpuCpuAperture(Pci,0,&Aperture);
        DEBUG((DEBUG_INFO,"PS5 native aperture: BAR0 status=%r\n",Status));
        if(!EFI_ERROR(Status)) {
          DEBUG((DEBUG_INFO,"PS5 native aperture: CPU base=%Lx bytes=%Lx; access/ownership not verified\n",Aperture.Base,Aperture.Bytes));
          CONST UINT32 HdpOffsets[2]={0x3d80,0x3d88};UINT32 Hdp[2]={0};BOOLEAN HdpValid=TRUE;
          for(UINTN N=0;N<2;N++) {
            DEBUG((DEBUG_INFO,"PS5 native HDP read intent: index=%u offset=%x\n",N,HdpOffsets[N]));
            Ps5GpuSnapshot();
            Status=Pci->Mem.Read(Pci,EfiPciIoWidthUint32,5,HdpOffsets[N],1,&Hdp[N]);
            DEBUG((DEBUG_INFO,"PS5 native HDP: index=%u status=%r value=%x\n",N,Status,EFI_ERROR(Status)?0:Hdp[N]));
            if(EFI_ERROR(Status))HdpValid=FALSE;
          }
          if(HdpValid) {
            struct ps5_dcn201_aperture_plan Plan;
            int Planned=ps5_dcn201_aperture_framebuffer(&Memory,Aperture.Base,Aperture.Bytes,Hdp[0],Hdp[1],PS5_FRAMEBUFFER_BYTES,&Plan);
            DEBUG((DEBUG_INFO,"PS5 native aperture plan=%d\n",Planned));
            if(Planned==0)DEBUG((DEBUG_INFO,"PS5 native aperture candidate: CPU=%Lx surface=%Lx bytes=%Lu; NO VRAM writes/ownership claim\n",Plan.cpu_address,Plan.surface_address,Plan.bytes));
          }
        }
        Ps5GpuSnapshot();
        Status=mPs5NativeFramebuffer?EFI_ALREADY_STARTED:Ps5NativeFramebufferReserve(&Memory,&mPs5NativeFramebuffer,Ps5FramebufferCheckpoint);
        DEBUG((DEBUG_INFO,"PS5 native framebuffer: reserve=%r CPU=%Lx bytes=%u; DCN/GOP not initialized\n",Status,mPs5NativeFramebuffer,PS5_FRAMEBUFFER_BYTES));
      }
    }
    // Inventory all four HUBPs and both OTGs before interpreting pipe 0.
    CONST UINT32 InventoryOffsets[18]={0xeacc,0xeb28,0xeb2c,0xee3c,0xee98,0xee9c,0xf1ac,0xf208,0xf20c,0xf51c,0xf578,0xf57c,0x14004,0x13fac,0x13fd8,0x14204,0x141ac,0x141d8};
    for(UINTN N=0;N<18;N++) {
      UINT32 Value=0;
      Status=Pci->Mem.Read(Pci,EfiPciIoWidthUint32,5,InventoryOffsets[N],1,&Value);
      DEBUG((DEBUG_INFO,"PS5 native DCN inventory: index=%u offset=%x status=%r value=%x\n",N,InventoryOffsets[N],Status,EFI_ERROR(Status)?0:Value));
    }
    // Byte offsets derived from DMU_SEG2 0x34c0 + DCN_2_0_3 definitions.
    CONST UINT32 Offsets[10]={0xeacc,0x14004,0xea94,0xea9c,0xeb68,0xeb1c,0xeb28,0xeb2c,0x13fac,0x13fd8};
    UINT32 Values[10];BOOLEAN Valid=TRUE;
    for(UINTN N=0;N<10;N++) {
      Status=Pci->Mem.Read(Pci,EfiPciIoWidthUint32,5,Offsets[N],1,&Values[N]);
      DEBUG((DEBUG_INFO,"PS5 native DCN: BAR5 offset=%x status=%r value=%x\n",Offsets[N],Status,EFI_ERROR(Status)?0:Values[N]));
      if(EFI_ERROR(Status)){Valid=FALSE;break;}
    }
    if(Valid) {
      struct ps5_dcn201_state S={Values[0],Values[1],Values[2],Values[3],Values[4],Values[5],Values[6],Values[7],Values[8],Values[9]};
      struct ps5_linear_scanout Out;
      int Result=ps5_dcn201_linear_scanout(&S,&Out);
      DEBUG((DEBUG_INFO,"PS5 native DCN decode=%d\n",Result));
      if(Result==0)DEBUG((DEBUG_INFO,"PS5 native DCN candidate: surface=%Lx bytes=%Lu width=%u height=%u pitch=%u; CPU mapping and HDMI link NOT verified\n",
        Out.surface_address,Out.bytes,Out.width,Out.height,Out.bytes_per_line));
    }
    break;
  }
  FreePool(Handles);
  EFI_GUID Snapshot={0x873f7798,0xe801,0x4ec4,{0x86,0x97,0x30,0xb7,0xb3,0xa1,0xfa,0x52}};
  EfiEventGroupSignal(&Snapshot);
}
