/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * display-modes draft (UNTESTED on hardware). One DCN201 pipe replay per boot mode. Generated
 * headers (tools/gen-pipe-replay.py, from a READ-ONLY PS5 Linux capture at that exact mode)
 * define one struct each; the 1080p entry wraps the existing, hardware-proven replay. */
#ifndef PS5_DCN201_MODE_REPLAY_H
#define PS5_DCN201_MODE_REPLAY_H
#include "dcn201-pipe-replay.h"
struct ps5_dcn201_mode_replay {
 const struct ps5_dcn201_replay *rows; unsigned count;
 unsigned width,height,pitch_pixels;
 unsigned sync[4];          /* OTG global sync: vstartup, vupdate offset, vupdate width, vready */
 unsigned vtg_fp2;          /* VTG0_FP2 belonging to that vstartup                              */
 uint32_t hubp_cntl[4]; unsigned hubp_count;   /* HUBPx_DCHUBP_CNTL to unblank after the replay */
 uint32_t dentist;          /* DENTIST_DISPCLK_CNTL of the mode (gen-pipe-replay --dentist); 0 = none, leave clocks */
};
/* Today's replay with the firmware's own global sync (ps5-native-dp-video.h:162, VTG FP2 =
 * startup-42, dcn201-timing.c:101) and the two HUBPs it unblanks (ps5-native-dp-video.h:125-126). */
static const struct ps5_dcn201_mode_replay ps5_dcn201_replay_1080p60_fw={
 ps5_dcn201_pipe_replay,sizeof(ps5_dcn201_pipe_replay)/sizeof(ps5_dcn201_pipe_replay[0]),
 1920U,1080U,PS5_DCN201_REPLAY_PITCH_PIXELS,{43U,2U,3U,4U},1U,{0xeacc,0xf51c},2U};
#endif
