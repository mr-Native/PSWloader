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

/* v1.20.0 (27.11.2026): preset-aware GameOS-quiet fan curves tuned from live GTA 5 (2219 MHz / 98% GPU) measurements.
 * Old v1.19.2 drove 67% duty at 77 C and 80% at 80 C even though standard targets 83 C and oc targets 86 C.
 * Each preset keeps whisper-quiet 10% up to 68 C, gentle GameOS-like 15..48% in the 72..80 C gaming band,
 * firm cooling from 84 C upward, and unchanged emergency maximum at 90 C (91 C in oc). */
static const struct ps5_fc_curve curve_efficiency = {
    { 55000, 68000, 72000, 76000, 80000, 84000, 87000, 89000 },
    { 26,    26,    26,    38,    61,    107,   173,   255 },   /* 10, 10, 10, 15, 24, 42, 68, 100 % */
    45000,
};
static const struct ps5_fc_curve curve_standard = {
    { 55000, 68000, 72000, 76000, 80000, 84000, 87000, 89000 },
    { 26,    26,    36,    56,    87,    117,   173,   255 },   /* 10, 10, 14, 22, 34, 46, 68, 100 % */
    45000,
};
static const struct ps5_fc_curve curve_oc = {
    { 55000, 68000, 72000, 76000, 80000, 84000, 87000, 90000 },
    { 26,    26,    41,    66,    97,    128,   184,   255 },   /* 10, 10, 16, 26, 38, 50, 72, 100 % */
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

void ps5_fc_preset_curve(struct ps5_fc_curve *c, unsigned preset, unsigned minimum_pwm)
{
    if (preset == PS5_FC_PRESET_EFFICIENCY) *c = curve_efficiency;
    else if (preset == PS5_FC_PRESET_OC) *c = curve_oc;
    else *c = curve_standard;
    if (minimum_pwm > 255) minimum_pwm = 255;
    for (int i = 0; i < PS5_FC_POINTS; i++)
        if (c->pwm[i] < minimum_pwm) c->pwm[i] = minimum_pwm;
}

void ps5_fc_board_curve(struct ps5_fc_curve *c, unsigned minimum_pwm)
{
    ps5_fc_preset_curve(c, PS5_FC_PRESET_STANDARD, minimum_pwm);
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
    /* v1.20.0: smoother acoustic ramp (+14 / -8 per 500 ms = ~5.5%/s up, ~3.1%/s down); emergency bypasses slew */
    if (wanted > previous && wanted - previous > 14) return previous + 14;
    if (wanted < previous && previous - wanted > 8) return previous - 8;
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
        for (unsigned f = 0; f < QUIET_FIELDS; f++) {
            uint32_t v = get32(record + quiet_servo[f].offset);
            /* v1.18.4: the firmware (r5.28.10) puts up the quiet table with the power preset's target (50..80 C, whole
             * degrees) instead of 50 C - still the quiet table, not GameOS' own (that comes from PswFanOriginal) */
            if (quiet_servo[f].offset == 0 ? ((v & 0xff) != 0 || v < 50 * 256 || v > 80 * 256) : v != quiet_servo[f].value) return 0;
        }
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

void ps5_fc_set_preset(struct ps5_fc_state *s, unsigned preset)
{
    if (preset > PS5_FC_PRESET_OC) preset = PS5_FC_PRESET_STANDARD;
    s->preset = preset;
    s->rise_now_mc = (preset == PS5_FC_PRESET_OC) ? PS5_FC_RISE_NOW_OC_MC : PS5_FC_RISE_NOW_MC;
    s->emergency_mc = (preset == PS5_FC_PRESET_OC) ? PS5_FC_EMERGENCY_OC_MC : PS5_FC_EMERGENCY_MC;
    ps5_fc_preset_curve(&s->curve, preset, s->min_pwm);
}

void ps5_fc_set_custom(struct ps5_fc_state *s, const uint8_t raw[PS5_FC_CUSTOM_BYTES])
{
    unsigned min_p = s->min_pwm > 255 ? 255 : (s->min_pwm < 26 ? 26 : s->min_pwm);
    struct ps5_fc_curve c;
    int prev_t = 40;
    unsigned prev_p = min_p;
    for (int i = 0; i < PS5_FC_POINTS; i++) {
        int tc = raw[i];
        if (tc < 40) tc = 40;
        if (tc > 89) tc = 89;
        if (tc < prev_t) tc = prev_t;
        prev_t = tc;
        c.temp[i] = tc * 1000;
        unsigned pct = raw[PS5_FC_POINTS + i];
        if (pct > 100) pct = 100;
        unsigned pwm = (pct * 255u + 50u) / 100u;
        if (pwm < prev_p) pwm = prev_p;
        if (pwm > 255) pwm = 255;
        prev_p = pwm;
        c.pwm[i] = pwm;
    }
    /* Hard thermal safety: last point always reaches 100% by <= 89 C, emergency at 90 C always active */
    c.pwm[PS5_FC_POINTS - 1] = 255;
    c.stop_temp = (c.temp[0] > 5000) ? c.temp[0] - 10000 : 35000;
    if (c.stop_temp > 45000) c.stop_temp = 45000;
    s->preset = PS5_FC_PRESET_CUSTOM;
    s->rise_now_mc = PS5_FC_RISE_NOW_MC;
    s->emergency_mc = PS5_FC_EMERGENCY_MC;
    s->curve = c;
}

void ps5_fc_init(struct ps5_fc_state *s, unsigned minimum_pwm, unsigned current_pwm)
{
    s->min_pwm = minimum_pwm;
    ps5_fc_set_preset(s, PS5_FC_PRESET_STANDARD);
    s->filtered_temp = 0; s->filtered_valid = 0; s->running = 0; s->emergency = 0; s->rise_steps = 0;
    s->requested_pwm = current_pwm;
}

int ps5_fc_step(struct ps5_fc_state *s, int temp, unsigned *pwm)
{
    if (!s->filtered_valid) { s->filtered_temp = temp; s->filtered_valid = 1; }
    else s->filtered_temp = (3 * s->filtered_temp + temp) / 4;
    if (temp >= s->emergency_mc) s->emergency = 1;
    else if (temp < s->emergency_mc - 5000) s->emergency = 0;
    if (s->emergency) { *pwm = 255; return PS5_FC_MODE_MAXIMUM; }
    unsigned p = ps5_fc_auto_pwm(&s->curve, temp, temp >= s->rise_now_mc ? temp : s->filtered_temp, &s->running);
    /* v1.19.2 / v1.20.0: up only after the curve asked for more for 10 s in a row; down at once; from rise_now_mc (84/85 C) at once */
    if (p > s->requested_pwm && temp < s->rise_now_mc) {
        if (s->rise_steps < PS5_FC_RISE_HOLD_STEPS) { s->rise_steps++; p = s->requested_pwm; }
    } else s->rise_steps = 0;
    *pwm = ps5_fc_slew(p, s->requested_pwm);
    return PS5_FC_MODE_MANUAL;
}
