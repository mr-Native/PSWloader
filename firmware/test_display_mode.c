/* Host unit test for ps5-display-mode.c (no hardware).
 * Build+run (see run-tests.sh):
 *   gcc -Wall -Wextra -Werror -I. -I<repo>/windows-native test_display_mode.c ps5-display-mode.c \
 *       <repo>/windows-native/dcn201-timing.c -o t && ./t <dell-original.bin>
 * Proves: (1) the OTG formula reproduces dcn201-timing.c:ps5_dcn201_1080p_sync exactly,
 * (2) the MSA formula reproduces every MSA readback captured on PS5 Linux,
 * (3) the ultrawide geometry constants equal the documented counter formula,
 * (4) EDID parsing/selection on the real Dell S3422DWG EDID and on synthetic TVs. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ps5-display-mode.h"
#include "dcn201-timing.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static void put_dtd(uint8_t *b, const struct ps5_timing *t) {
    unsigned p = t->clock_khz / 10, hb = t->ht - t->h, vb = t->vt - t->v;
    unsigned hf = t->hs - t->h, hw = t->he - t->hs, vf = t->vs - t->v, vw = t->ve - t->vs;
    b[0] = p & 255; b[1] = p >> 8; b[2] = t->h & 255; b[3] = hb & 255; b[4] = ((t->h >> 8) << 4) | (hb >> 8);
    b[5] = t->v & 255; b[6] = vb & 255; b[7] = ((t->v >> 8) << 4) | (vb >> 8);
    b[8] = hf & 255; b[9] = hw & 255; b[10] = ((vf & 15) << 4) | (vw & 15);
    b[11] = ((hf >> 8) << 6) | ((hw >> 8) << 4) | ((vf >> 4) << 2) | (vw >> 4);
    b[17] = 0x18 | (t->hpos << 1) | (t->vpos << 2);
}
static void finish(uint8_t *blk) { unsigned s = 0; for (int i = 0; i < 127; i++) s += blk[i]; blk[127] = (uint8_t)(-(int)s); }

/* Synthetic TV: preferred DTD + CTA ext with SVDs, optional HDMI VSDB / HF-VSDB. */
static unsigned make_tv(uint8_t e[256], const struct ps5_timing *pref, const uint8_t *vics, unsigned nvic,
                        unsigned vsdb_tmds_5mhz, unsigned hf_tmds_5mhz, int scdc) {
    memset(e, 0, 256);
    static const uint8_t hdr[8] = {0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0};
    memcpy(e, hdr, 8); e[18] = 1; e[19] = 4; e[126] = 1;
    put_dtd(e + 54, pref);
    finish(e);
    uint8_t *x = e + 128; unsigned i = 4;
    x[0] = 2; x[1] = 3;
    x[i++] = (2 << 5) | nvic; memcpy(x + i, vics, nvic); i += nvic;
    if (vsdb_tmds_5mhz) { uint8_t v[] = {(3 << 5) | 7, 0x03, 0x0c, 0x00, 0x10, 0x00, 0x00, (uint8_t)vsdb_tmds_5mhz}; memcpy(x + i, v, 8); i += 8; }
    if (hf_tmds_5mhz) { uint8_t v[] = {(3 << 5) | 6, 0xd8, 0x5d, 0xc4, 0x01, (uint8_t)hf_tmds_5mhz, (uint8_t)(scdc ? 0x88 : 0)}; memcpy(x + i, v, 7); i += 7; }
    x[2] = (uint8_t)i;
    finish(x);
    return 256;
}

int main(int argc, char **argv) {
    /* (1) OTG formula vs the proven 1080p constants */
    struct ps5_dcn201_register_write a[10], b[10];
    CHECK(ps5_dcn201_1080p_sync(b) == 0);
    CHECK(ps5_dcn201_otg_sync(&ps5_display_modes[PS5_MODE_1080P60].t, a) == 0);
    CHECK(memcmp(a, b, sizeof(a)) == 0);
    for (int i = 0; i < 10; i++)
        if (a[i].offset != b[i].offset || a[i].mask != b[i].mask || a[i].value != b[i].value)
            printf("  otg[%d] formula %x/%x/%x fixed %x/%x/%x\n", i, a[i].offset, a[i].mask, a[i].value, b[i].offset, b[i].mask, b[i].value);
    CHECK(ps5_vtg0_fp2(&ps5_display_modes[PS5_MODE_1080P60].t, 43) == 1);           /* dcn201-timing.c:101 */
    CHECK(ps5_vtg0_vcount_init(&ps5_display_modes[PS5_MODE_1080P60].t) == 0x461);   /* dcn201-timing.c:102 */

    /* (2) MSA vs PS5 Linux readbacks (evidence/20261018/{1080,1440,2160,3440}-dp-msa.json,
     *     final-coldstart-msa.json for 3440@100): raw[0..3] and vid_m */
    static const struct { enum ps5_mode_id id; uint32_t raw[4]; uint32_t m; int m_tol; } msa[] = {
        {PS5_MODE_1080P60,  {0x08980465, 0x00c00029, 0x002c0005, 0x07800438},  6007, 0},
        {PS5_MODE_1440P60,  {0x0aa005c9, 0x00700026, 0x00208005, 0x0a0005a0},  9770, 0},
        {PS5_MODE_2160P60,  {0x113008ca, 0x01800052, 0x0058000a, 0x0f000870}, 24031, 1}, /* hw-measured M */
        {PS5_MODE_3440P60,  {0x0e1005c9, 0x00700026, 0x0020800a, 0x0d7005a0}, 12935, 0},
        {PS5_MODE_3440P100, {0x0e1005e6, 0x00700043, 0x0020800a, 0x0d7005a0}, 21987, 0},
    };
    for (unsigned k = 0; k < sizeof(msa) / sizeof(msa[0]); k++) {
        uint32_t o[4]; const struct ps5_timing *t = &ps5_display_modes[msa[k].id].t;
        ps5_dp_msa(t, o);
        CHECK(memcmp(o, msa[k].raw, sizeof(o)) == 0);
        uint32_t m = ps5_dp_vid_m(t->clock_khz);
        CHECK(m + msa[k].m_tol >= msa[k].m && m <= msa[k].m + msa[k].m_tol);
        printf("msa %-26s %08x %08x %08x %08x  M=%u (captured %u)\n", ps5_display_modes[msa[k].id].name, o[0], o[1], o[2], o[3], m, msa[k].m);
    }
    CHECK(ps5_dp_vid_m(148500) == (uint32_t)(32768ULL * 148500 / 810000));            /* dcn201-stream.c:99 */

    /* (3) ultrawide constants = counter formula of hdmi.c:831-838 */
    for (int id = PS5_MODE_3440P60; id <= PS5_MODE_3440P100; id++) {
        struct ps5_flava3_mode f; const struct ps5_timing *t = &ps5_display_modes[id].t;
        CHECK(ps5_flava3_mode_for((enum ps5_mode_id)id, &f) == 0 && f.uw_geometry);
        uint32_t ht = t->ht, vt = t->vt, frame = ht * vt - 1, vsw = ht * (t->ve - t->vs) - 1;
        uint8_t c[13] = {(uint8_t)(ht - 1), (uint8_t)((ht - 1) >> 8), (uint8_t)(t->he - t->hs - 1), 0,
                         (uint8_t)frame, (uint8_t)(frame >> 8), (uint8_t)(frame >> 16), (uint8_t)(frame >> 24),
                         (uint8_t)vsw, (uint8_t)(vsw >> 8), (uint8_t)(vsw >> 16), (uint8_t)vt, (uint8_t)(vt >> 8)};
        CHECK(memcmp(c, f.uw_counters, 13) == 0);
        CHECK(f.uw_vstart[0] == (uint8_t)(vt - t->vs + 1) && f.uw_vtotal[0] == (uint8_t)vt && f.uw_vtotal[1] == (uint8_t)(vt >> 8));
        CHECK(f.uw_width[0] == (uint8_t)(t->h - 1) && f.uw_width[1] == (uint8_t)((t->h - 1) >> 8));
        CHECK(f.reg7227[0] == 5 && f.reg7070[5] == 0xfb && f.high_tmds == (id == PS5_MODE_3440P100));
    }
    { struct ps5_flava3_mode f; CHECK(ps5_flava3_mode_for(PS5_MODE_1080P60, &f) == 0 && f.reg7227[0] == 16 && f.reg7070[0] == 16 && f.reg7070[1] == 16 && !f.high_tmds); }
    { struct ps5_flava3_mode f; CHECK(ps5_flava3_mode_for(PS5_MODE_2160P60, &f) == 0 && f.reg7227[0] == 6 && f.vic97_mask && f.high_tmds); }

    /* (4a) real Dell S3422DWG EDID */
    const uint32_t all = (1u << PS5_MODE_COUNT) - 1;
    if (argc > 1) {
        uint8_t e[256]; FILE *fp = fopen(argv[1], "rb"); size_t n = fp ? fread(e, 1, sizeof(e), fp) : 0; if (fp) fclose(fp);
        CHECK(n == 256);
        struct ps5_edid_info info;
        CHECK(ps5_edid_parse(e, 256, &info) == 0 && info.valid && info.blocks == 2);
        CHECK(info.has_preferred && info.dtd[0].h == 3440 && info.dtd[0].clock_khz == 319750);
        CHECK(info.hdmi_vsdb && info.max_tmds_khz == 300000 && info.hf_vsdb && info.hf_max_tmds_khz == 600000 && info.scdc);
        printf("dell: %u DTDs, %u VICs, VSDB %u kHz, HF %u kHz scdc %u\n", info.dtd_count, info.vic_count, info.max_tmds_khz, info.hf_max_tmds_khz, info.scdc);
        for (int id = 0; id < PS5_MODE_COUNT; id++) printf("  offered %-26s %d\n", ps5_display_modes[id].name, ps5_display_offered(&info, (enum ps5_mode_id)id));
        CHECK(ps5_display_select(&info, 60, all) == PS5_MODE_3440P60);
        CHECK(ps5_display_select(&info, 0, all) == PS5_MODE_3440P100);
        CHECK(ps5_display_select(&info, 0, 1u << PS5_MODE_3440P60) == PS5_MODE_3440P60);
        CHECK(ps5_display_select(&info, 60, 0) == PS5_MODE_1080P60);                   /* only today's replay */
        CHECK(ps5_display_select(&info, 60, 1u << PS5_MODE_2160P60) == PS5_MODE_2160P60); /* VIC97 in SVD list */
        e[0] = 0x01;                                                                       /* PS5 ICC quirk */
        CHECK(ps5_edid_parse(e, 256, &info) == 0 && ps5_display_select(&info, 0, all) == PS5_MODE_3440P100);
        e[200] ^= 0x55;            /* corrupt extension: no HDMI VSDB left -> 165 MHz rule -> 1080p60 */
        CHECK(ps5_edid_parse(e, 256, &info) == 0 && ps5_display_select(&info, 0, all) == PS5_MODE_1080P60);
    } else printf("note: no Dell EDID path given, real-EDID cases skipped\n");
    /* (4a') MN864739 = the converter's own FALLBACK EDID (seen by Linux when ICC/EMC does not answer;
     *       evidence/windows-linux-edid-20261102.json): 4K DTD at 594.03 MHz, no HF-VSDB
     *       -> must stay 1080p60 */
    if (argc > 2) {
        uint8_t e2[256]; FILE *fp = fopen(argv[2], "rb"); size_t n = fp ? fread(e2, 1, sizeof(e2), fp) : 0; if (fp) fclose(fp);
        struct ps5_edid_info li;
        CHECK(n == 256 && ps5_edid_parse(e2, 256, &li) == 0 && li.dtd[0].h == 3840);
        CHECK(ps5_display_select(&li, 0, all) == PS5_MODE_1080P60);
        printf("converter fallback EDID MN864739: select -> %s\n", ps5_display_modes[ps5_display_select(&li, 0, all)].name);
    }

    /* (4b) synthetic sinks */
    uint8_t e[256]; struct ps5_edid_info info;
    const uint8_t tv4k[] = {97 | 0x80, 16, 63, 4};
    make_tv(e, &ps5_display_modes[PS5_MODE_2160P60].t, tv4k, sizeof(tv4k), 60, 120, 1);
    CHECK(ps5_edid_parse(e, 256, &info) == 0);
    CHECK(ps5_display_select(&info, 60, all) == PS5_MODE_2160P60);
    CHECK(ps5_display_select(&info, 60, all & ~(1u << PS5_MODE_2160P60)) == PS5_MODE_1080P60);
    CHECK(ps5_display_select(&info, 0, all & ~(1u << PS5_MODE_2160P60)) == PS5_MODE_1080P120);
    make_tv(e, &ps5_display_modes[PS5_MODE_2160P60].t, tv4k, sizeof(tv4k), 60, 0, 0);    /* no HF-VSDB */
    CHECK(ps5_edid_parse(e, 256, &info) == 0 && !ps5_display_offered(&info, PS5_MODE_2160P60));
    CHECK(ps5_display_select(&info, 0, all) == PS5_MODE_1080P120);
    const uint8_t tv1080[] = {16 | 0x80};
    make_tv(e, &ps5_display_modes[PS5_MODE_1080P60].t, tv1080, 1, 0, 0, 0);             /* DVI-ish, no VSDB */
    CHECK(ps5_edid_parse(e, 256, &info) == 0 && ps5_display_select(&info, 0, all) == PS5_MODE_1080P60);
    CHECK(ps5_display_select(NULL, 0, all) == PS5_MODE_1080P60);
    memset(e, 0, 256); CHECK(ps5_edid_parse(e, 256, &info) == -1 && ps5_display_select(&info, 0, all) == PS5_MODE_1080P60);
    struct ps5_timing lg = {319890, 3440, 3488, 3520, 3600, 1440, 1443, 1453, 1481, 1, 0};  /* unverified variant */
    make_tv(e, &lg, tv1080, 1, 60, 120, 1);
    CHECK(ps5_edid_parse(e, 256, &info) == 0 && ps5_display_select(&info, 0, all) == PS5_MODE_1080P60);

    printf(failures ? "FAILED: %d\n" : "ALL TESTS PASSED\n", failures);
    return failures != 0;
}
