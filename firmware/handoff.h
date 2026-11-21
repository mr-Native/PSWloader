/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PS5_HANDOFF_H
#define PS5_HANDOFF_H
#include <stddef.h>
#include <stdint.h>

#define PS5_HOB_BASE UINT64_C(0x40000000)
#define PS5_IMAGE_BASE UINT64_C(0x41000000)
#define PS5_WORK_TOP UINT64_C(0x44000000)
#define PS5_DXE_TOP UINT64_C(0x48000000)
#define PS5_HOB_CAPACITY 32768

struct ps5_handoff_result {
    uint64_t entry;
    uint64_t image_bytes;
    uint64_t hob_bytes;
    uint32_t resource_count;
    uint32_t fv_count;
};

/* Pure preparation: no MMIO, MSRs, allocation, copy to physical RAM or jump.
 * Caller must place generated HOBs at PS5_HOB_BASE and container+4096 at
 * PS5_IMAGE_BASE, then establish paging/CPU state before calling entry (MS ABI).
 * ACPI contents and PCI/graphics HOBs still require platform integration.
 */
int ps5_prepare_handoff(const uint8_t *boot_params, size_t boot_size,
                        const uint8_t *container, size_t container_size,
                        uint8_t *hobs, size_t capacity, unsigned physical_bits,
                        struct ps5_handoff_result *result);
#endif
