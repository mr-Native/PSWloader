/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PS5_ACPI_HANDOFF_H
#define PS5_ACPI_HANDOFF_H
#include "handoff.h"
#define PS5_ACPI_BASE UINT64_C(0x40200000)
/* v1: dedicated page-aligned EfiReserved scratch for the firmware IO-APIC (MADT IO-APIC address).
 * Sits in the first page after a 1-page ACPI arena. Carved out of the producer allocation as a
 * permanently reserved (MemoryType 0) segment so Windows never reclaims it; the HAL maps it UC and
 * marks the IO unit bad (it reads 0), replacing the ntoskrnl IO-APIC hook. Requires a 1-page arena. */
#define PS5_IOAPIC_SCRATCH_SIZE UINT64_C(0x1000)
/* Validate arena, split producer allocation, replace ACPI HOB and PCI HID.
 * On error caller discards output; inputs and result remain unchanged. */
int ps5_add_acpi_handoff(const uint8_t *input, size_t size, const uint8_t *arena,
                         size_t arena_size, uint8_t *output, size_t capacity,
                         struct ps5_handoff_result *result);
/* r5.26.7: rewrite the PCI0 _CRS memory windows of the DSDT in a (writable) arena to cut[] (ps5_pci_windows layout)
 * and fix its checksum. 0 ok, -1 invalid arena/windows, -2 descriptors not found, -3 arena invalid afterwards. */
int ps5_acpi_set_pci_windows(uint8_t *arena, size_t size, const uint64_t cut[6]);
#endif
