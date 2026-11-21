/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * UNTESTED ON HARDWARE - host unit test only (test_display_mode.c). See ps5-display-mode.h.
 * Pure C, no allocation, no hardware access: usable from HdmiProbeDxe, the platform BDS
 * library and a host test alike.
 */
#include "ps5-display-mode.h"
#include "flava3-manual.h"   /* stage F */

/* ---- the whitelist ----------------------------------------------------------------------
 * Every timing below is one PS5 Linux drove through FLAVA3 with a verified picture:
 *  1080p60/1080p120/2160p60/1440p60(241.5 MHz): isHdmiModeValid stock set
 *    (linux-vcn4/drivers/ps5/hdmi.c:1317-1327; ref linux.patch:12312-12329 isHdmiModeStock)
 *  3440x1440@60/@100: is_native_ultrawide_mode (hdmi.c:808-818), user-confirmed 2026-10-18
 *    (ultrawide kernel evidence 20261018, NATIVE-SUCCESS.md).
 * Geometry of 1440p60 from the kprobe trace evidence/20261018/3440-native-mode-trace.txt:13,
 * of 2160p60 / 1080p from CTA-861 VIC 97 / 16 / 63 (cross-checked by the MSA captures). */
const struct ps5_mode_entry ps5_display_modes[PS5_MODE_COUNT] = {
    [PS5_MODE_1080P60]  = {{148500, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, 1, 1}, 16, PS5_BRIDGE_VIC,        60, "1920x1080@60 (VIC16)"},
    [PS5_MODE_1080P120] = {{297000, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, 1, 1}, 63, PS5_BRIDGE_VIC,       120, "1920x1080@120 (VIC63)"},
    [PS5_MODE_1440P60]  = {{241500, 2560, 2608, 2640, 2720, 1440, 1443, 1448, 1481, 1, 0},  0, PS5_BRIDGE_1440_STOCK, 60, "2560x1440@60 (241.5 MHz)"},
    [PS5_MODE_2160P60]  = {{594000, 3840, 4016, 4104, 4400, 2160, 2168, 2178, 2250, 1, 1}, 97, PS5_BRIDGE_VIC97,      60, "3840x2160@60 (VIC97)"},
    [PS5_MODE_3440P60]  = {{319750, 3440, 3488, 3520, 3600, 1440, 1443, 1453, 1481, 1, 0},  0, PS5_BRIDGE_UW3440,     60, "3440x1440@60 (319.75 MHz)"},
    [PS5_MODE_3440P100] = {{543500, 3440, 3488, 3520, 3600, 1440, 1443, 1453, 1510, 1, 0},  0, PS5_BRIDGE_UW3440,    100, "3440x1440@100 (543.5 MHz)"},
    /* Stage B: the 2560x1440@60 DTD of the UGREEN-UHD (EDID 14.11.2026, runs/edid6-boot-20261114-091004/edid.bin,
     * CTA block DTD 6a 5e 00 a0 a0 a0 29 50 30 20 25 00 b0 13 32 00 00 00): H 2560/2608/2640/2720,
     * V 1440/1442/1447/1481, 241.7 MHz, sync byte 0x00 -> -H/-V. PS5 Linux drove exactly this timing on the UGREEN
     * (video=DP-1:2560x1440R@60 picks the EDID mode; linux-display-baseline-v171-full.json 03.11.:
     * OTG_H_TOTAL 0xa9f, OTG_H_BLANK_START_END 0x700a70, OTG_V_TOTAL 0x5c8, OTG_V_BLANK_START_END 0x2705c7, all
     * reproduced by ps5_dcn201_otg_sync below). isHdmiModeValid accepts 241700 and the bridge recipe is the
     * 1440 stock one (hdmi.c:1318-1319, 893-896: 0x7227=04, 0x7070[5]=fb). */
    [PS5_MODE_2560P120] = {{497750, 2560, 2608, 2640, 2720, 1440, 1443, 1448, 1525, 1, 0}, 0, PS5_BRIDGE_UW3440, 120, "2560x1440@120 (manual geometry 497.75 MHz)"},   /* stage F: Dell S3422DWG DTD 3 */
    /* r5.13: UGREEN-UHD base-block DTD 2 (EDID runs/edid6-boot-20261114-091004/edid.bin): 178.2 MHz, H 1280/1390/1430/1650,
     * V 720/725/730/750, sync byte 0x1e -> +H/+V; the only mode above 60 Hz this sink offers (user 16.11.: "100 Hz ohne Dell"). */
    [PS5_MODE_720P144]  = {{178200, 1280, 1390, 1430, 1650,  720,  725,  730,  750, 1, 1},  0, PS5_BRIDGE_MANUAL,   144, "1280x720@144 (manual 178.2 MHz)"},
    [PS5_MODE_UGREEN1440P60] = {{241700, 2560, 2608, 2640, 2720, 1440, 1442, 1447, 1481, 0, 0}, 0, PS5_BRIDGE_1440_STOCK, 60, "2560x1440@60 (UGREEN 241.7 MHz)"},
};

/* ---- EDID ---------------------------------------------------------------------------- */
static int same_timing(const struct ps5_timing *a, const struct ps5_timing *b) {
    return a->clock_khz == b->clock_khz && a->h == b->h && a->hs == b->hs && a->he == b->he &&
           a->ht == b->ht && a->v == b->v && a->vs == b->vs && a->ve == b->ve && a->vt == b->vt &&
           a->hpos == b->hpos && a->vpos == b->vpos;
}

/* 18-byte detailed timing descriptor (VESA E-EDID 1.4 section 3.10.2). 0 = not a usable
 * progressive digital-separate-sync timing. */
static int decode_dtd(const uint8_t *b, struct ps5_timing *t) {
    unsigned clk = (unsigned)b[0] | ((unsigned)b[1] << 8);
    if (!clk) return 0;
    unsigned h = b[2] | ((b[4] >> 4) << 8), hb = b[3] | ((b[4] & 15) << 8);
    unsigned v = b[5] | ((b[7] >> 4) << 8), vb = b[6] | ((b[7] & 15) << 8);
    unsigned hf = b[8] | ((b[11] >> 6) << 8), hw = b[9] | (((b[11] >> 4) & 3) << 8);
    unsigned vf = (b[10] >> 4) | (((b[11] >> 2) & 3) << 4), vw = (b[10] & 15) | ((b[11] & 3) << 4);
    if (b[17] & 0x80) return 0;                 /* interlaced */
    /* Stage B: any sync type is accepted like Linux (drm_edid.c:3555-3558 only logs "composite sync not
     * supported"); polarity from bits 1/2 as Linux does (0x00 = -H/-V, e.g. the UGREEN 1440p DTDs). */
    if (!h || !v || !hb || !vb || !hw || !vw) return 0;
    t->clock_khz = clk * 10u;
    t->h = (uint16_t)h; t->hs = (uint16_t)(h + hf); t->he = (uint16_t)(h + hf + hw); t->ht = (uint16_t)(h + hb);
    t->v = (uint16_t)v; t->vs = (uint16_t)(v + vf); t->ve = (uint16_t)(v + vf + vw); t->vt = (uint16_t)(v + vb);
    t->hpos = (b[17] >> 1) & 1; t->vpos = (b[17] >> 2) & 1;
    return 1;
}

static void add_dtd(struct ps5_edid_info *o, const uint8_t *b) {
    struct ps5_timing t;
    if (o->dtd_count < PS5_EDID_INFO_MAX_DTD && decode_dtd(b, &t)) o->dtd[o->dtd_count++] = t;
}

static void parse_cta(struct ps5_edid_info *o, const uint8_t *x) {
    unsigned d = x[2];                         /* 0: neither DTDs nor data blocks */
    if (d < 4 || d > 127) return;
    unsigned blocks_end = x[1] >= 3 ? d : 4;   /* data blocks exist from CTA revision 3 */
    for (unsigned i = 4; i < blocks_end;) {
        unsigned tag = x[i] >> 5, len = x[i] & 31;
        const uint8_t *p = x + i + 1;
        if (i + 1 + len > blocks_end) break;
        if (tag == 2) {                                   /* Video Data Block: SVDs */
            for (unsigned k = 0; k < len && o->vic_count < PS5_EDID_INFO_MAX_VIC; k++) {
                uint8_t s = p[k];
                o->vic[o->vic_count++] = (s >= 129 && s <= 192) ? (uint8_t)(s & 0x7f) : s;
            }
        } else if (tag == 3 && len >= 3) {                /* Vendor Specific Data Block */
            if (p[0] == 0x03 && p[1] == 0x0c && p[2] == 0x00) {
                o->hdmi_vsdb = 1;
                if (len >= 7 && p[6]) o->max_tmds_khz = (uint32_t)p[6] * 5000u;
            } else if (p[0] == 0xd8 && p[1] == 0x5d && p[2] == 0xc4 && len >= 6) {
                o->hf_vsdb = 1;
                o->hf_max_tmds_khz = (uint32_t)p[4] * 5000u;
                o->scdc = (p[5] >> 7) & 1;
            }
        }
        i += 1 + len;
    }
    for (unsigned off = d; off + 18 <= 127; off += 18) {
        if (!(x[off] | x[off + 1])) break;
        add_dtd(o, x + off);
    }
}

int ps5_edid_parse(const uint8_t *edid, unsigned bytes, struct ps5_edid_info *out) {
    static const uint8_t header[8] = {0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0};
    uint8_t b0[128];
    for (unsigned i = 0; i < sizeof(*out); i++) ((uint8_t *)out)[i] = 0;
    if (!edid || bytes < 128 || bytes % 128) return -1;
    for (unsigned i = 0; i < 128; i++) b0[i] = edid[i];
    /* PS5 quirk (hdmi.c:1341-1351 fix_edid; ref linux.patch:12712 "byte 0: HDCP 2.2 in bit 0,
     * trained FRL rate in bits 3:1"): byte 0 is not the header, so clear it and recompute. */
    if (b0[0] != 0) {
        unsigned sum = 0;
        b0[0] = 0;
        for (unsigned i = 0; i < 127; i++) sum += b0[i];
        b0[127] = (uint8_t)(-(int)sum);
    }
    for (unsigned i = 0; i < 8; i++) if (b0[i] != header[i]) return -1;
    unsigned sum = 0;
    for (unsigned i = 0; i < 128; i++) sum += b0[i];
    if (sum & 255) return -1;
    out->valid = 1;
    out->blocks = 1u + b0[126];
    if (out->blocks > bytes / 128) out->blocks = bytes / 128;
    for (unsigned off = 54; off <= 108; off += 18) {
        unsigned before = out->dtd_count;
        add_dtd(out, b0 + off);
        if (off == 54 && out->dtd_count > before) out->has_preferred = 1;
    }
    for (unsigned blk = 1; blk < out->blocks; blk++) {
        const uint8_t *x = edid + 128 * blk;
        unsigned s = 0;
        for (unsigned i = 0; i < 128; i++) s += x[i];
        if (s & 255) continue;                     /* skip a corrupt extension, keep the base */
        if (x[0] == 0x02) parse_cta(out, x);
    }
    return 0;
}

/* ---- selection ----------------------------------------------------------------------- */
int ps5_display_offered(const struct ps5_edid_info *e, enum ps5_mode_id id) {
    if (!e || !e->valid || (unsigned)id >= PS5_MODE_COUNT) return 0;
    const struct ps5_mode_entry *m = &ps5_display_modes[id];
    int offered = 0;
    for (unsigned i = 0; i < e->dtd_count && !offered; i++) offered = same_timing(&e->dtd[i], &m->t);
    for (unsigned i = 0; i < e->vic_count && !offered && m->vic; i++) offered = e->vic[i] == m->vic;
    if (!offered) return 0;
    /* ref linux.patch:12521-12526: 165 MHz without HDMI VSDB; above 340 MHz SCDC + scrambling. */
    uint32_t limit = e->hdmi_vsdb && e->max_tmds_khz ? e->max_tmds_khz : 165000u;
    if (e->hf_vsdb && e->hf_max_tmds_khz > limit) limit = e->hf_max_tmds_khz;
    if (m->t.clock_khz > limit) return 0;
    if (m->t.clock_khz > 340000u && !(e->hf_vsdb && e->scdc)) return 0;
    return 1;
}

static enum ps5_mode_id select_mode(const struct ps5_edid_info *e, unsigned max_hz, uint32_t replay_mask,
                                    int native_first) {
    int best = -1;
    replay_mask |= 1u << PS5_MODE_1080P60;
    if (!e || !e->valid) return PS5_MODE_1080P60;
    for (int pass = native_first ? 0 : 1; pass < 2 && best < 0; pass++) {
        for (int id = 0; id < PS5_MODE_COUNT; id++) {
            const struct ps5_mode_entry *m = &ps5_display_modes[id];
            if (!(replay_mask & (1u << id)) || (max_hz && m->refresh_hz > max_hz)) continue;
            if (!ps5_display_offered(e, (enum ps5_mode_id)id)) continue;
            if (pass == 0) {      /* the panel's native resolution first */
                if (!e->has_preferred || m->t.h != e->dtd[0].h || m->t.v != e->dtd[0].v) continue;
                if (best < 0 || m->refresh_hz > ps5_display_modes[best].refresh_hz) best = id;
            } else {              /* otherwise the largest area, then the higher refresh */
                if (best < 0) { best = id; continue; }
                const struct ps5_mode_entry *b = &ps5_display_modes[best];
                uint32_t am = (uint32_t)m->t.h * m->t.v, ab = (uint32_t)b->t.h * b->t.v;
                if (am > ab || (am == ab && m->refresh_hz > b->refresh_hz)) best = id;
            }
        }
    }
    return best < 0 ? PS5_MODE_1080P60 : (enum ps5_mode_id)best;
}

enum ps5_mode_id ps5_display_select(const struct ps5_edid_info *e, unsigned max_hz, uint32_t replay_mask) {
    return select_mode(e, max_hz, replay_mask, 1);
}

enum ps5_mode_id ps5_display_select_largest(const struct ps5_edid_info *e, unsigned max_hz, uint32_t replay_mask) {
    return select_mode(e, max_hz, replay_mask, 0);
}

enum ps5_mode_id ps5_display_select_fastest(const struct ps5_edid_info *e, unsigned max_hz, uint32_t replay_mask) {
    int best = -1;
    replay_mask |= 1u << PS5_MODE_1080P60;
    if (!e || !e->valid) return PS5_MODE_1080P60;
    for (int id = 0; id < PS5_MODE_COUNT; id++) {
        const struct ps5_mode_entry *m = &ps5_display_modes[id];
        if (!(replay_mask & (1u << id)) || (max_hz && m->refresh_hz > max_hz)) continue;
        if (!ps5_display_offered(e, (enum ps5_mode_id)id)) continue;
        if (best < 0) { best = id; continue; }
        const struct ps5_mode_entry *b = &ps5_display_modes[best];
        if (m->refresh_hz != b->refresh_hz) { if (m->refresh_hz > b->refresh_hz) best = id; continue; }
        int mn = e->has_preferred && m->t.h == e->dtd[0].h && m->t.v == e->dtd[0].v;
        int bn = e->has_preferred && b->t.h == e->dtd[0].h && b->t.v == e->dtd[0].v;
        if (mn != bn) { if (mn) best = id; continue; }
        if ((uint32_t)m->t.h * m->t.v > (uint32_t)b->t.h * b->t.v) best = id;
    }
    return best < 0 ? PS5_MODE_1080P60 : (enum ps5_mode_id)best;
}

/* r5.21 (refresh=max, policy 2): the highest refresh at the NATIVE resolution (ps5_display_select without a limit);
 * only if that is not above 60 Hz, the highest refresh offered at all (ps5_display_select_fastest). Dell S3422DWG:
 * 3440x1440@100 (native) instead of 2560x1440@120; UGREEN (native 1080p only at 60): 720p144 as before. */
enum ps5_mode_id ps5_display_select_refresh_max(const struct ps5_edid_info *e, uint32_t replay_mask) {
    enum ps5_mode_id id = ps5_display_select(e, 0, replay_mask);
    if (ps5_display_modes[id].refresh_hz > 60) return id;
    return ps5_display_select_fastest(e, 0, replay_mask);
}

/* ---- DCN values ---------------------------------------------------------------------- */
int ps5_dcn201_otg_sync(const struct ps5_timing *t, struct ps5_dcn201_register_write out[10]) {
    if (!t || !out || t->hs <= t->h || t->he <= t->hs || t->ht <= t->he ||
        t->vs <= t->v || t->ve <= t->vs || t->vt <= t->ve || t->ht > 0x8000 || t->vt > 0x8000) return -1;
    /* optc1_program_timing: blank_start = total - front porch, blank_end = blank_start - active,
     * sync A from 0 to sync width, polarity bit = 1 for negative. */
    uint32_t hbs = t->ht - (t->hs - t->h), hbe = hbs - t->h;
    uint32_t vbs = t->vt - (t->vs - t->v), vbe = vbs - t->v;
    const struct ps5_dcn201_register_write w[10] = {
        {0x13fa8, 0x7fff, (uint32_t)t->ht - 1},                      /* OTG_H_TOTAL */
        {0x13fb0, 0x7fff7fff, (uint32_t)(t->he - t->hs) << 16},      /* OTG_H_SYNC_A */
        {0x13fac, 0x7fff7fff, (hbe << 16) | hbs},                    /* OTG_H_BLANK_START_END */
        {0x13fb4, 0x1, t->hpos ? 0u : 1u},                           /* OTG_H_SYNC_A_CNTL.POL */
        {0x13fbc, 0x7fff, (uint32_t)t->vt - 1},                      /* OTG_V_TOTAL */
        {0x13fc4, 0x7fff, (uint32_t)t->vt - 1},                      /* OTG_V_TOTAL_MAX */
        {0x13fc0, 0x7fff, (uint32_t)t->vt - 1},                      /* OTG_V_TOTAL_MIN */
        {0x13fdc, 0x7fff7fff, (uint32_t)(t->ve - t->vs) << 16},      /* OTG_V_SYNC_A */
        {0x13fd8, 0x7fff7fff, (vbe << 16) | vbs},                    /* OTG_V_BLANK_START_END */
        {0x13fe0, 0x1, t->vpos ? 0u : 1u},                           /* OTG_V_SYNC_A_CNTL.POL */
    };
    for (unsigned i = 0; i < 10; i++) out[i] = w[i];
    return 0;
}

void ps5_dp_msa(const struct ps5_timing *t, uint32_t out[4]) {
    out[0] = ((uint32_t)t->ht << 16) | t->vt;
    out[1] = ((uint32_t)(t->ht - t->hs) << 16) | (uint32_t)(t->vt - t->vs);
    out[2] = ((uint32_t)((t->he - t->hs) | (t->hpos ? 0 : 0x8000)) << 16) |
             (uint32_t)((t->ve - t->vs) | (t->vpos ? 0 : 0x8000));
    out[3] = ((uint32_t)t->h << 16) | t->v;
}

uint32_t ps5_dp_vid_m(uint32_t clock_khz) {
    return (uint32_t)((32768ull * clock_khz + 405000ull) / 810000ull);
}

uint32_t ps5_vtg0_vcount_init(const struct ps5_timing *t) {
    return (uint32_t)(t->vt - (t->vs - t->v));                  /* vertical blank start */
}

uint32_t ps5_vtg0_fp2(const struct ps5_timing *t, unsigned vstartup) {
    /* optc1_program_timing: asic_blank_end = vtotal - vsync_start; line_start = blank_end -
     * vstartup + 1; fp2 = -line_start if negative. 1080p: 43 - 41 - 1 = 1 (= dcn201-timing.c:101). */
    unsigned blank_end = (unsigned)(t->vt - t->vs);
    return vstartup > blank_end + 1 ? vstartup - blank_end - 1 : 0;
}

/* ---- FLAVA3 values (verbatim from hdmi.c / ultrawide patch) --------------------------- */
int ps5_flava3_mode_for(enum ps5_mode_id id, struct ps5_flava3_mode *o) {
    if (!o || (unsigned)id >= PS5_MODE_COUNT) return -1;
    const struct ps5_mode_entry *m = &ps5_display_modes[id];
    for (unsigned i = 0; i < sizeof(*o); i++) ((uint8_t *)o)[i] = 0;
    o->high_tmds = m->t.clock_khz > 340000u;                    /* is_high_tmds_rate, hdmi.c:861-864 */
    switch (m->recipe) {
    case PS5_BRIDGE_VIC:                                        /* hdmi.c:884-887 */
        o->reg7227[0] = m->vic; o->reg7070[0] = m->vic; o->reg7070[1] = m->vic;
        break;
    case PS5_BRIDGE_1440_STOCK:                                 /* hdmi.c:893-896 */
        o->reg7227[0] = 0x04; o->reg7070[5] = 0xfb;
        break;
    case PS5_BRIDGE_VIC97:                                      /* hdmi.c:897-900, 938-941 */
        o->reg7227[0] = 0x06; o->vic97_mask = 1;
        break;
    case PS5_BRIDGE_UW3440: {                                   /* hdmi.c:117-125, 820-858 */
        /* stage F (16.11.2026): the stage-14 geometry is computed from the timing with PS5 Linux
         * setManualTimingFlava3 (flava3-manual.h); for 3440x1440@60/@100 this yields exactly the former constants
         * (host test), so any +H/-V timing that keeps hstart 110 and 1440 lines can use this proven path. */
        struct flava3_manual mt;
        if (flava3_manual_build(m->t.h, m->t.hs, m->t.he, m->t.ht, m->t.v, m->t.vs, m->t.ve, m->t.vt, m->t.clock_khz,
                                m->t.hpos, m->t.vpos, &mt) || mt.r7042[0] != 0x6e || mt.r7042[1] != 0x00 ||
            mt.r7042[6] != 0x9f || mt.r7042[7] != 0x05) return -1;   /* hstart/height must match the profile */
        o->reg7227[0] = 0x05; o->reg7070[5] = 0xfb;
        o->uw_geometry = 1;
        o->uw_vstart[0] = mt.r7042[2]; o->uw_vstart[1] = mt.r7042[3];
        o->uw_vtotal[0] = (uint8_t)m->t.vt; o->uw_vtotal[1] = (uint8_t)(m->t.vt >> 8);
        for (unsigned i = 0; i < 11; i++) o->uw_counters[i] = mt.r7063[i];
        o->uw_counters[11] = (uint8_t)m->t.vt; o->uw_counters[12] = (uint8_t)(m->t.vt >> 8);
        o->uw_width[0] = mt.r7042[4]; o->uw_width[1] = mt.r7042[5];
        break;
    }
    case PS5_BRIDGE_MANUAL:                                     /* hdmi.c setHdmiBasicVideoConfigFlava3 "manual" */
        o->reg7227[0] = 0x05;
        break;
    default:
        return -1;
    }
    return 0;
}

int ps5_flava3_modex_for(enum ps5_mode_id id, struct ps5_flava3_mode_x *o) {
    if (!o || ps5_flava3_mode_for(id, &o->m)) return -1;
    const struct ps5_mode_entry *m = &ps5_display_modes[id];
    struct flava3_manual mt;
    o->manual = 0;
    for (unsigned i = 0; i < 8; i++) o->m7042[i] = 0;
    for (unsigned i = 0; i < 11; i++) o->m7063[i] = 0;
    o->m7078[0] = o->m7078[1] = 0;
    if (m->recipe != PS5_BRIDGE_MANUAL) return 0;
    /* PS5 Linux takes the manual path only for +H/-V (isManualTiming); the written bytes do not depend on the
     * polarity. r5.13 deliberately builds them for this +H/+V DTD (test variant); DCN OTG and DP MSA carry the real
     * +H/+V. Whether the bridge output then has the sink's polarity is one of the things the test shows. */
    if (flava3_manual_build(m->t.h, m->t.hs, m->t.he, m->t.ht, m->t.v, m->t.vs, m->t.ve, m->t.vt, m->t.clock_khz,
                            1, 0, &mt)) return -1;
    o->manual = 1;
    for (unsigned i = 0; i < 8; i++) o->m7042[i] = mt.r7042[i];
    for (unsigned i = 0; i < 11; i++) o->m7063[i] = mt.r7063[i];
    o->m7078[0] = mt.r7078[0]; o->m7078[1] = mt.r7078[1];
    return 0;
}
