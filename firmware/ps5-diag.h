/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * ps5-diag.h v0.3 (18.11.2026, user: "traffic light" - verdict GREEN/ORANGE/RED from the results, log with the verdict on
 * top and the items grouped by area, always English; v0.2 17.11.2026, user: BIOS "System check") (header-only like
 * ps5-settings.h) - portable core of the read-only compatibility check.
 * The UEFI glue (Ps5DiagRun) gathers raw values through existing, proven read paths; this core only evaluates them
 * (PASS/WARN/FAIL/SKIP), keeps a bounded report, computes the verdict and formats \PS5DIAG.LOG. Host-tested, no I/O,
 * no allocation.
 * v0.3: the check output (titles, verdict, log) is English regardless of the menu language; the lang arguments are kept
 * for the callers' signatures and ignored. */
#ifndef PS5_DIAG_H_
#define PS5_DIAG_H_
#include "icc-power.h"   /* shared stdint/EDK2 type mapping, as button-keys.h */
enum ps5_diag_status { PS5_DIAG_PASS = 0, PS5_DIAG_WARN = 1, PS5_DIAG_FAIL = 2, PS5_DIAG_SKIP = 3 };
enum ps5_diag_id {                 /* order = order on screen and in the log; never renumber (log readers) */
    PS5_DIAG_APU = 0, PS5_DIAG_CORES, PS5_DIAG_PSTATES,
    PS5_DIAG_SMU, PS5_DIAG_TCTL, PS5_DIAG_FAN, PS5_DIAG_EMC_TEMP,
    PS5_DIAG_ICC, PS5_DIAG_BRIDGE, PS5_DIAG_EDID, PS5_DIAG_MODE, PS5_DIAG_REFRESH, PS5_DIAG_DISPCLK, PS5_DIAG_DPPCLK,
    PS5_DIAG_MEMORY, PS5_DIAG_VRAM,
    PS5_DIAG_M2, PS5_DIAG_USB_DISK, PS5_DIAG_ESP_CFG,
    PS5_DIAG_USB_INPUT, PS5_DIAG_NETWORK,
    PS5_DIAG_COUNT
};
#define PS5_DIAG_DETAIL 72
struct ps5_diag_item { uint8_t status, used; char detail[PS5_DIAG_DETAIL]; };
struct ps5_diag_report { struct ps5_diag_item item[PS5_DIAG_COUNT]; unsigned pass, warn, fail, skip; };
/* titles (English only since v0.3); all helpers static inline (header-only) */
static const char *const ps5_diag_titles[PS5_DIAG_COUNT] = {
    "Processor (APU)",
    "CPU cores",
    "CPU P-states",
    "Power controller (SMU)",
    "CPU temperature",
    "Fan",
    "Console temperature",
    "Console controller (EMC)",
    "HDMI bridge",
    "Display data (EDID)",
    "Display mode",
    "Refresh rate",
    "Display clock",
    "Pixel pipe clock",
    "Memory",
    "Graphics memory",
    "Internal M.2 SSD",
    "USB disks",
    "Settings file",
    "USB keyboard",
    "Network",
};
static const char *const ps5_diag_words[4] = { "PASS", "WARN", "FAIL", "SKIP" };
static inline const char *ps5_diag_title(unsigned id, unsigned lang) { (void)lang; return id < PS5_DIAG_COUNT ? ps5_diag_titles[id] : "?"; }
static inline const char *ps5_diag_status_word(unsigned s) { return s < 4 ? ps5_diag_words[s] : "????"; }
static inline void ps5_diag_copy_str(char *d, unsigned cap, const char *s)
{
    unsigned i = 0;
    if (!cap) return;
    if (s) while (s[i] && i + 1 < cap) { d[i] = s[i]; i++; }
    d[i] = 0;
}
static inline void ps5_diag_init(struct ps5_diag_report *r)
{
    unsigned i;
    if (!r) return;
    for (i = 0; i < PS5_DIAG_COUNT; i++) { r->item[i].status = PS5_DIAG_SKIP; r->item[i].used = 0; ps5_diag_copy_str(r->item[i].detail, PS5_DIAG_DETAIL, "not run"); }
    r->pass = r->warn = r->fail = 0; r->skip = PS5_DIAG_COUNT;
}
static inline void ps5_diag_set(struct ps5_diag_report *r, unsigned id, unsigned status, const char *detail)
{
    if (!r || id >= PS5_DIAG_COUNT) return;
    r->item[id].status = (uint8_t)(status <= PS5_DIAG_SKIP ? status : PS5_DIAG_FAIL);
    r->item[id].used = 1;
    ps5_diag_copy_str(r->item[id].detail, PS5_DIAG_DETAIL, detail ? detail : "");
}
static inline void ps5_diag_tally(struct ps5_diag_report *r)
{
    unsigned i;
    if (!r) return;
    r->pass = r->warn = r->fail = r->skip = 0;
    for (i = 0; i < PS5_DIAG_COUNT; i++)
        switch (r->item[i].status) { case PS5_DIAG_PASS: r->pass++; break; case PS5_DIAG_WARN: r->warn++; break;
                                     case PS5_DIAG_FAIL: r->fail++; break; default: r->skip++; }
}
static inline unsigned ps5_diag_eval_clock(uint32_t measured_khz, uint32_t target_khz)
{
    if (!target_khz) return PS5_DIAG_SKIP;
    if ((uint64_t)measured_khz * 100 >= (uint64_t)target_khz * 95) return PS5_DIAG_PASS;
    if ((uint64_t)measured_khz * 100 >= (uint64_t)target_khz * 80) return PS5_DIAG_WARN;
    return PS5_DIAG_FAIL;
}
static inline unsigned ps5_diag_eval_refresh(uint32_t measured_mhz, uint32_t nominal_hz)
{
    uint64_t nom = (uint64_t)nominal_hz * 1000, m = measured_mhz, dev;
    if (!nominal_hz) return PS5_DIAG_SKIP;
    if (!measured_mhz) return PS5_DIAG_FAIL;
    dev = m > nom ? m - nom : nom - m;
    if (dev * 100 <= nom) return PS5_DIAG_PASS;
    if (dev * 100 <= nom * 5) return PS5_DIAG_WARN;
    return PS5_DIAG_FAIL;
}
static inline unsigned ps5_diag_eval_temp(int32_t milli_c)
{
    if (milli_c <= 0 || milli_c > 150000) return PS5_DIAG_FAIL;   /* no or implausible reading */
    if (milli_c < 85000) return PS5_DIAG_PASS;
    if (milli_c < 95000) return PS5_DIAG_WARN;
    return PS5_DIAG_FAIL;
}
/* v0.3: Windows 11 minimums (4 GB RAM, 2 cores) are the FAIL limits; below the PS5 values only WARN */
static inline unsigned ps5_diag_eval_memory(uint32_t usable_mib)
{
    if (usable_mib >= 8192) return PS5_DIAG_PASS;
    if (usable_mib >= 4096) return PS5_DIAG_WARN;
    return PS5_DIAG_FAIL;
}
static inline unsigned ps5_diag_eval_cores(uint32_t enabled)
{
    if (enabled == 16) return PS5_DIAG_PASS;                       /* PS5: 8 cores, 16 threads */
    return enabled >= 2 ? PS5_DIAG_WARN : PS5_DIAG_FAIL;
}
static inline unsigned ps5_diag_eval_apu(uint32_t eax)
{
    uint32_t family = ((eax >> 8) & 0xF) + ((eax >> 20) & 0xFF);
    return family == 0x17 ? PS5_DIAG_PASS : PS5_DIAG_WARN;       /* Zen 2 = family 17h (as Ps5BootWindows r5.11) */
}
static inline uint32_t ps5_diag_dentist_khz(uint32_t did)
{
    uint32_t quarters;                      /* divider in quarters */
    if (did < 0x08 || did > 0x7F) return 0;
    if (did < 0x40) quarters = did;                                   /* 2.0 .. 15.75, step 0.25 */
    else if (did < 0x60) quarters = 64 + (did - 0x40) * 2;            /* 16 .. 31.5, step 0.5 */
    else quarters = 128 + (did - 0x60) * 4;                           /* 32 .. 63, step 1 */
    return 12000000u / quarters;                                      /* 3000 MHz * 4 / quarters, in kHz */
}
static inline unsigned ps5_diag_put(char *o, unsigned n, unsigned cap, const char *s)
{
    while (s && *s && n + 1 < cap) o[n++] = *s++;
    if (cap) o[n < cap ? n : cap - 1] = 0;
    return n;
}
static inline unsigned ps5_diag_put_u(char *o, unsigned n, unsigned cap, unsigned v)
{
    char b[12]; unsigned i = 0;
    do { b[i++] = (char)('0' + v % 10); v /= 10; } while (v && i < sizeof(b));
    while (i && n + 1 < cap) o[n++] = b[--i];
    if (cap) o[n < cap ? n : cap - 1] = 0;
    return n;
}
/* ---- v0.3: verdict ("traffic light") -------------------------------------------------------------------------
 * RED    = a critical item FAILs: Windows cannot be used on this system as it is.
 * ORANGE = any WARN, or a FAIL of a non-critical item: may be incompatible right now or cause unexpected problems.
 * GREEN  = only PASS and SKIP (SKIP = not measured / not applicable, never counts against the system).
 *
 *  Item                      critical  FAIL means (WARN is always ORANGE)
 *  Processor (APU)           no        - (not family 17h is WARN)
 *  CPU cores                 YES       fewer than 2 enabled (Windows 11 minimum); 2..15 = WARN
 *  CPU P-states              no        no enabled P-state (fixed clock in Windows)
 *  Power controller (SMU)    no        - (always SKIP in firmware)
 *  CPU temperature           no        >= 95 C, implausible or unreadable
 *  Fan                       no        EMC gives no fan answer
 *  Console temperature       no        >= 95 C or no answer
 *  Console controller (EMC)  no        ICC does not answer
 *  HDMI bridge               no        - (always SKIP in firmware)
 *  Display data (EDID)       no        - (missing EDID is WARN)
 *  Display mode              no        - (1080p fallback is WARN)
 *  Refresh rate              no        > 5 % off nominal or no frames (the menu itself is visible, so not RED)
 *  Display clock             no        < 80 % of the DENTIST target
 *  Pixel pipe clock          no        < 80 % of the DENTIST target
 *  Memory                    YES       < 4096 MiB usable (Windows 11 minimum) or no memory map; < 8192 = WARN
 *  Graphics memory           YES       no graphics output (GOP) at all: Windows has no display path
 *  Internal M.2 SSD          YES       no disk at all, neither internal nor USB: nothing to install or boot from
 *  USB disks                 no        - (count only)
 *  Settings file             no        \PS5UEFI.CFG damaged (defaults are used, so never RED)
 *  USB keyboard              no        - (none is WARN)
 *  Network                   no        - (no Ethernet is WARN)
 * The boot path itself (Windows Boot Manager on an ESP) is not part of this check; it is reported by the boot menu. */
enum ps5_diag_verdict { PS5_DIAG_GREEN = 0, PS5_DIAG_ORANGE = 1, PS5_DIAG_RED = 2 };
static const uint8_t ps5_diag_critical[PS5_DIAG_COUNT] = {
    0, 1, 0,             /* APU, CORES, PSTATES */
    0, 0, 0, 0,          /* SMU, TCTL, FAN, EMC_TEMP */
    0, 0, 0, 0, 0, 0, 0, /* ICC, BRIDGE, EDID, MODE, REFRESH, DISPCLK, DPPCLK */
    1, 1,                /* MEMORY, VRAM */
    1, 0, 0,             /* M2, USB_DISK, ESP_CFG */
    0, 0,                /* USB_INPUT, NETWORK */
};
static const char *const ps5_diag_verdict_words[3] = { "GREEN", "ORANGE", "RED" };
static const char *const ps5_diag_verdict_texts[3] = {
    "system compatible",
    "system could currently be incompatible or cause unexpected problems",
    "system incompatible",
};
static inline const char *ps5_diag_verdict_word(unsigned v) { return v < 3 ? ps5_diag_verdict_words[v] : "?"; }
static inline const char *ps5_diag_verdict_text(unsigned v) { return v < 3 ? ps5_diag_verdict_texts[v] : "?"; }
/* verdict of a report; *causes (optional) gets one bit per item that made it ORANGE or RED (bit = ps5_diag_id) */
static inline unsigned ps5_diag_verdict(const struct ps5_diag_report *r, uint32_t *causes)
{
    unsigned i, v = PS5_DIAG_GREEN;
    uint32_t c = 0;
    if (r)
        for (i = 0; i < PS5_DIAG_COUNT; i++) {
            unsigned st = r->item[i].status;
            if (st == PS5_DIAG_FAIL && ps5_diag_critical[i]) { v = PS5_DIAG_RED; c |= 1u << i; }
            else if (st == PS5_DIAG_FAIL || st == PS5_DIAG_WARN) { if (v < PS5_DIAG_ORANGE) v = PS5_DIAG_ORANGE; c |= 1u << i; }
        }
    else v = PS5_DIAG_RED;
    if (causes) *causes = c;
    return v;
}
/* areas of the log (and the UEFI log viewer); every item belongs to exactly one, shown in enum order inside it */
#define PS5_DIAG_GROUPS 6
static const char *const ps5_diag_group_names[PS5_DIAG_GROUPS] = {
    "Processor", "Power and cooling", "Display", "Memory", "Storage", "Input and network" };
static const uint8_t ps5_diag_group_of[PS5_DIAG_COUNT] = {
    0, 0, 0,             /* APU, CORES, PSTATES */
    1, 1, 1, 1,          /* SMU, TCTL, FAN, EMC_TEMP */
    1, 2, 2, 2, 2, 2, 2, /* ICC, BRIDGE, EDID, MODE, REFRESH, DISPCLK, DPPCLK */
    3, 2,                /* MEMORY, VRAM (framebuffer = display path) */
    4, 4, 4,             /* M2, USB_DISK, ESP_CFG */
    5, 5,                /* USB_INPUT, NETWORK */
};
#define PS5_DIAG_TITLE_COL 26   /* title column width in the log: status and detail columns line up */
/* "  WARN  CPU cores                 detail\r\n" */
static inline unsigned ps5_diag_put_item(const struct ps5_diag_report *r, unsigned i, char *out, unsigned n, unsigned cap)
{
    const char *t = ps5_diag_title(i, 0);
    unsigned k = 0;
    n = ps5_diag_put(out, n, cap, "  "); n = ps5_diag_put(out, n, cap, ps5_diag_status_word(r->item[i].status));
    n = ps5_diag_put(out, n, cap, "  "); n = ps5_diag_put(out, n, cap, t);
    while (t[k]) k++;
    do n = ps5_diag_put(out, n, cap, " "); while (++k < PS5_DIAG_TITLE_COL);
    n = ps5_diag_put(out, n, cap, r->item[i].detail);
    return ps5_diag_put(out, n, cap, "\r\n");
}
/* \PS5DIAG.LOG: verdict + causes first, then header and counts, then the items grouped by area. English only (lang
 * ignored since v0.3). Returns the length; output is always terminated and never overflows (cap >= 64). */
static inline unsigned ps5_diag_format(const struct ps5_diag_report *r, unsigned lang, const char *firmware, char *out, unsigned cap)
{
    unsigned n = 0, i, g, v;
    uint32_t causes;
    (void)lang;
    if (!r || !out || cap < 64) return 0;
    out[0] = 0;
    v = ps5_diag_verdict(r, &causes);
    n = ps5_diag_put(out, n, cap, "Verdict: "); n = ps5_diag_put(out, n, cap, ps5_diag_verdict_word(v));
    n = ps5_diag_put(out, n, cap, " - "); n = ps5_diag_put(out, n, cap, ps5_diag_verdict_text(v)); n = ps5_diag_put(out, n, cap, "\r\n");
    if (causes) {
        n = ps5_diag_put(out, n, cap, v == PS5_DIAG_RED ? "Caused by (critical FAIL first):\r\n" : "Caused by:\r\n");
        for (i = 0; i < PS5_DIAG_COUNT; i++)   /* RED: the critical failures first, then the rest */
            if ((causes >> i & 1) && v == PS5_DIAG_RED && ps5_diag_critical[i] && r->item[i].status == PS5_DIAG_FAIL) n = ps5_diag_put_item(r, i, out, n, cap);
        for (i = 0; i < PS5_DIAG_COUNT; i++)
            if ((causes >> i & 1) && !(v == PS5_DIAG_RED && ps5_diag_critical[i] && r->item[i].status == PS5_DIAG_FAIL)) n = ps5_diag_put_item(r, i, out, n, cap);
    }
    n = ps5_diag_put(out, n, cap, "\r\nPS5 system check (PSWloader)\r\nFirmware: ");
    n = ps5_diag_put(out, n, cap, firmware ? firmware : "?");
    n = ps5_diag_put(out, n, cap, "\r\nResult: ");
    n = ps5_diag_put_u(out, n, cap, r->pass); n = ps5_diag_put(out, n, cap, " PASS, ");
    n = ps5_diag_put_u(out, n, cap, r->warn); n = ps5_diag_put(out, n, cap, " WARN, ");
    n = ps5_diag_put_u(out, n, cap, r->fail); n = ps5_diag_put(out, n, cap, " FAIL, ");
    n = ps5_diag_put_u(out, n, cap, r->skip); n = ps5_diag_put(out, n, cap, " SKIP\r\n");
    for (g = 0; g < PS5_DIAG_GROUPS; g++) {
        n = ps5_diag_put(out, n, cap, "\r\n== "); n = ps5_diag_put(out, n, cap, ps5_diag_group_names[g]); n = ps5_diag_put(out, n, cap, " ==\r\n");
        for (i = 0; i < PS5_DIAG_COUNT; i++) if (ps5_diag_group_of[i] == g) n = ps5_diag_put_item(r, i, out, n, cap);
    }
    return n;
}
#endif
