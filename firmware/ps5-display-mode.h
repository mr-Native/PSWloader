/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * UNTESTED ON HARDWARE (host unit test only: test_display_mode.c).
 *
 * Boot display mode selection for the PS5 native UEFI payload:
 *   EDID (ICC HDMI service 0x10, notification subtype 2, see icc-edid.c)
 *   -> parse (DTDs of base block + CTA-861 extension, SVD VICs, HDMI VSDB, HF-VSDB)
 *   -> pick the best entry of a FIXED whitelist of FLAVA3 recipes that PS5 Linux has proven
 *   -> per-mode values for OTG timing, DP MSA, DP M/N, pixel DTO, DPG and the FLAVA3 bridge.
 *
 * No register value in here is invented:
 *  - whitelist timings/profiles: drivers/ps5/hdmi.c (7.1.7-ultrawide4,
 *    PS5 Linux (linux-vcn4) drivers/ps5/hdmi.c:803-930 and 1305-1330),
 *    ultrawide patch ps5-ultrawide2-full.patch:149-241.
 *  - OTG formula: AMD optc1_program_timing (Linux, MIT); reproduces the constants of
 *    dcn201-timing.c:ps5_dcn201_1080p_sync bit for bit (test).
 *  - MSA formula: reproduces the DP MSA readbacks captured on PS5 Linux for 1080p, 1440p,
 *    2160p, 3440x1440@60 and @100 (ultrawide kernel evidence 20261018 *-dp-msa.json, test).
 * Only FLAVA3 (converter type 2) is handled. VERDE keeps the fixed 1080p path.
 */
#ifndef PS5_DISPLAY_MODE_H
#define PS5_DISPLAY_MODE_H
#include "dcn201-test-pattern.h"   /* struct ps5_dcn201_register_write, <stdint.h> */
#include "flava3-mode.h"

struct ps5_timing {
    uint32_t clock_khz;
    uint16_t h, hs, he, ht;        /* hdisplay, hsync_start, hsync_end, htotal (DRM naming) */
    uint16_t v, vs, ve, vt;
    uint8_t hpos, vpos;            /* 1 = positive sync polarity */
};

/* FLAVA3 recipe families (how setHdmiBasicVideoConfigFlava3 selects 0x7227/0x7070). */
enum ps5_bridge_recipe {
    PS5_BRIDGE_VIC,          /* 0x7227={vic,0}, 0x7070={vic,vic,0,0,0,0} (VIC 16, 63)            */
    PS5_BRIDGE_1440_STOCK,   /* 0x7227={4,0},   0x7070={0,0,0,0,0,0xfb}                          */
    PS5_BRIDGE_VIC97,        /* 0x7227={6,0},   0x7070={0,...}, later 0x7227 masked to 97         */
    PS5_BRIDGE_UW3440,       /* 0x7227={5,0},   0x7070={0,0,0,0,0,0xfb} + geometry + VIFUPD      */
    PS5_BRIDGE_MANUAL,       /* r5.13: 0x7227={5,0}, 0x7070=6x0 + setManualTimingFlava3 in stage 2 */
};

/* Index into ps5_display_modes[]. The bit (1u<<id) is the "DCN replay compiled in" mask. */
enum ps5_mode_id {
    PS5_MODE_1080P60 = 0,
    PS5_MODE_1080P120,
    PS5_MODE_1440P60,
    PS5_MODE_2160P60,
    PS5_MODE_3440P60,
    PS5_MODE_3440P100,
    PS5_MODE_UGREEN1440P60,        /* Stage B: UGREEN-UHD 2560x1440@60 DTD (241.7 MHz, -H/-V), see ps5-display-mode.c */
    PS5_MODE_2560P120,             /* stage F (16.11.): 2560x1440@120 +H/-V, stage-14 geometry computed (flava3-manual.h) */
    PS5_MODE_720P144,              /* r5.13: UGREEN-UHD DTD 1280x720@144 (178.2 MHz, +H/+V), manual FLAVA3 timing */
    PS5_MODE_COUNT
};

struct ps5_mode_entry {
    struct ps5_timing t;
    uint8_t vic;                   /* 0 = not a CTA mode, must be offered as an exact DTD          */
    uint8_t recipe;                /* enum ps5_bridge_recipe                                        */
    uint16_t refresh_hz;           /* nominal refresh in Hz, for policy only                       */
    const char *name;
};
extern const struct ps5_mode_entry ps5_display_modes[PS5_MODE_COUNT];

#define PS5_EDID_INFO_MAX_DTD 16
#define PS5_EDID_INFO_MAX_VIC 64
struct ps5_edid_info {
    unsigned valid;                /* base block header/checksum OK (after the PS5 byte-0 fix)      */
    unsigned blocks;
    struct ps5_timing dtd[PS5_EDID_INFO_MAX_DTD];
    unsigned dtd_count;
    unsigned has_preferred;        /* dtd[0] is the base block's first DTD                          */
    uint8_t vic[PS5_EDID_INFO_MAX_VIC];
    unsigned vic_count;
    unsigned hdmi_vsdb;            /* HDMI 1.x VSDB present (00-0C-03)                              */
    uint32_t max_tmds_khz;         /* VSDB byte 7 * 5 MHz, 0 = not given                            */
    unsigned hf_vsdb;              /* HDMI Forum VSDB present (C4-5D-D8)                            */
    uint32_t hf_max_tmds_khz;      /* HF-VSDB Max_TMDS_Character_Rate * 5 MHz                       */
    unsigned scdc;                 /* HF-VSDB SCDC_Present                                         */
};

/* Parse a raw EDID as delivered by ICC (byte 0 may be 0x01, PS5 quirk: Linux fix_edid sets it
 * to 0 and recomputes the block-0 checksum; done here on a private copy). bytes multiple of 128.
 * Returns 0 and fills *out (valid=1) or -1 (out->valid=0, nothing else trustworthy). */
int ps5_edid_parse(const uint8_t *edid, unsigned bytes, struct ps5_edid_info *out);

/* Policy:
 *   max_hz        highest nominal refresh allowed (60 = conservative first trial, 0 = no limit)
 *   replay_mask   bit (1u<<ps5_mode_id) set for every mode whose DCN replay is compiled in.
 *                 PS5_MODE_1080P60 is always available (today's proven path).
 * Rule: native resolution (base block first DTD) first, then the highest refresh offered at that
 * resolution; if the native resolution has no usable entry, the largest area, then refresh.
 * Fallback: PS5_MODE_1080P60 (also for missing/invalid EDID). Never returns an error. */
enum ps5_mode_id ps5_display_select(const struct ps5_edid_info *edid, unsigned max_hz,
                                    uint32_t replay_mask);
/* Stage B test policy (build-uefi.py --display-policy 3): the largest offered area among the modes
 * with a compiled replay, then the higher refresh; the native resolution gets no preference.
 * Same fallback (PS5_MODE_1080P60). */
enum ps5_mode_id ps5_display_select_largest(const struct ps5_edid_info *edid, unsigned max_hz,
                                            uint32_t replay_mask);

/* r5.16 (setting refresh=max, policy 2 only): the highest nominal refresh among the offered modes with a compiled
 * replay; equal refresh -> the native resolution, then the larger area. Same fallback (PS5_MODE_1080P60). */
enum ps5_mode_id ps5_display_select_fastest(const struct ps5_edid_info *edid, unsigned max_hz,
                                            uint32_t replay_mask);

/* r5.21 (setting refresh=max, policy 2): highest refresh at the native resolution; if that is 60 Hz or less, the
 * highest refresh offered at all (ps5_display_select_fastest). Same fallback (PS5_MODE_1080P60). */
enum ps5_mode_id ps5_display_select_refresh_max(const struct ps5_edid_info *edid, uint32_t replay_mask);

/* Entry offered by the sink (exact DTD, or VIC in the SVD list) and within its TMDS limits
 * (165 MHz without HDMI VSDB; >340 MHz needs HF-VSDB with SCDC and a high enough rate). */
int ps5_display_offered(const struct ps5_edid_info *edid, enum ps5_mode_id id);

/* ---- per-mode DCN values -------------------------------------------------------------- */
/* The ten OTG writes of ps5_dcn201_1080p_sync for any progressive timing (same order/masks). */
int ps5_dcn201_otg_sync(const struct ps5_timing *t, struct ps5_dcn201_register_write out[10]);
/* DP_MSA_TIMING_PARAM1..4 (0x15830, 0x15834, 0x15838, 0x1583c). */
void ps5_dp_msa(const struct ps5_timing *t, uint32_t out[4]);
/* DP_VID_M for N=0x8000 at HBR3 (810000 kHz symbol clock), rounded to nearest. */
uint32_t ps5_dp_vid_m(uint32_t clock_khz);
/* VTG0_CONTROL low fields as in optc1_program_timing: VTG0_FP2 | VTG0_VCOUNT_INIT<<16
 * (dcn201-timing.c writes them with mask 0x7fff / 0x7fff0000). */
uint32_t ps5_vtg0_fp2(const struct ps5_timing *t, unsigned vstartup);
uint32_t ps5_vtg0_vcount_init(const struct ps5_timing *t);

/* ---- FLAVA3 bridge values (struct in flava3-mode.h) ----------------------------------- */
int ps5_flava3_mode_for(enum ps5_mode_id id, struct ps5_flava3_mode *out);
/* r5.13: base mode plus the manual-timing extension; manual=0 for every recipe except PS5_BRIDGE_MANUAL. */
int ps5_flava3_modex_for(enum ps5_mode_id id, struct ps5_flava3_mode_x *out);
#endif
