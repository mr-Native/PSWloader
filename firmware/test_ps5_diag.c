/* host test ps5-diag.h v0.2 (v0.2 18.11.2026: verdict GREEN/ORANGE/RED, grouped English log; v0.1: evaluation rules
 * against HW values of 16./17.11.2026, report, log format)
 * build + run: gcc -std=c99 -Wall -Wextra -I. -o /tmp/test_ps5_diag test_ps5_diag.c && /tmp/test_ps5_diag */
#include <stdio.h>
#include <string.h>
#include "ps5-diag.h"
static int fails;
#define STARTS(s, lit) (strncmp((s), (lit), sizeof(lit) - 1) == 0)
#define CHECK(x) do { if (!(x)) { printf("FAIL %d %s\n", __LINE__, #x); fails++; } } while (0)
int main(void)
{
    /* DENTIST: GameOS 0x64 = 83.4 MHz, p60 0x45 = 162.2, p100 0x2C = 272.7, DISPCLK 0x15 = 571 (all measured on HW) */
    CHECK(ps5_diag_dentist_khz(0x64) == 83333); CHECK(ps5_diag_dentist_khz(0x45) == 162162);
    CHECK(ps5_diag_dentist_khz(0x2C) == 272727); CHECK(ps5_diag_dentist_khz(0x15) == 571428);
    CHECK(ps5_diag_dentist_khz(0x07) == 0 && ps5_diag_dentist_khz(0x80) == 0 && ps5_diag_dentist_khz(0x7F) == 12000000u / 252);
    /* CLK4 counter (0.1 MHz) against the DENTIST target: r5.16.6 p100 dppclk 2727 -> PASS; the old 83.4 MHz -> FAIL */
    CHECK(ps5_diag_eval_clock(272700, ps5_diag_dentist_khz(0x2C)) == PS5_DIAG_PASS);
    CHECK(ps5_diag_eval_clock(83400, ps5_diag_dentist_khz(0x2C)) == PS5_DIAG_FAIL);
    CHECK(ps5_diag_eval_clock(240000, 272727) == PS5_DIAG_WARN);   /* 88 % */
    CHECK(ps5_diag_eval_clock(1, 0) == PS5_DIAG_SKIP);
    /* refresh: 99.842 Hz at nominal 100 PASS, 59.774 at 60 PASS, 95 at 100 WARN, 62.5 at 100 FAIL */
    CHECK(ps5_diag_eval_refresh(99842, 100) == PS5_DIAG_PASS); CHECK(ps5_diag_eval_refresh(59774, 60) == PS5_DIAG_PASS);
    CHECK(ps5_diag_eval_refresh(95000, 100) == PS5_DIAG_WARN); CHECK(ps5_diag_eval_refresh(62500, 100) == PS5_DIAG_FAIL);
    CHECK(ps5_diag_eval_refresh(0, 60) == PS5_DIAG_FAIL); CHECK(ps5_diag_eval_refresh(60000, 0) == PS5_DIAG_SKIP);
    /* temperature: 62.5 C PASS, 88 WARN, 96 FAIL, 0/implausible FAIL */
    CHECK(ps5_diag_eval_temp(62500) == PS5_DIAG_PASS); CHECK(ps5_diag_eval_temp(88000) == PS5_DIAG_WARN);
    CHECK(ps5_diag_eval_temp(96000) == PS5_DIAG_FAIL); CHECK(ps5_diag_eval_temp(0) == PS5_DIAG_FAIL);
    CHECK(ps5_diag_eval_temp(200000) == PS5_DIAG_FAIL);
    /* APU: Zen 2 family 17h (base 0xF + ext 0x08) PASS, family 19h WARN */
    CHECK(ps5_diag_eval_apu(0x00870F10) == PS5_DIAG_PASS); CHECK(ps5_diag_eval_apu(0x00A20F10) == PS5_DIAG_WARN);
    /* report */
    struct ps5_diag_report r; char log[4096]; unsigned n;
    ps5_diag_init(&r); ps5_diag_tally(&r);
    CHECK(r.skip == PS5_DIAG_COUNT && r.pass == 0);
    ps5_diag_set(&r, PS5_DIAG_DPPCLK, PS5_DIAG_PASS, "272.7 MHz (target 272.7, 100%)");
    ps5_diag_set(&r, PS5_DIAG_REFRESH, PS5_DIAG_WARN, "95.000 Hz (nominal 100)");
    ps5_diag_set(&r, PS5_DIAG_COUNT, PS5_DIAG_PASS, "ignored");                       /* out of range: ignored */
    ps5_diag_set(&r, PS5_DIAG_FAN, 7, "bad status");                                  /* invalid status -> FAIL */
    { char longd[200]; memset(longd, 'x', sizeof longd - 1); longd[sizeof longd - 1] = 0; ps5_diag_set(&r, PS5_DIAG_APU, PS5_DIAG_PASS, longd); }
    CHECK(strlen(r.item[PS5_DIAG_APU].detail) == PS5_DIAG_DETAIL - 1);              /* truncated, terminated */
    ps5_diag_tally(&r);
    CHECK(r.pass == 2 && r.warn == 1 && r.fail == 1 && r.skip == PS5_DIAG_COUNT - 4);
    n = ps5_diag_format(&r, 0, "r5.17", log, sizeof log);
    CHECK(n > 0 && n == strlen(log));
    /* fan FAIL (not critical) + refresh WARN -> ORANGE, causes listed right under the verdict, aligned columns */
    CHECK(STARTS(log, "Verdict: ORANGE - system could currently be incompatible or cause unexpected problems\r\nCaused by:\r\n"));
    CHECK(strstr(log, "Caused by:\r\n  FAIL  Fan                       bad status\r\n  WARN  Refresh rate              95.000 Hz"));
    CHECK(strstr(log, "\r\n== Display ==\r\n  SKIP  HDMI bridge               not run\r\n"));
    CHECK(strstr(log, "  PASS  Pixel pipe clock          272.7 MHz"));
    CHECK(strstr(log, "Result: 2 PASS, 1 WARN, 1 FAIL, 17 SKIP\r\n"));
    CHECK(strstr(log, "Firmware: r5.17\r\n"));
    /* groups in order, every item exactly once in the grouped part */
    { const char *p = strstr(log, "== Processor =="), *q = strstr(log, "== Power and cooling =="), *d = strstr(log, "== Display =="),
                   *m = strstr(log, "== Memory =="), *s = strstr(log, "== Storage =="), *u = strstr(log, "== Input and network ==");
      CHECK(p && q && d && m && s && u && p < q && q < d && d < m && m < s && s < u);
      CHECK(strstr(d, "Graphics memory") && strstr(d, "Graphics memory") < m); }
    /* the lang argument no longer changes anything: always English */
    { char log2[4096]; ps5_diag_format(&r, 1, "r5.17", log2, sizeof log2); CHECK(strcmp(log, log2) == 0); }
    CHECK(strcmp(ps5_diag_title(PS5_DIAG_FAN, 1), "Fan") == 0);

    /* ---- verdict: GREEN / ORANGE / RED ---- */
    { struct ps5_diag_report v; uint32_t c = 123; unsigned i;
      ps5_diag_init(&v);                                                  /* all SKIP (nothing measured) */
      CHECK(ps5_diag_verdict(&v, &c) == PS5_DIAG_GREEN && c == 0);
      for (i = 0; i < PS5_DIAG_COUNT; i++) ps5_diag_set(&v, i, PS5_DIAG_PASS, "ok");
      ps5_diag_set(&v, PS5_DIAG_SMU, PS5_DIAG_SKIP, "not queried");
      ps5_diag_tally(&v);
      CHECK(ps5_diag_verdict(&v, &c) == PS5_DIAG_GREEN && c == 0);
      n = ps5_diag_format(&v, 0, "x", log, sizeof log);
      CHECK(STARTS(log, "Verdict: GREEN - system compatible\r\n\r\nPS5 system check") && !strstr(log, "Caused by"));
      /* every WARN is ORANGE, also on a critical item */
      ps5_diag_set(&v, PS5_DIAG_MEMORY, PS5_DIAG_WARN, "6144 MiB usable");
      CHECK(ps5_diag_verdict(&v, &c) == PS5_DIAG_ORANGE && c == 1u << PS5_DIAG_MEMORY);
      /* non-critical FAILs (settings file damaged, CPU hot, clocks low) stay ORANGE */
      ps5_diag_set(&v, PS5_DIAG_MEMORY, PS5_DIAG_PASS, "ok");
      ps5_diag_set(&v, PS5_DIAG_ESP_CFG, PS5_DIAG_FAIL, "damaged");
      ps5_diag_set(&v, PS5_DIAG_TCTL, PS5_DIAG_FAIL, "96.0 C");
      ps5_diag_set(&v, PS5_DIAG_DPPCLK, PS5_DIAG_FAIL, "83.4 MHz");
      CHECK(ps5_diag_verdict(&v, &c) == PS5_DIAG_ORANGE && c == ((1u << PS5_DIAG_ESP_CFG) | (1u << PS5_DIAG_TCTL) | (1u << PS5_DIAG_DPPCLK)));
      /* each critical FAIL alone is RED */
      { static const unsigned crit[] = { PS5_DIAG_CORES, PS5_DIAG_MEMORY, PS5_DIAG_VRAM, PS5_DIAG_M2 };
        for (i = 0; i < 4; i++) { struct ps5_diag_report w = v; ps5_diag_set(&w, crit[i], PS5_DIAG_FAIL, "critical");
                                  CHECK(ps5_diag_verdict(&w, &c) == PS5_DIAG_RED && (c >> crit[i] & 1)); } }
      for (i = 0, n = 0; i < PS5_DIAG_COUNT; i++) n += ps5_diag_critical[i];
      CHECK(n == 4);
      /* RED log: critical failure listed first, then the ORANGE causes */
      ps5_diag_set(&v, PS5_DIAG_M2, PS5_DIAG_FAIL, "no disk at all (internal or USB): nothing to boot");
      ps5_diag_tally(&v);
      n = ps5_diag_format(&v, 0, "x", log, sizeof log);
      CHECK(STARTS(log, "Verdict: RED - system incompatible\r\nCaused by (critical FAIL first):\r\n  FAIL  Internal M.2 SSD"));
      CHECK(strstr(log, "  FAIL  Internal M.2 SSD          no disk at all") < strstr(log, "  FAIL  CPU temperature"));
      CHECK(ps5_diag_verdict(NULL, &c) == PS5_DIAG_RED);
      CHECK(strcmp(ps5_diag_verdict_word(PS5_DIAG_ORANGE), "ORANGE") == 0 && strcmp(ps5_diag_verdict_word(9), "?") == 0); }
    /* evaluation limits used by the critical items (Windows 11 minimums) */
    CHECK(ps5_diag_eval_memory(13000) == PS5_DIAG_PASS && ps5_diag_eval_memory(6000) == PS5_DIAG_WARN && ps5_diag_eval_memory(3000) == PS5_DIAG_FAIL);
    CHECK(ps5_diag_eval_cores(16) == PS5_DIAG_PASS && ps5_diag_eval_cores(8) == PS5_DIAG_WARN && ps5_diag_eval_cores(1) == PS5_DIAG_FAIL);
    /* worst case (every item FAIL with a full detail) fits the 6144-byte log/variable buffer (PS5_DIAG_LOG_MAX) */
    { struct ps5_diag_report w; char big[8192], longd[PS5_DIAG_DETAIL + 8]; unsigned i;
      memset(longd, 'y', sizeof longd - 1); longd[sizeof longd - 1] = 0;
      ps5_diag_init(&w); for (i = 0; i < PS5_DIAG_COUNT; i++) ps5_diag_set(&w, i, PS5_DIAG_FAIL, longd);
      ps5_diag_tally(&w);
      n = ps5_diag_format(&w, 0, "PSWloader v1.0 (r5.25.3), EDK II rev 10000", big, sizeof big);
      CHECK(n < 6144);
      printf("worst-case log %u bytes\n", n);
      { unsigned lines = 0; for (i = 0; big[i]; i++) lines += big[i] == '\n'; CHECK(lines <= 60); printf("worst-case lines %u\n", lines); } }
    /* small buffer: never overflows, always terminated */
    char small[80]; memset(small, 0x55, sizeof small);
    n = ps5_diag_format(&r, 0, "r5.17", small, 64);
    CHECK(n < 64 && small[n] == 0 && (unsigned char)small[64] == 0x55);
    CHECK(ps5_diag_format(&r, 0, "x", small, 10) == 0);
    /* every check has a title that fits the title column and belongs to an area */
    for (unsigned i = 0; i < PS5_DIAG_COUNT; i++) CHECK(ps5_diag_title(i, 0)[0] && strlen(ps5_diag_title(i, 0)) < PS5_DIAG_TITLE_COL && ps5_diag_group_of[i] < PS5_DIAG_GROUPS);
    printf("%s (%d failures)\n", fails ? "FAILED" : "ALL OK", fails);
    return fails != 0;
}
