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
/* r5.26.7 (version independence): the windows are no longer the ones measured on 13.60 but the ones GameOS programmed
 * into the two root ports (ps5_pci_windows), like PS5 Linux takes them. On 13.60 they are the old constants:
 * 80000000..c43fffff, d0000000..e06fffff and 980000000..99fffffff. */
static int overlaps(uint64_t a, uint64_t b, uint64_t c, uint64_t d) { return a < d && c < b; }
static int in_aperture(const uint64_t *cuts, uint64_t a, uint64_t b) {
    return overlaps(a,b,cuts[0],cuts[3]) || overlaps(a,b,cuts[4],cuts[5]);
}
static int in_window(const uint64_t *cuts, uint64_t a, uint64_t b) {
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
        {0x1a,0xff,{0x40,0x20}}, {0x1c,0xff,{0xf1,0x21}},   /* 0x19/0x1a: expected only for the report, see bus_ok */
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
        /* r5.26.7 (version independence, issue #4; r5.25.24 only accepted windows inside the 13.60 ones): any active
         * 32-bit memory window between 2 GiB and the ECAM at f0000000 is accepted - the HOB and ACPI apertures are built
         * from it (ps5_pci_windows); the windows of both ports must not overlap (checked below). Sony may have any active
         * 64-bit prefetch window between 4 GiB and 1 TiB; AMD's stays disabled. Bus numbers: primary 0, then any
         * secondary/subordinate range inside the ECAM buses 01..7f, the two ranges disjoint. */
        uint32_t mem_base = (uint32_t)(r16(p + 0x20) & 0xfff0) << 16;
        uint32_t mem_limit = ((uint32_t)(r16(p + 0x22) & 0xfff0) << 16) | 0xfffff;
        int mem_inside = mem_base <= mem_limit && mem_base >= 0x80000000u && mem_limit < 0xf0000000u
            && (p[0x20] & 0x0f) == 0 && (p[0x22] & 0x0f) == 0;
        int pf_inside = !b && !prefetch_disabled && (p[0x24] & 0x0f) == 1 && (p[0x26] & 0x0f) == 1
            && prefetch_base >= UINT64_C(0x100000000) && prefetch_limit < (UINT64_C(1) << 40);
        int bus_ok = p[0x19] >= 1 && p[0x19] <= p[0x1a] && p[0x1a] <= 0x7f;
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
            if ((fields[i].offset == 0x19 || fields[i].offset == 0x1a) && bus_ok) continue;
            if (fields[i].offset >= 0x20 && fields[i].offset <= 0x23 && mem_inside) continue;
            if (fields[i].offset >= 0x24 && fields[i].offset <= 0x2f && pf_inside) continue;
            if ((p[fields[i].offset] & bits) != required) {
                *bridge = b; *offset = fields[i].offset;
                *actual = p[fields[i].offset]; *expected = required;
                *mask = bits;
                return 1;
            }
        }
    }
    /* r5.26.7: the two ports must not share buses or memory */
    if (!(amd[0x1a] < sony[0x19] || sony[0x1a] < amd[0x19])) {
        *bridge = 1; *offset = 0x19; *actual = amd[0x19]; *expected = 0; *mask = 0; return 1;
    }
    uint64_t sb = (uint64_t)(r16(sony + 0x20) & 0xfff0) << 16, sl = (((uint64_t)(r16(sony + 0x22) & 0xfff0) << 16) | 0xfffff) + 1;
    uint64_t ab = (uint64_t)(r16(amd + 0x20) & 0xfff0) << 16, al = (((uint64_t)(r16(amd + 0x22) & 0xfff0) << 16) | 0xfffff) + 1;
    if (overlaps(sb,sl,ab,al)) {
        *bridge = 1; *offset = 0x20; *actual = amd[0x20]; *expected = 0; *mask = 0; return 1;
    }
    return 0;
}
int ps5_pci_windows(const uint8_t *sony, const uint8_t *amd, size_t config_size, struct ps5_pci_windows *w) {
    unsigned bridge, offset; uint8_t actual, expected, mask;
    if (!w) return -1;
    int m = ps5_pci_profile_mismatch(sony,amd,config_size,&bridge,&offset,&actual,&expected,&mask);
    if (m) return m;
    uint64_t sb = (uint64_t)(r16(sony + 0x20) & 0xfff0) << 16, sl = (((uint64_t)(r16(sony + 0x22) & 0xfff0) << 16) | 0xfffff) + 1;
    uint64_t ab = (uint64_t)(r16(amd + 0x20) & 0xfff0) << 16, al = (((uint64_t)(r16(amd + 0x22) & 0xfff0) << 16) | 0xfffff) + 1;
    if (sb < ab) { w->cut[0] = sb; w->cut[1] = sl; w->cut[2] = ab; w->cut[3] = al; }
    else { w->cut[0] = ab; w->cut[1] = al; w->cut[2] = sb; w->cut[3] = sl; }
    uint64_t pb = ((uint64_t)r32(sony + 0x28) << 32) | ((uint64_t)(r16(sony + 0x24) & 0xfff0) << 16);
    uint64_t pl = ((uint64_t)r32(sony + 0x2c) << 32) | ((uint64_t)(r16(sony + 0x26) & 0xfff0) << 16) | 0xfffff;
    if (pb <= pl) { w->cut[4] = pb; w->cut[5] = pl + 1; }
    else { w->cut[4] = UINT64_C(0x980000000); w->cut[5] = UINT64_C(0x9a0000000); }   /* disabled: the 13.60 aperture as before */
    return 0;
}
/* r5.26.7: a memory window grows to the reserved block of the memory map that encloses it (never below 2 GiB, never
 * into ECAM or the other window). r5.25.24 accepted a window GameOS made smaller than the 13.60 one while the memory map
 * still reserved the whole 13.60 range; that range stays the aperture. On 13.60 window and block are the same. */
static int reserved_at(const uint8_t *input, size_t input_size, uint64_t x) {
    size_t off = 0;
    while (off + 8 <= input_size) {
        const uint8_t *p = input + off; unsigned type = r16(p), length = r16(p + 2);
        if (type == 0xffff || length < 8 || length > input_size - off) break;
        if (type == 3 && length == 48 && r32(p + 24) == 5) {
            uint64_t a = r64(p + 32), n = r64(p + 40);
            if (x >= a && x - a < n) return 1;
        }
        off += length;
    }
    return 0;
}
static void grow(const uint8_t *input, size_t input_size, uint64_t *lo, uint64_t *hi, uint64_t floor, uint64_t ceiling) {
    /* whole MiB steps only (bridge windows have MiB granularity) */
    while (*lo >= floor + 0x100000 && reserved_at(input,input_size,*lo - 1) && reserved_at(input,input_size,*lo - 0x100000))
        *lo -= 0x100000;
    while (*hi + 0x100000 <= ceiling && reserved_at(input,input_size,*hi) && reserved_at(input,input_size,*hi + 0xfffff))
        *hi += 0x100000;
}
int ps5_pci_handoff_windows(const uint8_t *input, size_t input_size, const uint8_t *sony, const uint8_t *amd,
                            size_t config_size, struct ps5_pci_windows *w) {
    if (!input || input_size < 64 || input_size > PS5_HOB_CAPACITY) return -1;
    int m = ps5_pci_windows(sony,amd,config_size,w);
    if (m) return m;
    uint64_t lo0 = w->cut[0], hi0 = w->cut[1], lo1 = w->cut[2], hi1 = w->cut[3];
    grow(input,input_size,&lo0,&hi0,0x80000000u,lo1);
    grow(input,input_size,&lo1,&hi1,hi0,0xf0000000u);
    w->cut[0] = lo0; w->cut[1] = hi0; w->cut[2] = lo1; w->cut[3] = hi1;
    return 0;
}
int ps5_add_pci_handoff(const uint8_t *input, size_t input_size,
                        const uint8_t *sony, const uint8_t *amd, size_t config_size,
                        uint8_t *output, size_t capacity, struct ps5_handoff_result *result) {
    if (!input || !sony || !amd || !output || !result || config_size < 64
            || input_size < 64 || input_size > PS5_HOB_CAPACITY || capacity < 64
            || result->hob_bytes != input_size || input == output) return -1;
    struct ps5_pci_windows win;
    if (ps5_pci_handoff_windows(input,input_size,sony,amd,config_size,&win) != 0) return -2;
    const uint64_t *cuts = win.cut;
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
            if (a + n < a || in_aperture(cuts,a,a + n)) return -4;
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
                if (in_aperture(cuts,a,b)) {
                    // Never turn RAM, ACPI/NVS or an unexplained reserved gap
                    // into a PCI aperture. Only known MMIO windows are retagged.
                    if (!in_window(cuts,a,b) || kind != 5) return -4;
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
            || coverage[0] == 0 || coverage[1] == 0) return -4;   /* r5.26.7: a window may extend over a gap of the memory map (no RAM there), not over RAM */
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
    w64(root + 78,cuts[0]); w64(root + 86,cuts[3] - 1);
    w64(root + 150,cuts[4]); w64(root + 158,cuts[5] - 1);
    w32(root + 174,0x0a0341d0); // PNP0A03, UID0, matching current DSDT.
    used += 216;
    w16(output + used,0xffff); w16(output + used + 2,8); used += 8;
    w64(output + 40,PS5_HOB_BASE + used); w64(output + 48,PS5_HOB_BASE + used - 8);
    result->hob_bytes = used; result->resource_count = resource_count;
    return 0;
}
