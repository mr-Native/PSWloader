/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PS5_DCN201_TIMING_H
#define PS5_DCN201_TIMING_H
#include "dcn201-test-pattern.h"
/* Fixed CEA16 progressive sync registers only. Does not establish pixel
 * clock, global synchronization, VTG, OPP routing or a DP/HDMI link. */
int ps5_dcn201_1080p_sync(struct ps5_dcn201_register_write out[10]);
struct ps5_dcn201_mmio {
 void *context;
 int (*read)(void *,uint32_t,uint32_t *);
 int (*write)(void *,uint32_t,uint32_t);
 void (*delay_us)(void *,unsigned);
};
/* Enable OPTC/OTG and OPP pipe gates and wait for actual clock-on status. Clock
 * source/dividers must already be configured. -1 invalid I/O, -2 access
 * failure, -3 timeout. No automatic retries or assertion of visible output. */
int ps5_dcn201_enable_timing_clocks(const struct ps5_dcn201_mmio *);
struct ps5_dcn201_global_sync {unsigned startup,update_offset,update_width,ready_offset;};
/* Configure inactive OTG0+VTG0+OPP0 for CEA16 RGB8 test pattern. Caller must
 * establish valid pixel/display clocks and source appropriate sync parameters
 * (normally calculated by display mode library). Does NOT configure DP link.
 * Reject active OTG; -2 I/O error, -3 clock timeout, -4 active pipeline.
 * Writes before an error may persist; no automatic replay or rollback. */
int ps5_dcn201_start_solid_1080p(const struct ps5_dcn201_mmio *,
                                const struct ps5_dcn201_global_sync *,
                                unsigned red,unsigned green,unsigned blue);
/* display-modes draft (UNTESTED on hardware): the same sequence for any progressive mode.
 * otg = ps5_dcn201_otg_sync() (ps5-display-mode.c), vcount_init = vertical blank start,
 * fp2 = VTG0_FP2 (Linux value for captured modes). The 1080p function above is this one with
 * 1080p constants (bit-identical writes). Global sync values must be <= vtotal-1. */
int ps5_dcn201_start_solid_mode(const struct ps5_dcn201_mmio *,const struct ps5_dcn201_register_write otg[10],
                               unsigned width,unsigned height,unsigned vtotal,unsigned vcount_init,unsigned fp2,
                               const struct ps5_dcn201_global_sync *,unsigned red,unsigned green,unsigned blue);
#endif
