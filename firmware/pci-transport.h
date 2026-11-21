/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PS5_PCI_TRANSPORT_H
#define PS5_PCI_TRANSPORT_H
#ifdef PS5_EDK2
#include <Base.h>
typedef UINT32 uint32_t;
#else
#include <stdint.h>
#endif
/* Callbacks perform raw ECAM cycles, never recurse through this transport.
 * enter/leave must serialize the complete RID + selector + data transaction,
 * including interrupt exclusion. Return zero on success, negative on error.
 * The caller determines ARI forwarding from the upstream PCIe bridge; do not
 * infer it merely from a nonzero value at endpoint offset 0x150.
 */
struct ps5_pci_io {
    void *context;
    int (*enter)(void *context);
    void (*leave)(void *context);
    int (*read)(void *context, uint32_t ecam_offset, unsigned width, uint32_t *value);
    int (*write)(void *context, uint32_t ecam_offset, unsigned width, uint32_t value);
};
int ps5_pci_access(const struct ps5_pci_io *io, uint32_t address, unsigned width,
                   int ari_forwarding, int write, uint32_t *value);
#endif
