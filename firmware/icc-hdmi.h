/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PS5_ICC_HDMI_H
#define PS5_ICC_HDMI_H
#include "icc-power.h"
struct ps5_hdmi_config {
    uint8_t raw[8];
    uint8_t converter; /* 2 FLAVA3, 3 VERDE, following Linux interpretation */
    uint8_t tmds_polarity;
    uint8_t tmds_swap;
    uint8_t dp_polarity;
    uint8_t dp_swap;
};
/* General service 2, request 0x16, selector 0x10. No mode-setting commands.
 * Caller must own ICC. Output unchanged on failure. -6: short reply. Fields 3/4/5 follow Linux equality-to-one
 * semantics; other values normalize to FLAVA3/normal while raw is retained. First three payload bytes remain uninterpreted and preserved in raw;
 * successful decoding is NOT proof of service status or working video.
 */
int ps5_hdmi_read_config(const struct ps5_icc_io *io,struct ps5_hdmi_config *out);
/* Use with ps5_edid_notification to retain EDID arriving during the query. */
int ps5_hdmi_read_config_events(const struct ps5_icc_io *io,struct ps5_hdmi_config *out,
                                 ps5_icc_notification callback,void *context);
#endif
