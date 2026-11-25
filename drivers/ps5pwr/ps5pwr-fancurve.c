/* SPDX-License-Identifier: GPL-2.0-only */
/* ps5pwr v1.17: PS5 Linux drivers/ps5/fan.c control logic, same numbers and order (see ps5pwr-fancurve.h). */
#include "ps5pwr-fancurve.h"
#include "ps5-autoservo.h"

typedef char ps5_fc_size_check[sizeof(autoservo_param_sancarlo) == PS5_FC_SERVO_SIZE ? 1 : -1];

#define GDDR6_SAMSUNG 0x01
#define GDDR6_HYNIX   0x06
#define GDDR6_MICRON  0x0f
#define BOARD_MASK_A  0xffffffffffffff00ull
#define BOARD_MASK_B  0xffffffffff00ff00ull
#define BOARD_SANCARLO_1       0x2001010104010400ull
#define BOARD_SANCARLO_2       0x2001010104010500ull
#define BOARD_A_EIGER_1        0x3002010104010400ull
#define BOARD_A_EIGER_2        0x3002010104010500ull
#define BOARD_A_DENALI_1       0x3002010104030400ull
#define BOARD_A_DENALI_2       0x3002010104030500ull
#define BOARD_B_EIGER_DENALI_1 0x3002020101000400ull
#define BOARD_B_EIGER_DENALI_2 0x3002020101000500ull
#define BOARD_D_EIGER_DENALI_1 0x3002040101000400ull
#define BOARD_D_EIGER_DENALI_2 0x3002040101000500ull

static const struct ps5_fc_curve quiet_default = {
    { 50000, 60000, 65000, 70000, 75000, 80000, 85000, 87000 },
    { 26, 77, 102, 128, 166, 204, 242, 255 },
    45000,
};
static const struct { unsigned offset, value; } quiet_servo[] = {
    { 0, 50 * 256 }, { 4, 5243 }, { 8, 0 }, { 12, 0 }, { 16, 64 }, { 20, 16 },
    { 24, 0x0fffffff }, { 28, 0x0fffffff }, { 32, 0x0fffffff }, { 36, 0x0fffffff },
    { 40, 0 }, { 56, 70 * 256 }, { 60, 0 }, { 64, 2621 }, { 68, 0 },
};
#define QUIET_FIELDS (sizeof(quiet_servo) / sizeof(quiet_servo[0]))

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint32_t get32(const uint8_t *p) { return get16(p) | (uint32_t)get16(p + 2) << 16; }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }
static uint64_t be64(uint64_t v)
{
    uint64_t r = 0;
    for (int i = 0; i < 8; i++) { r = r << 8 | (v & 0xff); v >>= 8; }
    return r;
}

const uint8_t *ps5_fc_board_table(uint64_t board_id_raw, uint16_t gddr6_id, const char **name)
{
    uint64_t id = be64(board_id_raw), a = id & BOARD_MASK_A, b = id & BOARD_MASK_B;
    unsigned v = gddr6_id & 0xf, r = (gddr6_id >> 4) & 0xf;
    const char *n = "unknown board";
    const uint8_t *p = 0;
    if (a == BOARD_SANCARLO_1 || a == BOARD_SANCARLO_2) { n = "Sancarlo"; p = autoservo_param_sancarlo; }
    else if (a == BOARD_A_EIGER_1 || a == BOARD_A_EIGER_2) {
        if (v == GDDR6_SAMSUNG) { n = "A-Eiger Samsung"; p = autoservo_param_a_eiger_samsung; }
        else if (v == GDDR6_HYNIX) { n = "A-Eiger Hynix"; p = autoservo_param_a_eiger_hynix; }
        else if (v == GDDR6_MICRON) { n = "A-Eiger Micron"; p = autoservo_param_a_eiger_micron; }
        else n = "A-Eiger unknown memory";
    } else if (a == BOARD_A_DENALI_1 || a == BOARD_A_DENALI_2) {
        if (v == GDDR6_SAMSUNG) { n = "A-Denali Samsung"; p = autoservo_param_a_denali_samsung; }
        else if (v == GDDR6_HYNIX) { n = "A-Denali Hynix"; p = autoservo_param_a_denali_hynix; }
        else if (v == GDDR6_MICRON) { n = "A-Denali Micron"; p = autoservo_param_a_denali_micron; }
        else n = "A-Denali unknown memory";
    } else if (b == BOARD_B_EIGER_DENALI_1 || b == BOARD_B_EIGER_DENALI_2) {
        if (v == GDDR6_SAMSUNG && r == 8) { n = "B-Eiger/Denali Samsung D1x"; p = autoservo_param_b_eiger_denali_samsung_d1x; }
        else if (v == GDDR6_SAMSUNG && r == 9) n = "B-Eiger/Denali Samsung D1z";   /* PS5 Linux: no table */
        else if (v == GDDR6_HYNIX && r == 0) { n = "B-Eiger/Denali Hynix 1x"; p = autoservo_param_b_eiger_denali_hynix_1x_micron_120s; }
        else if (v == GDDR6_HYNIX && r == 1) { n = "B-Eiger/Denali Hynix 1y"; p = autoservo_param_b_eiger_denali_hynix_1y_micron_130s; }
        else if (v == GDDR6_MICRON && r == 0) { n = "B-Eiger/Denali Micron 120s"; p = autoservo_param_b_eiger_denali_hynix_1x_micron_120s; }
        else if (v == GDDR6_MICRON && r == 1) { n = "B-Eiger/Denali Micron 130s"; p = autoservo_param_b_eiger_denali_hynix_1y_micron_130s; }
        else n = "B-Eiger/Denali unknown memory";
    } else if (b == BOARD_D_EIGER_DENALI_1 || b == BOARD_D_EIGER_DENALI_2) {
        if (v == GDDR6_SAMSUNG && r == 9) { n = "D-Eiger/Denali Samsung D1z"; p = autoservo_param_d_eiger_denali_samsung_d1z; }
        else if (v == GDDR6_HYNIX && r == 1) { n = "D-Eiger/Denali Hynix 1y"; p = autoservo_param_d_eiger_denali_hynix_1y; }
        else if (v == GDDR6_MICRON && r == 1) { n = "D-Eiger/Denali Micron 130s"; p = autoservo_param_d_eiger_denali_micron_130s_y31j; }
        else if (v == GDDR6_MICRON && r == 2) { n = "D-Eiger/Denali Micron Y31J"; p = autoservo_param_d_eiger_denali_micron_130s_y31j; }
        else n = "D-Eiger/Denali unknown memory";
    }
    if (name) *name = n;
    return p;
}

void ps5_fc_board_curve(struct ps5_fc_curve *c, unsigned minimum_pwm)
{
    *c = quiet_default;
    if (minimum_pwm > 255) minimum_pwm = 255;
    for (int i = 0; i < PS5_FC_POINTS; i++)
        if (c->pwm[i] < minimum_pwm) c->pwm[i] = minimum_pwm;
}

unsigned ps5_fc_interpolate(const struct ps5_fc_curve *c, int temp)
{
    if (temp <= c->temp[0]) return c->pwm[0];
    for (int i = 1; i < PS5_FC_POINTS; i++) {
        if (temp <= c->temp[i]) {
            unsigned span = (unsigned)(c->temp[i] - c->temp[i - 1]);
            unsigned rise = c->pwm[i] - c->pwm[i - 1];
            return c->pwm[i - 1] + (rise * (unsigned)(temp - c->temp[i - 1]) + span / 2) / span;
        }
    }
    return 255;
}

unsigned ps5_fc_auto_pwm(const struct ps5_fc_curve *c, int raw_temp, int filtered_temp, int *running)
{
    if (raw_temp < c->stop_temp) *running = 0;
    else if (raw_temp >= c->temp[0]) *running = 1;
    if (!*running) return 0;
    return ps5_fc_interpolate(c, filtered_temp);
}

unsigned ps5_fc_slew(unsigned wanted, unsigned previous)
{
    if (wanted > previous && wanted - previous > 26) return previous + 26;
    if (wanted < previous && previous - wanted > 13) return previous - 13;
    return wanted;
}

unsigned ps5_fc_pwm_to_duty(unsigned pwm) { return (pwm * 1024 + 127) / 255; }
unsigned ps5_fc_duty_to_pwm(unsigned duty) { return (duty * 255 + 512) / 1024; }

void ps5_fc_prepare_backup(uint8_t backup[PS5_FC_SERVO_SIZE], const uint8_t original[PS5_FC_SERVO_SIZE])
{
    for (unsigned i = 0; i < PS5_FC_SERVO_SIZE; i++) backup[i] = original[i];
    for (int block = 0; block < 4; block++) {
        uint8_t *head = backup + block * 0x160, *record = head + 0x20;
        for (int zone = 1; zone < 4; zone++) put16(head + zone * 4 + 2, 0);
        put16(head, 1024);
        if (get16(head + 2) < 103) put16(head + 2, 103);
        put16(head + 0x10, 1024);
        for (unsigned f = 0; f < QUIET_FIELDS; f++) put32(record + quiet_servo[f].offset, quiet_servo[f].value);
    }
    put16(backup + 0x583, 0);
}

int ps5_fc_quiet_left(const uint8_t table[PS5_FC_SERVO_SIZE])
{
    for (int block = 0; block < 4; block++) {
        const uint8_t *record = table + block * 0x160 + 0x20;
        for (unsigned f = 0; f < QUIET_FIELDS; f++)
            if (get32(record + quiet_servo[f].offset) != quiet_servo[f].value) return 0;
    }
    return 1;
}

int ps5_fc_decode_temp(const uint8_t reply[4], int *milli)
{
    int raw = (int16_t)get16(reply + 2);
    if (raw < 0 || raw >= 125 * 256) return -1;
    *milli = (raw * 1000 + 128) / 256;      /* DIV_ROUND_CLOSEST for raw >= 0 */
    return 0;
}

void ps5_fc_init(struct ps5_fc_state *s, unsigned minimum_pwm, unsigned current_pwm)
{
    ps5_fc_board_curve(&s->curve, minimum_pwm);
    s->filtered_temp = 0; s->filtered_valid = 0; s->running = 0; s->emergency = 0;
    s->requested_pwm = current_pwm;
}

int ps5_fc_step(struct ps5_fc_state *s, int temp, unsigned *pwm)
{
    if (!s->filtered_valid) { s->filtered_temp = temp; s->filtered_valid = 1; }
    else s->filtered_temp = (3 * s->filtered_temp + temp) / 4;
    if (temp >= PS5_FC_EMERGENCY_MC) s->emergency = 1;
    else if (temp < PS5_FC_EMERGENCY_MC - 5000) s->emergency = 0;
    if (s->emergency) { *pwm = 255; return PS5_FC_MODE_MAXIMUM; }
    unsigned p = ps5_fc_auto_pwm(&s->curve, temp, temp >= 80000 ? temp : s->filtered_temp, &s->running);
    *pwm = ps5_fc_slew(p, s->requested_pwm);
    return PS5_FC_MODE_MANUAL;
}
