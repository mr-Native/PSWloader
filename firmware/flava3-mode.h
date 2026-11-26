/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * UNTESTED ON HARDWARE. Per-mode FLAVA3 parameters, shared by the BDS mode selection
 * (ps5-display-mode.c) and HdmiProbeDxe (flava3-video.c). Values only from PS5 Linux
 * drivers/ps5/hdmi.c and the ultrawide patch; see ps5_flava3_mode_for(). NULL = VIC16 (today). */
#ifndef PS5_FLAVA3_MODE_H
#define PS5_FLAVA3_MODE_H
#include "icc-power.h"   /* uint8_t for both EDK2 (PS5_EDK2) and host builds */
struct ps5_flava3_mode {
    uint8_t reg7227[2];
    uint8_t reg7070[6];
    uint8_t high_tmds;             /* clock > 340 MHz: scrambling path of hdmi.c:917-926, 950-973  */
    uint8_t vic97_mask;            /* hdmi.c:938-941                                               */
    uint8_t uw_geometry;           /* hdmi.c:820-858, applied after setVideoAdditionalConfig       */
    uint8_t uw_vstart[2], uw_vtotal[2], uw_counters[13], uw_width[2];
};
/* r5.13 (16.11.2026): PS5 Linux setManualTimingFlava3 (flava3-manual.h), written in stage 2 right after
 * 0x7202 as hdmi.c does. Passed to HdmiProbeDxe as the larger SetMode size; struct ps5_flava3_mode itself stays 30
 * bytes (Ps5HdmiMode variable contract with ps5pwr), so a hotplug replay from that variable lacks these writes. */
struct ps5_flava3_mode_x {
    struct ps5_flava3_mode m;
    uint8_t manual;                /* 1: write m7042/0x704f=0x12/m7063/m7078 in stage 2                */
    uint8_t m7042[8], m7063[11], m7078[2];
};
#endif
