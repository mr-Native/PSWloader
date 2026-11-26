/* rtc-emu.h v1.0 (16.11.2026) - MC146818 register view of the current local time, for ps5cmos.sys.
 * The PS5 has no legacy RTC/CMOS (FADT: CMOS absent); PS5 Linux sets x86_platform.legacy.rtc = 0. The CPUID SDK used by
 * 3DMark SystemInfo still polls RTC Status A for the update-in-progress edge and never stops. This header renders a
 * truthful RTC from the OS clock: BCD, 24 h (Status B = 0x02), Status A = 0x26 with UIP set during the last 244 us of
 * every second, Status C = 0, Status D = 0x80 (valid RAM/time), century at 0x32, all other CMOS bytes 0. Writes are never
 * applied. Portable C (host-tested). */
#ifndef RTC_EMU_H
#define RTC_EMU_H
#include <stdint.h>
struct rtc_time { unsigned year, month, day, hour, minute, second, weekday; /* weekday 0 = Sunday */
                  uint32_t subsec_100ns; /* 0..9999999 within the current second */ };
#define RTC_UIP_WINDOW_100NS 2440u    /* 244 us */
static inline uint8_t rtc_bcd(unsigned v) { return (uint8_t)(((v / 10) % 10) << 4 | (v % 10)); }
static inline uint8_t rtc_emu_reg(const struct rtc_time *t, unsigned index)
{
    switch (index & 0x7f) {
    case 0x00: return rtc_bcd(t->second);
    case 0x02: return rtc_bcd(t->minute);
    case 0x04: return rtc_bcd(t->hour);
    case 0x06: return rtc_bcd(t->weekday + 1);          /* 1 = Sunday */
    case 0x07: return rtc_bcd(t->day);
    case 0x08: return rtc_bcd(t->month);
    case 0x09: return rtc_bcd(t->year % 100);
    case 0x0a: return (uint8_t)(0x26 | (t->subsec_100ns >= 10000000u - RTC_UIP_WINDOW_100NS ? 0x80 : 0));
    case 0x0b: return 0x02;                              /* 24 h, BCD, no interrupts */
    case 0x0c: return 0x00;
    case 0x0d: return 0x80;                              /* VRT: time valid */
    case 0x32: return rtc_bcd(t->year / 100);            /* century */
    default:   return 0x00;                              /* alarms and CMOS RAM: zero */
    }
}
/* Which accesses the filter takes over. Ports 0x70/0x71 only (0x72/0x73 extended CMOS also have no device: same view). */
static inline int rtc_emu_port(uint32_t port) { return port >= 0x70 && port <= 0x73; }
#endif
