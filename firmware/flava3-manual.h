/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * flava3-manual.h v1.0 (16.11.2026, stage F) - FLAVA3 manual timing for EDID modes without a stock recipe,
 * exactly PS5 Linux hdmi.c setManualTimingFlava3()/isTimingValid()/isManualTiming() (linux.patch:11409-11470) with the
 * profile of setHdmiBasicVideoConfigFlava3() (0x7227 = 05 00, 0x7070 = 6 x 00). Host check: for 3440x1440@60/@100 the
 * 0x7042 vstart/width and the first 11 bytes of 0x7063 equal the hardware-proven stage-14 bytes (ps5-display-mode.c c60/c100).
 * Pure function: fills byte arrays, no I/O. */
#ifndef FLAVA3_MANUAL_H
#define FLAVA3_MANUAL_H
#include <stdint.h>
struct flava3_manual { uint8_t r7042[8], r7063[11], r7078[2]; };   /* + W(0x704f,0x12) between 7042 and 7063 */
/* timing: h, hsync_start, hsync_end, htotal, v, vsync_start, vsync_end, vtotal, clock_khz, hpos, vpos (1 = positive) */
static inline int flava3_manual_valid(unsigned h, unsigned hs, unsigned he, unsigned ht, unsigned v, unsigned vs, unsigned ve,
                                      unsigned vt, unsigned clk, int hpos, int vpos)
{
    if (!(hpos == 1 && vpos == 0)) return 0;                    /* isManualTiming: exactly +HSync -VSync */
    if (h < 640 || h > 3840 || v < 480 || v > 2160 || clk < 25000 || clk > 600000) return 0;
    if (hs < h || he <= hs || ht < he || ht > 65535 || ht - hs < 2) return 0;
    if (vs < v || ve <= vs || vt < ve || vt > 65535) return 0;
    return (uint64_t)ht * vt <= 0x100000000ULL && (uint64_t)ht * (ve - vs) <= 0x1000000ULL;
}
static inline int flava3_manual_build(unsigned h, unsigned hs, unsigned he, unsigned ht, unsigned v, unsigned vs, unsigned ve,
                                      unsigned vt, unsigned clk, int hpos, int vpos, struct flava3_manual *o)
{
    if (!o || !flava3_manual_valid(h, hs, he, ht, v, vs, ve, vt, clk, hpos, vpos)) return -1;
    uint16_t hstart = (uint16_t)(ht - hs - 2), vstart = (uint16_t)(vt - vs + 1), width = (uint16_t)(h - 1), height = (uint16_t)(v - 1);
    uint16_t htotal = (uint16_t)(ht - 1), hsync = (uint16_t)(he - hs - 1), clock = (uint16_t)(clk / 1000);
    uint32_t frame = (uint32_t)ht * vt - 1, vsync = (uint32_t)ht * (ve - vs) - 1;
    const uint8_t a[8] = { (uint8_t)hstart, (uint8_t)(hstart >> 8), (uint8_t)vstart, (uint8_t)(vstart >> 8),
                           (uint8_t)width, (uint8_t)(width >> 8), (uint8_t)height, (uint8_t)(height >> 8) };
    const uint8_t b[11] = { (uint8_t)htotal, (uint8_t)(htotal >> 8), (uint8_t)hsync, (uint8_t)(hsync >> 8),
                            (uint8_t)frame, (uint8_t)(frame >> 8), (uint8_t)(frame >> 16), (uint8_t)(frame >> 24),
                            (uint8_t)vsync, (uint8_t)(vsync >> 8), (uint8_t)(vsync >> 16) };
    for (int i = 0; i < 8; i++) o->r7042[i] = a[i];
    for (int i = 0; i < 11; i++) o->r7063[i] = b[i];
    o->r7078[0] = (uint8_t)clock; o->r7078[1] = (uint8_t)(0x80 | ((clock >> 8) & 0x07));
    return 0;
}
#endif
