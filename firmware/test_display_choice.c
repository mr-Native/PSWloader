/* r5.27 host test: settings choice (resolution=) against the real UGREEN-UHD and Dell S3422DWG EDIDs.
 * Build: gcc -std=c11 -Wall -Wextra -Werror -I. test_display_choice.c ps5-display-mode.c dcn201-timing.c
 *        dcn201-test-pattern.c && ./a.out <ugreen-edid.bin> <dell-edid.bin> */
#include "ps5-display-mode.h"
#include "ps5-settings.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned load(const char *p, uint8_t *b) { FILE *f = fopen(p, "rb"); assert(f); unsigned n = (unsigned)fread(b, 1, 2048, f); fclose(f); return n; }
int main(int argc, char **argv) {
    uint8_t u[2048], d[2048]; struct ps5_edid_info U, D; enum ps5_mode_id id;
    assert(argc > 2);
    unsigned nu = load(argv[1], u), nd = load(argv[2], d);
    assert(ps5_edid_parse(u, nu, &U) == 0 && ps5_edid_parse(d, nd, &D) == 0);
    /* release replay set (build-vN.sh): 3440p60 3440p100 ugreen1440p60 2560x1440p120 720p144, 1080p60 always */
    const uint32_t REL = 1u << PS5_MODE_3440P60 | 1u << PS5_MODE_3440P100 | 1u << PS5_MODE_UGREEN1440P60 |
                         1u << PS5_MODE_2560P120 | 1u << PS5_MODE_720P144;
    assert(PS5_DISPLAY_CHOICES + 1 == PS5_SET_RES_COUNT);
    /* every choice name matches its table entries */
    for (int m = 0; m < PS5_MODE_COUNT; m++) {
        unsigned c = ps5_display_choice_of((enum ps5_mode_id)m); char want[32];
        if (!c) continue;
        snprintf(want, sizeof want, "%ux%u@%u", ps5_display_modes[m].t.h, ps5_display_modes[m].t.v, ps5_display_modes[m].refresh_hz);
        assert(strcmp(want, ps5_set_res[c]) == 0);
    }
    /* UGREEN: 1080p60, 2560x1440@60 (its own DTD), 720p144 */
    uint32_t mu = ps5_display_choice_mask(&U, REL);
    printf("UGREEN choices %03x:", mu); for (unsigned c = 1; c <= PS5_DISPLAY_CHOICES; c++) if (mu >> c & 1) printf(" %s", ps5_set_res[c]); puts("");
    assert(mu == (1u << 1 | 1u << 3 | 1u << 8));
    assert(ps5_display_select_choice(&U, 3, REL, &id) && id == PS5_MODE_UGREEN1440P60);
    assert(ps5_display_select_choice(&U, 8, REL, &id) && id == PS5_MODE_720P144);
    assert(ps5_display_select_choice(&U, 1, 0, &id) && id == PS5_MODE_1080P60);            /* 1080p60 needs no replay bit */
    id = PS5_MODE_COUNT;
    assert(!ps5_display_select_choice(&U, 5, REL, &id) && id == PS5_MODE_COUNT);           /* not offered: untouched */
    assert(!ps5_display_select_choice(&U, 3, 0, &id));                                     /* offered, no replay */
    assert(!ps5_display_select_choice(&U, 0, REL, &id) && !ps5_display_select_choice(&U, 9, REL, &id));
    /* Dell: 3440x1440@60/100, 2560x1440@120, 1080p60 */
    uint32_t md = ps5_display_choice_mask(&D, REL);
    printf("Dell choices %03x:", md); for (unsigned c = 1; c <= PS5_DISPLAY_CHOICES; c++) if (md >> c & 1) printf(" %s", ps5_set_res[c]); puts("");
    fflush(stdout); assert(md >> 1 & 1 && md >> 5 & 1 && md >> 6 & 1 && md >> 4 & 1);
    assert(ps5_display_select_choice(&D, 6, REL, &id) && id == PS5_MODE_3440P100);
    struct ps5_edid_info bad; memset(&bad, 0, sizeof bad);
    assert(ps5_display_choice_mask(&bad, REL) == 0);
    puts("ALL OK"); return 0;
}
