/* host test for pswdisk-desc.h v1.1: gcc -Wall -Wextra -Werror -fsanitize=address,undefined test_pswdisk_desc.c */
#include <stdio.h>
#include <string.h>
#include "pswdisk-desc.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* descriptor builder: strings appended after an optional raw block; off_* = 0 -> absent */
static unsigned long mk(unsigned char *b, unsigned bus, const char *ven, const char *prd, const char *rev, const char *ser, unsigned raw)
{
    unsigned long p = PSWDISK_HDR;
    memset(b, 0, 1024);
    pswdisk_wr32(b, 40); b[8] = 0; b[10] = 1;
    pswdisk_wr32(b + 28, bus); pswdisk_wr32(b + 32, raw);
    for (unsigned k = 0; k < raw; k++) b[p++] = (unsigned char)(0xA0 + k);
    const char *s[4] = { ven, prd, rev, ser };
    for (int i = 0; i < 4; i++) {
        if (!s[i]) continue;
        pswdisk_wr32(b + 12 + 4 * i, (unsigned)p);
        memcpy(b + p, s[i], strlen(s[i]) + 1); p += strlen(s[i]) + 1;
    }
    pswdisk_wr32(b + 4, (unsigned)p);
    return p;
}
static const char *str(const unsigned char *d, unsigned off) { return off ? (const char *)d + off : "(none)"; }

int main(void)
{
    unsigned char in[1024], out[1024];
    unsigned long n, r;

    /* typical USB-NVMe bridge: padded SCSI strings */
    n = mk(in, PSWDISK_BUS_USB, "Realtek ", "RTL9210B-CG     ", "1.00", "012345678904", 0);
    r = pswdisk_desc_rebuild(in, n, out, sizeof out);
    CHECK(r > PSWDISK_HDR && r <= pswdisk_desc_bound(n));
    CHECK(pswdisk_rd32(out + 4) == r);
    CHECK(pswdisk_rd32(out + 28) == PSWDISK_BUS_SATA && out[10] == 0);
    CHECK(pswdisk_rd32(out + 12) == 0);
    CHECK(!strcmp(str(out, pswdisk_rd32(out + 16)), "USB: Realtek RTL9210B-CG"));
    CHECK(!strcmp(str(out, pswdisk_rd32(out + 20)), "1.00"));
    CHECK(!strcmp(str(out, pswdisk_rd32(out + 24)), "012345678904"));

    /* raw properties are kept */
    n = mk(in, PSWDISK_BUS_USB, "SanDisk", "Extreme SSD", "1012", NULL, 8);
    r = pswdisk_desc_rebuild(in, n, out, sizeof out);
    CHECK(pswdisk_rd32(out + 32) == 8 && out[PSWDISK_HDR] == 0xA0 && out[PSWDISK_HDR + 7] == 0xA7);
    CHECK(!strcmp(str(out, pswdisk_rd32(out + 16)), "USB: SanDisk Extreme SSD"));
    CHECK(pswdisk_rd32(out + 24) == 0);

    /* not USB: unchanged */
    n = mk(in, PSWDISK_BUS_SATA, "ATA", "Samsung", "1", "2", 0);
    CHECK(pswdisk_desc_rebuild(in, n, out, sizeof out) == 0);
    n = mk(in, 17 /* NVMe */, NULL, "WD", NULL, NULL, 0);
    CHECK(pswdisk_desc_rebuild(in, n, out, sizeof out) == 0);

    /* no strings at all */
    n = mk(in, PSWDISK_BUS_USB, NULL, NULL, NULL, NULL, 0);
    r = pswdisk_desc_rebuild(in, n, out, sizeof out);
    CHECK(!strcmp(str(out, pswdisk_rd32(out + 16)), "USB: disk"));
    CHECK(pswdisk_rd32(out + 20) == 0 && pswdisk_rd32(out + 24) == 0);

    /* empty / blank strings */
    n = mk(in, PSWDISK_BUS_USB, "", "   ", "", "    ", 0);
    r = pswdisk_desc_rebuild(in, n, out, sizeof out);
    CHECK(!strcmp(str(out, pswdisk_rd32(out + 16)), "USB: disk"));
    CHECK(pswdisk_rd32(out + 24) != 0 && !strcmp(str(out, pswdisk_rd32(out + 24)), ""));

    /* only vendor or only product */
    n = mk(in, PSWDISK_BUS_USB, "Generic", NULL, NULL, NULL, 0);
    pswdisk_desc_rebuild(in, n, out, sizeof out);
    CHECK(!strcmp(str(out, pswdisk_rd32(out + 16)), "USB: Generic"));
    n = mk(in, PSWDISK_BUS_USB, NULL, "Flash Disk", NULL, NULL, 0);
    pswdisk_desc_rebuild(in, n, out, sizeof out);
    CHECK(!strcmp(str(out, pswdisk_rd32(out + 16)), "USB: Flash Disk"));

    /* offsets outside the returned data (truncated by the lower driver) or inside the header: treated as absent */
    n = mk(in, PSWDISK_BUS_USB, "JMicron", "JMS583", "1", "SER", 0);
    pswdisk_wr32(in + 16, 900);
    pswdisk_wr32(in + 24, 8);
    pswdisk_desc_rebuild(in, n, out, sizeof out);
    CHECK(!strcmp(str(out, pswdisk_rd32(out + 16)), "USB: JMicron"));
    CHECK(pswdisk_rd32(out + 24) == 0);

    /* string without NUL at the end of the data */
    n = mk(in, PSWDISK_BUS_USB, "ASMedia", "ASM2362", NULL, NULL, 0);
    r = pswdisk_desc_rebuild(in, n - 1, out, sizeof out);       /* last NUL cut off */
    CHECK(r && !strcmp(str(out, pswdisk_rd32(out + 16)), "USB: ASMedia ASM2362"));

    /* very long names are cut to PSWDISK_NAME_MAX */
    {
        char longp[200]; memset(longp, 'X', 199); longp[199] = 0;
        n = mk(in, PSWDISK_BUS_USB, "V", longp, NULL, NULL, 0);
        r = pswdisk_desc_rebuild(in, n, out, sizeof out);
        CHECK(strlen(str(out, pswdisk_rd32(out + 16))) == PSWDISK_NAME_MAX);
        CHECK(r <= pswdisk_desc_bound(n));
    }

    /* Size field smaller than the returned length wins; header-only or short input: refused */
    n = mk(in, PSWDISK_BUS_USB, "A", "B", NULL, NULL, 0);
    CHECK(pswdisk_desc_rebuild(in, 8, out, sizeof out) == 0);
    CHECK(pswdisk_desc_rebuild(in, PSWDISK_HDR - 1, out, sizeof out) == 0);
    CHECK(pswdisk_desc_rebuild(in, PSWDISK_HDR, out, sizeof out) != 0);   /* header only, no strings: "USB: disk" */
    CHECK(!strcmp(str(out, pswdisk_rd32(out + 16)), "USB: disk"));

    /* output capacity too small: nothing written, 0 */
    n = mk(in, PSWDISK_BUS_USB, "Realtek", "RTL9210", NULL, NULL, 0);
    memset(out, 0x5A, sizeof out);
    CHECK(pswdisk_desc_rebuild(in, n, out, PSWDISK_HDR + 4) == 0 && out[0] == 0x5A);

    /* bound always holds, for every truncation of a real-looking descriptor */
    n = mk(in, PSWDISK_BUS_USB, "Realtek ", "RTL9210B-CG     ", "1.00", "012345678904", 12);
    for (unsigned long t = 0; t <= n; t++) {
        r = pswdisk_desc_rebuild(in, t, out, pswdisk_desc_bound(t));
        CHECK(t < PSWDISK_HDR ? r == 0 : (r != 0 && r <= pswdisk_desc_bound(t)));
    }

    printf(fails ? "%d FAILED\n" : "ALL OK\n", fails);
    return fails != 0;
}
