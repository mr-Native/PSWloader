/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * One-button UEFI input from the PS5 power button via ICC service 0x08
 * (PS5 Linux spcie.c: 0x8010 down, 0x8011 up). Short press (<2 s) = Down,
 * long press (2..8 s) = Enter. r5.2: two short presses within 400 ms = Esc (back); a single short press is
 * reported as Down once that window has passed (ps5_button_tick from the poller). Pure state machine, host-testable. */
#ifndef PS5_BUTTON_KEYS_H
#define PS5_BUTTON_KEYS_H
#include "icc-power.h" /* shared stdint/EDK2 type mapping */
#define PS5_BUTTON_SERVICE 0x08
#define PS5_KEY_DOWN 1
#define PS5_KEY_ENTER 2
#define PS5_KEY_ESC 3
struct ps5_button_keys {uint64_t down_at,hz;int pressed;unsigned head,tail;uint8_t queue[16];unsigned dropped;
  int short_pending;uint64_t short_up_at;};   /* r5.2: double-press window */
void ps5_button_init(struct ps5_button_keys *s,uint64_t hz);
/* Feed an ICC notification; now = timestamp in hz units. Returns key queued or 0. */
int ps5_button_event(struct ps5_button_keys *s,uint8_t service,uint16_t type,uint64_t now);
/* r5.2: release a pending single short press as Down once the double-press window is over; call periodically. */
int ps5_button_tick(struct ps5_button_keys *s,uint64_t now);
/* Pop one key, 0 if empty. */
int ps5_button_pop(struct ps5_button_keys *s);
int ps5_button_pending(const struct ps5_button_keys *s);
#endif
