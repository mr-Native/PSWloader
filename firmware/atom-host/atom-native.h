/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PS5_ATOM_NATIVE_H
#define PS5_ATOM_NATIVE_H
#include <Uefi.h>
#include <Protocol/PciIo.h>
/* BSP-only, serialized command execution. No automatic hardware invocation.
 * ROM must be the validated 64KiB platform capture. Params is in/out dwords.
 * PCI MMIO accesses use BAR5 and stop on errors or range violations.
 * MC/PLL legacy interfaces are unsupported and abort. Caller must validate
 * actual connector/engine/PLL parameters and own the display hardware. */
EFI_STATUS Ps5AtomExecute(EFI_PCI_IO_PROTOCOL *Pci,const UINT8 *Rom,UINTN RomBytes,
                          UINTN Table,UINT32 *Params,UINTN ParamDwords);
#endif
