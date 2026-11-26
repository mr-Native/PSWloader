/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PS5_VERDE_VIDEO_H
#define PS5_VERDE_VIDEO_H
#include "hdmi-commands.h"
#include "icc-hdmi.h"
#include "flava3-mode.h"
/* VERDE HDMI bridge (PS5 Slim / late revisions, converter == 3) packet builders.
 * Register sequences from PS5 Linux drivers/ps5/hdmi.c. Build-only; no hardware access. */
int ps5_verde_edid_stage(struct ps5_hdmi_commands *out);
/* Returns 0 if a command packet was built into out, 1 if the stage is a no-op on VERDE
 * (stages 4, 5, 8, 9, 10, 14), or -1 on invalid arguments. NULL mode selects 1080p60 (VIC16). */
int ps5_verde_video_stage_mode(const struct ps5_hdmi_config *config, unsigned stage,
                               const struct ps5_flava3_mode *mode, struct ps5_hdmi_commands *out);
#endif
