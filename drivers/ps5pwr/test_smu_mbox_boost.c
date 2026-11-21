/* test_smu_mbox_boost.c (16.11.2026) - host test for smu-mbox v0.4 boost window (+ v0.6 HOLD / above-stock request) with a simulated SMU. */
#include <stdio.h>
#include <string.h>
#include "smu-mbox.h"
static uint32_t reg[8]; static uint32_t writes[32][2]; static int nw, busy, answer = 1;
static int idx(uint32_t a) { switch (a) { case PS5_SMU_MSG: return 0; case PS5_SMU_ARG: return 1; case PS5_SMU_ARG2: return 2;
  case PS5_SMU_RESP: return 3; case PS5_SMU_C2PMSG_56: return 4; case PS5_SMU_C2PMSG_57: return 5; case PS5_SMU_C2PMSG_58: return 6;
  case PS5_SMU_C2PMSG_59: return 7; } return -1; }
static int rd(void *c, uint32_t a, uint32_t *v) { int i = idx(a); if (i < 0) return 1; *v = (i == 3 && busy) ? 0 : reg[i]; return 0; }
static int wr(void *c, uint32_t a, uint32_t v) { int i = idx(a); if (i < 0) return 1; writes[nw][0] = a; writes[nw++][1] = v; reg[i] = v;
  if (i == 0) { reg[3] = answer; if (v == PS5_SMU_MSG_GET_GFX_FREQ) reg[1] = 2075; } return 0; }
static void st(void *c, uint32_t us) {}
static const struct ps5_smn_ops o = { 0, rd, wr, st };
static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); fails++; } } while (0)
int main(void) {
  struct ps5_smu_state s = {0}; uint32_t resp, val;
  reg[3] = 1; nw = 0;
  CHECK(ps5_smu_boost_enter(&o, &s, &resp) == 0 && resp == 1);
  CHECK(nw == 8);
  CHECK(writes[0][0] == PS5_SMU_C2PMSG_56 && writes[0][1] == 0x449cd);
  CHECK(writes[1][0] == PS5_SMU_C2PMSG_57 && writes[1][1] == ((2000u << 16) | 3200u));
  CHECK(writes[2][0] == PS5_SMU_C2PMSG_58 && writes[2][1] == ((2230u << 16) | 3500u));
  CHECK(writes[3][0] == PS5_SMU_C2PMSG_59 && writes[3][1] == 0x25c6a3c8);
  CHECK(writes[4][0] == PS5_SMU_RESP && writes[4][1] == 0 && writes[5][0] == PS5_SMU_ARG && writes[5][1] == 0);
  CHECK(writes[6][0] == PS5_SMU_ARG2 && writes[7][0] == PS5_SMU_MSG && writes[7][1] == 0x22);
  nw = 0; CHECK(ps5_smu_send(&o, &s, PS5_SMU_MSG_UNIVERSAL_EXIT, 0, &resp, &val) == 0 && writes[3][1] == 0x23);
  nw = 0; CHECK(ps5_smu_send(&o, &s, PS5_SMU_MSG_GET_GFX_FREQ, 0, &resp, &val) == 0 && val == 2075);
  nw = 0; CHECK(ps5_smu_send(&o, &s, PS5_SMU_MSG_UNIVERSAL_EXIT, 5, &resp, &val) == -1 && nw == 0);     /* arg must be 0 */
  nw = 0; CHECK(ps5_smu_send(&o, &s, PS5_SMU_MSG_REQUEST_GFXCLK, 2231, &resp, &val) == -1 && nw == 0);  /* cap 2230 */
  nw = 0; CHECK(ps5_smu_send(&o, &s, 0x24, 3, &resp, &val) == -1 && nw == 0);                          /* SleepEntry not allowed */
  answer = 0xfe; nw = 0; CHECK(ps5_smu_boost_enter(&o, &s, &resp) == -5 && resp == 0xfe && !s.poisoned); answer = 1;
  busy = 1; nw = 0; CHECK(ps5_smu_boost_enter(&o, &s, &resp) == -3 && nw == 0 && s.poisoned);           /* busy: nothing written */
  busy = 0; nw = 0; CHECK(ps5_smu_boost_enter(&o, &s, &resp) == -2 && nw == 0);                         /* poisoned stays off */
  /* v0.6: HOLD variant writes only C2PMSG_57 differently */
  { struct ps5_smu_state h = {0}; reg[3] = 1; nw = 0;
    CHECK(ps5_smu_boost_enter_ex(&o, &h, 1, &resp) == 0 && nw == 8);
    CHECK(writes[0][1] == 0x449cd && writes[1][0] == PS5_SMU_C2PMSG_57 && writes[1][1] == ((2230u << 16) | 3200u));
    CHECK(writes[2][1] == ((2230u << 16) | 3500u) && writes[3][1] == 0x25c6a3c8 && writes[7][1] == 0x22);
    nw = 0; CHECK(ps5_smu_boost_enter_ex(&o, &h, 0, &resp) == 0 && writes[1][1] == ((2000u << 16) | 3200u));
  /* v0.6: above-stock request only through the OC path, 500..2400, readback query + freq */
    uint32_t q, f;
    nw = 0; CHECK(ps5_smu_send(&o, &h, PS5_SMU_MSG_REQUEST_GFXCLK, 2300, &resp, &val) == -1 && nw == 0);   /* normal path still 2230 */
    nw = 0; CHECK(ps5_smu_request_gfxclk_oc(&o, &h, 2401, &resp, &q, &f) == -1 && nw == 0 && q == 0 && f == 0);
    nw = 0; CHECK(ps5_smu_request_gfxclk_oc(&o, &h, 499, &resp, &q, &f) == -1 && nw == 0);
    nw = 0; reg[1] = 0; CHECK(ps5_smu_request_gfxclk_oc(&o, &h, 2400, &resp, &q, &f) == 0 && resp == 1);
    CHECK(nw == 12 && writes[1][0] == PS5_SMU_ARG && writes[1][1] == 2400 && writes[3][1] == PS5_SMU_MSG_REQUEST_GFXCLK);
    CHECK(writes[7][1] == PS5_SMU_MSG_QUERY_GFXCLK && writes[11][1] == PS5_SMU_MSG_GET_GFX_FREQ);
    CHECK(f == 2075);                                   /* simulated GetGfxFrequency */
    answer = 0xfe; nw = 0; CHECK(ps5_smu_request_gfxclk_oc(&o, &h, 2300, &resp, &q, &f) == -5 && resp == 0xfe && q == 0 && f == 0 && !h.poisoned);
    answer = 1; busy = 1; nw = 0; CHECK(ps5_smu_request_gfxclk_oc(&o, &h, 2300, &resp, &q, &f) == -3 && nw == 0 && h.poisoned);
    busy = 0; nw = 0; CHECK(ps5_smu_request_gfxclk_oc(&o, &h, 2300, &resp, &q, &f) == -2 && nw == 0);
    nw = 0; CHECK(ps5_smu_boost_enter_ex(&o, &h, 1, &resp) == -2 && nw == 0); }
  printf("%s (%d failures)\n", fails ? "FAILED" : "OK", fails);
  return fails != 0;
}
