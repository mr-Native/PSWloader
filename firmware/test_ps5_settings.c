/* host test ps5-settings.h v1.10 (resolution); v1.6: round trip, bootlist,, pstates + refresh + diag keys, unknown keys kept, old files unchanged */
#include <stdio.h>
#include <string.h>
#include "ps5-settings.h"
static unsigned crc32(const char *d, unsigned n) { unsigned c = ~0u; for (unsigned i = 0; i < n; i++) { c ^= (unsigned char)d[i]; for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xEDB88320u & -(c & 1)); } return ~c; }
int main(void)
{
    int fail = 0; char t[200]; struct ps5_settings s, r, q2;
    ps5_settings_default(&s); s.preset = 2; s.lang = 1; s.fastboot = 1;
    unsigned n = ps5_settings_format(&s, crc32, t, sizeof t);
    if (strstr(t, "pstates")) { puts("FAIL pstates written when 0"); fail++; }
    if (!ps5_settings_parse(t, n, crc32, &r) || r.pstates != 0 || r.preset != 2 || r.fastboot != 1) { puts("FAIL rt0"); fail++; }
    s.pstates = 1; n = ps5_settings_format(&s, crc32, t, sizeof t);
    if (!strstr(t, "pstates=1\r\ncrc32=")) { puts("FAIL pstates line"); fail++; }
    if (!ps5_settings_parse(t, n, crc32, &r) || r.pstates != 1 || r.lang != 1) { puts("FAIL rt1"); fail++; }
    t[5] ^= 1; if (ps5_settings_parse(t, n, crc32, &r) || r.pstates != 0) { puts("FAIL crc"); fail++; }
    s.preset = 1; s.lang = 0; s.fastboot = 0; s.pstates = 0; n = ps5_settings_format(&s, crc32, t, sizeof t);   /* v1.2: refresh */
    if (strstr(t, "refresh")) { puts("FAIL refresh written when native"); fail++; }
    s.refresh = 1; n = ps5_settings_format(&s, crc32, t, sizeof t);
    if (!strstr(t, "refresh=max\r\ncrc32=")) { puts("FAIL refresh line"); fail++; }
    if (!ps5_settings_parse(t, n, crc32, &r) || r.refresh != 1 || r.pstates != 0) { puts("FAIL rt refresh"); fail++; }
    { char o[200]; struct ps5_settings q; ps5_settings_default(&q); q.preset = 2;   /* a v1.1 file parses with refresh native */
      unsigned m = ps5_settings_format(&q, crc32, o, sizeof o); if (!ps5_settings_parse(o, m, crc32, &r) || r.refresh != 0) { puts("FAIL v1.1 compat"); fail++; } }
    ps5_settings_default(&s); n = ps5_settings_format(&s, crc32, t, sizeof t);   /* v1.3: diag */
    if (strstr(t, "diag")) { puts("FAIL diag written when absent"); fail++; }
    s.diag = 1; s.refresh = 1; n = ps5_settings_format(&s, crc32, t, sizeof t);
    if (!strstr(t, "refresh=max\r\ndiag=once\r\ncrc32=")) { puts("FAIL diag once line"); fail++; }
    if (!ps5_settings_parse(t, n, crc32, &r) || r.diag != 1 || r.refresh != 1) { puts("FAIL rt diag once"); fail++; }
    r.diag = 2; n = ps5_settings_format(&r, crc32, t, sizeof t);              /* firmware reset after the run */
    if (!strstr(t, "diag=off\r\ncrc32=") || !ps5_settings_parse(t, n, crc32, &s) || s.diag != 2 || s.refresh != 1) { puts("FAIL diag off"); fail++; }
    { const char *bad = "# x\r\ndiag=\r\n"; unsigned m = (unsigned)strlen(bad); char b[64]; memcpy(b, bad, m);
      unsigned c = crc32(b, m); m += (unsigned)sprintf(b + m, "crc32=%08X\r\n", c);
      if (!ps5_settings_parse(b, m, crc32, &r) || r.diag != 0) { puts("FAIL empty diag value"); fail++; } }
    { char b[400], o[400]; unsigned m = 0;   /* v1.4: unknown keys survive parse -> format, invalid known values do not */
      const char *in = "# x\r\npreset=oc\r\nfuture=abc 1\r\nrefresh=bogus\r\n# note\r\nhdr=on\r\n";
      m = (unsigned)strlen(in); memcpy(b, in, m); unsigned c = crc32(b, m); m += (unsigned)sprintf(b + m, "crc32=%08X\r\n", c);
      if (!ps5_settings_parse(b, m, crc32, &r) || r.preset != 2 || r.refresh != 0) { puts("FAIL extra parse"); fail++; }
      unsigned k = ps5_settings_format(&r, crc32, o, sizeof o);
      if (!strstr(o, "preset=oc\r\nlang=en\r\nfastboot=0\r\nbootorder=internal\r\nfuture=abc 1\r\nhdr=on\r\ncrc32=") || strstr(o, "bogus") || strstr(o, "note"))
        { puts("FAIL extra format"); puts(o); fail++; }
      if (!ps5_settings_parse(o, k, crc32, &q2) || q2.extra_len != r.extra_len) { puts("FAIL extra round trip"); fail++; }
      char big[600]; m = (unsigned)sprintf(big, "# x\r\n"); for (int i = 0; i < 20; i++) m += (unsigned)sprintf(big + m, "key%02d=0123456789\r\n", i);
      c = crc32(big, m); m += (unsigned)sprintf(big + m, "crc32=%08X\r\n", c);
      if (!ps5_settings_parse(big, m, crc32, &r) || r.extra_len >= PS5_SET_EXTRA || r.extra_len % 18 != 0 || r.extra_len < PS5_SET_EXTRA - 18) { puts("FAIL extra cap"); fail++; } }
    ps5_settings_default(&s); s.diag = 3; n = ps5_settings_format(&s, crc32, t, sizeof t);   /* v1.5: diag=full */
    if (!strstr(t, "diag=full\r\ncrc32=") || !ps5_settings_parse(t, n, crc32, &r) || r.diag != 3) { puts("FAIL diag full"); fail++; }
    { char b[600]; struct ps5_settings q, w; ps5_settings_default(&q);   /* v1.6: bootlist */
      q.bootlist_n = 3; q.bootlist[0] = 0x1A2B3C4Du; q.bootlist[1] = 0xDEADBEEFu; q.bootlist[2] = 7;
      unsigned m = ps5_settings_format(&q, crc32, b, sizeof b);
      if (!strstr(b, "bootlist=1A2B3C4D,DEADBEEF,00000007\r\ncrc32=")) { puts("FAIL bootlist line"); puts(b); fail++; }
      if (!ps5_settings_parse(b, m, crc32, &w) || w.bootlist_n != 3 || w.bootlist[1] != 0xDEADBEEFu || w.bootlist[2] != 7) { puts("FAIL bootlist rt"); fail++; }
      const char *bad = "# x\r\nbootlist=11111111,11111111,zz,22222222\r\n"; m = (unsigned)strlen(bad); memcpy(b, bad, m);
      unsigned c = crc32(b, m); m += (unsigned)sprintf(b + m, "crc32=%08X\r\n", c);
      if (!ps5_settings_parse(b, m, crc32, &w) || w.bootlist_n != 1 || w.bootlist[0] != 0x11111111u) { puts("FAIL bootlist dup/bad"); fail++; }
      ps5_settings_default(&q); m = ps5_settings_format(&q, crc32, b, sizeof b); if (strstr(b, "bootlist")) { puts("FAIL bootlist written when empty"); fail++; }
      /* order: stored first, absent skipped, new appended; legacy usb_first without a list */
      unsigned stored[3] = { 0xB, 0xA, 0xC }, present[3] = { 0xA, 0xB, 0xD }, out[16]; unsigned char usb[3] = { 0, 1, 1 };
      unsigned k = ps5_bootlist_order(stored, 3, present, usb, 3, 0, out);
      if (k != 3 || out[0] != 1 || out[1] != 0 || out[2] != 2) { puts("FAIL bootlist order"); fail++; }
      k = ps5_bootlist_order(stored, 0, present, usb, 3, 1, out);
      if (k != 3 || out[0] != 1 || out[1] != 2 || out[2] != 0) { puts("FAIL bootlist usb_first migration"); fail++; }
      k = ps5_bootlist_order(stored, 0, present, usb, 3, 0, out);
      if (k != 3 || out[0] != 0 || out[1] != 1 || out[2] != 2) { puts("FAIL bootlist discovery order"); fail++; }
      /* merge: old A,X(unplugged),B; user sets B,A and a new C -> B,X,A,C */
      unsigned old[3] = { 0xA, 0xF, 0xB }, neword[3] = { 0xB, 0xA, 0xC }, mg[16];
      k = ps5_bootlist_merge(old, 3, neword, 3, mg);
      if (k != 4 || mg[0] != 0xB || mg[1] != 0xF || mg[2] != 0xA || mg[3] != 0xC) { puts("FAIL bootlist merge"); fail++; }
      k = ps5_bootlist_merge(old, 0, neword, 3, mg);
      if (k != 3 || mg[0] != 0xB || mg[2] != 0xC) { puts("FAIL bootlist merge empty"); fail++; }
      unsigned big[16], bn[16]; for (int i = 0; i < 16; i++) { big[i] = 100 + i; bn[i] = 200 + i; }
      k = ps5_bootlist_merge(big, 16, bn, 16, mg); if (k != 16) { puts("FAIL bootlist merge cap"); fail++; }
      q.bootlist_n = 16; for (int i = 0; i < 16; i++) q.bootlist[i] = 0xFFFFFF00u + i; q.refresh = 1; q.diag = 1; q.pstates = 1;
      m = ps5_settings_format(&q, crc32, b, sizeof b); printf("max len with bootlist %u (Save buffer 640)\n", m); if (m + PS5_SET_EXTRA + 16 > 640) { puts("FAIL too long"); fail++; } }
    ps5_settings_default(&s); n = ps5_settings_format(&s, crc32, t, sizeof t);   /* v1.7: testsign */
    if (strstr(t, "testsign")) { puts("FAIL testsign written when auto"); fail++; }
    s.testsign = 1; n = ps5_settings_format(&s, crc32, t, sizeof t);
    if (!strstr(t, "testsign=off\r\ncrc32=") || !ps5_settings_parse(t, n, crc32, &r) || r.testsign != 1) { puts("FAIL testsign off"); fail++; }
    s.preset = 0; s.bootorder = 1; s.pstates = 1; s.fastboot = 1; s.lang = 1; s.refresh = 1; s.diag = 1; n = ps5_settings_format(&s, crc32, t, sizeof t);
    printf("max len %u (Save buffer 640)\n", n); if (n > 150) fail++;
    {   /* r5.24 (user: English is the default): no / empty / damaged file -> defaults with lang=en; a file without lang= -> en */
        struct ps5_settings d; char o[512]; unsigned m;
        ps5_settings_default(&d);
        if (d.lang != 0) { puts("FAIL default lang"); fail++; }
        if (ps5_settings_parse("", 0, crc32, &d) || ps5_settings_parse("garbage\r\n", 9, crc32, &d) || d.lang != 0) { puts("FAIL empty/garbage keeps en"); fail++; }
        ps5_settings_default(&d); d.preset = 2; m = ps5_settings_format(&d, crc32, o, sizeof o); o[m] = 0;
        if (!strstr(o, "\r\nlang=en\r\n")) { puts("FAIL default written as lang=en"); fail++; }
        {   /* drop the lang line, fix the CRC by formatting again from a parse without lang */
            struct ps5_settings e; const char *nolang = "# PSWloader settings v1\r\npreset=oc\r\n";
            ps5_settings_default(&e);
            (void)ps5_settings_parse(nolang, (unsigned)strlen(nolang), crc32, &e);
            if (e.lang != 0) { puts("FAIL file without lang -> en"); fail++; }
        }
    }
    {   /* r5.25.1: usbpowercycle is a known key, default 1 (not written), 0 written and read back; r5.25 files with =1 stay valid */
        struct ps5_settings w, u; char f[640]; unsigned fn;
        ps5_settings_default(&w);
        if (w.usbcycle != 1) { puts("FAIL usbcycle default"); fail++; }
        fn = ps5_settings_format(&w, crc32, f, sizeof f); f[fn] = 0;
        if (strstr(f, "usbpowercycle")) { puts("FAIL default written"); fail++; }
        w.usbcycle = 0; fn = ps5_settings_format(&w, crc32, f, sizeof f); f[fn] = 0;
        if (!strstr(f, "\r\nusbpowercycle=0\r\n")) { puts("FAIL usbpowercycle=0 not written"); fail++; }
        ps5_settings_default(&u);
        if (!ps5_settings_parse(f, fn, crc32, &u) || u.usbcycle != 0 || u.extra_len) { puts("FAIL usbpowercycle=0 read back / ended in extra"); fail++; }
        ps5_settings_default(&w); strcpy(w.extra, "usbpowercycle=1\r\n"); w.extra_len = (unsigned)strlen(w.extra);   /* an r5.25 file */
        fn = ps5_settings_format(&w, crc32, f, sizeof f); ps5_settings_default(&u);
        if (!ps5_settings_parse(f, fn, crc32, &u) || u.usbcycle != 1 || u.extra_len) { puts("FAIL r5.25 usbpowercycle=1 file"); fail++; }
    }
    {   /* v1.10 (r5.27): resolution, written only when not auto; an old firmware keeps it as an unknown key */
        char f[700]; struct ps5_settings w, u; unsigned fn;
        ps5_settings_default(&w); fn = ps5_settings_format(&w, crc32, f, sizeof f); f[fn] = 0;
        if (strstr(f, "resolution")) { puts("FAIL resolution written when auto"); fail++; }
        w.resolution = 3; w.refresh = 1; fn = ps5_settings_format(&w, crc32, f, sizeof f); f[fn] = 0;
        if (!strstr(f, "refresh=max\r\nresolution=2560x1440@60\r\n")) { puts("FAIL resolution line"); fail++; }
        ps5_settings_default(&u);
        if (!ps5_settings_parse(f, fn, crc32, &u) || u.resolution != 3 || u.refresh != 1 || u.extra_len) { puts("FAIL resolution read back"); fail++; }
        for (unsigned i = 1; i < PS5_SET_RES_COUNT; i++) {
            w.resolution = i; fn = ps5_settings_format(&w, crc32, f, sizeof f); ps5_settings_default(&u);
            if (!ps5_settings_parse(f, fn, crc32, &u) || u.resolution != i) { printf("FAIL resolution %u round trip\n", i); fail++; }
        }
        ps5_settings_default(&w); strcpy(w.extra, "resolution=640x480@60\r\n"); w.extra_len = (unsigned)strlen(w.extra);   /* invalid value */
        fn = ps5_settings_format(&w, crc32, f, sizeof f); ps5_settings_default(&u);
        if (!ps5_settings_parse(f, fn, crc32, &u) || u.resolution != 0 || u.extra_len) { puts("FAIL invalid resolution not dropped"); fail++; }
        ps5_settings_default(&w); w.resolution = 6; w.bootlist_n = PS5_BOOTLIST_MAX; for (unsigned i = 0; i < PS5_BOOTLIST_MAX; i++) w.bootlist[i] = 0x10000000u + i;
        strcpy(w.extra, "x-tool-key-with-a-long-value=0123456789012345678901234567890123456789\r\n"); w.extra_len = (unsigned)strlen(w.extra);
        w.refresh = 1; w.diag = 3; w.testsign = 1; w.usbcycle = 0; w.fastrestart = 1; w.pstates = 1; w.fastboot = 1;
        { char g[640]; fn = ps5_settings_format(&w, crc32, g, sizeof g); ps5_settings_default(&u);   /* firmware buffer size */
          if (fn + 1 >= sizeof g || !ps5_settings_parse(g, fn, crc32, &u) || u.resolution != 6 || u.bootlist_n != PS5_BOOTLIST_MAX) { printf("FAIL full file in 640 bytes (%u)\n", fn); fail++; } }
    }
    puts(fail ? "FAILED" : "ALL OK"); return fail;
}
