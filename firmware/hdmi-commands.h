/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PS5_HDMI_COMMANDS_H
#define PS5_HDMI_COMMANDS_H
#include "icc-power.h"
/* ICC HDMI service wire builder. No allocation, no implicit retry. Caller
 * exclusively owns ICC. Building packets never accesses hardware. */
struct ps5_hdmi_commands {uint8_t data[0x7e4];unsigned used;};
/* Linux frl_reg_read wire format: converter register read, no sink DDC bus.
 * 1..32 bytes, output unchanged on failure. No implicit retry. */
int ps5_hdmi_register_read(const struct ps5_icc_io *,uint16_t reg,unsigned count,
                           uint8_t *out,ps5_icc_notification,void *context);
int ps5_hdmi_commands_init(struct ps5_hdmi_commands *p,unsigned code);
struct ps5_hdmi_register_write {uint16_t reg;const uint8_t *data;unsigned bytes;};
int ps5_hdmi_commands_write_block(struct ps5_hdmi_commands *p,const struct ps5_hdmi_register_write *writes,unsigned count);
int ps5_hdmi_commands_write(struct ps5_hdmi_commands *p,uint16_t reg,const uint8_t *data,unsigned bytes);
int ps5_hdmi_commands_write_single(struct ps5_hdmi_commands *p,uint16_t reg,uint8_t value);
/* Event minor 1: HDCP mode (0..3); minor 3: output stop/TMDS start (0..1);
 * minor 4: HDMI event (EDID/hotplug notification) enable (0..1), Linux sceControlHdmiEvent
 * (linux-vcn4/drivers/ps5/hdmi.c:558-563, i2c_cmd_5_4 :400-415). UNTESTED on hardware. */
int ps5_hdmi_commands_event(struct ps5_hdmi_commands *p,unsigned minor,uint8_t value);
/* Linux i2c_cmd_4_16 (hdmi.c:367-381): a converter sequence passed verbatim (the EDID/PLL
 * sequences of hdmiSystemResume, hdmi.c:509-535). 1..251 bytes. UNTESTED on hardware. */
int ps5_hdmi_commands_sequence(struct ps5_hdmi_commands *p,const uint8_t *data,unsigned bytes);
/* Linux i2c_cmd_4_2 (hdmi.c:462-482): count entries of 4 bytes each. UNTESTED on hardware. */
int ps5_hdmi_commands_table(struct ps5_hdmi_commands *p,const uint8_t *data,unsigned count);
int ps5_hdmi_commands_mask(struct ps5_hdmi_commands *p,uint16_t reg,uint8_t value,uint8_t mask);
/* Converter-side masked wait: set=1 waits set, set=0 waits clear.
 * ICC transport deadline still applies; no host-side repeat. */
int ps5_hdmi_commands_wait(struct ps5_hdmi_commands *p,uint16_t reg,uint8_t mask,int set);
int ps5_hdmi_commands_delay(struct ps5_hdmi_commands *p,unsigned milliseconds);
/* -6 service rejection/short reply; transport errors propagated unchanged.
 * Success acknowledges command processing, not a functioning display. */
int ps5_hdmi_commands_submit(const struct ps5_icc_io *io,const struct ps5_hdmi_commands *p,
                            ps5_icc_notification callback,void *context);
#endif
