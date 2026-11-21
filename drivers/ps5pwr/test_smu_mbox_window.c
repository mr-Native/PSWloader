/* test_smu_mbox_window.c (18.11.2026) - host test for smu-mbox v0.9 op 21 (boost window with a raised ceiling P3):
 * exact C2PMSG_56..59 values, range refusal before any access, HOLD/normal paths unchanged.
 * build: gcc -Wall -Werror -fsanitize=address,undefined -o t test_smu_mbox_window.c smu-mbox.c && ./t */
#include <stdio.h>
#include "smu-mbox.h"
static uint32_t r56, r57, r58, r59, rmsg, rresp = 1; static int nw;
static int rd(void *c, uint32_t a, uint32_t *v) { (void)c; *v = a == PS5_SMU_RESP ? rresp : 0; return 0; }
static int wr(void *c, uint32_t a, uint32_t v) { (void)c; nw++;
  if (a == PS5_SMU_C2PMSG_56) r56 = v; else if (a == PS5_SMU_C2PMSG_57) r57 = v; else if (a == PS5_SMU_C2PMSG_58) r58 = v;
  else if (a == PS5_SMU_C2PMSG_59) r59 = v; else if (a == PS5_SMU_MSG) { rmsg = v; rresp = 1; } else if (a == PS5_SMU_RESP) rresp = v;
  return 0; }
static void st(void *c, uint32_t us) { (void)c; (void)us; }
static const struct ps5_smn_ops o = { 0, rd, wr, st };
static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); fails++; } } while (0)
int main(void) {
  struct ps5_smu_state s = {0}; uint32_t resp, arg, q;
  uint32_t p3 = (2300u << 16) | 3600u;
  CHECK(ps5_smu_oc_decode(21, p3, &arg, &q) == PS5_SMU_MSG_UNIVERSAL_ENTRY && arg == p3 && q == 0);
  CHECK(ps5_smu_oc_decode(21, (2229u << 16) | 3600u, &arg, &q) == 0);
  CHECK(ps5_smu_oc_decode(21, (2401u << 16) | 3600u, &arg, &q) == 0);
  CHECK(ps5_smu_oc_decode(21, (2300u << 16) | 3499u, &arg, &q) == 0);
  CHECK(ps5_smu_oc_decode(21, (2300u << 16) | 3801u, &arg, &q) == 0);
  CHECK(ps5_smu_oc_decode(21, 0, &arg, &q) == 0);
  nw = 0; CHECK(ps5_smu_boost_enter_p3(&o, &s, (2500u << 16) | 3600u, &resp) == -1 && nw == 0);
  CHECK(ps5_smu_boost_enter_p3(&o, &s, p3, &resp) == 0 && resp == 1);
  CHECK(r56 == PS5_SMU_BOOST_P1 && r57 == PS5_SMU_BOOST_P2 && r58 == p3 && r59 == PS5_SMU_BOOST_P4 && rmsg == PS5_SMU_MSG_UNIVERSAL_ENTRY);
  CHECK(ps5_smu_boost_enter_ex(&o, &s, 1, &resp) == 0 && r57 == PS5_SMU_BOOST_P2_HOLD && r58 == PS5_SMU_BOOST_P3);
  CHECK(ps5_smu_boost_enter(&o, &s, &resp) == 0 && r57 == PS5_SMU_BOOST_P2 && r58 == PS5_SMU_BOOST_P3);
  CHECK(ps5_smu_boost_enter_p3(&o, &s, (2230u << 16) | 3500u, &resp) == 0 && r58 == PS5_SMU_BOOST_P3);   /* lower edge == stock */
  /* v0.9 GFX CAC scale: signed 16-bit halves, rounded to nearest, 100 = identity, range refused before any access */
  { struct ps5_bapm_report br = {0}; uint32_t w, pct;
    for (pct = 85; pct <= 100; pct++) for (w = 0; w < 0x40000u; w += 0x101u) {   /* low half sweep + high half 0..3 */
      uint32_t x = ps5_bapm_scale_gfx(w * 0x9e37u, pct), in = w * 0x9e37u; int h;
      for (h = 0; h < 2; h++) { int32_t a = (int16_t)(in >> (16 * h)), b = (int16_t)(x >> (16 * h)); double e = (double)a * pct / 100.0;
        CHECK(b - e <= 0.5 && e - b <= 0.5); if (a >= 0) CHECK(b >= 0 && b <= a); else CHECK(b <= 0 && b >= a); } }
    CHECK(ps5_bapm_scale_gfx(0x1085ffffu, 100) == 0x1085ffffu && ps5_bapm_scale_gfx(0x1085ffffu, 90) == 0x0edeffffu);
    CHECK(ps5_bapm_scale_gfx(0xefau, 90) == 0xd7bu && ps5_bapm_scale_gfx(0, 85) == 0);
    nw = 0; CHECK(ps5_smu_bapm_weights_scaled(&o, &s, &br, 84) == -1 && nw == 0 && br.step == 5);
    nw = 0; CHECK(ps5_smu_bapm_weights_scaled(&o, &s, &br, 101) == -1 && nw == 0); }
  printf(fails ? "%d FAILED\n" : "window: all ok\n", fails);
  return fails != 0;
}
