/* Host-side protocol checks for ps5pwr-sensors.c. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "ps5pwr-sensors.h"

static void put_le32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16); p[3] = (uint8_t)(value >> 24);
}

int main(void)
{
    uint8_t get[PS5PWR_FAN_GET_BYTES], set[PS5PWR_FAN_SET_BYTES], reply[28] = {0};
    uint32_t curset[PS5PWR_FAN_CURSET_COUNT] = {0x5000, 0x11223344, 0x55667788, 4, 5, 6};
    uint32_t decoded[PS5PWR_FAN_CURSET_COUNT] = {0};
    int32_t temp = 0;

    ps5pwr_fan_get_request(get, 0);
    assert(sizeof(get) == 20 && get[0] == 0);
    for (unsigned i = 1; i < sizeof(get); i++) assert(get[i] == 0);

    ps5pwr_fan_set_request(set, 0, 0, curset);
    assert(sizeof(set) == 52 && set[0] == 0 && set[1] == 0 && set[2] == 0 && set[3] == 0);
    assert(set[4] == 0x00 && set[5] == 0x50 && set[6] == 0 && set[7] == 0);
    assert(set[8] == 0x44 && set[9] == 0x33 && set[10] == 0x22 && set[11] == 0x11);
    for (unsigned i = 28; i < sizeof(set); i++) assert(set[i] == 0);

    for (unsigned i = 0; i < PS5PWR_FAN_CURSET_COUNT; i++) put_le32(reply + 4 + 4 * i, curset[i]);
    assert(ps5pwr_fan_decode_curset(reply, 27, decoded) != 0);
    assert(ps5pwr_fan_decode_curset(reply, sizeof(reply), decoded) == 0);
    assert(memcmp(decoded, curset, sizeof(curset)) == 0);
    assert(ps5pwr_fan_target_c(decoded) == 80);
    ps5pwr_fan_set_target_c(decoded, 75);
    assert(ps5pwr_fan_target_c(decoded) == 75 && decoded[1] == curset[1] && decoded[5] == curset[5]);

    assert(ps5pwr_tctl_from_raw(680u << 21, &temp) == 0 && temp == 85000);
    assert(ps5pwr_tctl_from_raw((600u << 21) | (1u << 19), &temp) == 0 && temp == 26000);
    assert(ps5pwr_tctl_from_raw(680u << 21, NULL) != 0);
    puts("ps5pwr sensor protocol checks passed");
    return 0;
}
