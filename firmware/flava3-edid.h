/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PS5_FLAVA3_EDID_H
#define PS5_FLAVA3_EDID_H
#include "hdmi-commands.h"
/* UNTESTED ON HARDWARE. FLAVA3 part of PS5 Linux hdmiSystemResume() in the verified order
 * (linux-vcn4/drivers/ps5/hdmi.c:565-583, called from spcie.c:727-729 before
 * sceHdmiInitVideoConfig): 0 i2c_cmd_4_2, 1..4 sceSetEdidSequence(0..3), 5 sceSetWaitPllSequence,
 * 6 unk_akv, 7 sceControlHdmiEvent(1). Afterwards the southbridge reads the sink EDID and sends
 * HDMI notification subtype 2 (icc-edid.c collects it); unplug arrives as subtype 1.
 * Today's payload never sends these, which is why every UEFI log shows "EDID bytes=0".
 * Build-only; caller owns ICC. Output unchanged on failure. */
#define PS5_FLAVA3_EDID_STAGES 8
int ps5_flava3_edid_stage(unsigned stage, struct ps5_hdmi_commands *out);
/* Submit stages 0..7 once each, stop at the first error; *completed = acknowledged stages. */
int ps5_flava3_edid_request(const struct ps5_icc_io *io, unsigned *completed,
                            ps5_icc_notification callback, void *context);
#endif
