/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Linux-loader transport -> PI HOB preparation, with no Linux OS execution.
 * Fixed placement is accepted only when the actual boot map covers the full
 * producer + DXE workspace with RAM and no overlapping firmware reservation.
 */
#include "handoff.h"

struct region { uint64_t start, end; uint32_t type; };
struct writer { uint8_t *bytes; size_t used, capacity; };

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t u32(const uint8_t *p) {
    return (uint32_t)u16(p) | (uint32_t)u16(p + 2) << 16;
}
static uint64_t u64(const uint8_t *p) { return (uint64_t)u32(p) | (uint64_t)u32(p + 4) << 32; }
static void w16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t *p, uint32_t v) { w16(p, (uint16_t)v); w16(p + 2, (uint16_t)(v >> 16)); }
static void w64(uint8_t *p, uint64_t v) { w32(p, (uint32_t)v); w32(p + 4, (uint32_t)(v >> 32)); }
static void copy(uint8_t *to, const uint8_t *from, size_t n) { while (n--) *to++ = *from++; }
static int equal(const uint8_t *a, const uint8_t *b, size_t n) {
    while (n--) if (*a++ != *b++) return 0;
    return 1;
}
static uint32_t crc32(const uint8_t *p, size_t n) {
    uint32_t crc = UINT32_MAX;
    while (n--) {
        crc ^= *p++;
        for (unsigned k = 0; k < 8; k++) crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (0U - (crc & 1U)));
    }
    return ~crc;
}
static uint8_t *hob(struct writer *w, uint16_t type, size_t bytes) {
    bytes = (bytes + 7U) & ~(size_t)7;
    if (bytes > UINT16_MAX || bytes > w->capacity - w->used) return 0;
    uint8_t *p = w->bytes + w->used;
    for (size_t i = 0; i < bytes; i++) p[i] = 0;
    w16(p, type); w16(p + 2, (uint16_t)bytes);
    w->used += bytes;
    return p;
}
static int allocation(struct writer *w, uint64_t start, uint64_t end, uint32_t type) {
    if (start >= end || (start & 4095) || (end & 4095)) return -1;
    uint8_t *p = hob(w, 2, 48);
    if (!p) return -1;
    w64(p + 24, start); w64(p + 32, end - start); w32(p + 40, type);
    return 0;
}
static unsigned priority(uint32_t type) {
    // A reservation/TMR always wins over usable RAM. Unknown E820 types are
    // conservatively reserved; they must never silently become usable RAM.
    if (type == 1) return 1;
    if (type == 3) return 2;
    if (type == 4) return 3;
    return 4;
}

int ps5_prepare_handoff(const uint8_t *bp, size_t bp_size,
                        const uint8_t *image, size_t image_size,
                        uint8_t *out, size_t capacity, unsigned physical_bits,
                        struct ps5_handoff_result *result) {
    static const uint8_t extra_guid[16] = {0xf6,0xba,0xa5,0x15,0x91,0x1c,0x7d,0x46,0x9d,0xfb,0x31,0x9d,0x17,0x8d,0x4b,0xb4};
    static const uint8_t acpi_guid[16] = {0x06,0x95,0x9a,0x9f,0x97,0x55,0x15,0x45,0xba,0xb6,0x8b,0xcd,0xe7,0x84,0xba,0x87};
    if (!bp || !image || !out || !result || bp_size < 4096 || image_size < 4096
            || capacity < 1024 || physical_bits < 36 || physical_bits > 52) return -1;
    if (!equal(image, (const uint8_t *)"PS5UPL01", 8) || u32(image + 8) != 1
            || u32(image + 12) != 4096 || u64(image + 16) != PS5_IMAGE_BASE
            || crc32(image, 4092) != u32(image + 4092)) return -2;
    uint64_t bytes = u64(image + 24), entry = u64(image + 32);
    uint32_t count = u32(image + 40);
    if (!bytes || bytes > 32 * UINT64_C(1024) * 1024 || (bytes & 4095)
            || bytes != image_size - 4096 || entry < PS5_IMAGE_BASE || entry >= PS5_IMAGE_BASE + bytes
            || !count || count > 8 || crc32(image + 4096, (size_t)bytes) != u32(image + 44)) return -3;
    uint64_t fv_starts[8], fv_ends[8];
    for (unsigned i = 0; i < count; i++) {
        const uint8_t *rec = image + 48 + i * 32;
        uint64_t base = u64(rec + 16), size = u64(rec + 24);
        if (base < PS5_IMAGE_BASE || base - PS5_IMAGE_BASE > bytes || size < 56
                || size > bytes - (base - PS5_IMAGE_BASE) || (base & 4095)
                || !rec[0] || rec[15] != 0) return -4;
        if (i == 0 && !equal(rec, (const uint8_t *)"uefi_fv\0", 8)) return -4;
        const uint8_t *fv = image + 4096 + (base - PS5_IMAGE_BASE);
        if (!equal(fv + 40, (const uint8_t *)"_FVH", 4) || u64(fv + 32) != size) return -4;
        unsigned length = u16(fv + 48), sum = 0;
        if (length < 56 || length > size || (length & 1)) return -4;
        for (unsigned k = 0; k < length; k += 2) sum += u16(fv + k);
        if (sum & 0xffff) return -4;
        fv_starts[i] = base; fv_ends[i] = base + size;
        if (entry >= base && entry < base + size) return -4;
        for (unsigned j = 0; j < i; j++) {
            if (base < fv_ends[j] && fv_starts[j] < base + size) return -4;
            if (equal(rec, image + 48 + j * 32, 16)) return -4;
        }
    }
    struct region input[130], regions[260];
    uint64_t bounds[260];
    unsigned entries = bp[0x1e8], nb = 0, nr = 0;
    if (!entries || entries > 128) return -5;
    for (unsigned i = 0; i < entries; i++) {
        const uint8_t *e = bp + 0x2d0 + i * 20;
        uint64_t start = u64(e), size = u64(e + 8);
        if (!size || start + size < start || start + size > (UINT64_C(1) << physical_bits)) return -5;
        input[i] = (struct region){start, start + size, u32(e + 16)};
    }
    // The retained PS5 loader/AP code and stacks occupy this firmware handoff
    // arena. Keep it unavailable even when the transport map labels it RAM.
    input[entries++] = (struct region){UINT64_C(0x100000000), UINT64_C(0x112000000), 2};
    input[entries++] = (struct region){UINT64_C(0x60000), UINT64_C(0x70000), 2};
    for (unsigned i = 0; i < entries; i++) {
        bounds[nb++] = input[i].start; bounds[nb++] = input[i].end;
    }
    for (unsigned i = 1; i < nb; i++) {
        uint64_t v = bounds[i]; unsigned j = i;
        while (j && bounds[j - 1] > v) { bounds[j] = bounds[j - 1]; j--; }
        bounds[j] = v;
    }
    for (unsigned i = 0; i + 1 < nb; i++) {
        uint64_t start = bounds[i], end = bounds[i + 1];
        unsigned rank = 0; uint32_t type = 0;
        if (start == end) continue;
        for (unsigned j = 0; j < entries; j++) {
            if (input[j].start <= start && input[j].end >= end && priority(input[j].type) > rank) {
                rank = priority(input[j].type); type = input[j].type;
            }
        }
        if (!rank) continue; // A map gap is not RAM.
        if (nr && regions[nr - 1].type == type && regions[nr - 1].end == start)
            regions[nr - 1].end = end;
        else regions[nr++] = (struct region){start, end, type};
    }
    int workspace_ram = 0;
    for (unsigned i = 0; i < nr; i++)
        if (regions[i].type == 1 && regions[i].start <= PS5_HOB_BASE && regions[i].end >= PS5_DXE_TOP) workspace_ram = 1;
    if (!workspace_ram) return -6;
    struct writer w = {out, 0, capacity};
    uint8_t *phit = hob(&w, 1, 56);
    if (!phit) return -7;
    w32(phit + 8, 9); // PI HOB version; BOOT_WITH_FULL_CONFIGURATION = 0.
    w64(phit + 16, PS5_WORK_TOP); w64(phit + 24, PS5_HOB_BASE);
    w64(phit + 32, PS5_HOB_BASE + UINT64_C(0x100000));
    for (unsigned i = 0; i < nr; i++) {
        struct region r = regions[i];
        uint8_t *p = hob(&w, 3, 48);
        if (!p) return -7;
        w32(p + 24, r.type == 1 ? 0 : 5);
        // Reserved is not untested initialized RAM. DXE PromoteMemoryResource
        // may turn PRESENT|INITIALIZED without TESTED into conventional RAM.
        // Preserve the reservation by omitting INITIALIZED, not by inventing
        // a successful memory test for firmware/MMIO/TMR/unknown ranges.
        w32(p + 28, r.type == 1 ? 0x3c07 : 0x401);
        w64(p + 32, r.start); w64(p + 40, r.end - r.start);
        if (r.type == 3 || r.type == 4) {
            // Exact ACPI/NVS ranges; reject rather than round across a TMR.
            if (allocation(&w, r.start, r.end, r.type == 3 ? 9 : 10)) return -8;
        }
    }
    // Protect producer HOBs, packed image, bootstrap and page tables from DXE.
    if (allocation(&w, PS5_HOB_BASE, PS5_WORK_TOP, 4)
            || allocation(&w, 0x100000, 0x1000000, 4)) return -7;
    uint8_t *cpu = hob(&w, 6, 16);
    if (!cpu) return -7;
    cpu[8] = (uint8_t)physical_bits; cpu[9] = 16;
    uint8_t *extra = hob(&w, 4, 24 + 8 + count * 32);
    if (!extra) return -7;
    copy(extra + 8, extra_guid, 16); extra[24] = 1;
    w16(extra + 26, (uint16_t)(8 + count * 32)); w32(extra + 28, count);
    copy(extra + 32, image + 48, count * 32);
    uint64_t rsdp = u64(bp + 0x70);
    if (rsdp) {
        uint8_t *acpi = hob(&w, 4, 36);
        if (!acpi) return -7;
        copy(acpi + 8, acpi_guid, 16); acpi[24] = 1;
        w16(acpi + 26, 12); w64(acpi + 28, rsdp);
    }
    size_t last = w.used;
    if (!hob(&w, 0xffff, 8)) return -7;
    w64(phit + 40, PS5_HOB_BASE + w.used);
    w64(phit + 48, PS5_HOB_BASE + last);
    *result = (struct ps5_handoff_result){entry, bytes, w.used, nr, count};
    return 0;
}
