/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PS5_DCN201_AUX_H
#define PS5_DCN201_AUX_H
#include "dcn201-timing.h"
/* Native DPCD access over DCN2.0.3 DP_AUXn SW engine, following Linux
 * dc/dce/dce_aux.c (acquire, submit_channel_request, read_channel_reply,
 * release). len 1..16. Returns bytes read/written >=0, or
 * -1 invalid, -2 mmio, -3 timeout, -5 arbitration, -6 HPD discon/RX error,
 * -7 NACK/DEFER (reply code in *reply). *status receives AUX_SW_STATUS. */
int ps5_dcn201_aux_dpcd(const struct ps5_dcn201_mmio *io,unsigned instance,
                        int write,uint32_t address,uint8_t *data,unsigned len,
                        uint32_t *status,uint8_t *reply);
#endif
