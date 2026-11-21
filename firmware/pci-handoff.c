/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "pci-handoff.h"
static uint16_t r16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static uint32_t r32(const uint8_t *p) { return r16(p) | (uint32_t)r16(p + 2) << 16; }
static uint64_t r64(const uint8_t *p) { return r32(p) | (uint64_t)r32(p + 4) << 32; }
static void w16(uint8_t *p, uint16_t x) { p[0] = (uint8_t)x; p[1] = (uint8_t)(x >> 8); }
static void w32(uint8_t *p, uint32_t x) { w16(p, (uint16_t)x); w16(p + 2, (uint16_t)(x >> 16)); }
static void w64(uint8_t *p, uint64_t x) { w32(p, (uint32_t)x); w32(p + 4, (uint32_t)(x >> 32)); }
static void copy(uint8_t *d, const uint8_t *s, size_t n) { while (n--) *d++ = *s++; }
static const uint8_t guid[16] = {0xcb,0xba,0x4e,0xec,0x38,0x26,0x6e,0x41,0xbe,0x80,0xe5,0xfa,0x4b,0x51,0x19,0x01};
static const uint64_t cuts[] = {0x80000000,0xc4400000,0xd0000000,0xe0700000,UINT64_C(0x980000000),UINT64_C(0x9a0000000)};
static int overlaps(uint64_t a, uint64_t b, uint64_t c, uint64_t d) { return a < d && c < b; }
static int in_aperture(uint64_t a, uint64_t b) {
    return overlaps(a,b,0x80000000,0xe0700000) || overlaps(a,b,cuts[4],cuts[5]);
}
static int in_window(uint64_t a, uint64_t b) {
    for (unsigned i = 0; i < 6; i += 2) if (a >= cuts[i] && b <= cuts[i + 1]) return 1;
    return 0;
}
/* Only fields used by the bounded bridge profile are observed. I/O decode
 * is optional: the existing MMIO/USB path needs memory decode and bus master.
 * Preserve the captured I/O windows even when their decode is disabled. */
int ps5_pci_profile_mismatch(const uint8_t *sony, const uint8_t *amd, size_t config_size,
                             unsigned *bridge, unsigned *offset, uint8_t *actual,
                             uint8_t *expected, uint8_t *mask) {
    static const struct { uint8_t offset, mask, expected[2]; } fields[] = {
        {0x00,0xff,{0x22,0x22}}, {0x01,0xff,{0x10,0x10}},
        {0x02,0xff,{0xdf,0xe5}}, {0x03,0xff,{0x13,0x13}},
        {0x04,0x06,{0x06,0x06}}, {0x0e,0x7f,{0x01,0x01}},
        {0x18,0xff,{0x00,0x00}}, {0x19,0xff,{0x40,0x20}},
        {0x1a,0xff,{0x40,0x20}}, {0x1c,0xff,{0xf1,0x21}},
        {0x1d,0xff,{0x01,0x21}}, {0x20,0xff,{0x00,0x00}},
        {0x21,0xff,{0x80,0xd0}}, {0x22,0xff,{0x30,0x60}},
        {0x23,0xff,{0xc4,0xe0}}, {0x24,0xff,{0x01,0xf1}},
        {0x25,0xff,{0x80,0xff}}, {0x26,0xff,{0xf1,0x01}},
        {0x27,0xff,{0x9f,0x00}}, {0x28,0xff,{0x09,0x00}},
        {0x29,0xff,{0x00,0x00}}, {0x2a,0xff,{0x00,0x00}},
        {0x2b,0xff,{0x00,0x00}}, {0x2c,0xff,{0x09,0x00}},
        {0x2d,0xff,{0x00,0x00}}, {0x2e,0xff,{0x00,0x00}},
        {0x2f,0xff,{0x00,0x00}}, {0x30,0xff,{0x00,0x00}},
        {0x31,0xff,{0x00,0x00}}, {0x32,0xff,{0x00,0x00}},
        {0x33,0xff,{0x00,0x00}}
    };
    if (!sony || !amd || config_size < 64 || !bridge || !offset
            || !actual || !expected || !mask) return -1;
    for (unsigned b = 0; b < 2; b++) {
        const uint8_t *p = b ? amd : sony;
        /* Decode the entire 32-bit I/O window before deciding it is disabled.
         * Firmware may use a high-base sentinel with nonzero upper words. */
        uint32_t io_base = ((uint32_t)r16(p + 0x30) << 16)
            | ((uint32_t)(p[0x1c] & 0xf0) << 8);
        uint32_t io_limit = ((uint32_t)r16(p + 0x32) << 16)
            | ((uint32_t)(p[0x1d] & 0xf0) << 8) | 0xfff;
        int io_disabled = io_base > io_limit;
        uint64_t prefetch_base = ((uint64_t)r32(p + 0x28) << 32)
            | ((uint64_t)(r16(p + 0x24) & 0xfff0) << 16);
        uint64_t prefetch_limit = ((uint64_t)r32(p + 0x2c) << 32)
            | ((uint64_t)(r16(p + 0x26) & 0xfff0) << 16) | 0xfffff;
        int prefetch_disabled = prefetch_base > prefetch_limit;
        for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
            uint8_t required = fields[i].expected[b], bits = fields[i].mask;
            if (fields[i].offset == 0x1c || fields[i].offset == 0x1d) {
                /* Type is always checked. Active windows must be exactly
                 * the assigned 4KiB range, including zero upper words below. */
                required = 1; bits = 0x0f;
                if (!io_disabled && (p[0x1c] & 0x0f) == 1
                        && (p[0x1d] & 0x0f) == 1) {
                    required = b ? 0x21 : 0x11; bits = 0xff;
                }
            }
            if (fields[i].offset >= 0x24 && fields[i].offset <= 0x2f) {
                if (fields[i].offset == 0x24 || fields[i].offset == 0x26) {
                    /* 64-bit type is required even for a disabled window. */
                    if (prefetch_disabled || (p[0x24] & 0x0f) != 1
                            || (p[0x26] & 0x0f) != 1) {
                        required = 1; bits = 0x0f;
                    }
                } else if (prefetch_disabled) continue;
            }
            if (fields[i].offset >= 0x30 && io_disabled) continue;
            if ((p[fields[i].offset] & bits) != required) {
                *bridge = b; *offset = fields[i].offset;
                *actual = p[fields[i].offset]; *expected = required;
                *mask = bits;
                return 1;
            }
        }
    }
    return 0;
}
int ps5_add_pci_handoff(const uint8_t *input, size_t input_size,
                        const uint8_t *sony, const uint8_t *amd, size_t config_size,
                        uint8_t *output, size_t capacity, struct ps5_handoff_result *result) {
    if (!input || !sony || !amd || !output || !result || config_size < 64
            || input_size < 64 || input_size > PS5_HOB_CAPACITY || capacity < 64
            || result->hob_bytes != input_size || input == output) return -1;
    unsigned bridge, offset_mismatch;
    uint8_t actual, expected, mask;
    if (ps5_pci_profile_mismatch(sony,amd,config_size,&bridge,&offset_mismatch,
                                 &actual,&expected,&mask) != 0) return -2;
    if (r16(input) != 1 || r16(input + 2) != 56
            || r64(input + 48) != PS5_HOB_BASE + input_size - 8) return -3;
    size_t used = 0, offset = 0;
    uint32_t resource_count = 0;
    uint64_t coverage[2] = {0,0}, previous_resource_end = 0;
    while (offset < input_size - 8) {
        const uint8_t *p = input + offset;
        unsigned type = r16(p), length = r16(p + 2);
        if (length < 8 || (length & 7) || length > input_size - 8 - offset || type == 0xffff) return -3;
        if (type == 4 && length >= 24) {
            unsigned equal = 1;
            for (unsigned i = 0; i < 16; i++) if (p[8 + i] != guid[i]) equal = 0;
            if (equal) return -3; // Already annotated: never duplicate a root.
        }
        if (type == 2) {
            if (length != 48) return -3;
            uint64_t a = r64(p + 24), n = r64(p + 32);
            if (a + n < a || in_aperture(a,a + n)) return -4;
        }
        if (type == 3) {
            if (length != 48) return -3;
            uint64_t a = r64(p + 32), n = r64(p + 40), end = a + n;
            if (!n || end < a || a < previous_resource_end) return -3;
            previous_resource_end = end;
            uint32_t kind = r32(p + 24);
            while (a < end) {
                uint64_t b = end;
                for (unsigned i = 0; i < 6; i++) if (cuts[i] > a && cuts[i] < b) b = cuts[i];
                uint32_t target_kind = kind;
                if (in_aperture(a,b)) {
                    // Never turn RAM, ACPI/NVS or an unexplained reserved gap
                    // into a PCI aperture. Only known MMIO windows are retagged.
                    if (!in_window(a,b) || kind != 5) return -4;
                    target_kind = 1; // EFI_RESOURCE_MEMORY_MAPPED_IO
                    for (unsigned i = 0; i < 2; i++)
                        if (a >= cuts[i * 2] && b <= cuts[i * 2 + 1]) coverage[i] += b - a;
                }
                if (capacity - used < 48) return -5;
                copy(output + used,p,48);
                w32(output + used + 24,target_kind);
                w64(output + used + 32,a); w64(output + used + 40,b - a);
                used += 48; resource_count++; a = b;
            }
        } else {
            if (length > capacity - used) return -5;
            copy(output + used,p,length); used += length;
        }
        offset += length;
    }
    if (r16(input + offset) != 0xffff || r16(input + offset + 2) != 8
            || coverage[0] != cuts[1] - cuts[0] || coverage[1] != cuts[3] - cuts[2]) return -4;
    if (capacity - used < 224) return -5;
    uint8_t *h = output + used;
    for (unsigned i = 0; i < 224; i++) h[i] = 0;
    w16(h,4); w16(h + 2,216); copy(h + 8,guid,16);
    h[24] = 1; w16(h + 26,188); h[28] = 1; h[29] = 1;
    uint8_t *root = h + 30;
    // Segment0, no legacy attributes, DMA buffers below4GiB, extended config.
    w64(root + 22,3); // Combine below-4G MEM/PMEM, support 64-bit MMIO decode.
    for (unsigned i = 0; i < 6; i++) w64(root + 30 + i * 24,UINT64_MAX);
    w64(root + 30,0); w64(root + 38,0x7f);
    w64(root + 54,0x1000); w64(root + 62,0x2fff);
    w64(root + 78,0x80000000); w64(root + 86,0xe06fffff);
    w64(root + 150,cuts[4]); w64(root + 158,cuts[5] - 1);
    w32(root + 174,0x0a0341d0); // PNP0A03, UID0, matching current DSDT.
    used += 216;
    w16(output + used,0xffff); w16(output + used + 2,8); used += 8;
    w64(output + 40,PS5_HOB_BASE + used); w64(output + 48,PS5_HOB_BASE + used - 8);
    result->hob_bytes = used; result->resource_count = resource_count;
    return 0;
}
