/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PS5_FLAVA3_VIDEO_H
#define PS5_FLAVA3_VIDEO_H
#include "flava3-prepare.h"
#include "flava3-mode.h"
/* Build-only fixed RGB8 CEA VIC16 (1920x1080p60, 148.5MHz) TMDS stages:
 * 0 preconfig, 1 DP link training, 2 basic video, 3 PHY first, 4 PHY second, 5 additional video/output start,
 * 6 explicit HDCP-off policy, 7 encoder quiesce, 11 converter unmute (code 4, full write 0x705f=0 after stage 5).
 * Training requires a valid GPU DP source; do not submit before DCN/link setup.
 * Policy/quiesce precede preparation/video stages. Encoder quiesce alone
 * does not confirm sink SCDC stopped or replace a general-service HDCP stop.
 * No arbitrary timings, FRL, HDR or 4K. Output unchanged on failure. */
int ps5_flava3_video_stage(const struct ps5_hdmi_config *config,unsigned stage,
                           struct ps5_hdmi_commands *out);
/* UNTESTED ON HARDWARE. Same stages for a whitelisted mode (ps5-display-mode.c). mode NULL builds
 * exactly the packets of ps5_flava3_video_stage (VIC16). Mode-dependent: stage 2 (0x7227/0x7070
 * profile, high-TMDS flags hdmi.c:884-928), stage 5 (VIC97 mask, scrambling hdmi.c:932-983) and
 * the new stage 14 (3440x1440 geometry + VIFUPD, hdmi.c:820-858; run after 5, before 11). */
int ps5_flava3_video_stage_mode(const struct ps5_hdmi_config *config,unsigned stage,
                                const struct ps5_flava3_mode *mode,struct ps5_hdmi_commands *out);
/* r5.13: the same with the optional manual-timing extension (NULL or manual=0: as ps5_flava3_video_stage_mode). */
int ps5_flava3_video_stage_modex(const struct ps5_hdmi_config *config,unsigned stage,
                                 const struct ps5_flava3_mode_x *mode,struct ps5_hdmi_commands *out);
#endif
