/* SPDX-License-Identifier: GPL-2.0-only */
#include "ps5pwr-sensors.h"

static void put_le32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void ps5pwr_fan_get_request(uint8_t request[PS5PWR_FAN_GET_BYTES], uint8_t zone)
{
    for (unsigned i = 0; i < PS5PWR_FAN_GET_BYTES; i++) request[i] = 0;
    request[0] = zone;
}

void ps5pwr_fan_set_request(uint8_t request[PS5PWR_FAN_SET_BYTES], uint8_t zone,
                            uint8_t config, const uint32_t curset[PS5PWR_FAN_CURSET_COUNT])
{
    for (unsigned i = 0; i < PS5PWR_FAN_SET_BYTES; i++) request[i] = 0;
    request[0] = zone;
    request[1] = config;
    for (unsigned i = 0; i < PS5PWR_FAN_CURSET_COUNT; i++)
        put_le32(request + 4 + i * 4, curset[i]);
}

int ps5pwr_fan_decode_curset(const uint8_t *reply, unsigned bytes,
                             uint32_t curset[PS5PWR_FAN_CURSET_COUNT])
{
    if (!reply || !curset || bytes < PS5PWR_FAN_REPLY_MIN_BYTES) return -1;
    for (unsigned i = 0; i < PS5PWR_FAN_CURSET_COUNT; i++)
        curset[i] = get_le32(reply + 4 + i * 4);
    return 0;
}

uint32_t ps5pwr_fan_target_c(const uint32_t curset[PS5PWR_FAN_CURSET_COUNT])
{
    return curset ? curset[0] >> 8 : 0;
}

void ps5pwr_fan_set_target_c(uint32_t curset[PS5PWR_FAN_CURSET_COUNT], uint32_t celsius)
{
    if (curset) curset[0] = celsius << 8;
}

int ps5pwr_tctl_from_raw(uint32_t raw, int32_t *milli_celsius)
{
    int32_t value;
    if (!milli_celsius) return -1;
    value = (int32_t)((raw >> 21) * 125u);
    if ((raw & (1u << 19)) || ((raw & (3u << 16)) == (3u << 16)))
        value -= 49000;
    *milli_celsius = value;
    return 0;
}
