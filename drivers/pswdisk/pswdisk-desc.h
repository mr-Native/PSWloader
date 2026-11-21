/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * pswdisk-desc.h v1.1 (17.11.2026, PSWloader) - rebuilds a STORAGE_DEVICE_DESCRIPTOR of a USB disk so it reads as a
 * fixed internal disk with a readable, device-independent name. Pure C on byte buffers (no WDK types), shared by the
 * driver and the host test (test_pswdisk_desc.c).
 *
 * Input: the descriptor as the disk stack returned it (inlen bytes). Output layout:
 *   36-byte header (copied; BusType -> SATA, RemovableMedia -> 0, Size -> new total, VendorIdOffset -> 0)
 *   RawDeviceProperties (copied, clamped to the input)
 *   product "USB: <vendor> <product>" (trimmed, at most PSWDISK_NAME_MAX chars), revision, serial (when present)
 * The vendor moves into the product string (tools show "vendor product" and would print it twice otherwise).
 * Strings are read bounded (offset inside the input, NUL or end of input), so short, empty or missing strings are fine.
 * Returns the size of the new descriptor (0 = not a USB disk descriptor or too short: leave it unchanged); at most
 * that many bytes are written to out (cap must be >= the return value, pswdisk_desc_bound(inlen) is always enough). */
#ifndef PSWDISK_DESC_H
#define PSWDISK_DESC_H

#define PSWDISK_HDR          36u        /* FIELD_OFFSET(STORAGE_DEVICE_DESCRIPTOR, RawDeviceProperties) */
#define PSWDISK_BUS_USB      7u         /* BusTypeUsb */
#define PSWDISK_BUS_SATA     11u        /* BusTypeSata */
#define PSWDISK_NAME_MAX     64u
#define PSWDISK_PREFIX       "USB: "

static unsigned pswdisk_rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }
static void pswdisk_wr32(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24); }

/* bounded string at offset off: start and length (trailing/leading blanks removed); 0 length when absent */
static unsigned pswdisk_str(const unsigned char *in, unsigned long inlen, unsigned off, const unsigned char **s)
{
    unsigned long e;
    *s = 0;
    if (off < PSWDISK_HDR || off >= inlen) return 0;
    for (e = off; e < inlen && in[e]; e++) {}
    while (off < e && in[off] == ' ') off++;
    while (e > off && in[e - 1] == ' ') e--;
    *s = in + off;
    return (unsigned)(e - off);
}

static unsigned long pswdisk_desc_bound(unsigned long inlen) { return inlen + PSWDISK_NAME_MAX + 8u; }

static unsigned long pswdisk_desc_rebuild(const unsigned char *in, unsigned long inlen, unsigned char *out, unsigned long cap)
{
    const unsigned char *ven, *prd, *rev, *ser;
    unsigned lv, lp, lr, ls, raw, n, k;
    unsigned long pos, need;
    if (inlen < PSWDISK_HDR || pswdisk_rd32(in + 28) != PSWDISK_BUS_USB) return 0;
    if (pswdisk_rd32(in + 4) < inlen) inlen = pswdisk_rd32(in + 4) >= PSWDISK_HDR ? pswdisk_rd32(in + 4) : inlen;
    lv = pswdisk_str(in, inlen, pswdisk_rd32(in + 12), &ven);
    lp = pswdisk_str(in, inlen, pswdisk_rd32(in + 16), &prd);
    lr = pswdisk_str(in, inlen, pswdisk_rd32(in + 20), &rev);
    ls = pswdisk_str(in, inlen, pswdisk_rd32(in + 24), &ser);
    raw = pswdisk_rd32(in + 32);
    if (raw > inlen - PSWDISK_HDR) raw = (unsigned)(inlen - PSWDISK_HDR);
    /* name: "USB: " + vendor + " " + product, cut to PSWDISK_NAME_MAX */
    n = (unsigned)sizeof(PSWDISK_PREFIX) - 1 + lv + (lv && lp ? 1 : 0) + lp;
    if (!lv && !lp) n += 4;                                   /* "USB: disk" */
    if (n > PSWDISK_NAME_MAX) n = PSWDISK_NAME_MAX;
    need = PSWDISK_HDR + raw + n + 1 + (rev ? lr + 1 : 0) + (ser ? ls + 1 : 0);
    if (need > cap) return 0;
    for (k = 0; k < PSWDISK_HDR; k++) out[k] = in[k];
    for (k = 0; k < raw; k++) out[PSWDISK_HDR + k] = in[PSWDISK_HDR + k];
    pos = PSWDISK_HDR + raw;
    pswdisk_wr32(out + 12, 0);                                /* vendor: now part of the product string */
    pswdisk_wr32(out + 16, (unsigned)pos);
    {
        char tmp[sizeof(PSWDISK_PREFIX) + 2 * 256 + 8];
        unsigned t = 0;
        const char *pre = PSWDISK_PREFIX;
        while (*pre) tmp[t++] = *pre++;
        if (!lv && !lp) { tmp[t++] = 'd'; tmp[t++] = 'i'; tmp[t++] = 's'; tmp[t++] = 'k'; }
        for (k = 0; k < lv && k < 256; k++) tmp[t++] = (char)ven[k];
        if (lv && lp) tmp[t++] = ' ';
        for (k = 0; k < lp && k < 256; k++) tmp[t++] = (char)prd[k];
        for (k = 0; k < n; k++) out[pos++] = (unsigned char)tmp[k];
        out[pos++] = 0;
    }
    pswdisk_wr32(out + 20, rev ? (unsigned)pos : 0);
    if (rev) { for (k = 0; k < lr; k++) out[pos++] = rev[k]; out[pos++] = 0; }
    pswdisk_wr32(out + 24, ser ? (unsigned)pos : 0);
    if (ser) { for (k = 0; k < ls; k++) out[pos++] = ser[k]; out[pos++] = 0; }
    pswdisk_wr32(out + 4, (unsigned)pos);                     /* Size = the whole new descriptor */
    out[10] = 0;                                              /* RemovableMedia */
    pswdisk_wr32(out + 28, PSWDISK_BUS_SATA);
    pswdisk_wr32(out + 32, raw);
    return pos;
}
#endif
