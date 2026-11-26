/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PS5_NATIVE_FRAMEBUFFER_H
#define PS5_NATIVE_FRAMEBUFFER_H
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include "dcn201-memory.h"
#define PS5_FRAMEBUFFER_BYTES (1920U*1080U*4U)
/* CPU RAM reservation only. Does not program DCN or publish GOP. Keep the
 * reserved pages owned until a later real graphics driver takes ownership. */
STATIC EFI_STATUS Ps5NativeFramebufferReserve(const struct ps5_dcn201_memory *Memory,
                                             EFI_PHYSICAL_ADDRESS *Output,
                                             VOID (*Checkpoint)(UINT32,EFI_PHYSICAL_ADDRESS)) {
  if(!Memory || !Output)return EFI_INVALID_PARAMETER;
  UINTN Bytes=0,Key=0,Stride=0;UINT32 Version=0;
  EFI_STATUS Status=gBS->GetMemoryMap(&Bytes,NULL,&Key,&Stride,&Version);
  if(Status!=EFI_BUFFER_TOO_SMALL || Stride<sizeof(EFI_MEMORY_DESCRIPTOR) ||
     Stride>4096 || Bytes>0x200000-8*Stride)return EFI_COMPROMISED_DATA;
  for(UINTN Attempt=0;Attempt<3;Attempt++) {
    UINTN Capacity=Bytes+8*Stride;
    EFI_MEMORY_DESCRIPTOR *Map=AllocatePool(Capacity);
    if(Map==NULL)return EFI_OUT_OF_RESOURCES;
    Bytes=Capacity;
    Status=gBS->GetMemoryMap(&Bytes,Map,&Key,&Stride,&Version);
    if(Status==EFI_BUFFER_TOO_SMALL) {
      FreePool(Map);
      if(Stride<sizeof(EFI_MEMORY_DESCRIPTOR) || Stride>4096 || Bytes>0x200000-8*Stride)return EFI_COMPROMISED_DATA;
      continue;
    }
    if(EFI_ERROR(Status)){FreePool(Map);return Status;}
    if(Stride<sizeof(EFI_MEMORY_DESCRIPTOR) || Version!=EFI_MEMORY_DESCRIPTOR_VERSION ||
       Bytes>Capacity || Bytes%Stride){FreePool(Map);return EFI_COMPROMISED_DATA;}
    EFI_PHYSICAL_ADDRESS Candidate=0;UINTN Overlaps=0;
    for(UINTN Offset=0;Offset<Bytes;Offset+=Stride) {
      EFI_MEMORY_DESCRIPTOR *D=(VOID *)((UINT8 *)Map+Offset);
      if(D->NumberOfPages<=(~0ULL>>EFI_PAGE_SHIFT)) {
        UINT64 Size=D->NumberOfPages<<EFI_PAGE_SHIFT;
        if(Size && D->PhysicalStart<=MAX_UINT64-Size &&
           D->PhysicalStart<Memory->uma_end && D->PhysicalStart+Size>Memory->uma_base) {
          Overlaps++;
          DEBUG((DEBUG_INFO,"PS5 native UMA map: type=%u base=%Lx pages=%Lx attr=%Lx\n",D->Type,D->PhysicalStart,D->NumberOfPages,D->Attribute));
        }
      }
      if(D->Type!=EfiConventionalMemory || (D->Attribute&EFI_MEMORY_RUNTIME) ||
         (D->PhysicalStart&EFI_PAGE_MASK) || D->NumberOfPages>(~0ULL>>EFI_PAGE_SHIFT))continue;
      uint64_t Address=0;
      if(ps5_dcn201_framebuffer_candidate(Memory,D->PhysicalStart,
         D->NumberOfPages<<EFI_PAGE_SHIFT,PS5_FRAMEBUFFER_BYTES,&Address)==0 && Address>Candidate)Candidate=Address;
    }
    if(Candidate==0){DEBUG((DEBUG_INFO,"PS5 native UMA: eligible RAM absent; overlapping descriptors=%u\n",Overlaps));FreePool(Map);return EFI_NOT_FOUND;}
    if(Checkpoint)Checkpoint(1,Candidate);
    Status=gBS->AllocatePages(AllocateAddress,EfiReservedMemoryType,
                              EFI_SIZE_TO_PAGES(PS5_FRAMEBUFFER_BYTES),&Candidate);
    FreePool(Map);
    if(EFI_ERROR(Status))return Status;
    if(Checkpoint)Checkpoint(2,Candidate);
    SetMem((VOID *)(UINTN)Candidate,PS5_FRAMEBUFFER_BYTES,0);
    if(Checkpoint)Checkpoint(3,Candidate);
    *Output=Candidate;return EFI_SUCCESS;
  }
  return EFI_ABORTED;
}
#endif
