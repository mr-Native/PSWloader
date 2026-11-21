/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * ps5-bcd.h v1.0 (17.11.2026, r5.22) - portable (host-tested) edit of a Windows BCD store (registry hive, "regf") in
 * memory: make sure the default boot loader object has element 16000049 (BcdLibraryBoolean_AllowPrereleaseSignatures,
 * the "testsigning" switch) set to 1. Our boot-start storage drivers (ps5nvmsi, ps5m2flt) are PSWloader-signed; a
 * Windows installed by the stock setup GUI gets a BCD without testsigning and would stop with INACCESSIBLE_BOOT_DEVICE
 * before any Windows-side hook can run. This is the official switch (Windows shows "Test Mode"); no Windows code is
 * changed.
 *
 * Path: Objects\{9dea862c-5cdd-4e70-acc1-f32b344d4795} ({bootmgr}) \Elements\23000003 "Element" = REG_SZ "{guid}"
 * (default object) -> Objects\{guid}\Elements\16000049 "Element" = REG_BINARY 01.
 * The new key goes into one new 4 KiB hbin appended at the end (nk, value list, vk with inline data, new sorted lf/lh/li
 * subkey list); the old subkey list cell is freed, the security descriptor's reference count raised, the base block's
 * sequence numbers, length and checksum updated. A dirty hive (sequence numbers differ: pending transaction log) is not
 * touched. Only lf/lh/li subkey lists are written (BCD stores have no ri lists at the Elements level).
 * Returns: 0 already set, 1 added (*out_len = new length), negative = not touched (see PS5_BCD_E_*). */
#ifndef PS5_BCD_H_
#define PS5_BCD_H_
#include <stdint.h>
#include <stddef.h>

#define PS5_BCD_E_FORMAT   (-1)   /* not a regf hive / damaged / unsupported list type */
#define PS5_BCD_E_DIRTY    (-2)   /* sequence numbers differ: a transaction log is pending */
#define PS5_BCD_E_NODEF    (-3)   /* no {bootmgr} default object */
#define PS5_BCD_E_SPACE    (-4)   /* buffer too small for the new hbin */
#define PS5_BCD_E_VALUE    (-5)   /* 16000049 exists with another value (left as it is) */

static inline uint32_t ps5_bcd_u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline uint16_t ps5_bcd_u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline void ps5_bcd_w32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static inline void ps5_bcd_w16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

/* cell data pointer for a cell offset (relative to the first hbin at 0x1000), or 0 if out of range */
static inline uint8_t *ps5_bcd_cell(uint8_t *h, size_t len, uint32_t off, uint32_t need)
{
    size_t at = 0x1000u + (size_t)off;
    if (off == 0xFFFFFFFFu || at + 4 > len) return 0;
    int32_t size = (int32_t)ps5_bcd_u32(h + at);
    uint32_t abs = size < 0 ? (uint32_t)(-size) : (uint32_t)size;
    if (abs < 4 + need || at + abs > len) return 0;
    return h + at + 4;
}
static inline int ps5_bcd_upc(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
/* name of a key/value cell equal to an ASCII name, case-insensitive; comp = 1: Latin-1 bytes, 0: UTF-16LE */
static inline int ps5_bcd_name_eq(const uint8_t *n, unsigned nlen, int comp, const char *want)
{
    unsigned i = 0;
    if (comp) { for (; i < nlen; i++) { if (!want[i] || ps5_bcd_upc(n[i]) != ps5_bcd_upc((unsigned char)want[i])) return 0; } return want[i] == 0; }
    if (nlen & 1) return 0;
    for (; i < nlen / 2; i++) { uint16_t c = ps5_bcd_u16(n + 2 * i); if (!want[i] || c > 0x7F || ps5_bcd_upc(c) != ps5_bcd_upc((unsigned char)want[i])) return 0; }
    return want[i] == 0;
}
/* ASCII name of a nk cell into out (case kept); returns length or -1 */
static inline int ps5_bcd_nk_name(uint8_t *h, size_t len, uint32_t nk, char *out, unsigned cap)
{
    uint8_t *k = ps5_bcd_cell(h, len, nk, 0x4C);
    if (!k || k[0] != 'n' || k[1] != 'k') return -1;
    unsigned nl = ps5_bcd_u16(k + 0x48), comp = (ps5_bcd_u16(k + 2) & 0x20) != 0, n = comp ? nl : nl / 2;
    if (!ps5_bcd_cell(h, len, nk, 0x4C + nl) || n + 1 > cap) return -1;
    for (unsigned i = 0; i < n; i++) { unsigned c = comp ? k[0x4C + i] : ps5_bcd_u16(k + 0x4C + 2 * i); out[i] = (char)(c < 0x80 ? c : '?'); }
    out[n] = 0; return (int)n;
}
/* subkey of nk named want; 0 if none */
static inline uint32_t ps5_bcd_subkey(uint8_t *h, size_t len, uint32_t nk, const char *want)
{
    uint8_t *k = ps5_bcd_cell(h, len, nk, 0x4C);
    if (!k || k[0] != 'n' || k[1] != 'k' || ps5_bcd_u32(k + 0x14) == 0) return 0;
    uint32_t lists[1] = { ps5_bcd_u32(k + 0x1C) }, stack[64]; unsigned sn = 0;
    stack[sn++] = lists[0];
    while (sn) {
        uint32_t lo = stack[--sn]; uint8_t *l = ps5_bcd_cell(h, len, lo, 4);
        if (!l) return 0;
        unsigned cnt = ps5_bcd_u16(l + 2), step = (l[1] == 'f' || l[1] == 'h') ? 8 : 4;
        if (!(l[0] == 'l' && (l[1] == 'f' || l[1] == 'h' || l[1] == 'i')) && !(l[0] == 'r' && l[1] == 'i')) return 0;
        if (!ps5_bcd_cell(h, len, lo, 4 + cnt * step)) return 0;
        for (unsigned i = 0; i < cnt; i++) {
            uint32_t o = ps5_bcd_u32(l + 4 + i * step);
            if (l[0] == 'r') { if (sn < 64) stack[sn++] = o; continue; }
            uint8_t *c = ps5_bcd_cell(h, len, o, 0x4C);
            if (!c || c[0] != 'n' || c[1] != 'k') continue;
            unsigned nl = ps5_bcd_u16(c + 0x48);
            if (ps5_bcd_cell(h, len, o, 0x4C + nl) && ps5_bcd_name_eq(c + 0x4C, nl, (ps5_bcd_u16(c + 2) & 0x20) != 0, want)) return o;
        }
    }
    return 0;
}
/* value "want" of nk: data pointer and size (inline data points into the vk); 0 if none */
static inline const uint8_t *ps5_bcd_value(uint8_t *h, size_t len, uint32_t nk, const char *want, uint32_t *type, uint32_t *size)
{
    uint8_t *k = ps5_bcd_cell(h, len, nk, 0x4C);
    if (!k || k[0] != 'n' || k[1] != 'k') return 0;
    uint32_t n = ps5_bcd_u32(k + 0x24); uint8_t *vl = ps5_bcd_cell(h, len, ps5_bcd_u32(k + 0x28), n * 4);
    if (!n || !vl) return 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t vo = ps5_bcd_u32(vl + 4 * i); uint8_t *v = ps5_bcd_cell(h, len, vo, 0x14);
        if (!v || v[0] != 'v' || v[1] != 'k') continue;
        unsigned nl = ps5_bcd_u16(v + 2);
        if (!ps5_bcd_cell(h, len, vo, 0x14 + nl) || !ps5_bcd_name_eq(v + 0x14, nl, (ps5_bcd_u16(v + 0x10) & 1) != 0, want)) continue;
        uint32_t ds = ps5_bcd_u32(v + 4); *type = ps5_bcd_u32(v + 0xC);
        if (ds & 0x80000000u) { *size = ds & 0x7FFFFFFFu; return *size <= 4 ? v + 8 : 0; }
        *size = ds; return ps5_bcd_cell(h, len, ps5_bcd_u32(v + 8), ds);
    }
    return 0;
}
static inline uint32_t ps5_bcd_csum(const uint8_t *h) { uint32_t x = 0; for (unsigned i = 0; i < 508; i += 4) x ^= ps5_bcd_u32(h + i); return x; }

/* default loader object key (Objects\{guid}), or 0 */
static inline uint32_t ps5_bcd_default_object(uint8_t *h, size_t len)
{
    uint32_t objs = ps5_bcd_subkey(h, len, ps5_bcd_u32(h + 0x24), "Objects"), type = 0, size = 0; char guid[40];
    uint32_t bm = objs ? ps5_bcd_subkey(h, len, objs, "{9dea862c-5cdd-4e70-acc1-f32b344d4795}") : 0;
    uint32_t el = bm ? ps5_bcd_subkey(h, len, bm, "Elements") : 0, de = el ? ps5_bcd_subkey(h, len, el, "23000003") : 0;
    const uint8_t *d = de ? ps5_bcd_value(h, len, de, "Element", &type, &size) : 0;
    if (!d || type != 1 || size < 76) return 0;
    for (unsigned i = 0; i < 38; i++) { uint16_t c = ps5_bcd_u16(d + 2 * i); if (c > 0x7F) return 0; guid[i] = (char)c; }
    guid[38] = 0;
    return ps5_bcd_subkey(h, len, objs, guid);
}

/* any BOOLEAN element (8-character name, value 01) in the default boot loader object; r5.25.4 generalised from testsigning */
static inline int ps5_bcd_enable_element(uint8_t *h, size_t len, size_t cap, size_t *out_len, const char *nm)
{
    if (len < 0x2000 || h[0] != 'r' || h[1] != 'e' || h[2] != 'g' || h[3] != 'f' || ps5_bcd_csum(h) != ps5_bcd_u32(h + 0x1FC)) return PS5_BCD_E_FORMAT;
    if (ps5_bcd_u32(h + 4) != ps5_bcd_u32(h + 8)) return PS5_BCD_E_DIRTY;
    uint32_t data = ps5_bcd_u32(h + 0x28);
    if (data & 0xFFF || 0x1000u + (size_t)data > len) return PS5_BCD_E_FORMAT;
    len = 0x1000u + data;                              /* bytes after the hbins are slack, the new hbin goes there */
    uint32_t obj = ps5_bcd_default_object(h, len);
    if (!obj) return PS5_BCD_E_NODEF;
    uint32_t el = ps5_bcd_subkey(h, len, obj, "Elements"), type = 0, size = 0;
    if (!el) return PS5_BCD_E_NODEF;
    uint32_t ex = ps5_bcd_subkey(h, len, el, nm);
    if (ex) {
        const uint8_t *d = ps5_bcd_value(h, len, ex, "Element", &type, &size);
        return (d && type == 3 && size >= 1 && d[0] == 1) ? 0 : PS5_BCD_E_VALUE;
    }
    uint8_t *e = ps5_bcd_cell(h, len, el, 0x4C);
    uint32_t cnt = ps5_bcd_u32(e + 0x14), lo = ps5_bcd_u32(e + 0x1C), sec = ps5_bcd_u32(e + 0x2C);
    uint8_t *ol = ps5_bcd_cell(h, len, lo, 4), *sk = ps5_bcd_cell(h, len, sec, 0x10);
    if (!ol || !sk || sk[0] != 's' || sk[1] != 'k' || ol[0] != 'l' || !(ol[1] == 'f' || ol[1] == 'h' || ol[1] == 'i') || ps5_bcd_u16(ol + 2) != cnt) return PS5_BCD_E_FORMAT;
    unsigned step = ol[1] == 'i' ? 4 : 8, lsize = (4 + 4 + (cnt + 1) * step + 7) & ~7u;
    if (!ps5_bcd_cell(h, len, lo, 4 + cnt * step) || 0x20 + 0x60 + 8 + 0x20 + lsize > 0x1000) return PS5_BCD_E_FORMAT;
    if (len + 0x1000 > cap) return PS5_BCD_E_SPACE;
    /* sorted insert position (Windows compares the upper-cased names) */
    unsigned pos = cnt;
    for (unsigned i = 0; i < cnt; i++) {
        char other[64]; int n = ps5_bcd_nk_name(h, len, ps5_bcd_u32(ol + 4 + i * step), other, sizeof other), c = 0;
        if (n < 0) return PS5_BCD_E_FORMAT;
        for (int j = 0; !c; j++) { int a = ps5_bcd_upc((unsigned char)nm[j]), b = ps5_bcd_upc((unsigned char)other[j]); c = a - b; if (!a || !b) break; }
        if (c < 0) { pos = i; break; }
    }
    /* new hbin */
    uint8_t *hb = h + len; uint32_t base = data;   /* cell offsets in the new hbin start at base */
    for (unsigned i = 0; i < 0x1000; i++) hb[i] = 0;
    hb[0] = 'h'; hb[1] = 'b'; hb[2] = 'i'; hb[3] = 'n'; ps5_bcd_w32(hb + 4, base); ps5_bcd_w32(hb + 8, 0x1000);
    uint32_t nko = base + 0x20, vlo = nko + 0x60, vko = vlo + 8, nlo = vko + 0x20, fro = nlo + lsize;
    uint8_t *c;
    c = hb + 0x20;            ps5_bcd_w32(c, (uint32_t)-0x60); c += 4;               /* nk */
    c[0] = 'n'; c[1] = 'k'; ps5_bcd_w16(c + 2, 0x20);
    for (unsigned i = 0; i < 8; i++) c[4 + i] = e[4 + i];                             /* last written = parent's */
    ps5_bcd_w32(c + 0x10, el); ps5_bcd_w32(c + 0x1C, 0xFFFFFFFFu); ps5_bcd_w32(c + 0x20, 0xFFFFFFFFu);
    ps5_bcd_w32(c + 0x24, 1); ps5_bcd_w32(c + 0x28, vlo); ps5_bcd_w32(c + 0x2C, sec); ps5_bcd_w32(c + 0x30, 0xFFFFFFFFu);
    ps5_bcd_w32(c + 0x3C, 14); ps5_bcd_w32(c + 0x40, 1); ps5_bcd_w16(c + 0x48, 8);
    for (unsigned i = 0; i < 8; i++) c[0x4C + i] = (uint8_t)nm[i];
    c = hb + 0x20 + 0x60;     ps5_bcd_w32(c, (uint32_t)-8); ps5_bcd_w32(c + 4, vko);    /* value list */
    c = hb + 0x20 + 0x68;     ps5_bcd_w32(c, (uint32_t)-0x20); c += 4;               /* vk "Element" = REG_BINARY 01 inline */
    c[0] = 'v'; c[1] = 'k'; ps5_bcd_w16(c + 2, 7); ps5_bcd_w32(c + 4, 0x80000001u); c[8] = 1; ps5_bcd_w32(c + 0xC, 3); ps5_bcd_w16(c + 0x10, 1);
    for (unsigned i = 0; i < 7; i++) c[0x14 + i] = (uint8_t)"Element"[i];
    c = hb + 0x20 + 0x88;     ps5_bcd_w32(c, (uint32_t)(-(int32_t)lsize)); c += 4;   /* new subkey list, same type */
    c[0] = 'l'; c[1] = ol[1]; ps5_bcd_w16(c + 2, (uint16_t)(cnt + 1));
    for (unsigned i = 0, j = 0; i <= cnt; i++) {
        uint8_t *dst = c + 4 + i * step;
        if (i == pos) {
            ps5_bcd_w32(dst, nko);
            if (ol[1] == 'f') { for (unsigned k = 0; k < 4; k++) dst[4 + k] = (uint8_t)nm[k]; }
            else if (ol[1] == 'h') { uint32_t hs = 0; for (unsigned k = 0; k < 8; k++) hs = hs * 37 + (uint32_t)ps5_bcd_upc((unsigned char)nm[k]); ps5_bcd_w32(dst + 4, hs); }
        } else { for (unsigned k = 0; k < step; k++) dst[k] = ol[4 + j * step + k]; j++; }
    }
    ps5_bcd_w32(hb + (fro - base), 0x1000 - (fro - base));                             /* rest of the hbin: one free cell */
    /* parent, old list, security, base block */
    ps5_bcd_w32(e + 0x14, cnt + 1); ps5_bcd_w32(e + 0x1C, nlo);
    if (ps5_bcd_u32(e + 0x34) < 16) ps5_bcd_w32(e + 0x34, 16);
    { uint8_t *olc = ol - 4; int32_t s = (int32_t)ps5_bcd_u32(olc); if (s < 0) ps5_bcd_w32(olc, (uint32_t)(-s)); }
    ps5_bcd_w32(sk + 0x0C, ps5_bcd_u32(sk + 0x0C) + 1);
    uint32_t seq = ps5_bcd_u32(h + 4) + 1;
    ps5_bcd_w32(h + 4, seq); ps5_bcd_w32(h + 8, seq); ps5_bcd_w32(h + 0x28, data + 0x1000);
    ps5_bcd_w32(h + 0x1FC, ps5_bcd_csum(h));
    *out_len = len + 0x1000;
    return 1;
}
static inline int ps5_bcd_enable_testsigning(uint8_t *h, size_t len, size_t cap, size_t *out_len)
{
    return ps5_bcd_enable_element(h, len, cap, out_len, "16000049");
}
/* lab (r5.25.4): {bootmgr} default -> the first other GUID of its displayorder (in place, same length). 1 changed, 0 nothing
 * to change, <0 error; old/new GUID strings (39 bytes) for the log */
static inline int ps5_bcd_default_to_other(uint8_t *h, size_t len, char *oldg, char *newg)
{
    uint32_t objs = ps5_bcd_subkey(h, len, ps5_bcd_u32(h + 0x24), "Objects"), type = 0, size = 0, t2 = 0, s2 = 0;
    uint32_t bm = objs ? ps5_bcd_subkey(h, len, objs, "{9dea862c-5cdd-4e70-acc1-f32b344d4795}") : 0;
    uint32_t el = bm ? ps5_bcd_subkey(h, len, bm, "Elements") : 0;
    uint32_t de = el ? ps5_bcd_subkey(h, len, el, "23000003") : 0, dor = el ? ps5_bcd_subkey(h, len, el, "24000001") : 0;
    uint8_t *d = de ? (uint8_t *)ps5_bcd_value(h, len, de, "Element", &type, &size) : 0;
    const uint8_t *o = dor ? ps5_bcd_value(h, len, dor, "Element", &t2, &s2) : 0;
    if (!d || type != 1 || size < 76 || !o || t2 != 7) return PS5_BCD_E_NODEF;
    for (unsigned i = 0; i < 38; i++) oldg[i] = (char)d[2 * i];
    oldg[38] = 0;
    for (uint32_t off = 0; off + 76 <= s2; off += 78) {
        char g[39]; int same = 1;
        for (unsigned i = 0; i < 38; i++) { g[i] = (char)o[off + 2 * i]; if (ps5_bcd_upc((unsigned char)g[i]) != ps5_bcd_upc((unsigned char)oldg[i])) same = 0; }
        g[38] = 0;
        if (g[0] != '{' || same || !ps5_bcd_subkey(h, len, objs, g)) continue;
        for (unsigned i = 0; i < 38; i++) { d[2 * i] = (uint8_t)g[i]; d[2 * i + 1] = 0; newg[i] = g[i]; }
        newg[38] = 0;
        return 1;
    }
    return 0;
}
#endif
