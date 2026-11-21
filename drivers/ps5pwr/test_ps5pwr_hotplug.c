/* SPDX-License-Identifier: GPL-2.0-only
 * Host test for the ps5pwr v1.4 hotplug core with a mocked ICC mailbox (EMC side simulated at the
 * register level used by icc-power.c: doorbell status 0x14, length 0x808, body 0x800, EMW/EMR, ACK).
 * Build: see run-hotplug-tests.sh (icc-power.c with -DPS5_ICC_LINUX_NOTIFY like ps5pwr.sys). */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ps5pwr-hotplug.h"
#include "../../firmware/flava3-prepare.h"
#include "../../firmware/flava3-video.h"
#include "../../firmware/ps5-display-mode.h"
#include "../../firmware/ps5-hdmi-mode-var.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); } } while (0)

static int verbose;
static char last_log[256];
static unsigned log_lines;
void ps5_hp_log(void *ctx, const char *fmt, ...)
{
    va_list ap; (void)ctx;
    va_start(ap, fmt); vsnprintf(last_log, sizeof(last_log), fmt, ap); va_end(ap);
    log_lines++;
    if (verbose) printf("  log: %s\n", last_log);
}

/* ------------------------------------------------------------------ mocked mailbox */
#define MAXQ 64
struct mock {
    uint8_t mbox[0x1000];
    uint32_t status;                     /* doorbell region offset 0x14: bit0 inbound, bit1 transport ACK */
    uint8_t in[MAXQ][0x7f0]; unsigned in_len[MAXQ], in_head, in_count;
    int presenting, endless;
    uint64_t ticks;
    uint8_t regs[0x10000];               /* FLAVA3 register file for register reads */
    uint8_t sub[64][0x7e4]; unsigned sub_len[64], submits;   /* HDMI command packets as received */
    unsigned stop_hdcp, config_reads, reg_reads;
    int fail_submit_from, fail_submit_to;     /* service error for submits with index in [from, to) */
    int unplug_at_submit;                     /* inject an unplug notification with this submit (1-based) */
    unsigned acks;
} M;

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void checksum(uint8_t *m, unsigned n)
{
    uint16_t s = 0;
    for (unsigned i = 0; i < n; i++) if (i != 10 && i != 11) s = (uint16_t)(s + m[i]);
    put16(m + 10, s);
}
static void present(void)
{
    if (M.presenting) return;
    if (!M.in_count && M.endless) {   /* endless stream of HDMI status notifications (EMC style, magic 0x24) */
        uint8_t *m = M.in[(M.in_head + M.in_count) % MAXQ]; unsigned n = 32;
        memset(m, 0, n); m[0] = 0x24; m[1] = 0x10; put16(m + 2, 0x8001); put16(m + 8, n); m[12] = 0x10; m[13] = 0x07;
        checksum(m, n); M.in_len[(M.in_head + M.in_count++) % MAXQ] = n;
    }
    if (!M.in_count) return;
    unsigned n = M.in_len[M.in_head];
    memset(M.mbox + 0x800, 0, 0x7f0);
    memcpy(M.mbox + 0x800, M.in[M.in_head], n < 0x7f0 ? n : 0x7f0);
    if (n < 12 || n > 0x7f0) put16(M.mbox + 0x808, n);   /* wrong wire length announced */
    M.status |= 1; M.presenting = 1;
}
static void queue_raw(const uint8_t *m, unsigned n, unsigned declared)
{
    uint8_t *d = M.in[(M.in_head + M.in_count) % MAXQ];
    memset(d, 0, 0x7f0); memcpy(d, m, n);
    M.in_len[(M.in_head + M.in_count++) % MAXQ] = declared;
    present();
}
/* notification: magic 0x24 like the EMC (edid5 log: head 24 10 01 80), payload as given */
static void queue_note(uint8_t svc, uint16_t type, const uint8_t *payload, unsigned n)
{
    uint8_t m[0x7f0] = { 0 };
    m[0] = 0x24; m[1] = svc; put16(m + 2, type); put16(m + 8, 12 + n);
    memcpy(m + 12, payload, n); checksum(m, 12 + n);
    queue_raw(m, 12 + n, 12 + n);
}
/* as logged by edid5/edid6 on the console (14.11.): svc 10 type 8001, rx 1 len 32 data 10 01 00 00 (20 B payload),
 * rx 2 len 272 data 10 02 00 01 + 256 B EDID with byte 0 = 01 */
static void queue_unplug(void) { uint8_t p[20] = { 0x10, 0x01, 0, 0 }; queue_note(0x10, 0x8001, p, 20); }
static void queue_edid(const uint8_t *edid, unsigned size, unsigned declared)
{
    uint8_t p[4 + 512] = { 0x10, 0x02, (uint8_t)declared, (uint8_t)(declared >> 8) };
    memcpy(p + 4, edid, size); queue_note(0x10, 0x8001, p, 4 + size);
}
static void reply(const uint8_t *req, const uint8_t *payload, unsigned n)
{
    uint8_t m[0x7f0] = { 0 };
    unsigned len = 12 + n; if (len < 32) len = 32;
    m[0] = 0x42; m[1] = req[1]; put16(m + 2, le16(req + 2) | 0x4000); put16(m + 4, 3); put16(m + 6, le16(req + 6));
    put16(m + 8, len); memcpy(m + 12, payload, n); checksum(m, len);
    uint8_t *d = M.in[(M.in_head + M.in_count) % MAXQ];
    memcpy(d, m, len); M.in_len[(M.in_head + M.in_count++) % MAXQ] = len;
}
static void emc_consume(void)
{
    uint8_t req[0x7f0]; unsigned len = le16(M.mbox + 8);
    memcpy(req, M.mbox, len);
    put16(M.mbox + 0x7f0, 0); put16(M.mbox + 0x7f4, 1);      /* idle again: SOW=0, SOR=1 */
    const uint8_t *body = req + 12;
    uint8_t out[64] = { 0 };
    if (req[1] == 2 && le16(req + 2) == 0x1f) { M.stop_hdcp++; reply(req, out, 2); }
    else if (req[1] == 2 && le16(req + 2) == 0x16) {
        M.config_reads++;
        uint8_t cfg[8] = { 0x10, 0, 0, 0 /* FLAVA3 */, 0, 0, 0xff, 0xff };
        reply(req, cfg, 8);
    } else if (req[1] == 0x10 && le16(req + 2) == 0 && body[0] == 4 && body[4] == 1 && body[5] == 8) {
        unsigned count = body[8], reg = (unsigned)body[9] << 8 | body[10];
        M.reg_reads++;
        out[4] = 1; out[5] = (uint8_t)count; out[6] = (uint8_t)(reg >> 8); out[7] = (uint8_t)reg;   /* echo like the EMC */
        memcpy(out + 8, M.regs + reg, count); reply(req, out, 28 + count);                         /* padded to 28+count */
    } else if (req[1] == 0x10 && le16(req + 2) == 0) {
        unsigned used = le16(body + 1), i = M.submits++;
        if (i < 64) { memcpy(M.sub[i], body, used); M.sub_len[i] = used; }
        if (M.unplug_at_submit && (int)M.submits == M.unplug_at_submit) {
            uint8_t p[4] = { 0x10, 0x01, 0, 0 }, m[16] = { 0x24, 0x10, 0x00, 0x80, 0, 0, 0, 0, 16, 0 };
            memcpy(m + 12, p, 4); checksum(m, 16);
            uint8_t *d = M.in[(M.in_head + M.in_count) % MAXQ]; memcpy(d, m, 16);
            M.in_len[(M.in_head + M.in_count++) % MAXQ] = 16;
        }
        if ((int)i >= M.fail_submit_from && (int)i < M.fail_submit_to) out[0] = 1;   /* service rejection */
        reply(req, out, 2);
    } else reply(req, out, 2);
    present();
}
static uint32_t mread(void *c, unsigned region, unsigned off, unsigned width)
{
    (void)c;
    if (region) return off == 0x14 ? M.status : 0;
    uint32_t v = 0;
    for (unsigned i = 0; i < width; i++) v |= (uint32_t)M.mbox[off + i] << (8 * i);
    return v;
}
static void mwrite(void *c, unsigned region, unsigned off, unsigned width, uint32_t v)
{
    (void)c;
    if (region) {
        if (off == 0x14) M.status &= ~v;
        else if (off == 4 && v == 1) emc_consume();
        else if (off == 4 && v == 2) {                          /* host consumed the inbound message */
            M.acks++;
            if (M.presenting) { M.presenting = 0; M.in_head = (M.in_head + 1) % MAXQ; M.in_count--; }
            present();
        }
        return;
    }
    for (unsigned i = 0; i < width; i++) M.mbox[off + i] = (uint8_t)(v >> (8 * i));
}
static uint64_t mticks(void *c) { (void)c; return ++M.ticks; }
static void mrelax(void *c) { (void)c; }
static const struct ps5_icc_io IO = { 0, mread, mwrite, mticks, mrelax, 1000000 };

static void mock_reset(void)
{
    memset(&M, 0, sizeof(M));
    put16(M.mbox + 0x7f4, 1);       /* idle */
    M.fail_submit_from = M.fail_submit_to = -1;
}

/* ------------------------------------------------------------------ fixtures */
static uint8_t EDID_A[256], EDID_B[256];
static void make_edid(uint8_t *e, uint16_t product)
{
    static const uint8_t hdr[8] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };
    memset(e, 0, 256); memcpy(e, hdr, 8);
    e[8] = 0x55; e[9] = 0x52;  /* "UGR" */
    e[10] = (uint8_t)product; e[11] = (uint8_t)(product >> 8); e[126] = 1;
    for (unsigned i = 128; i < 256; i++) e[i] = (uint8_t)(i * 7);
    ps5_hp_edid_fix(e);
}
static struct ps5_hp HP;
static struct ps5_hdmi_commands PKT;
static void hp_setup(unsigned mode, int ugreen)
{
    memset(&HP, 0, sizeof(HP));
    HP.mode = mode;
    HP.cfg.converter = 2; HP.cfg.dp_polarity = 0xff; HP.cfg.dp_swap = 0xff;
    if (ugreen) { ps5_flava3_mode_for(PS5_MODE_UGREEN1440P60, &HP.bridge); HP.bridge_null = 0; HP.source = PS5_HP_SRC_BRIDGE; }
    else ps5_hp_mode_default(&HP);
    memcpy(HP.boot_edid, EDID_A, 256); HP.boot_edid_bytes = 256;
    ps5_hp_arm_reset(&HP, 1000);
}
/* raw EDID as the EMC sends it: byte 0 = 0x01 (PS5 quirk), checksum of the raw block */
static void raw_edid(const uint8_t *fixed, uint8_t *raw)
{
    memcpy(raw, fixed, 256); raw[0] = 0x01; raw[127] = (uint8_t)(fixed[127] - 1);
}
/* driver-like tick: poll + process (ICC "lock" is implicit) */
static void tick(uint64_t now) { ps5_hp_poll(&HP, &IO, now); ps5_hp_process(&HP, now); }
static int run_due(uint64_t now)
{
    if (!ps5_hp_due(&HP, now)) return 0;
    int r = ps5_hp_replay(&HP, &IO, &PKT, 0, 0);
    ps5_hp_replay_done(&HP, r, now);
    ps5_hp_process(&HP, now);
    return 1;
}

/* ------------------------------------------------------------------ tests */
static void test_wire_filtering(void)
{
    mock_reset(); hp_setup(PS5_HP_REPLAY_BOOT, 0);
    uint32_t dropped0 = ps5_icc_dropped.count;
    uint8_t junk[16] = { 0x24, 0x10, 0x00, 0x80 };
    queue_raw(junk, 16, 8);          /* wire length 8  < 12: dropped without reading the body */
    queue_raw(junk, 16, 0x900);      /* wire length 0x900 > 0x7f0: dropped */
    uint8_t short_payload[2] = { 0x10, 0x02 };
    queue_note(0x10, 0x8000, short_payload, 2);        /* HDMI payload < 4 bytes: Linux ignores it */
    uint8_t raw[256]; raw_edid(EDID_A, raw);
    queue_edid(raw, 256, 300);                         /* EDID size larger than the payload: bad size */
    queue_edid(raw, 256, 100);                         /* EDID size < 128: bad size */
    queue_unplug();
    uint8_t other[4] = { 0x00, 0x01, 0, 0 };
    queue_note(0x08, 0x8000, other, 4);                /* power button service: not HDMI */
    for (unsigned t = 0; t < 10; t++) tick(2000 + 250 * t);
    CHECK(ps5_icc_dropped.count - dropped0 == 2);
    CHECK(HP.c.short_note == 1);
    CHECK(HP.c.edid_bad == 2);
    CHECK(HP.c.edid == 0);
    CHECK(HP.c.unplug == 1);
    CHECK(HP.c.other_service == 1);
    CHECK(!HP.pending && HP.c.replays == 0);
    CHECK(M.in_count == 0 && M.acks == 7);             /* every message acknowledged exactly once */
}

static void test_endless_stream_terminates(void)
{
    mock_reset(); hp_setup(PS5_HP_REPLAY_BOOT, 0);
    M.endless = 1; present();
    unsigned total = 0, max = 0, ticks = 0, paused_ticks = 0;
    for (uint64_t now = 2000; now < 2000 + 30000; now += 250, ticks++) {
        int n = ps5_hp_poll(&HP, &IO, now);
        ps5_hp_process(&HP, now);
        CHECK(n >= 0 && n <= (int)PS5_HP_POLL_MAX_MSGS);
        if ((unsigned)n > max) max = (unsigned)n;
        if (n == 0) paused_ticks++;
        total += (unsigned)n;
    }
    CHECK(max == PS5_HP_POLL_MAX_MSGS);
    CHECK(HP.c.storms >= 2);                           /* pause, resume, pause again */
    CHECK(paused_ticks >= 40);                         /* 10 s pauses = 40 ticks each */
    CHECK(total <= (PS5_HP_STORM_MSGS + PS5_HP_POLL_MAX_MSGS) * 3);
    CHECK(HP.c.other_event == total);                  /* status event 07 counted, never acted on */
    CHECK(HP.c.replays == 0);
    /* a query in the middle of the stream still completes: <= 256 inbound per query (PS5_ICC_MAX_INBOUND_PER_QUERY) */
    struct ps5_hdmi_config cfg;
    CHECK(ps5_hdmi_read_config_events(&IO, &cfg, ps5_hp_note, &HP) == 0 && cfg.converter == 2);
}

/* the packets the firmware sends for this mode, in the firmware's order */
static unsigned expected_packets(unsigned mode, const struct ps5_flava3_mode *bridge, int geometry,
                                 struct ps5_hdmi_commands *exp, unsigned *stages)
{
    static const unsigned boot[] = { 6, 7, 8, 9, 10, 0, 1, 2, 3, 4, 5 }, shrt[] = { 0, 1, 2, 3, 4, 5 };
    struct ps5_hdmi_config cfg = { { 0 }, 2, 0, 0, 0xff, 0xff };
    unsigned n = 0, list[16], k = 0;
    const unsigned *b = mode == PS5_HP_REPLAY_SHORT ? shrt : boot;
    unsigned bn = mode == PS5_HP_REPLAY_SHORT ? 6 : 11;
    for (unsigned i = 0; i < bn; i++) list[k++] = b[i];
    if (geometry) list[k++] = 14;
    list[k++] = 11; list[k++] = 12; list[k++] = 13;
    for (unsigned i = 0; i < k; i++) {
        int r = (list[i] >= 8 && list[i] <= 10) ? ps5_flava3_prepare_stage(&cfg, list[i] - 8, &exp[n])
              : ps5_flava3_video_stage_mode(&cfg, list[i], bridge, &exp[n]);
        CHECK(r == 0);
        stages[n++] = list[i];
    }
    return n;
}

static void test_replay_once_per_event(void)
{
    static struct ps5_hdmi_commands exp[16];
    unsigned stages[16];
    mock_reset(); hp_setup(PS5_HP_REPLAY_BOOT, 1);
    uint8_t raw[256]; raw_edid(EDID_A, raw);
    queue_unplug(); queue_edid(raw, 256, 256);
    tick(10000);
    CHECK(HP.c.unplug == 1 && HP.c.edid == 1 && HP.c.edid_same == 1);
    CHECK(HP.pending && HP.pub_new && HP.pub_same && memcmp(HP.pub_edid, EDID_A, 256) == 0);
    CHECK(!run_due(10499));                            /* 500 ms debounce */
    CHECK(run_due(10500));
    CHECK(HP.last_result == 0 && HP.c.replay_ok == 1 && !HP.pending);
    unsigned n = expected_packets(PS5_HP_REPLAY_BOOT, &HP.bridge, 0, exp, stages);
    CHECK(n == 14 && M.submits == n && M.stop_hdcp == 1);
    for (unsigned i = 0; i < n && i < M.submits; i++)
        CHECK(M.sub_len[i] == exp[i].used && memcmp(M.sub[i], exp[i].data, exp[i].used) == 0);
    /* the 1440 profile really is in stage 2 (0x7227 = 04, 0x7070[5] = fb) */
    CHECK(HP.bridge.reg7227[0] == 4 && HP.bridge.reg7070[5] == 0xfb && !HP.bridge.high_tmds);
    for (uint64_t t = 10500; t < 30000; t += 250) CHECK(!run_due(t));   /* exactly once */
    /* the replay itself may cause an EDID notification: ignored inside the guard window */
    queue_edid(raw, 256, 256); tick(11000);
    CHECK(!HP.pending && HP.c.guard_ignored == 1);
    /* a later EDID (e.g. monitor input switched back) re-applies like Linux: once */
    queue_edid(raw, 256, 256); tick(20000);
    CHECK(HP.pending && run_due(20500) && HP.c.replay_ok == 2 && M.submits == 28);
}

static void test_short_list_and_geometry(void)
{
    static struct ps5_hdmi_commands exp[16];
    unsigned stages[16], list[16];
    mock_reset(); hp_setup(PS5_HP_REPLAY_SHORT, 0);
    ps5_flava3_mode_for(PS5_MODE_3440P100, &HP.bridge); HP.bridge_null = 0;
    unsigned n = ps5_hp_stage_list(&HP, list);
    unsigned want[] = { 0, 1, 2, 3, 4, 5, 14, 11, 12, 13 };
    CHECK(n == 10 && memcmp(list, want, sizeof(want)) == 0);
    CHECK(ps5_hp_replay(&HP, &IO, &PKT, 0, 0) == 0 && M.stop_hdcp == 0);
    unsigned e = expected_packets(PS5_HP_REPLAY_SHORT, &HP.bridge, 1, exp, stages);
    CHECK(e == 10 && M.submits == 10);
    for (unsigned i = 0; i < e && i < M.submits; i++)
        CHECK(M.sub_len[i] == exp[i].used && memcmp(M.sub[i], exp[i].data, exp[i].used) == 0);
    /* VIC16 boot list = today's firmware 1080p packets (mode NULL) */
    hp_setup(PS5_HP_REPLAY_BOOT, 0);
    unsigned b[] = { 6, 7, 8, 9, 10, 0, 1, 2, 3, 4, 5, 11, 12, 13 };
    n = ps5_hp_stage_list(&HP, list);
    CHECK(n == 14 && memcmp(list, b, sizeof(b)) == 0 && HP.bridge_null);
}

static void test_retry_with_backoff(void)
{
    mock_reset(); hp_setup(PS5_HP_REPLAY_BOOT, 0);
    M.fail_submit_from = 0; M.fail_submit_to = 1000;   /* every HDMI packet rejected by the service */
    uint8_t raw[256]; raw_edid(EDID_A, raw);
    queue_unplug(); queue_edid(raw, 256, 256); tick(10000);
    CHECK(run_due(10500) && HP.last_result == -6 && HP.last_stage == 6 && HP.pending);
    CHECK(!run_due(11499) && run_due(11500));          /* retry 2 after 1 s */
    CHECK(!run_due(13499) && run_due(13500));          /* retry 3 after 2 s */
    CHECK(!HP.pending && HP.c.replays == 3 && HP.c.retries == 2 && HP.c.replay_fail == 1);
    for (uint64_t t = 13500; t < 40000; t += 250) CHECK(!run_due(t));   /* bounded: no 4th attempt */
    CHECK(M.submits == 3);                             /* each attempt stopped at its first failing stage */
    /* the next event starts again with a fresh budget */
    M.fail_submit_from = M.fail_submit_to = -1;
    queue_unplug(); queue_edid(raw, 256, 256); tick(41000);
    CHECK(run_due(41500) && HP.last_result == 0 && HP.c.replay_ok == 1);
}

static void test_unplug_cancels_and_aborts(void)
{
    mock_reset(); hp_setup(PS5_HP_REPLAY_BOOT, 0);
    uint8_t raw[256]; raw_edid(EDID_A, raw);
    queue_unplug(); queue_edid(raw, 256, 256); tick(10000);
    CHECK(HP.pending);
    queue_unplug(); tick(10250);                       /* sink gone again before the debounce ended */
    CHECK(!HP.pending && !run_due(10500) && M.submits == 0);
    queue_edid(raw, 256, 256); tick(12000);
    M.unplug_at_submit = 3;                            /* unplug arrives during the third packet */
    CHECK(run_due(12500) && HP.last_result == PS5_HP_ABORT_UNPLUG && HP.last_stage == 8);
    CHECK(!HP.pending && HP.c.replay_abort == 1 && M.submits == 3 && HP.unplugged);
    queue_edid(raw, 256, 256); tick(12600);            /* replug right after: not blocked by any guard */
    CHECK(HP.pending && run_due(13100) && HP.last_result == 0);
}

static void test_baseline_and_other_sink(void)
{
    mock_reset(); hp_setup(PS5_HP_REPLAY_BOOT, 0);
    uint8_t raw[256]; raw_edid(EDID_A, raw);
    queue_edid(raw, 256, 256); tick(1200);             /* initial report right after events-on */
    CHECK(!HP.pending && HP.c.baseline == 1);
    raw_edid(EDID_B, raw);
    queue_edid(raw, 256, 256); tick(1500);             /* a different sink: logged, current mode replayed */
    CHECK(HP.pending && HP.c.edid_diff == 1 && !HP.pub_same && memcmp(HP.pub_edid, EDID_B, 256) == 0);
    CHECK(run_due(2000) && HP.last_result == 0);
    /* identical EDID inside the arm window but after an unplug: a real replug */
    mock_reset(); hp_setup(PS5_HP_REPLAY_BOOT, 0);
    raw_edid(EDID_A, raw);
    queue_unplug(); queue_edid(raw, 256, 256); tick(1200);
    CHECK(HP.pending && HP.c.baseline == 0);
}

/* the EMC answered the firmware's events-on with 01 then 02 (edid5 log). If it does the same when ps5pwr switches
 * the events on, that is treated like a replug: exactly one replay right after arming (documented in BUILD.md). */
static void test_arm_sequence_like_edid5(void)
{
    mock_reset(); hp_setup(PS5_HP_REPLAY_BOOT, 1);
    CHECK(ps5_hp_events(&HP, &IO, &PKT, 1) == 0);
    uint8_t raw[256]; raw_edid(EDID_A, raw);
    queue_unplug(); tick(1250);
    queue_edid(raw, 256, 256); tick(1500);
    CHECK(HP.c.unplug == 1 && HP.c.edid_same == 1 && HP.c.baseline == 0 && HP.pending);
    CHECK(run_due(2000) && HP.c.replay_ok == 1);
    for (uint64_t t = 2000; t < 20000; t += 250) { tick(t); CHECK(!run_due(t)); }
    CHECK(HP.c.replays == 1 && M.submits == 1 + 14);
}

static void test_log_only_and_unknown_mode(void)
{
    mock_reset(); hp_setup(PS5_HP_LOG, 0);
    uint8_t raw[256]; raw_edid(EDID_A, raw);
    queue_unplug(); queue_edid(raw, 256, 256); tick(10000);
    CHECK(!HP.pending && HP.c.log_only == 1 && HP.c.edid == 1);
    for (uint64_t t = 10000; t < 20000; t += 250) CHECK(!run_due(t));
    mock_reset(); hp_setup(PS5_HP_REPLAY_BOOT, 0); HP.source = PS5_HP_SRC_NONE;
    queue_unplug(); queue_edid(raw, 256, 256); tick(10000);
    CHECK(!HP.pending && M.submits == 0);
    CHECK(ps5_hp_replay(&HP, &IO, &PKT, 0, 0) == PS5_HP_NO_MODE && M.submits == 0);
}

static void test_rate_hold(void)
{
    mock_reset(); hp_setup(PS5_HP_REPLAY_BOOT, 0);
    uint8_t raw[256]; raw_edid(EDID_A, raw);
    uint64_t t = 10000;
    for (unsigned i = 0; i < 3; i++, t += 5000) { queue_edid(raw, 256, 256); tick(t); CHECK(run_due(t + 500)); }
    CHECK(HP.held && HP.c.replay_ok == 3);
    queue_edid(raw, 256, 256); tick(t);                /* 4th EDID without unplug within 60 s: hold */
    CHECK(!HP.pending && HP.c.held == 1);
    queue_unplug(); queue_edid(raw, 256, 256); tick(t + 1000);
    CHECK(!HP.held && HP.pending && run_due(t + 1500));
}

static unsigned gate_calls, gate_stop_at = 99;
static int gate(void *ctx, unsigned index, unsigned stage, int after)
{
    (void)ctx; (void)stage;
    gate_calls++;
    return !after && index == gate_stop_at;
}
static void test_gate_stop(void)
{
    mock_reset(); hp_setup(PS5_HP_REPLAY_BOOT, 0);
    gate_calls = 0; gate_stop_at = 4;
    CHECK(ps5_hp_replay(&HP, &IO, &PKT, gate, 0) == PS5_HP_ABORT_STOP);
    CHECK(M.submits == 4 && gate_calls == 9);          /* 4 pairs + the refused 5th "before" */
    gate_calls = 0; gate_stop_at = 99;
    CHECK(ps5_hp_replay(&HP, &IO, &PKT, gate, 0) == 0 && gate_calls == 28);
}

static void test_events_packet(void)
{
    mock_reset(); hp_setup(PS5_HP_REPLAY_BOOT, 0);
    CHECK(ps5_hp_events(&HP, &IO, &PKT, 1) == 0 && M.submits == 1);
    static const uint8_t on[9] = { 4, 9, 0, 1, 5, 5, 4, 1, 1 };
    CHECK(M.sub_len[0] == 9 && memcmp(M.sub[0], on, 9) == 0);
    CHECK(ps5_hp_events(&HP, &IO, &PKT, 0) == 0 && M.sub[1][8] == 0);
}

static void test_mode_sources(void)
{
    struct ps5_hdmi_mode_var v; struct ps5_flava3_mode f;
    hp_setup(PS5_HP_REPLAY_BOOT, 0);
    ps5_flava3_mode_for(PS5_MODE_3440P60, &f);
    ps5_hdmi_mode_pack(&v, PS5_MODE_3440P60, &f, 3440, 1440, 60, 319750);
    CHECK(ps5_hp_mode_from_var(&HP, (const uint8_t *)&v, sizeof(v)) == 0);
    CHECK(HP.source == PS5_HP_SRC_VAR && !HP.bridge_null && memcmp(&HP.bridge, &f, sizeof(f)) == 0);
    ps5_hdmi_mode_pack(&v, PS5_MODE_1080P60, 0, 1920, 1080, 60, 148500);
    CHECK(ps5_hp_mode_from_var(&HP, (const uint8_t *)&v, sizeof(v)) == 0 && HP.bridge_null);
    ((uint8_t *)&v)[20] ^= 1;
    CHECK(ps5_hp_mode_from_var(&HP, (const uint8_t *)&v, sizeof(v)) == -2);
    CHECK(ps5_hp_mode_from_var(&HP, (const uint8_t *)&v, 63) == -1);
    hp_setup(PS5_HP_REPLAY_BOOT, 0); HP.source = PS5_HP_SRC_NONE;          /* v1.10.1: manual-timing mode is not replayed */
    ps5_flava3_mode_for(PS5_MODE_720P144, &f);
    ps5_hdmi_mode_pack(&v, PS5_MODE_720P144, &f, 1280, 720, 144, 178200);
    CHECK(ps5_hp_mode_from_var(&HP, (const uint8_t *)&v, sizeof(v)) == 1 && HP.source == PS5_HP_SRC_NONE);

    static const struct { uint8_t r7227, v0, v1; int want; int id; } cases[] = {
        { 16, 0, 0, 0, PS5_MODE_1080P60 }, { 63, 0, 0, 0, PS5_MODE_1080P120 }, { 4, 0, 0, 0, PS5_MODE_1440P60 },
        { 97, 0, 0, 0, PS5_MODE_2160P60 }, { 5, 0xc9, 0x05, 0, PS5_MODE_3440P60 }, { 5, 0xe6, 0x05, 0, PS5_MODE_3440P100 },
        { 5, 0x00, 0x00, 1, -1 }, { 0x33, 0, 0, 1, -1 },
        { 5, 0x00, 0x00, 1, PS5_MODE_720P144 },   /* v1.10.1: 0x7063 identifies 720p144 (manual timing) -> no replay */
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        mock_reset(); hp_setup(PS5_HP_REPLAY_BOOT, 0); HP.source = PS5_HP_SRC_NONE;
        M.regs[0x7227] = cases[i].r7227; M.regs[0x7056] = cases[i].v0; M.regs[0x7057] = cases[i].v1;
        if (cases[i].id == PS5_MODE_720P144) { struct ps5_flava3_mode_x mx; ps5_flava3_modex_for(PS5_MODE_720P144, &mx);
            memcpy(&M.regs[0x7063], mx.m7063, sizeof(mx.m7063)); }
        int r = ps5_hp_mode_from_bridge(&HP, &IO);
        CHECK(r == cases[i].want);
        if (cases[i].want == 0) {
            ps5_flava3_mode_for((enum ps5_mode_id)cases[i].id, &f);   /* bridge = the base recipe (manual bytes come from modex) */
            CHECK(HP.source == PS5_HP_SRC_BRIDGE && HP.mode_id == (uint32_t)cases[i].id);
            CHECK(cases[i].id == PS5_MODE_1080P60 ? HP.bridge_null : memcmp(&HP.bridge, &f, sizeof(f)) == 0);
        } else CHECK(HP.source == PS5_HP_SRC_NONE);
        CHECK(M.reg_reads == (cases[i].r7227 == 5 ? 4u : 2u));   /* v1.10.1: 7227, 7070, 7063 (manual), 7056 (ultrawide) */
    }
}

/* ------------------------------------------------------------------ v1.13 pick + program */
#ifndef PS5_TESTDATA
#define PS5_TESTDATA "../../firmware/testdata"
#endif
static unsigned load(const char *name, uint8_t *buf, unsigned cap)
{
    char path[512]; snprintf(path, sizeof path, "%s/%s", PS5_TESTDATA, name);
    FILE *f = fopen(path, "rb"); if (!f) { printf("missing %s\n", path); return 0; }
    unsigned n = (unsigned)fread(buf, 1, cap, f); fclose(f); return n;
}
static void test_pick_real_edids(void)
{
    uint8_t e[512]; unsigned n; struct ps5_hp_pick p;
    memset(&p, 0, sizeof p);
    CHECK((n = load("dell-s3422dwg-edid.bin", e, sizeof e)) >= 128);
    ps5_hp_pick_from_edid(e, n, PS5_HP_PICK_MASK, &p);
    CHECK(p.edid_ok == 1 && p.mode_id == PS5_MODE_3440P100 && p.refresh_hz == 100 && p.t.h == 3440 && p.t.v == 1440);
    CHECK((p.offered_mask & (1u << PS5_MODE_3440P60)) && (p.offered_mask & (1u << PS5_MODE_3440P100)) &&
          !(p.offered_mask & (1u << PS5_MODE_720P144)) && p.edid_bytes == n && memcmp(p.edid, e, n) == 0);
    CHECK((n = load("ugreen-uhd-edid.bin", e, sizeof e)) >= 128);
    ps5_hp_pick_from_edid(e, n, PS5_HP_PICK_MASK, &p);
    CHECK(p.edid_ok == 1 && p.mode_id == PS5_MODE_720P144 && p.refresh_hz == 144 && p.t.h == 1280);   /* = firmware refresh=max */
    CHECK((p.offered_mask & (1u << PS5_MODE_UGREEN1440P60)) && (p.offered_mask & (1u << PS5_MODE_720P144)) &&
          !(p.offered_mask & (1u << PS5_MODE_3440P100)));
    ps5_hp_pick_from_edid(e, n, 1u << PS5_MODE_1080P60, &p); CHECK(p.mode_id == PS5_MODE_1080P60);   /* mask honoured */
    memset(e, 0x5a, 256);                                              /* garbage: 1080p60 fallback */
    ps5_hp_pick_from_edid(e, 256, PS5_HP_PICK_MASK, &p);
    CHECK(p.edid_ok == 0 && p.mode_id == PS5_MODE_1080P60 && p.offered_mask == 0 && p.t.h == 1920);
    p.seq = 7; ps5_hp_pick_from_edid(e, 256, PS5_HP_PICK_MASK, &p); CHECK(p.seq == 7);   /* seq is the caller's */
}
static void test_pick_on_other_sink(void)
{
    uint8_t dell[256], raw[256];
    CHECK(load("dell-s3422dwg-edid.bin", dell, 256) == 256);
    /* REPLAY_BOOT: other sink -> no pick (unchanged behaviour) */
    mock_reset(); hp_setup(PS5_HP_REPLAY_BOOT, 1);
    raw_edid(EDID_A, raw); queue_edid(raw, 256, 256); tick(1200);
    raw_edid(dell, raw); queue_edid(raw, 256, 256); tick(1500);
    CHECK(HP.c.edid_diff == 1 && !HP.pick_new && HP.pick.seq == 0);
    /* PICK: other sink -> pick 3440p100, seq 1, and the current mode is still replayed */
    mock_reset(); hp_setup(PS5_HP_PICK, 1);
    raw_edid(EDID_A, raw); queue_edid(raw, 256, 256); tick(1200);
    CHECK(!HP.pick_new);                                                /* the boot sink itself: nothing to pick */
    raw_edid(dell, raw); queue_edid(raw, 256, 256); tick(1500);
    CHECK(HP.pick_new && HP.pick.seq == 1 && HP.pick.mode_id == PS5_MODE_3440P100 && HP.pick.edid_ok == 1);
    CHECK(HP.pending && run_due(2000) && HP.last_result == 0);          /* boot-list replay unchanged in mode 4 */
    unsigned st[PS5_HP_MAX_STAGES]; CHECK(ps5_hp_stage_list(&HP, st) == 14);   /* boot list (UGREEN: no geometry stage) */
    /* v1.13.1: back to the boot sink after a pick -> pick 2 with the boot sink's EDID */
    HP.pick_new = 0;
    raw_edid(EDID_A, raw); queue_edid(raw, 256, 256); tick(4000);
    CHECK(HP.c.edid_same == 2 && HP.pick_new && HP.pick.seq == 2 && HP.pick.edid_bytes >= 128 &&
          memcmp(HP.pick.edid + 8, EDID_A + 8, 10) == 0);
}
static void test_program_mode(void)
{
    unsigned st[PS5_HP_MAX_STAGES], n = 99, ref_n; int res[PS5_HP_MAX_STAGES];
    static uint8_t ref[64][0x7e4]; static unsigned ref_len[64];
    /* UGREEN 1440p60 through program_mode == the replay with the UGREEN bridge, packet by packet */
    mock_reset(); hp_setup(PS5_HP_REPLAY_BOOT, 1);
    CHECK(ps5_hp_replay(&HP, &IO, &PKT, 0, 0) == 0);
    ref_n = M.submits; memcpy(ref, M.sub, sizeof ref); memcpy(ref_len, M.sub_len, sizeof ref_len);
    mock_reset(); hp_setup(PS5_HP_REPLAY_BOOT, 0);                       /* current mode 1080p60: the id decides */
    CHECK(ps5_hp_program_mode(&HP, &IO, &PKT, PS5_MODE_UGREEN1440P60, 0, 0, st, res, &n) == 0 && n == 14);
    CHECK(M.submits == ref_n && M.stop_hdcp == 1);
    int same = 1; for (unsigned i = 0; i < ref_n; i++) same &= M.sub_len[i] == ref_len[i] && !memcmp(M.sub[i], ref[i], ref_len[i]);
    CHECK(same);
    for (unsigned i = 0; i < n; i++) CHECK(res[i] == 0);
    /* every mode of the mask builds and submits; 3440 adds the geometry stage 14 */
    for (unsigned id = 0; id < PS5_MODE_COUNT; id++) {
        if (!(PS5_HP_PICK_MASK & (1u << id))) continue;
        mock_reset(); hp_setup(PS5_HP_REPLAY_BOOT, 0);
        int r = ps5_hp_program_mode(&HP, &IO, &PKT, id, 0, 0, st, res, &n);
        struct ps5_flava3_mode_x mx; CHECK(ps5_flava3_modex_for((enum ps5_mode_id)id, &mx) == 0);
        unsigned want = mx.m.uw_geometry ? 15 : 14;
        if (!(r == 0 && n == want && st[0] == 6 && st[n - 1] == 13)) printf("  mode %u: r %d n %u want %u\n", id, r, n, want);
        CHECK(r == 0 && n == want && st[0] == 6 && st[n - 1] == 13);
        if (want == 15) CHECK(st[11] == 14);
        if (id == PS5_MODE_3440P60 || id == PS5_MODE_3440P100) CHECK(want == 15);   /* ultrawide geometry */
    }
    /* 720p144: stage 2 carries the manual timing (packet differs from the base recipe) */
    {
        struct ps5_flava3_mode_x mx; struct ps5_hdmi_commands a, b;
        CHECK(ps5_flava3_modex_for(PS5_MODE_720P144, &mx) == 0 && mx.manual == 1);
        CHECK(ps5_flava3_video_stage_modex(&HP.cfg, 2, &mx, &a) == 0);
        mx.manual = 0; CHECK(ps5_flava3_video_stage_modex(&HP.cfg, 2, &mx, &b) == 0);
        CHECK(a.used != b.used || memcmp(a.data, b.data, a.used) != 0);
    }
    /* refused ids: not in the mask, out of range -> nothing sent */
    mock_reset(); hp_setup(PS5_HP_REPLAY_BOOT, 0);
    CHECK(ps5_hp_program_mode(&HP, &IO, &PKT, PS5_MODE_1080P120, 0, 0, st, res, &n) == PS5_HP_NO_MODE && n == 0 && M.submits == 0);
    CHECK(ps5_hp_program_mode(&HP, &IO, &PKT, PS5_MODE_COUNT, 0, 0, st, res, &n) == PS5_HP_NO_MODE && M.submits == 0);
    /* service rejection, unplug, gate stop */
    mock_reset(); hp_setup(PS5_HP_REPLAY_BOOT, 0); M.fail_submit_from = 2; M.fail_submit_to = 3;
    CHECK(ps5_hp_program_mode(&HP, &IO, &PKT, PS5_MODE_3440P60, 0, 0, st, res, &n) != 0 && n == 3 && res[0] == 0 && res[2] != 0);
    mock_reset(); hp_setup(PS5_HP_REPLAY_BOOT, 0); M.unplug_at_submit = 3;
    CHECK(ps5_hp_program_mode(&HP, &IO, &PKT, PS5_MODE_3440P60, 0, 0, st, res, &n) == PS5_HP_ABORT_UNPLUG && n == 3);
    mock_reset(); hp_setup(PS5_HP_REPLAY_BOOT, 0); gate_calls = 0; gate_stop_at = 4;
    CHECK(ps5_hp_program_mode(&HP, &IO, &PKT, PS5_MODE_3440P60, gate, 0, st, res, &n) == PS5_HP_ABORT_STOP && n == 4 && M.submits == 4);
    gate_stop_at = 99;
    /* v1.13.2: a failed program leaves the current mode alone */
    CHECK(HP.mode_id != PS5_MODE_3440P60 || HP.source != PS5_HP_SRC_PROGRAM);
    /* v1.13.2: success -> current mode for later replays, scheduled replay cancelled; the replay then sends the program's packets */
    {
        static uint8_t prog[64][0x7e4]; static unsigned prog_len[64]; unsigned pn;
        mock_reset(); hp_setup(PS5_HP_REPLAY_BOOT, 0); HP.pending = 1;
        CHECK(ps5_hp_program_mode(&HP, &IO, &PKT, PS5_MODE_3440P100, 0, 0, st, res, &n) == 0 && n == 15);
        CHECK(HP.pending == 0 && HP.mode_id == PS5_MODE_3440P100 && HP.source == PS5_HP_SRC_PROGRAM && HP.bridge_null == 0 &&
              HP.bridge.uw_geometry != 0);
        pn = M.submits; memcpy(prog, M.sub, sizeof prog); memcpy(prog_len, M.sub_len, sizeof prog_len);
        M.submits = 0;
        CHECK(ps5_hp_replay(&HP, &IO, &PKT, 0, 0) == 0 && M.submits == pn);
        for (unsigned i = 0; i < pn && i < 64; i++) CHECK(M.sub_len[i] == prog_len[i] && memcmp(M.sub[i], prog[i], prog_len[i]) == 0);
        /* 1080p60 -> VIC16 packets (bridge_null), 720p144 (manual timing) -> no later replay */
        CHECK(ps5_hp_program_mode(&HP, &IO, &PKT, PS5_MODE_1080P60, 0, 0, st, res, &n) == 0 && HP.bridge_null == 1 &&
              HP.source == PS5_HP_SRC_PROGRAM && HP.mode_id == PS5_MODE_1080P60);
        CHECK(ps5_hp_program_mode(&HP, &IO, &PKT, PS5_MODE_720P144, 0, 0, st, res, &n) == 0 && HP.source == PS5_HP_SRC_NONE &&
              HP.mode_id == PS5_MODE_720P144 && ps5_hp_replay(&HP, &IO, &PKT, 0, 0) == PS5_HP_NO_MODE);
    }
}

int main(int argc, char **argv)
{
    verbose = argc > 1 && !strcmp(argv[1], "-v");
    make_edid(EDID_A, 0x1234); make_edid(EDID_B, 0x4321);
    struct { const char *name; void (*fn)(void); } t[] = {
        { "wire filtering (0x24 accepted, wrong lengths dropped)", test_wire_filtering },
        { "endless notification stream terminates", test_endless_stream_terminates },
        { "replay once per event, byte-identical firmware packets", test_replay_once_per_event },
        { "short list and 3440 geometry stage", test_short_list_and_geometry },
        { "bounded retries with backoff", test_retry_with_backoff },
        { "unplug cancels / aborts", test_unplug_cancels_and_aborts },
        { "baseline report and other sink", test_baseline_and_other_sink },
        { "events-on answered with 01+02 (edid5) = one replay", test_arm_sequence_like_edid5 },
        { "log-only and unknown mode", test_log_only_and_unknown_mode },
        { "rate hold", test_rate_hold },
        { "gate stop", test_gate_stop },
        { "HDMI events packet", test_events_packet },
        { "mode sources (variable, bridge readback)", test_mode_sources },
        { "v1.13 pick from the Dell and UGREEN EDIDs", test_pick_real_edids },
        { "v1.13 pick on another sink (HotplugMode 4)", test_pick_on_other_sink },
        { "v1.13 program bridge for a mode id", test_program_mode },
    };
    for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        int before = failures;
        t[i].fn();
        printf("%s %s\n", failures == before ? "ok  " : "FAIL", t[i].name);
    }
    printf("%d checks, %d failures, %u log lines\n", checks, failures, log_lines);
    return failures != 0;
}
