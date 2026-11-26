/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PS5_NATIVE_PAGE_MAP_H
#define PS5_NATIVE_PAGE_MAP_H
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include "framebuffer-page-map.h"
struct ps5_page_map_context {EFI_MEMORY_DESCRIPTOR *Map;UINTN Bytes,Stride;};
/* The platform firmware uses identity-mapped RAM. Limit table reads to its
 * low 4GiB RAM; never dereference arbitrary MMIO/reserved/target VRAM pages.
 * No generic nonidentity UEFI environment is supported by this callback. */
STATIC int Ps5ReadPageEntry(void *Context,uint64_t Address,uint64_t *Out) {
  struct ps5_page_map_context *C=Context;
  if(!C || !Out || (Address&7) || Address<0x1000 || Address>0xfffffff8ULL)return -1;
  for(UINTN Offset=0;Offset<C->Bytes;Offset+=C->Stride) {
    EFI_MEMORY_DESCRIPTOR *D=(VOID *)((UINT8 *)C->Map+Offset);
    if(D->Type!=EfiLoaderCode && D->Type!=EfiLoaderData && D->Type!=EfiBootServicesCode &&
       D->Type!=EfiBootServicesData && D->Type!=EfiRuntimeServicesCode &&
       D->Type!=EfiRuntimeServicesData && D->Type!=EfiConventionalMemory)continue;
    if(D->PhysicalStart>Address || D->NumberOfPages>(MAX_UINT64>>EFI_PAGE_SHIFT))continue;
    UINT64 Size=D->NumberOfPages<<EFI_PAGE_SHIFT;
    if(Size<8 || Address-D->PhysicalStart>Size-8)continue;
    *Out=*(volatile UINT64 *)(UINTN)Address;return 0;
  }
  return -1;
}
STATIC EFI_STATUS Ps5NativeFramebufferPageCheck(UINT64 Address,UINT64 Size) {
  if(AsmReadCr4()&BIT12)return EFI_UNSUPPORTED; // LA57 uses five levels
  UINTN Bytes=0,Key=0,Stride=0;UINT32 Version=0;
  EFI_STATUS Status=gBS->GetMemoryMap(&Bytes,NULL,&Key,&Stride,&Version);
  if(Status!=EFI_BUFFER_TOO_SMALL || Stride<sizeof(EFI_MEMORY_DESCRIPTOR) ||
     Stride>4096 || Bytes>0x200000-8*Stride)return EFI_COMPROMISED_DATA;
  for(UINTN Attempt=0;Attempt<3;Attempt++) {
    UINTN Capacity=Bytes+8*Stride;
    EFI_MEMORY_DESCRIPTOR *Map=AllocatePool(Capacity);
    if(!Map)return EFI_OUT_OF_RESOURCES;
    Bytes=Capacity;Status=gBS->GetMemoryMap(&Bytes,Map,&Key,&Stride,&Version);
    if(Status==EFI_BUFFER_TOO_SMALL) {
      FreePool(Map);
      if(Stride<sizeof(EFI_MEMORY_DESCRIPTOR) || Stride>4096 || Bytes>0x200000-8*Stride)return EFI_COMPROMISED_DATA;
      continue;
    }
    if(EFI_ERROR(Status)){FreePool(Map);return Status;}
    if(Stride<sizeof(EFI_MEMORY_DESCRIPTOR) || Version!=EFI_MEMORY_DESCRIPTOR_VERSION ||
       Bytes>Capacity || Bytes%Stride){FreePool(Map);return EFI_COMPROMISED_DATA;}
    struct ps5_page_map_context Context={Map,Bytes,Stride};
    int Result=ps5_framebuffer_identity_writable(AsmReadCr3(),Address,Size,Ps5ReadPageEntry,&Context);
    FreePool(Map);
    return Result==0?EFI_SUCCESS:EFI_NO_MAPPING;
  }
  return EFI_ABORTED;
}
#endif
