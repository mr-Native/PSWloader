/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * ps5-fan-quiet.h v1.0 (24.11.2026, r5.26.2, user: the fan spun up at every Windows start) - the EMC MainSoC servo table
 * (ICC service 0x0a: type 0x0f snapshot, type 0x0e upload, 0x6ae bytes = four 0x160-byte blocks) and its quiet variant: the
 * hardware values PS5 Linux puts on the EMC as the fallback for when the host stops driving the fan (target 50 C, gentle
 * servo gains, zones 1-3 off, duty limits 103..1024). Values only, pure helpers; test_ps5_fan_quiet.py checks the result
 * byte for byte against ps5pwr's table (drivers/ps5pwr). The ICC transport stays with HdmiProbeDxe. */
#ifndef PS5_FAN_QUIET_H_
#define PS5_FAN_QUIET_H_
#define PS5_FQ_SIZE        0x6aeu
#define PS5_FQ_BLOCKS      4u
#define PS5_FQ_BLOCK       0x160u
#define PS5_FQ_RECORD      0x20u
#define PS5_FQ_TEMP_SERVICE 0x0bu
#define PS5_FQ_MODE_AUTO   1u
static const unsigned ps5_fq_fields[][2] = {   /* {offset in the block's servo record, value} */
  { 0, 50u * 256u }, { 4, 5243u }, { 8, 0u }, { 12, 0u }, { 16, 64u }, { 20, 16u },
  { 24, 0x0fffffffu }, { 28, 0x0fffffffu }, { 32, 0x0fffffffu }, { 36, 0x0fffffffu },
  { 40, 0u }, { 56, 70u * 256u }, { 60, 0u }, { 64, 2621u }, { 68, 0u },
};
#define PS5_FQ_FIELDS (sizeof(ps5_fq_fields) / sizeof(ps5_fq_fields[0]))
static inline unsigned ps5_fq_get16(const unsigned char *p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }
static inline void ps5_fq_put16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static inline unsigned ps5_fq_get32(const unsigned char *p) { return ps5_fq_get16(p) | ps5_fq_get16(p + 2) << 16; }
static inline void ps5_fq_put32(unsigned char *p, unsigned v) { ps5_fq_put16(p, v & 0xffffu); ps5_fq_put16(p + 2, v >> 16); }
/* 1 = the table already carries the quiet servo values (an earlier session left it), 0 = not */
static inline int ps5_fq_is_quiet(const unsigned char *t)
{
  for (unsigned b = 0; b < PS5_FQ_BLOCKS; b++)
    for (unsigned f = 0; f < PS5_FQ_FIELDS; f++)
      if (ps5_fq_get32(t + b * PS5_FQ_BLOCK + PS5_FQ_RECORD + ps5_fq_fields[f][0]) != ps5_fq_fields[f][1]) return 0;
  return 1;
}
static inline void ps5_fq_prepare(unsigned char *q, const unsigned char *orig)
{
  for (unsigned i = 0; i < PS5_FQ_SIZE; i++) q[i] = orig[i];
  for (unsigned b = 0; b < PS5_FQ_BLOCKS; b++) {
    unsigned char *h = q + b * PS5_FQ_BLOCK;
    for (unsigned z = 1; z < 4; z++) ps5_fq_put16(h + z * 4 + 2, 0);   /* zones 1-3 off */
    ps5_fq_put16(h, 1024);                                              /* duty limits */
    if (ps5_fq_get16(h + 2) < 103) ps5_fq_put16(h + 2, 103);
    ps5_fq_put16(h + 0x10, 1024);
    for (unsigned f = 0; f < PS5_FQ_FIELDS; f++) ps5_fq_put32(h + PS5_FQ_RECORD + ps5_fq_fields[f][0], ps5_fq_fields[f][1]);
  }
  ps5_fq_put16(q + 0x583, 0);
}
#endif
