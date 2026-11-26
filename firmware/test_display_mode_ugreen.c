/* Stage B host test (no hardware): ps5-display-mode.c with the real UGREEN-UHD EDID (14.11.2026,
 * runs/edid6-boot-20261114-091004/edid.bin, fix_edid-repaired; also tested with the raw PS5 byte 0 = 0x01).
 * Build: gcc -std=c11 -Wall -Wextra -Werror -I. test_display_mode_ugreen.c ps5-display-mode.c dcn201-timing.c
 *        dcn201-test-pattern.c && ./a.out <ugreen-edid.bin> <dell-edid.bin> */
#include "ps5-display-mode.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned load(const char *p, uint8_t *b) { FILE *f = fopen(p, "rb"); assert(f); unsigned n = (unsigned)fread(b, 1, 2048, f); fclose(f); return n; }
int main(int argc, char **argv) {
    uint8_t e[2048], d[2048];
    assert(argc > 2);
    unsigned n = load(argv[1], e), nd = load(argv[2], d);
    const uint32_t UG = 1u << PS5_MODE_UGREEN1440P60;
    for (int quirk = 0; quirk < 2; quirk++) {
        uint8_t x[2048]; memcpy(x, e, n);
        if (quirk) { unsigned s = 0; x[0] = 1; for (int i = 0; i < 127; i++) s += x[i]; x[127] = (uint8_t)(0u - s); }
        struct ps5_edid_info I;
        assert(ps5_edid_parse(x, n, &I) == 0 && I.valid);
        /* (1) sync byte 0x00 DTDs are kept like Linux: 2 base + 3 CTA DTDs */
        assert(I.dtd_count == 5 && I.has_preferred && I.dtd[0].h == 1920 && I.dtd[0].v == 1080);
        int found = 0;
        for (unsigned i = 0; i < I.dtd_count; i++) {
            const struct ps5_timing *t = &I.dtd[i];
            if (t->clock_khz == 241700) {
                assert(t->h == 2560 && t->hs == 2608 && t->he == 2640 && t->ht == 2720);
                assert(t->v == 1440 && t->vs == 1442 && t->ve == 1447 && t->vt == 1481 && !t->hpos && !t->vpos);
                found = 1;
            }
        }
        assert(found);
        /* (2) the whitelist entry is exactly that DTD; the Dell 241.5 entry is not offered */
        assert(ps5_display_offered(&I, PS5_MODE_UGREEN1440P60) == 1);
        assert(ps5_display_offered(&I, PS5_MODE_1440P60) == 0);
        assert(ps5_display_offered(&I, PS5_MODE_1080P60) == 1);
        /* (3)/(4) policy 3 (largest area with replay): 2560x1440 with the replay, 1080p60 without */
        assert(ps5_display_select_largest(&I, 0, UG) == PS5_MODE_UGREEN1440P60);
        assert(ps5_display_select_largest(&I, 60, UG) == PS5_MODE_UGREEN1440P60);
        assert(ps5_display_select_largest(&I, 0, 0) == PS5_MODE_1080P60);
        assert(ps5_display_select_largest(&I, 0, 1u << PS5_MODE_1440P60) == PS5_MODE_1080P60);
        /* policies 1/2 keep the native resolution first: the UGREEN prefers 1080p */
        assert(ps5_display_select(&I, 0, UG) == PS5_MODE_1080P60 && ps5_display_select(&I, 60, UG) == PS5_MODE_1080P60);
        /* r5.21 refresh=max: native 1080p has only 60 Hz -> fastest offered, 720p144 as before */
        assert(ps5_display_select_refresh_max(&I, UG | (1u << PS5_MODE_720P144)) == ps5_display_select_fastest(&I, 0, UG | (1u << PS5_MODE_720P144)));
        assert(ps5_display_select_refresh_max(&I, UG | (1u << PS5_MODE_720P144)) == PS5_MODE_720P144);
    }
    /* Dell S3422DWG is not affected: it does not offer the 241.7 MHz timing */
    struct ps5_edid_info D;
    assert(ps5_edid_parse(d, nd, &D) == 0);
    assert(ps5_display_offered(&D, PS5_MODE_UGREEN1440P60) == 0);
    assert(ps5_display_select_largest(&D, 0, UG) == PS5_MODE_1080P60);
    assert(ps5_display_select(&D, 0, (1u << PS5_MODE_3440P60) | (1u << PS5_MODE_3440P100)) == PS5_MODE_3440P100);
    assert(ps5_display_select_fastest(&D, 0, (1u << PS5_MODE_3440P60) | (1u << PS5_MODE_3440P100)) == PS5_MODE_3440P100);   /* r5.16 */
    assert(ps5_display_select_fastest(&D, 60, (1u << PS5_MODE_3440P60) | (1u << PS5_MODE_3440P100)) == PS5_MODE_3440P60);
    {   /* r5.21 refresh=max: Dell (DTDs 3440x1440@60 preferred, @100, 2560x1440@120) -> native 3440@100, not 2560@120 */
        const uint32_t dellmask = (1u << PS5_MODE_3440P60) | (1u << PS5_MODE_3440P100) | (1u << PS5_MODE_2560P120);
        assert(ps5_display_select_fastest(&D, 0, dellmask) == PS5_MODE_2560P120);         /* old r5.16 semantics */
        assert(ps5_display_select_refresh_max(&D, dellmask) == PS5_MODE_3440P100);        /* r5.21 */
        assert(ps5_display_select_refresh_max(&D, (1u << PS5_MODE_3440P60) | (1u << PS5_MODE_2560P120)) == PS5_MODE_2560P120);   /* native only 60 */
    }
    /* DCN values of the entry; OTG checked against PS5 Linux on the UGREEN (linux-display-baseline-v171-full.json) */
    const struct ps5_timing *t = &ps5_display_modes[PS5_MODE_UGREEN1440P60].t;
    struct ps5_dcn201_register_write w[10];
    assert(ps5_dcn201_otg_sync(t, w) == 0);
    assert(w[0].offset == 0x13fa8 && w[0].value == 0xa9f);            /* OTG_H_TOTAL            (Linux 0xa9f)     */
    assert(w[2].offset == 0x13fac && w[2].value == 0x700a70);         /* OTG_H_BLANK_START_END  (Linux 0x700a70)  */
    assert(w[4].offset == 0x13fbc && w[4].value == 0x5c8);            /* OTG_V_TOTAL            (Linux 0x5c8)     */
    assert(w[8].offset == 0x13fd8 && w[8].value == 0x2705c7);         /* OTG_V_BLANK_START_END  (Linux 0x2705c7)  */
    assert(w[1].value == 0x200000 && w[7].value == 0x50000);          /* sync widths 32 / 5                       */
    assert(w[3].value == 1 && w[9].value == 1);                       /* negative H and V sync                    */
    uint32_t msa[4]; ps5_dp_msa(t, msa);
    assert(msa[0] == 0x0aa005c9 && msa[1] == 0x00700027 && msa[2] == 0x80208005 && msa[3] == 0x0a0005a0);
    uint32_t m = ps5_dp_vid_m(t->clock_khz);
    assert(m == (uint32_t)((32768ull * 241700 + 405000) / 810000));
    struct ps5_flava3_mode f;
    assert(ps5_flava3_mode_for(PS5_MODE_UGREEN1440P60, &f) == 0);
    assert(f.reg7227[0] == 0x04 && f.reg7070[5] == 0xfb && !f.high_tmds && !f.uw_geometry);
    printf("ugreen1440p60: OTG H_TOTAL %#x H_BLANK %#x V_TOTAL %#x V_BLANK %#x | MSA %08x %08x %08x %08x | M %u | DTO %u Hz | 7227=%02x 7070[5]=%02x\n",
           w[0].value, w[2].value, w[4].value, w[8].value, msa[0], msa[1], msa[2], msa[3], m, t->clock_khz * 1000u, f.reg7227[0], f.reg7070[5]);
    printf("vtg0_vcount_init %u, vtg0_fp2(vstartup 43) %u\n", ps5_vtg0_vcount_init(t), ps5_vtg0_fp2(t, 43));
    puts("UGREEN STAGE B TESTS PASSED");
    return 0;
}
