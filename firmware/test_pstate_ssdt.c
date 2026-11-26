/* host test for ps5-pstate-ssdt.h v1.0: decode + AML; writes the SSDT for iasl -d */
#include <stdio.h>
#include <string.h>
#include "ps5-pstate-ssdt.h"
static unsigned long long mk(unsigned fid, unsigned did, unsigned vid, unsigned idd, unsigned div)
{ return 1ull << 63 | (unsigned long long)div << 30 | (unsigned long long)idd << 22 | (unsigned long long)vid << 14 | did << 8 | fid; }
int main(int argc, char **argv)
{
    static const unsigned dids[8] = { 8, 10, 11, 13, 14, 16, 20, 32 }, want[8] = { 3200, 2560, 2327, 1969, 1828, 1600, 1280, 800 };
    unsigned long long def[8]; struct ps5_pstate ps[8]; int fail = 0;
    for (int i = 0; i < 8; i++) def[i] = mk(128, dids[i], 0x40 + 4 * i, 100 - 8 * i, 1);
    unsigned n = ps5_pstate_decode(def, 0x70, ps);
    if (n != 8) { printf("FAIL count %u\n", n); fail++; }
    for (unsigned i = 0; i < n; i++) if (ps[i].mhz != want[i]) { printf("FAIL P%u %u\n", i, ps[i].mhz); fail++; }
    printf("P0 %u MHz %u mW (vid 0x40 -> 1150 mV, 10.0 A)\n", ps[0].mhz, ps[0].mw);
    if (ps[0].mw != 11500) { printf("FAIL power\n"); fail++; }
    unsigned long long d2[8]; memcpy(d2, def, sizeof d2); d2[3] &= ~(1ull << 63);
    if (ps5_pstate_decode(d2, 0x70, ps) != 3) { printf("FAIL stop at disabled\n"); fail++; }
    if (ps5_pstate_decode(def, 0x30, ps) != 4) { printf("FAIL PstateMaxVal\n"); fail++; }
    if (ps5_pstate_decode(def, 0x70, ps) != 8) fail++;
    static unsigned char buf[8192]; unsigned len = ps5_pstate_ssdt(ps, 8, buf, sizeof buf);
    unsigned char s = 0; for (unsigned i = 0; i < len; i++) s += buf[i];
    printf("SSDT %u bytes, checksum %s\n", len, s == 0 ? "ok" : "BAD"); if (!len || s) fail++;
    if (ps5_pstate_ssdt(ps, 8, buf, 200) != 0) { printf("FAIL small cap\n"); fail++; }
    len = ps5_pstate_ssdt(ps, 8, buf, sizeof buf);
    if (argc > 1) { FILE *f = fopen(argv[1], "wb"); fwrite(buf, 1, len, f); fclose(f); }
    printf(fail ? "FAILED %d\n" : "ALL OK\n", fail); return fail != 0;
}
