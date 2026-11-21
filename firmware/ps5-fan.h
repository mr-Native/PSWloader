/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * ps5-fan.h v1.0 (16.11.2026, r5.14, user: heat/fan) - MainSoC fan service of the EMC over ICC, exactly as
 * PS5 Linux drivers/ps5/fan.c and ps5pwr.sys v1.8.2 use it (service 0x0a):
 *   type 1  duty read      no payload        reply +2 = le16 duty 0..1024 (fan PWM share; there is no tachometer)
 *   type 7  curset GET     20 B, [0] = zone  reply +4 = six le32 curset words, word 0 = target temperature << 8
 *   type 6  curset SET     52 B, [0] = zone, [1] = config 0, +4 = six le32 words (all preserved except word 0)
 * Pure helpers, host-tested (test_ps5_fan.c). The ICC transport stays with the caller (HdmiProbeDxe). */
#ifndef PS5_FAN_H_
#define PS5_FAN_H_
#define PS5_FAN_SERVICE      0x0a
#define PS5_FAN_ZONE_MAINSOC 0
#define PS5_FAN_GET_BYTES    20u
#define PS5_FAN_SET_BYTES    52u
#define PS5_FAN_WORDS        6u
#define PS5_FAN_TARGET_MIN   50u    /* PS5 Linux quiet servo writes 50 << 8 */
#define PS5_FAN_TARGET_MAX   100u
static inline unsigned ps5_fan_le32(const unsigned char *p) { return (unsigned)p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24; }
static inline void ps5_fan_get_request(unsigned char r[PS5_FAN_GET_BYTES]) { for (unsigned i = 0; i < PS5_FAN_GET_BYTES; i++) r[i] = 0; r[0] = PS5_FAN_ZONE_MAINSOC; }
static inline int ps5_fan_decode(const unsigned char *reply, unsigned bytes, unsigned w[PS5_FAN_WORDS])
{
    if (!reply || bytes < 4u + 4u * PS5_FAN_WORDS) return -1;
    for (unsigned i = 0; i < PS5_FAN_WORDS; i++) w[i] = ps5_fan_le32(reply + 4 + 4 * i);
    return 0;
}
static inline void ps5_fan_set_request(unsigned char r[PS5_FAN_SET_BYTES], const unsigned w[PS5_FAN_WORDS], unsigned target_c)
{
    for (unsigned i = 0; i < PS5_FAN_SET_BYTES; i++) r[i] = 0;
    r[0] = PS5_FAN_ZONE_MAINSOC; r[1] = 0;
    for (unsigned i = 0; i < PS5_FAN_WORDS; i++) {
        unsigned v = i == 0 ? target_c << 8 : w[i];
        r[4 + 4 * i] = (unsigned char)v; r[5 + 4 * i] = (unsigned char)(v >> 8);
        r[6 + 4 * i] = (unsigned char)(v >> 16); r[7 + 4 * i] = (unsigned char)(v >> 24);
    }
}
static inline int ps5_fan_duty(const unsigned char *reply, unsigned bytes, unsigned *duty)
{
    if (!reply || bytes < 4u) return -1;
    *duty = (unsigned)reply[2] | (unsigned)reply[3] << 8;
    return *duty <= 1024u ? 0 : -1;
}
/* preset (Ps5PowerPreset 0 efficiency, 1 standard, 2 oc) -> target C. r5.23 (17.11.): 72 / 70 / 66 = the FanTargetC of
 * ThermalManager v1.6+ presets, so the fan is as quiet before Windows (and in Windows Setup, where no ThermalManager runs)
 * as in Windows; before: 65 / 60 / 50 (16.11.), loud on the setup stick. */
static inline unsigned ps5_fan_target_for_preset(unsigned preset) { return preset == 0 ? 72u : preset == 2 ? 66u : 70u; }
#endif
