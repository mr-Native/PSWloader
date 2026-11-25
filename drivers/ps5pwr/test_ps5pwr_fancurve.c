/* SPDX-License-Identifier: GPL-2.0-only */
/* Host tests for ps5pwr-fancurve.c (values worked out from PS5 Linux drivers/ps5/fan.c). */
#include <stdio.h>
#include <string.h>
#include "ps5pwr-fancurve.h"
#include "ps5-autoservo.h"
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
static int same(const uint8_t *a, const uint8_t *b) { return a && b && !memcmp(a, b, PS5_FC_SERVO_SIZE); }
static uint64_t le_of_be(uint64_t id) { uint64_t r = 0; for (int i = 0; i < 8; i++) { r = r << 8 | (id & 0xff); id >>= 8; } return r; }
int main(void)
{
    struct ps5_fc_curve c; ps5_fc_board_curve(&c, 0);
    CHECK(ps5_fc_interpolate(&c, 40000) == 26);
    CHECK(ps5_fc_interpolate(&c, 50000) == 26);
    CHECK(ps5_fc_interpolate(&c, 55000) == 52);
    CHECK(ps5_fc_interpolate(&c, 60000) == 77);
    CHECK(ps5_fc_interpolate(&c, 72500) == 147);
    CHECK(ps5_fc_interpolate(&c, 86000) == 249);
    CHECK(ps5_fc_interpolate(&c, 87000) == 255);
    CHECK(ps5_fc_interpolate(&c, 95000) == 255);
    ps5_fc_board_curve(&c, 40);   /* board minimum from the servo table raises the low points */
    CHECK(c.pwm[0] == 40 && c.pwm[1] == 77);
    ps5_fc_board_curve(&c, 0);
    int run = 0;
    CHECK(ps5_fc_auto_pwm(&c, 44000, 44000, &run) == 0 && !run);
    CHECK(ps5_fc_auto_pwm(&c, 47000, 47000, &run) == 0 && !run);     /* hysteresis: still stopped */
    CHECK(ps5_fc_auto_pwm(&c, 50000, 50000, &run) == 26 && run);
    CHECK(ps5_fc_auto_pwm(&c, 47000, 47000, &run) == 26 && run);     /* still running above 45 C */
    CHECK(ps5_fc_auto_pwm(&c, 44999, 44999, &run) == 0 && !run);
    CHECK(ps5_fc_slew(255, 100) == 126 && ps5_fc_slew(0, 100) == 87 && ps5_fc_slew(110, 100) == 110 && ps5_fc_slew(90, 100) == 90);
    CHECK(ps5_fc_pwm_to_duty(255) == 1024 && ps5_fc_pwm_to_duty(26) == 104 && ps5_fc_pwm_to_duty(0) == 0);
    CHECK(ps5_fc_duty_to_pwm(1024) == 255 && ps5_fc_duty_to_pwm(103) == 26 && ps5_fc_duty_to_pwm(0) == 0);
    uint8_t r[4] = { 0, 0, 0x80, 0x3c };          /* 60.5 C */
    int m = 0; CHECK(ps5_fc_decode_temp(r, &m) == 0 && m == 60500);
    r[3] = 0x7d; r[2] = 0; CHECK(ps5_fc_decode_temp(r, &m) == -1);     /* 125 C invalid */
    r[3] = 0xff; CHECK(ps5_fc_decode_temp(r, &m) == -1);               /* negative */
    /* control steps */
    struct ps5_fc_state s; unsigned pwm;
    ps5_fc_init(&s, 0, 100);
    CHECK(ps5_fc_step(&s, 70000, &pwm) == PS5_FC_MODE_MANUAL && pwm == 126);   /* 128 wanted, up by 26 at most */
    s.requested_pwm = pwm;
    CHECK(ps5_fc_step(&s, 70000, &pwm) == PS5_FC_MODE_MANUAL && pwm == 128);
    s.requested_pwm = pwm;
    CHECK(ps5_fc_step(&s, 90000, &pwm) == PS5_FC_MODE_MAXIMUM && pwm == 255 && s.emergency);
    s.requested_pwm = 255;
    CHECK(ps5_fc_step(&s, 86000, &pwm) == PS5_FC_MODE_MAXIMUM);                 /* emergency until < 85 C */
    CHECK(ps5_fc_step(&s, 84000, &pwm) == PS5_FC_MODE_MANUAL && !s.emergency && pwm == 242);  /* raw >= 80 C: 236 wanted, -13 max */
    /* boards */
    const char *n;
    CHECK(same(ps5_fc_board_table(le_of_be(0x3002010104010401ull), 0x01, &n), autoservo_param_a_eiger_samsung));
    CHECK(same(ps5_fc_board_table(le_of_be(0x3002010104030500ull), 0x0f, &n), autoservo_param_a_denali_micron));
    CHECK(same(ps5_fc_board_table(le_of_be(0x2001010104010400ull), 0x00, &n), autoservo_param_sancarlo));
    CHECK(same(ps5_fc_board_table(le_of_be(0x3002020101aa04bbull), 0x81, &n), autoservo_param_b_eiger_denali_samsung_d1x));
    CHECK(ps5_fc_board_table(le_of_be(0x3002020101000400ull), 0x91, &n) == NULL && !strcmp(n, "B-Eiger/Denali Samsung D1z"));
    CHECK(same(ps5_fc_board_table(le_of_be(0x3002040101000500ull), 0x2f, &n), autoservo_param_d_eiger_denali_micron_130s_y31j));
    CHECK(ps5_fc_board_table(le_of_be(0x3002010104010400ull), 0x05, &n) == NULL);
    CHECK(ps5_fc_board_table(0, 0, &n) == NULL && !strcmp(n, "unknown board"));
    /* backup table */
    uint8_t b[PS5_FC_SERVO_SIZE];
    CHECK(!ps5_fc_quiet_left(autoservo_param_a_eiger_samsung));
    ps5_fc_prepare_backup(b, autoservo_param_a_eiger_samsung);
    CHECK(ps5_fc_quiet_left(b));
    CHECK(b[0] == 0x00 && b[1] == 0x04 && b[0x10] == 0x00 && b[0x11] == 0x04 && b[0x583] == 0 && b[0x584] == 0);
    CHECK(b[0x20] == 0 && b[0x21] == 50);          /* 50 C target, <<8 */
    printf(fails ? "%d FAILED\n" : "all fancurve tests passed\n", fails);
    return fails != 0;
}
