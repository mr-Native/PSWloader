/* test_smu_mbox_oc.c (18.11.2026) - host test for smu-mbox v0.8 OC ops with a simulated SMU: exact registers, message ids,
 * VID conversion (never above the requested voltage), ranges refused before any access, queue 3 transport, composites.
 * build: gcc -Wall -Werror -o t test_smu_mbox_oc.c smu-mbox.c && ./t */
#include <stdio.h>
#include <string.h>
#include "smu-mbox.h"
static uint32_t reg[8]; static uint32_t writes[64][2]; static int nw, answer = 1, answer3c = 1;
static int idx(uint32_t a) { switch (a) { case PS5_SMU_MSG: return 0; case PS5_SMU_ARG: return 1; case PS5_SMU_ARG2: return 2;
  case PS5_SMU_RESP: return 3; case PS5_SMU_TEST_MSG: return 4; case PS5_SMU_TEST_ARG: return 5; case PS5_SMU_TEST_RESP: return 6; } return -1; }
static int rd(void *c, uint32_t a, uint32_t *v) { int i = idx(a); if (i < 0) return 1; *v = reg[i]; return 0; }
static int wr(void *c, uint32_t a, uint32_t v) { int i = idx(a); if (i < 0) return 1; writes[nw][0] = a; writes[nw++][1] = v; reg[i] = v;
  if (i == 0) { reg[3] = v == PS5_SMU_MSG_UNFORCE_GFX_VID ? answer3c : answer; if (v == PS5_SMU_MSG_GET_GFX_VID) reg[1] = 80; if (v == PS5_SMU_MSG_GET_FEATURES) reg[1] = 0x1234; }
  if (i == 4) { reg[6] = answer; if (v == PS5_SMU_Q3_GET_CORE_FREQ) reg[5] = 3500; if (v == PS5_SMU_Q3_GET_CPU_VOLTAGE) reg[5] = 1100; }
  return 0; }
static void st(void *c, uint32_t us) {}
static const struct ps5_smn_ops o = { 0, rd, wr, st };
static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); fails++; } } while (0)
int main(void) {
  struct ps5_smu_state s = {0}; uint32_t resp, val, arg, q, mv;
  reg[3] = reg[6] = 1;
  /* VID: rounded up = programmed voltage <= request, cap 1129 mV -> VID 68 = 1125 mV, floor 700 mV -> VID 136 */
  CHECK(PS5_SMU_VID_MIN == 68 && PS5_SMU_VID_MAX == 136);
  for (mv = 700; mv <= 1129; mv++) { uint32_t vid = PS5_SMU_MV_TO_VID(mv); CHECK(PS5_SMU_VID_TO_MV100(vid) <= mv * 100u); CHECK(PS5_SMU_VID_TO_MV100(vid) + 625u > mv * 100u); }
  CHECK(PS5_SMU_MV_TO_VID(1050) == 80 && PS5_SMU_VID_TO_MV100(80) == 105000);    /* exact steps map 1:1 */
  CHECK(PS5_SMU_MV_TO_VID(1000) == 88 && PS5_SMU_MV_TO_VID(950) == 96);
  /* decode: ranges, queue, argument */
  CHECK(ps5_smu_oc_decode(7, 0, &arg, &q) == 0x38 && q == 0 && arg == 0);
  CHECK(ps5_smu_oc_decode(8, 0, &arg, &q) == 0x3D);
  CHECK(ps5_smu_oc_decode(9, 2300, &arg, &q) == 0x39 && arg == 2300);
  CHECK(ps5_smu_oc_decode(9, 2601, &arg, &q) == 0 && arg == 0 && ps5_smu_oc_decode(9, 499, &arg, &q) == 0);
  CHECK(ps5_smu_oc_decode(11, 1050, &arg, &q) == 0x3B && arg == 80);
  CHECK(ps5_smu_oc_decode(11, 1130, &arg, &q) == 0 && ps5_smu_oc_decode(11, 1150, &arg, &q) == 0 && ps5_smu_oc_decode(11, 699, &arg, &q) == 0);
  CHECK(ps5_smu_oc_decode(10, 0, &arg, &q) == 0x3A && ps5_smu_oc_decode(12, 0, &arg, &q) == 0x3C && ps5_smu_oc_decode(13, 0, &arg, &q) == 0x3A);
  CHECK(ps5_smu_oc_decode(14, 0, &arg, &q) == 0x36 && q == 3);
  CHECK(ps5_smu_oc_decode(15, 7, &arg, &q) == 0x43 && arg == 7 && q == 3 && ps5_smu_oc_decode(15, 8, &arg, &q) == 0);
  CHECK(ps5_smu_oc_decode(16, 3850, &arg, &q) == 0x8F && arg == 3850 && ps5_smu_oc_decode(16, 4001, &arg, &q) == 0 && ps5_smu_oc_decode(16, 3199, &arg, &q) == 0);
  CHECK(ps5_smu_oc_decode(17, (uint32_t)-50, &arg, &q) == 0x50 && arg == 0xFFCE && ps5_smu_oc_decode(17, 0, &arg, &q) == 0x50 && arg == 0);
  CHECK(ps5_smu_oc_decode(17, 1, &arg, &q) == 0 && ps5_smu_oc_decode(17, (uint32_t)-51, &arg, &q) == 0);   /* no overvolting the curve */
  CHECK(ps5_smu_oc_decode(18, 85, &arg, &q) == 0x8B && ps5_smu_oc_decode(19, 70, &arg, &q) == 0x8C && ps5_smu_oc_decode(18, 96, &arg, &q) == 0 && ps5_smu_oc_decode(19, 100, &arg, &q) == 0);
  CHECK(ps5_smu_oc_decode(20, 0, &arg, &q) == 0x8F && arg == 3500 && q == 3);
  CHECK(ps5_smu_oc_decode(21, 0, &arg, &q) == 0);
  CHECK(ps5_smu_oc_decode(1, 2300, &arg, &q) == PS5_SMU_MSG_REQUEST_GFXCLK && q == 0);   /* old ops unchanged (range is the allow-list's) */
  /* allow-lists refuse without any access */
  nw = 0; CHECK(ps5_smu_send(&o, &s, 0x3B, 67, &resp, &val) == -1 && nw == 0);          /* VID 67 = 1131.25 mV > cap */
  nw = 0; CHECK(ps5_smu_send(&o, &s, 0x3B, 137, &resp, &val) == -1 && nw == 0);
  nw = 0; CHECK(ps5_smu_send(&o, &s, 0x39, 2700, &resp, &val) == -1 && nw == 0);
  nw = 0; CHECK(ps5_smu_send(&o, &s, 0x3A, 1, &resp, &val) == -1 && nw == 0);
  nw = 0; CHECK(ps5_smu_test_send(&o, &s, 0x8B, 100, &resp, &val) == -1 && nw == 0);    /* raising a limit: refused */
  nw = 0; CHECK(ps5_smu_test_send(&o, &s, 0x50, 1, &resp, &val) == -1 && nw == 0);
  nw = 0; CHECK(ps5_smu_test_send(&o, &s, 0x8F, 4100, &resp, &val) == -1 && nw == 0);
  nw = 0; CHECK(ps5_smu_test_send(&o, &s, 0x0F, 0, &resp, &val) == -1 && nw == 0);      /* queue-3 VID force: not allowed */
  nw = 0; CHECK(ps5_smu_test_send(&o, &s, 0x25, 0xFF0F00, &resp, &val) == -1 && nw == 0); /* per-core OC clock: not allowed */
  /* queue 0 sequence: RESP=0, ARG, ARG2=0, MSG */
  nw = 0; CHECK(ps5_smu_send(&o, &s, 0x3B, 80, &resp, &val) == 0 && nw == 4);
  CHECK(writes[0][0] == PS5_SMU_RESP && writes[1][0] == PS5_SMU_ARG && writes[1][1] == 80 && writes[2][0] == PS5_SMU_ARG2 && writes[3][0] == PS5_SMU_MSG && writes[3][1] == 0x3B);
  nw = 0; CHECK(ps5_smu_send(&o, &s, 0x38, 0, &resp, &val) == 0 && val == 80);
  nw = 0; CHECK(ps5_smu_send(&o, &s, 0x3D, 0, &resp, &val) == 0 && val == 0x1234);
  /* queue 3 sequence: RESP(96)=0, ARG(98), MSG(72) */
  nw = 0; CHECK(ps5_smu_test_send(&o, &s, 0x8F, 3850, &resp, &val) == 0 && nw == 3);
  CHECK(writes[0][0] == PS5_SMU_TEST_RESP && writes[1][0] == PS5_SMU_TEST_ARG && writes[1][1] == 3850 && writes[2][0] == PS5_SMU_TEST_MSG && writes[2][1] == 0x8F);
  nw = 0; CHECK(ps5_smu_test_send(&o, &s, 0x43, 2, &resp, &val) == 0 && val == 3500);
  nw = 0; CHECK(ps5_smu_test_send(&o, &s, 0x50, 0xFFCE, &resp, &val) == 0 && writes[1][1] == 0xFFCE);
  /* composites: GPU unforce = 0x3A then 0x3C, 0x3C refusal ignored; CPU reset = 0x8F 3500 then 0x50 0 */
  answer3c = 0xFE; nw = 0; CHECK(ps5_smu_gpu_unforce(&o, &s, &resp) == 0 && nw == 8 && writes[3][1] == 0x3A && writes[7][1] == 0x3C && !s.poisoned); answer3c = 1;
  answer = 0xFF; nw = 0; CHECK(ps5_smu_gpu_unforce(&o, &s, &resp) == -5 && nw == 8); answer = 1;   /* 0x3A refused: 0x3C still sent */
  nw = 0; CHECK(ps5_smu_cpu_oc_reset(&o, &s, &resp) == 0 && nw == 6 && writes[1][1] == 3500 && writes[2][1] == 0x8F && writes[4][1] == 0 && writes[5][1] == 0x50);
  /* old behaviour kept: WC CPU */
  nw = 0; CHECK(ps5_smu_test_send(&o, &s, PS5_SMU_TEST_SET_WC_CPU, 1, &resp, &val) == 0 && ps5_smu_test_send(&o, &s, PS5_SMU_TEST_SET_WC_CPU, 0, &resp, &val) == -1);
  printf("%s (%d failures)\n", fails ? "FAILED" : "OK", fails);
  return fails != 0;
}
