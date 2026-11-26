/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PS5_GPU_APERTURE_H
#define PS5_GPU_APERTURE_H
#include <Uefi.h>
#include <Protocol/PciIo.h>
#include <IndustryStandard/Acpi.h>
#include <Library/MemoryAllocationLib.h>
struct ps5_gpu_cpu_aperture {UINT64 Base,Bytes;};
/* GetBarAttributes describes an aperture, not ownership of its VRAM contents.
 * Nonzero host translation is deliberately unsupported. No BAR changes. */
STATIC EFI_STATUS Ps5GpuCpuAperture(EFI_PCI_IO_PROTOCOL *Pci,UINT8 Bar,
                                    struct ps5_gpu_cpu_aperture *Out) {
  if(!Pci || !Out || Bar>=6)return EFI_INVALID_PARAMETER;
  VOID *Resources=NULL;UINT64 Supports=0;
  EFI_STATUS Status=Pci->GetBarAttributes(Pci,Bar,&Supports,&Resources);
  if(EFI_ERROR(Status)){if(Resources)FreePool(Resources);return Status;}
  if(Resources==NULL)return EFI_COMPROMISED_DATA;
  EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *D=Resources;
  if(D->Desc!=ACPI_ADDRESS_SPACE_DESCRIPTOR || D->Len!=sizeof(*D)-3 ||
     D->ResType!=ACPI_ADDRESS_SPACE_TYPE_MEM || D->AddrTranslationOffset!=0 ||
     !D->AddrLen || D->AddrRangeMin<0x100000 || D->AddrRangeMin>MAX_UINT64-D->AddrLen) {
    FreePool(Resources);return EFI_UNSUPPORTED;
  }
  struct ps5_gpu_cpu_aperture Value={D->AddrRangeMin,D->AddrLen};
  FreePool(Resources);*Out=Value;return EFI_SUCCESS;
}
#endif
