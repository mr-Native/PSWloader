/* smu-mbox v0.7 - see smu-mbox.h */
#include "smu-mbox.h"
#include "ps5-bapm-set2.h"
static int send_raw(const struct ps5_smn_ops *o, struct ps5_smu_state *s, uint32_t msg, uint32_t arg, uint32_t *resp, uint32_t *value);

int ps5_smu_allowed(uint32_t msg, uint32_t arg)
{
    switch (msg) {
    case PS5_SMU_MSG_GET_VERSION:
    case PS5_SMU_MSG_GET_IF_VERSION:
    case PS5_SMU_MSG_QUERY_GFXCLK:
    case PS5_SMU_MSG_UNIVERSAL_ENTRY:
    case PS5_SMU_MSG_UNIVERSAL_EXIT:
    case PS5_SMU_MSG_GET_GFX_FREQ:
        return arg == 0;
    case PS5_SMU_MSG_REQUEST_GFXCLK:
        return arg >= PS5_SMU_GFX_MIN_MHZ && arg <= PS5_SMU_GFX_MAX_MHZ;
    case PS5_SMU_MSG_GET_GFX_VID:                         /* v0.8 */
    case PS5_SMU_MSG_UNFORCE_GFX_FREQ:
    case PS5_SMU_MSG_UNFORCE_GFX_VID:
    case PS5_SMU_MSG_GET_FEATURES:
        return arg == 0;
    case PS5_SMU_MSG_FORCE_GFX_FREQ:
        return arg >= PS5_SMU_FORCE_MIN_MHZ && arg <= PS5_SMU_FORCE_MAX_MHZ;
    case PS5_SMU_MSG_FORCE_GFX_VID:                       /* VID 68..136 = 1125..700 mV: never above the cap */
        return arg >= PS5_SMU_VID_MIN && arg <= PS5_SMU_VID_MAX;
    default:
        return 0;
    }
}

static int wait_resp(const struct ps5_smn_ops *o, uint32_t *tries_used, int *err)
{
    uint32_t v = 0, t;
    for (t = 0; t < PS5_SMU_TRIES; t++) {
        if (o->rd(o->ctx, PS5_SMU_RESP, &v)) { *err = 1; *tries_used = t; return 1; }
        if (v) { *tries_used = t; return 0; }
        o->stall_us(o->ctx, 10);
    }
    *tries_used = t;
    return 1;
}

int ps5_smu_send(const struct ps5_smn_ops *o, struct ps5_smu_state *s, uint32_t msg, uint32_t arg,
                 uint32_t *resp, uint32_t *value)
{
    if (resp) *resp = 0;
    if (value) *value = 0;
    if (!ps5_smu_allowed(msg, arg)) return -1;
    return send_raw(o, s, msg, arg, resp, value);
}

/* the mailbox protocol itself; callers have checked msg/arg (allow-list or the v0.6 OC range) */
static int send_raw(const struct ps5_smn_ops *o, struct ps5_smu_state *s, uint32_t msg, uint32_t arg,
                    uint32_t *resp, uint32_t *value)
{
    int err = 0;
    uint32_t r = 0, v = 0;
    if (resp) *resp = 0;
    if (value) *value = 0;
    if (s->poisoned) return -2;
    if (wait_resp(o, &s->last_wait_before, &err)) { s->poisoned = 1; return err ? -6 : -3; }
    if (o->wr(o->ctx, PS5_SMU_RESP, 0) || o->wr(o->ctx, PS5_SMU_ARG, arg) || o->wr(o->ctx, PS5_SMU_ARG2, 0) ||
        o->wr(o->ctx, PS5_SMU_MSG, msg)) {
        s->poisoned = 1; return -6;
    }
    if (wait_resp(o, &s->last_wait_after, &err)) { s->poisoned = 1; return err ? -6 : -4; }
    if (o->rd(o->ctx, PS5_SMU_RESP, &r) || o->rd(o->ctx, PS5_SMU_ARG, &v)) { s->poisoned = 1; return -6; }
    if (resp) *resp = r;
    if ((r & 0xffu) != 1u) return -5;
    if (value) *value = v;
    return 0;
}

int ps5_smu_boost_enter(const struct ps5_smn_ops *o, struct ps5_smu_state *s, uint32_t *resp)
{
    return ps5_smu_boost_enter_ex(o, s, 0, resp);
}

static int boost_enter_raw(const struct ps5_smn_ops *o, struct ps5_smu_state *s, uint32_t p2, uint32_t p3, uint32_t *resp);
int ps5_smu_boost_enter_ex(const struct ps5_smn_ops *o, struct ps5_smu_state *s, int hold, uint32_t *resp)
{
    return boost_enter_raw(o, s, hold ? PS5_SMU_BOOST_P2_HOLD : PS5_SMU_BOOST_P2, PS5_SMU_BOOST_P3, resp);
}

int ps5_smu_boost_enter_p3(const struct ps5_smn_ops *o, struct ps5_smu_state *s, uint32_t p3, uint32_t *resp)
{
    if (resp) *resp = 0;
    if (!PS5_SMU_WIN_P3_OK(p3)) return -1;
    return boost_enter_raw(o, s, PS5_SMU_BOOST_P2, p3, resp);
}

static int boost_enter_raw(const struct ps5_smn_ops *o, struct ps5_smu_state *s, uint32_t p2, uint32_t p3, uint32_t *resp)
{
    int err = 0;
    if (resp) *resp = 0;
    if (s->poisoned) return -2;
    /* the arguments must not change under a message still in flight: same idle check as ps5_smu_send */
    if (wait_resp(o, &s->last_wait_before, &err)) { s->poisoned = 1; return err ? -6 : -3; }
    if (o->wr(o->ctx, PS5_SMU_C2PMSG_56, PS5_SMU_BOOST_P1) || o->wr(o->ctx, PS5_SMU_C2PMSG_57, p2) ||
        o->wr(o->ctx, PS5_SMU_C2PMSG_58, p3) || o->wr(o->ctx, PS5_SMU_C2PMSG_59, PS5_SMU_BOOST_P4)) {
        s->poisoned = 1; return -6;
    }
    return ps5_smu_send(o, s, PS5_SMU_MSG_UNIVERSAL_ENTRY, 0, resp, (uint32_t *)0);
}

uint32_t ps5_smu_gfxclk_decode(uint32_t op, uint32_t mhz, uint32_t *arg)
{
    static const uint32_t msg[7] = { PS5_SMU_MSG_QUERY_GFXCLK, PS5_SMU_MSG_REQUEST_GFXCLK, PS5_SMU_MSG_UNIVERSAL_ENTRY,
                                     PS5_SMU_MSG_UNIVERSAL_EXIT, PS5_SMU_MSG_GET_GFX_FREQ, PS5_SMU_MSG_UNIVERSAL_ENTRY,
                                     PS5_SMU_MSG_REQUEST_GFXCLK };
    if (arg) *arg = (op == 1 || op == 6) ? mhz : 0;
    return op < 7 ? msg[op] : 0;
}

/* v0.6: above-stock request; the bounded wait/poison rules of ps5_smu_send apply unchanged */
int ps5_smu_request_gfxclk_oc(const struct ps5_smn_ops *o, struct ps5_smu_state *s, uint32_t mhz, uint32_t *resp,
                              uint32_t *query_mhz, uint32_t *freq_mhz)
{
    int rc;
    if (query_mhz) *query_mhz = 0;
    if (freq_mhz) *freq_mhz = 0;
    if (resp) *resp = 0;
    if (mhz < PS5_SMU_GFX_MIN_MHZ || mhz > PS5_SMU_GFX_OC_MAX_MHZ) return -1;
    rc = send_raw(o, s, PS5_SMU_MSG_REQUEST_GFXCLK, mhz, resp, (uint32_t *)0);
    if (!s->poisoned) {
        (void)ps5_smu_send(o, s, PS5_SMU_MSG_QUERY_GFXCLK, 0, (uint32_t *)0, query_mhz);
        if (!s->poisoned) (void)ps5_smu_send(o, s, PS5_SMU_MSG_GET_GFX_FREQ, 0, (uint32_t *)0, freq_mhz);
    }
    return rc;
}

/* v0.5 ---------------------------------------------------------------------------------------------------------- */
static int wait_reg(const struct ps5_smn_ops *o, uint32_t reg, uint32_t *tries_used, int *err)
{
    uint32_t v = 0, t;
    for (t = 0; t < PS5_SMU_TRIES; t++) {
        if (o->rd(o->ctx, reg, &v)) { *err = 1; *tries_used = t; return 1; }
        if (v) { *tries_used = t; return 0; }
        o->stall_us(o->ctx, 10);
    }
    *tries_used = t;
    return 1;
}

/* main mailbox with the extra argument (PS5 Linux mp1fw_sendmsg with msg->extra = 1); only called with table values */
static int send_extra(const struct ps5_smn_ops *o, struct ps5_smu_state *s, uint32_t msg, uint32_t arg, uint32_t extra,
                      uint32_t *resp)
{
    int err = 0;
    uint32_t r = 0;
    *resp = 0;
    if (s->poisoned) return -2;
    if (wait_reg(o, PS5_SMU_RESP, &s->last_wait_before, &err)) { s->poisoned = 1; return err ? -6 : -3; }
    if (o->wr(o->ctx, PS5_SMU_RESP, 0) || o->wr(o->ctx, PS5_SMU_ARG, arg) || o->wr(o->ctx, PS5_SMU_C2PMSG_81, extra) ||
        o->wr(o->ctx, PS5_SMU_MSG, msg)) {
        s->poisoned = 1; return -6;
    }
    if (wait_reg(o, PS5_SMU_RESP, &s->last_wait_after, &err)) { s->poisoned = 1; return err ? -6 : -4; }
    if (o->rd(o->ctx, PS5_SMU_RESP, &r)) { s->poisoned = 1; return -6; }
    *resp = r;
    return (r & 0xffu) == 1u ? 0 : -5;
}

/* v0.8: queue 3 allow-list (v0.5: only WC CPU get/set 1) */
int ps5_smu_test_allowed(uint32_t msg, uint32_t arg)
{
    switch (msg) {
    case PS5_SMU_TEST_GET_WC_CPU: return arg == 0;
    case PS5_SMU_TEST_SET_WC_CPU: return arg == 1;
    case PS5_SMU_Q3_GET_CPU_VOLTAGE: return arg == 0;
    case PS5_SMU_Q3_GET_CORE_FREQ: return arg < 8u;
    case PS5_SMU_Q3_SET_CPU_BOOST: return arg >= PS5_SMU_CPU_BOOST_MIN && arg <= PS5_SMU_CPU_BOOST_MAX;
    case PS5_SMU_Q3_SCALE_FVID: return arg == 0 || (arg >= 0xFFCEu && arg <= 0xFFFFu);   /* -50..0 as signed 16 bit */
    case PS5_SMU_Q3_SET_CPU_TMAX:
    case PS5_SMU_Q3_SET_GPU_TMAX: return arg >= PS5_SMU_TMAX_MIN && arg <= PS5_SMU_TMAX_MAX;
    default: return 0;
    }
}

uint32_t ps5_smu_oc_decode(uint32_t op, uint32_t val, uint32_t *arg, uint32_t *queue)
{
    uint32_t a = 0, q = 0, m = 0;
    int32_t sv = (int32_t)val;
    if (op < 7) m = ps5_smu_gfxclk_decode(op, val, &a);
    else switch (op) {
    case 7: m = PS5_SMU_MSG_GET_GFX_VID; break;
    case 8: m = PS5_SMU_MSG_GET_FEATURES; break;
    case 9: if (val >= PS5_SMU_FORCE_MIN_MHZ && val <= PS5_SMU_FORCE_MAX_MHZ) { m = PS5_SMU_MSG_FORCE_GFX_FREQ; a = val; } break;
    case 10: case 13: m = PS5_SMU_MSG_UNFORCE_GFX_FREQ; break;
    case 11: if (val >= PS5_SMU_VDDC_MIN_MV && val <= PS5_SMU_VDDC_MAX_MV) { m = PS5_SMU_MSG_FORCE_GFX_VID; a = PS5_SMU_MV_TO_VID(val); } break;
    case 12: m = PS5_SMU_MSG_UNFORCE_GFX_VID; break;
    case 14: m = PS5_SMU_Q3_GET_CPU_VOLTAGE; q = 3; break;
    case 15: if (val < 8u) { m = PS5_SMU_Q3_GET_CORE_FREQ; a = val; q = 3; } break;
    case 16: if (val >= PS5_SMU_CPU_BOOST_MIN && val <= PS5_SMU_CPU_BOOST_MAX) { m = PS5_SMU_Q3_SET_CPU_BOOST; a = val; q = 3; } break;
    case 17: if (sv >= -50 && sv <= 0) { m = PS5_SMU_Q3_SCALE_FVID; a = (uint32_t)(uint16_t)(int16_t)sv; q = 3; } break;
    case 18: case 19: if (val >= PS5_SMU_TMAX_MIN && val <= PS5_SMU_TMAX_MAX) { m = op == 18 ? PS5_SMU_Q3_SET_CPU_TMAX : PS5_SMU_Q3_SET_GPU_TMAX; a = val; q = 3; } break;
    case 20: m = PS5_SMU_Q3_SET_CPU_BOOST; a = PS5_SMU_CPU_BOOST_DEFAULT; q = 3; break;
    case 21: if (PS5_SMU_WIN_P3_OK(val)) { m = PS5_SMU_MSG_UNIVERSAL_ENTRY; a = val; } break;   /* v0.9 */
    default: break;
    }
    if (arg) *arg = m ? a : 0;
    if (queue) *queue = m ? q : 0;
    return m;
}

int ps5_smu_gpu_unforce(const struct ps5_smn_ops *o, struct ps5_smu_state *s, uint32_t *resp)
{
    uint32_t r2 = 0;
    int rc = ps5_smu_send(o, s, PS5_SMU_MSG_UNFORCE_GFX_FREQ, 0, resp, 0);
    int rc2 = ps5_smu_send(o, s, PS5_SMU_MSG_UNFORCE_GFX_VID, 0, &r2, 0);   /* also after a refused 0x3A */
    if (rc2 == -5) rc2 = 0;                                                 /* status ignored, as the governor */
    return rc && rc != -5 ? rc : (rc2 ? rc2 : rc);
}

int ps5_smu_cpu_oc_reset(const struct ps5_smn_ops *o, struct ps5_smu_state *s, uint32_t *resp)
{
    uint32_t r2 = 0;
    int rc = ps5_smu_test_send(o, s, PS5_SMU_Q3_SET_CPU_BOOST, PS5_SMU_CPU_BOOST_DEFAULT, resp, 0);
    int rc2 = ps5_smu_test_send(o, s, PS5_SMU_Q3_SCALE_FVID, 0, &r2, 0);
    return rc ? rc : rc2;
}

int ps5_smu_test_send(const struct ps5_smn_ops *o, struct ps5_smu_state *s, uint32_t msg, uint32_t arg,
                      uint32_t *resp, uint32_t *value)
{
    int err = 0;
    uint32_t r = 0, v = 0;
    if (resp) *resp = 0;
    if (value) *value = 0;
    if (!ps5_smu_test_allowed(msg, arg)) return -1;
    if (s->poisoned) return -2;
    if (wait_reg(o, PS5_SMU_TEST_RESP, &s->last_wait_before, &err)) { s->poisoned = 1; return err ? -6 : -3; }
    if (o->wr(o->ctx, PS5_SMU_TEST_RESP, 0) || o->wr(o->ctx, PS5_SMU_TEST_ARG, arg) || o->wr(o->ctx, PS5_SMU_TEST_MSG, msg)) {
        s->poisoned = 1; return -6;
    }
    if (wait_reg(o, PS5_SMU_TEST_RESP, &s->last_wait_after, &err)) { s->poisoned = 1; return err ? -6 : -4; }
    if (o->rd(o->ctx, PS5_SMU_TEST_RESP, &r) || o->rd(o->ctx, PS5_SMU_TEST_ARG, &v)) { s->poisoned = 1; return -6; }
    if (resp) *resp = r;
    if (r != 1u) return -5;                               /* Linux mp1fw_test_sendmsg: resp must be exactly 1 */
    if (value) *value = v;
    return 0;
}

#define FAIL(st, idx, rc) do { r->status = (rc); r->step = (st); r->index = (idx); r->resp = resp; return (rc); } while (0)
int ps5_smu_bapm_prepare(const struct ps5_smn_ops *o, struct ps5_smu_state *s, struct ps5_bapm_report *r)
{
    uint32_t resp = 0, mode = 0;
    int rc;
    rc = ps5_smu_send(o, s, PS5_SMU_MSG_UNIVERSAL_EXIT, 0, &resp, (uint32_t *)0);
    if (rc) FAIL(1, 0, rc);
    rc = ps5_smu_test_send(o, s, PS5_SMU_TEST_GET_WC_CPU, 0, &resp, &mode);
    if (rc) FAIL(2, 0, rc);
    r->wc_before = r->wc_after = mode;
    if (mode != 1) {
        rc = ps5_smu_test_send(o, s, PS5_SMU_TEST_SET_WC_CPU, 1, &resp, (uint32_t *)0);
        if (rc) FAIL(3, 0, rc);
        rc = ps5_smu_test_send(o, s, PS5_SMU_TEST_GET_WC_CPU, 0, &resp, &mode);
        if (rc) FAIL(2, 1, rc);
        r->wc_after = mode;
    }
    return 0;
}

/* v0.9: each signed 16-bit half of a packed GFX CAC weight times pct/100, rounded to nearest (ties away from 0) */
uint32_t ps5_bapm_scale_gfx(uint32_t w, uint32_t pct)
{
    uint32_t out = 0, h;
    for (h = 0; h < 2; h++) {
        int32_t v = (int16_t)(uint16_t)(w >> (16 * h)), p = (int32_t)v * (int32_t)pct;
        int32_t q = p >= 0 ? (p + 50) / 100 : -((-p + 50) / 100);
        out |= (uint32_t)(uint16_t)(int16_t)q << (16 * h);
    }
    return out;
}

int ps5_smu_bapm_weights(const struct ps5_smn_ops *o, struct ps5_smu_state *s, struct ps5_bapm_report *r)
{
    return ps5_smu_bapm_weights_scaled(o, s, r, 100u);
}

int ps5_smu_bapm_weights_scaled(const struct ps5_smn_ops *o, struct ps5_smu_state *s, struct ps5_bapm_report *r, uint32_t gfx_pct)
{
    const struct bapm_param_set *set = &ps5_bapm_set2;
    uint32_t resp = 0, i;
    int rc;
    if (gfx_pct < PS5_BAPM_GFX_PCT_MIN || gfx_pct > 100u) FAIL(5, 0, -1);
    for (i = 0; i < 80; i++) {
        rc = send_extra(o, s, PS5_SMU_MSG_L3_CACW, i | 0x20000u, set->l3_cacw[i], &resp);
        if (rc) FAIL(4, i, rc);
        r->l3_sent++;
    }
    for (i = 0; i < set->gfx_cacw_len && i < 88u; i++) {
        rc = send_extra(o, s, PS5_SMU_MSG_GFX_CACW, i | 0x20000u, gfx_pct == 100u ? set->gfx_cacw[i] : ps5_bapm_scale_gfx(set->gfx_cacw[i], gfx_pct), &resp);
        if (rc) FAIL(5, i, rc);
        r->gfx_sent++;
    }
    for (i = 0; i < 176; i++) {
        if (!set->bapm_param[i][0]) continue;
        rc = send_extra(o, s, PS5_SMU_MSG_BAPM_PARAM, i, set->bapm_param[i][1], &resp);
        if (rc) FAIL(6, i, rc);
        r->param_sent++;
    }
    return 0;
}
#undef FAIL

/* per-core DPM_WAC values for the caller's processor broadcast (PS5 Linux update_bapm_weights_smp) */
const uint64_t *ps5_bapm_dpm_wac(void) { return ps5_bapm_set2.dpm_wac; }
