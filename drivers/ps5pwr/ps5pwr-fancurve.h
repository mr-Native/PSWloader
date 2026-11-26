/* SPDX-License-Identifier: GPL-2.0-only */
/* ps5pwr v1.17: the PS5 Linux fan control (drivers/ps5/fan.c) 1:1 - board servo table, quiet backup table and the
 * quiet curve driven every 500 ms. Pure logic only (no ICC, no Windows), shared with test_ps5pwr_fancurve.c. */
#ifndef PS5PWR_FANCURVE_H
#define PS5PWR_FANCURVE_H
#include <stdint.h>

#define PS5_FC_SERVO_SIZE   0x6ae
#define PS5_FC_POINTS       8
#define PS5_FC_EMERGENCY_MC 90000
#define PS5_FC_SERVICE      0x0a        /* ICC fan service */
#define PS5_FC_TEMP_SERVICE 0x0b        /* ICC temperature service */
enum { PS5_FC_MODE_AUTO = 1, PS5_FC_MODE_MAXIMUM = 2, PS5_FC_MODE_MINIMUM = 3, PS5_FC_MODE_MANUAL = 4 };

struct ps5_fc_curve { int temp[PS5_FC_POINTS]; unsigned pwm[PS5_FC_POINTS]; int stop_temp; };

/* Board servo table for the GameOS boot parameters at 0x9b000: board id (readq at +0x328, big endian on the
 * wire, pass the raw little-endian read) and GDDR6 id (readw at +0x1184). NULL = no table ("FanServo: Not update"),
 * *name gets the board/memory text either way. */
const uint8_t *ps5_fc_board_table(uint64_t board_id_raw, uint16_t gddr6_id, const char **name);

void ps5_fc_board_curve(struct ps5_fc_curve *c, unsigned minimum_pwm);
unsigned ps5_fc_interpolate(const struct ps5_fc_curve *c, int temp);
unsigned ps5_fc_auto_pwm(const struct ps5_fc_curve *c, int raw_temp, int filtered_temp, int *running);
unsigned ps5_fc_slew(unsigned wanted, unsigned previous);
unsigned ps5_fc_pwm_to_duty(unsigned pwm);
unsigned ps5_fc_duty_to_pwm(unsigned duty);
void ps5_fc_prepare_backup(uint8_t backup[PS5_FC_SERVO_SIZE], const uint8_t original[PS5_FC_SERVO_SIZE]);
int ps5_fc_quiet_left(const uint8_t table[PS5_FC_SERVO_SIZE]);
/* ICC temperature reply (+2 s16 / 256 C) -> milli-C; -1 when out of 0..125 C (PS5 Linux -ENODATA). */
int ps5_fc_decode_temp(const uint8_t reply[4], int *milli);

/* One control step of ps5_fan_update_locked() in automatic mode. requested_pwm is what the EMC last accepted;
 * the caller stores pwm there after a successful write. */
struct ps5_fc_state {
    struct ps5_fc_curve curve;
    int filtered_temp, filtered_valid, running, emergency;
    unsigned rise_steps;                /* v1.19.2: steps (500 ms) the curve has asked for more than requested_pwm */
    unsigned requested_pwm;
};
#define PS5_FC_RISE_HOLD_STEPS 20     /* v1.19.2: 10 s at 500 ms per step before the fan goes up (user 26.11.: no short spin-ups) */
#define PS5_FC_RISE_NOW_MC 80000      /* from 80 C the fan goes up at once */
void ps5_fc_init(struct ps5_fc_state *s, unsigned minimum_pwm, unsigned current_pwm);
/* Returns PS5_FC_MODE_MAXIMUM (emergency, pwm 255) or PS5_FC_MODE_MANUAL with *pwm to write. */
int ps5_fc_step(struct ps5_fc_state *s, int temp_milli, unsigned *pwm);

#endif
