/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PS5_DCN201_SCANOUT_H
#define PS5_DCN201_SCANOUT_H
#include <stdint.h>
struct ps5_dcn201_state {
    uint32_t hubp_control,otg_control,surface_config,tiling_config,surface_control;
    uint32_t pitch,address_low,address_high,h_blank,v_blank;
};
struct ps5_linear_scanout {
    uint64_t surface_address,bytes;
    uint32_t width,height,pixels_per_line,bytes_per_line;
};
/* Decode a live register snapshot only. No GPU writes, no mode setting, no
 * assertion that the programmed surface address is CPU-accessible or that HDMI transmits pixels.
 * Output unchanged on failure. See display reference evidence for provenance.
 */
int ps5_dcn201_linear_scanout(const struct ps5_dcn201_state *,struct ps5_linear_scanout *);
#endif
