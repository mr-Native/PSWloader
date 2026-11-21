/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef PS5PWR_SENSORS_H
#define PS5PWR_SENSORS_H

#include <stdint.h>

#define PS5PWR_FAN_ZONE_MAINSOC 0u
#define PS5PWR_FAN_CURSET_COUNT 6u
#define PS5PWR_FAN_GET_BYTES 20u       /* ICC_MSG_MIN_SIZE - sizeof(struct icc_msg) */
#define PS5PWR_FAN_SET_BYTES 52u       /* 0x40 - sizeof(struct icc_msg) */
#define PS5PWR_FAN_REPLY_MIN_BYTES 28u /* six u32 curset words beginning at reply + 4 */
#define PS5PWR_FAN_REPLY_MAX_BYTES 128u /* accept firmware reply extensions; Linux reads only the first six words */

void ps5pwr_fan_get_request(uint8_t request[PS5PWR_FAN_GET_BYTES], uint8_t zone);
void ps5pwr_fan_set_request(uint8_t request[PS5PWR_FAN_SET_BYTES], uint8_t zone,
                            uint8_t config, const uint32_t curset[PS5PWR_FAN_CURSET_COUNT]);
int ps5pwr_fan_decode_curset(const uint8_t *reply, unsigned bytes,
                             uint32_t curset[PS5PWR_FAN_CURSET_COUNT]);
uint32_t ps5pwr_fan_target_c(const uint32_t curset[PS5PWR_FAN_CURSET_COUNT]);
void ps5pwr_fan_set_target_c(uint32_t curset[PS5PWR_FAN_CURSET_COUNT], uint32_t celsius);

/* Linux k10temp's Zen reported-temperature decoding (PS5 is family 17h model 48h). */
int ps5pwr_tctl_from_raw(uint32_t raw, int32_t *milli_celsius);

#endif
