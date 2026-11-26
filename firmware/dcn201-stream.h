/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PS5_DCN201_STREAM_H
#define PS5_DCN201_STREAM_H
#include "dcn201-timing.h"
/* Fixed DIGA/OTG0 CEA16 RGB8 SST at HBR3 x4. Setup refuses active streams.
 * Unblank requires caller-confirmed receiver training and running OTG0.
 * -1 invalid I/O, -2 access failure, -4 pipeline state mismatch. */
int ps5_dcn201_stream_setup(const struct ps5_dcn201_mmio *);
/* Read DCN201 CLK4_CLK2_CURRENT_CNT (100kHz units) and program the
 * 148.5MHz DTO against that actual reference. Reject zero/unreasonable
 * counts instead of silently falling back to an unverified 600MHz. */
int ps5_dcn201_pixel_dto(const struct ps5_dcn201_mmio *,uint32_t *reference_hz);
/* Experimental fixed PHY-A/DIGA frontend clock route. Set documented
 * clock-enable/FE-force/source fields; require readback AND actual
 * DIG_SYMCLK_FE_ON within 1ms. Preserve unrelated fields. */
int ps5_dcn201_symbol_clock(const struct ps5_dcn201_mmio *);
int ps5_dcn201_stream_unblank(const struct ps5_dcn201_mmio *,int receiver_trained);
/* display-modes draft (UNTESTED on hardware): mode-parameterised variants; the functions above
 * are these with the CEA16 values (bit-identical writes). msa = ps5_dp_msa(), vid_m = ps5_dp_vid_m(). */
int ps5_dcn201_stream_setup_msa(const struct ps5_dcn201_mmio *,const uint32_t msa[4]);
int ps5_dcn201_pixel_dto_hz(const struct ps5_dcn201_mmio *,uint32_t pixel_hz,uint32_t *reference_hz);
int ps5_dcn201_stream_unblank_m(const struct ps5_dcn201_mmio *,int receiver_trained,uint32_t vid_m);
#endif
