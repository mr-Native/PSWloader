/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PS5_PCI_HANDOFF_H
#define PS5_PCI_HANDOFF_H
#include "handoff.h"
/* Pure captured-profile check: -1 invalid arguments, 0 match, 1 mismatch.
 * Outputs change only on 1. Bridge 0 is Sony, 1 is AMD; the first mismatch
 * is selected in bridge order, then ascending configuration-byte offset.
 * actual is the raw byte; expected and mask describe its required bits. */
int ps5_pci_profile_mismatch(const uint8_t *sony, const uint8_t *amd, size_t config_size,
                             unsigned *bridge, unsigned *offset, uint8_t *actual,
                             uint8_t *expected, uint8_t *mask);
/* Convert prepared HOBs into a separate output buffer. Inputs are raw first
 * 64 configuration bytes from measured root ports 00:01.1 and 00:08.1.
 * Only the captured topology and bounded windows are accepted. Either I/O window may
 * be disabled after full 32-bit decoding; active Sony is 1000..1fff and AMD
 * is 2000..2fff. Full 64-bit prefetch windows may be disabled; only Sony
 * may have an active prefetch window, exactly 980000000..99fffffff. No MMIO or PCI writes.
 * ResourceAssigned means preserve firmware assignments, not that every Sony
 * endpoint has a usable BAR. Unassigned devices are not provisioned here.
 */
int ps5_add_pci_handoff(const uint8_t *input, size_t input_size,
                        const uint8_t *sony, const uint8_t *amd, size_t config_size,
                        uint8_t *output, size_t capacity,
                        struct ps5_handoff_result *result);
#endif
