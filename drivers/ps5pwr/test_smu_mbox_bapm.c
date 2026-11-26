/* test_smu_mbox_bapm.c (17.11.2026) - host test for smu-mbox v0.5 BAPM init (PS5 Linux mp1_init) with a simulated SMU:
 * exact register sequence, message ids, arguments from the fixed table, test mailbox, failure paths.
 * build: gcc -Wall -Werror -o t test_smu_mbox_bapm.c smu-mbox.c && ./t */
#include <stdio.h>
#include <string.h>
#include "smu-mbox.h"
#include "ps5-bapm-set2.h"

static uint32_t main_resp = 1, main_arg, ex81, test_resp = 1, test_arg, wc_mode = 0, busy_main, fail_msg = 0xffff;
static uint32_t log_msg[400], log_arg[400], log_ex[400], nlog, ntest, test_msgs[8], test_args[8], arg2_writes;
static int rd(void *c, uint32_t a, uint32_t *v)
{
    (void)c;
    if (a == PS5_SMU_RESP) { *v = busy_main ? 0 : main_resp; return 0; }
    if (a == PS5_SMU_ARG) { *v = main_arg; return 0; }
    if (a == PS5_SMU_TEST_RESP) { *v = test_resp; return 0; }
    if (a == PS5_SMU_TEST_ARG) { *v = test_arg; return 0; }
    return 1;
}
static int wr(void *c, uint32_t a, uint32_t v)
{
    (void)c;
    if (a == PS5_SMU_RESP) { main_resp = v; return 0; }
    if (a == PS5_SMU_ARG) { main_arg = v; return 0; }
    if (a == PS5_SMU_ARG2) { arg2_writes++; return 0; }
    if (a == PS5_SMU_C2PMSG_81) { ex81 = v; return 0; }
    if (a == PS5_SMU_MSG) {
        log_msg[nlog] = v; log_arg[nlog] = main_arg; log_ex[nlog] = (v == 0x2f || v == 0x30 || v == 0x33) ? ex81 : 0; nlog++;
        main_resp = (v == fail_msg) ? 0xfe : 1;
        return 0;
    }
    if (a == PS5_SMU_TEST_RESP) { test_resp = v; return 0; }
    if (a == PS5_SMU_TEST_ARG) { test_arg = v; return 0; }
    if (a == PS5_SMU_TEST_MSG) {
        test_msgs[ntest] = v; test_args[ntest] = test_arg; ntest++;
        if (v == PS5_SMU_TEST_SET_WC_CPU) wc_mode = test_arg;
        if (v == PS5_SMU_TEST_GET_WC_CPU) test_arg = wc_mode;
        test_resp = 1;
        return 0;
    }
    return 1;
}
static void st(void *c, uint32_t us) { (void)c; (void)us; }
static const struct ps5_smn_ops o = { 0, rd, wr, st };
static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); fails++; } } while (0)

int main(void)
{
    struct ps5_smu_state s; struct ps5_bapm_report r; uint32_t i, k, active = 0;
    for (i = 0; i < 176; i++) active += ps5_bapm_set2.bapm_param[i][0] != 0;

    /* full run from wc mode 0: exit, get, set(1), get, then 80 + 64 + active weights */
    memset(&s, 0, sizeof s); memset(&r, 0, sizeof r); nlog = ntest = 0; wc_mode = 0;
    CHECK(ps5_smu_bapm_prepare(&o, &s, &r) == 0);
    CHECK(nlog == 1 && log_msg[0] == 0x23 && log_arg[0] == 0);
    CHECK(ntest == 3 && test_msgs[0] == 0x9c && test_msgs[1] == 0x9b && test_args[1] == 1 && test_msgs[2] == 0x9c);
    CHECK(r.wc_before == 0 && r.wc_after == 1);
    nlog = 0;
    CHECK(ps5_smu_bapm_weights(&o, &s, &r) == 0 && r.status == 0);
    CHECK(r.l3_sent == 80 && r.gfx_sent == 64 && r.param_sent == active && nlog == 80 + 64 + active);
    for (i = 0; i < 80; i++) CHECK(log_msg[i] == 0x30 && log_arg[i] == (i | 0x20000u) && log_ex[i] == ps5_bapm_set2.l3_cacw[i]);
    for (i = 0; i < 64; i++) CHECK(log_msg[80 + i] == 0x2f && log_arg[80 + i] == (i | 0x20000u) && log_ex[80 + i] == ps5_bapm_set2.gfx_cacw[i]);
    for (i = 0, k = 144; i < 176; i++) {
        if (!ps5_bapm_set2.bapm_param[i][0]) continue;
        CHECK(log_msg[k] == 0x33 && log_arg[k] == i && log_ex[k] == ps5_bapm_set2.bapm_param[i][1]); k++;
    }
    CHECK(log_ex[71] == 0x3ff80000u);                                   /* the one non-zero L3 weight */

    /* wc already 1: no set */
    memset(&s, 0, sizeof s); memset(&r, 0, sizeof r); nlog = ntest = 0; wc_mode = 1;
    CHECK(ps5_smu_bapm_prepare(&o, &s, &r) == 0 && ntest == 1 && r.wc_before == 1 && r.wc_after == 1);

    /* SMU refuses a weight: stop there, report step/index, mailbox stays usable */
    memset(&s, 0, sizeof s); memset(&r, 0, sizeof r); nlog = 0; fail_msg = 0x2f;
    CHECK(ps5_smu_bapm_weights(&o, &s, &r) == -5 && r.step == 5 && r.index == 0 && r.resp == 0xfe && r.l3_sent == 80 && !s.poisoned);
    fail_msg = 0xffff;

    /* busy mailbox: poisoned, nothing written after */
    memset(&s, 0, sizeof s); memset(&r, 0, sizeof r); nlog = ntest = 0; busy_main = 1;
    CHECK(ps5_smu_bapm_prepare(&o, &s, &r) == -3 && r.step == 1 && s.poisoned && nlog == 0 && ntest == 0);
    busy_main = 0;
    CHECK(ps5_smu_bapm_weights(&o, &s, &r) == -2 && nlog == 0);

    /* test mailbox allow-list */
    memset(&s, 0, sizeof s); ntest = 0;
    CHECK(ps5_smu_test_send(&o, &s, 0x9b, 2, 0, 0) == -1 && ntest == 0);
    CHECK(ps5_smu_test_send(&o, &s, 0x75, 0, 0, 0) == -1 && ntest == 0);
    CHECK(ps5_smu_test_send(&o, &s, 0x9c, 1, 0, 0) == -1 && ntest == 0);
    /* table sanity against PS5 Linux */
    CHECK(ps5_bapm_dpm_wac()[0] == 0x0000c3adb08d0000ull && ps5_bapm_dpm_wac()[20] == 0xa700000000000000ull);
    printf("%s (%d failures, %u active params)\n", fails ? "FAILED" : "OK", fails, active);
    return fails != 0;
}
