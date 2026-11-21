/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PS5_FLAVA3_PREPARE_H
#define PS5_FLAVA3_PREPARE_H
#include "icc-hdmi.h"
#include "hdmi-commands.h"
/* Prepare one stage: 0 interrupt configuration, 1 DP input, 2 VSync.
 * Building only: caller submits explicitly after validating ICC ownership.
 * Not full HDMI initialization: FRL cleanup, video timing, PHY/link and DCN
 * setup are separate prerequisites. All stages require actual FLAVA3 config.
 * Rejected arguments leave output unchanged. */
int ps5_flava3_prepare_stage(const struct ps5_hdmi_config *config,unsigned stage,
                            struct ps5_hdmi_commands *out);
/* Submit stages 0..2 once each, stop on first error. completed counts only
 * acknowledged stages; partial hardware effects cannot be rolled back here.
 * Caller owns ICC and notification lifetime. Not full link initialization. */
int ps5_flava3_prepare(const struct ps5_icc_io *io,const struct ps5_hdmi_config *config,
                      unsigned *completed,ps5_icc_notification callback,void *context);
#endif
