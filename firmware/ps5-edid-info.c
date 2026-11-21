/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * ps5-edid-info v1.0 (edid6), see ps5-edid-info.h. */
#include "ps5-edid-info.h"
uint8_t ps5_edid_fix(uint8_t *e) {
    unsigned sum = 0;
    e[0] = 0x00;
    for (unsigned i = 0; i < 127; i++) sum += e[i];
    e[127] = (uint8_t)(0u - sum);
    return e[127];
}
static void add_dtd(const uint8_t *d, uint8_t block, struct ps5_edid_summary *s) {
    unsigned pclk = (unsigned)d[0] | (unsigned)d[1] << 8;
    if (!pclk || s->dtd_count >= PS5_EDID_INFO_MAX_DTD) return;
    struct ps5_edid_dtd *t = &s->dtd[s->dtd_count++];
    unsigned hact = d[2] | (unsigned)(d[4] & 0xf0) << 4, hbl = d[3] | (unsigned)(d[4] & 0x0f) << 8;
    unsigned vact = d[5] | (unsigned)(d[7] & 0xf0) << 4, vbl = d[6] | (unsigned)(d[7] & 0x0f) << 8;
    t->pclk_khz = pclk * 10u;
    t->hact = (uint16_t)hact; t->vact = (uint16_t)vact;
    t->htot = (uint16_t)(hact + hbl); t->vtot = (uint16_t)(vact + vbl);
    t->interlaced = (d[17] & 0x80) != 0; t->block = block;
    uint64_t pixels = (uint64_t)t->htot * t->vtot;
    t->refresh_mhz = pixels ? (uint32_t)(((uint64_t)pclk * 10000u * 1000u + pixels / 2) / pixels) : 0;
}
int ps5_edid_summary(const uint8_t *e, unsigned bytes, struct ps5_edid_summary *s) {
    uint8_t *z = (uint8_t *)s;
    for (unsigned i = 0; i < sizeof(*s); i++) z[i] = 0;
    if (!e || bytes < 128) return -1;
    s->raw_byte0 = e[0];
    s->header_ok = e[7] == 0;
    for (unsigned i = 1; i < 7; i++) if (e[i] != 0xff) s->header_ok = 0;
    unsigned sum = 0;
    for (unsigned i = 0; i < 128; i++) sum += e[i];
    s->block0_ok = (sum & 255) == 0;
    unsigned m = (unsigned)e[8] << 8 | e[9];
    s->vendor[0] = (char)('@' + ((m >> 10) & 31)); s->vendor[1] = (char)('@' + ((m >> 5) & 31));
    s->vendor[2] = (char)('@' + (m & 31)); s->vendor[3] = 0;
    s->product = (uint16_t)(e[10] | e[11] << 8);
    s->serial = (uint32_t)e[12] | (uint32_t)e[13] << 8 | (uint32_t)e[14] << 16 | (uint32_t)e[15] << 24;
    s->version = e[18]; s->revision = e[19]; s->extensions = e[126];
    for (unsigned k = 0; k < 4; k++) {
        const uint8_t *d = e + 54 + 18 * k;
        if (d[0] | d[1]) { add_dtd(d, 0, s); continue; }
        if (d[3] == 0xfc) {
            unsigned n = 0;
            for (unsigned i = 5; i < 18 && d[i] != 0x0a && n < 13; i++) s->name[n++] = (char)((d[i] >= 0x20 && d[i] < 0x7f) ? d[i] : '?');
            while (n && s->name[n - 1] == ' ') n--;
            s->name[n] = 0;
        }
    }
    unsigned blocks = bytes / 128;
    for (unsigned b = 1; b < blocks && b <= e[126]; b++) {
        const uint8_t *x = e + 128 * b;
        if (x[0] != 0x02) continue;               /* CTA-861 extension only */
        s->cta = 1;
        unsigned dtd = x[2];
        if (dtd < 4 || dtd > 127) dtd = 127;
        for (unsigned p = 4; p < dtd;) {
            unsigned tag = x[p] >> 5, len = x[p] & 31;
            if (p + 1 + len > dtd) break;
            if (tag == 2)
                for (unsigned i = 0; i < len && s->vic_count < PS5_EDID_INFO_MAX_VIC; i++) {
                    unsigned v = x[p + 1 + i];
                    if (v >= 129 && v <= 192) v &= 0x7f;   /* native flag */
                    s->vic[s->vic_count++] = (uint8_t)v;
                }
            p += 1 + len;
        }
        for (unsigned p = x[2]; p >= 4 && p + 18 <= 127; p += 18) {
            if (!(x[p] | x[p + 1])) break;
            add_dtd(x + p, (uint8_t)b, s);
        }
    }
    return 0;
}
uint32_t ps5_edid_crc32(const uint8_t *data, unsigned bytes) {
    uint32_t c = 0xffffffffu;
    for (unsigned i = 0; i < bytes; i++) {
        c ^= data[i];
        for (unsigned k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
    }
    return ~c;
}
