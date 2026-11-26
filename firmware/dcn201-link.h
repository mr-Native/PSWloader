/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PS5_DCN201_LINK_H
#define PS5_DCN201_LINK_H
#include "dcn201-test-pattern.h"
struct ps5_dcn201_link { unsigned phy, hpd; };
/* Only the two internal DP paths actually present in the captured PS5 ROM.
 * This maps topology; it does not decide which connector is attached. */
int ps5_dcn201_link_decode(unsigned connector,unsigned encoder,unsigned hpd,
                          struct ps5_dcn201_link *out);
/* ATOM transmitter v1.6 parameters. Actions: DISABLE=0, INIT=7, ENABLE=1.
 * DISABLE requires zero lanes/clock, as in AMD disable_output.
 * Link clock in 10kHz, not the CRTC pixel clock. FE selection is always zero
 * on DCN; real frontend routing is configured separately in DIG_BE_CNTL. */
int ps5_dcn201_transmitter_params(const struct ps5_dcn201_link *,unsigned action,
                                unsigned lanes,unsigned clock_10khz,uint32_t out[4]);
/* Route DIGA to the selected backend, DP SST, HPD, lane count and scrambler.
 * Caller must own an inactive pipeline. This does not start/train the PHY,
 * program stream attributes or establish display/pixel clocks. */
int ps5_dcn201_link_plan(const struct ps5_dcn201_link *,unsigned lanes,
                        struct ps5_dcn201_register_write out[3]);
/* Fixed ATOM digxencodercontrol v1.5 STREAM_SETUP payload: DIGA, DP SST,
 * four lanes, CEA16 RGB8. Builder only; does not execute the ROM. */
int ps5_dcn201_encoder_setup_params(uint32_t out[3]);
#endif
