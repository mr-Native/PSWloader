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
 * IDs, command and header type must match; r5.26.7: bus numbers, memory windows (2 GiB..ECAM, disjoint) and the Sony
 * prefetch window (4 GiB..1 TiB) are taken from the ports, the apertures follow them. Either I/O window may
 * be disabled after full 32-bit decoding; active Sony is 1000..1fff and AMD
 * is 2000..2fff. AMD's 64-bit prefetch window must be disabled. No MMIO or PCI writes.
 * ResourceAssigned means preserve firmware assignments, not that every Sony
 * endpoint has a usable BAR. Unassigned devices are not provisioned here.
 */
/* r5.26.7 (version independence): the PCI windows GameOS programmed into the two root ports, after the profile check.
 * cut[0..1] and cut[2..3] are the two memory windows in address order, cut[4..5] the Sony prefetch window (the 13.60
 * one when disabled); ends exclusive. 0 ok, 1 profile mismatch, -1 invalid arguments. */
struct ps5_pci_windows { uint64_t cut[6]; };
int ps5_pci_windows(const uint8_t *sony, const uint8_t *amd, size_t config_size, struct ps5_pci_windows *w);
/* The windows the handoff uses: ps5_pci_windows, each memory window grown to the enclosing reserved block of the
 * prepared HOB memory map (input of ps5_add_pci_handoff). Same return values. */
int ps5_pci_handoff_windows(const uint8_t *input, size_t input_size, const uint8_t *sony, const uint8_t *amd,
                            size_t config_size, struct ps5_pci_windows *w);
int ps5_add_pci_handoff(const uint8_t *input, size_t input_size,
                        const uint8_t *sony, const uint8_t *amd, size_t config_size,
                        uint8_t *output, size_t capacity,
                        struct ps5_handoff_result *result);
#endif
