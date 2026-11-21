/* smu-mbox v0.8 (18.11.2026, user night task "real OC mode", sources: github.com/filippor/cyan-skillfish-governor
 * src/smu/api/queue0.rs + codec.rs, github.com/bc250-collective/bc250_smu_oc bc250_smu/api_q3.py + bc250_detect.py, Linux
 * cyan_skillfish_ppt.c (VDDC 700..1129 mV, SVI2 VID): queue 0 GetGfxVid 0x38, ForceGfxFreq 0x39 (500..2600 MHz),
 * UnforceGfxFreq 0x3A, ForceGfxVid 0x3B (VID for 700..1129 mV, never above), UnforceGfxVid 0x3C (status ignored, as the
 * governor), GetEnabledSmuFeatures 0x3D; queue 3 (= the test mailbox C2PMSG_72/96/98) GetCpuVoltage 0x36, GetCoreFreq 0x43,
 * SetMaxCpuBoostClk 0x8F (3200..4000), ScaleFVidCurve 0x50 (-50..0 only, signed 16 bit), SetCpu/GpuMaxTemperature 0x8B/0x8C
 * (70..95 only: lower than the default never raises a limit); op table ps5_smu_oc_decode) - smu-mbox v0.7 (17.11.2026: + ps5_smu_gfxclk_decode = the IOCTL_PS5PWR_GFXCLK op table, shared by driver and host test) -
 * smu-mbox v0.6 (17.11.2026, user-approved OC package a+b: boost window variant HOLD with GPU_MHZ_MIN = 2230 (P2 =
 * (2230<<16)|3200, P3 unchanged) and RequestGfxclk above stock up to 2400 MHz only through ps5_smu_request_gfxclk_oc() with an
 * immediate QueryGfxclk + GetGfxFrequency readback; the normal allow-list still caps RequestGfxclk at 2230) - smu-mbox v0.5 (17.11.2026: + BAPM init exactly as PS5 Linux mp1_init - messages with an extra argument in C2PMSG_81
 * (GfxCacWeight 0x2F, L3CacWeight 0x30, BAPM param 0x33, only from the fixed table ps5-bapm-set2.h) and the test mailbox
 * C2PMSG_72/96/98 (WC CPU get 0x9C, set 0x9B with arg 1 only)) - smu-mbox v0.4 (16.11.2026: + Boost-Fenster UniversalModeEntry 0x22/Exit 0x23 with the fixed PS5 Linux
 * MP1_BOOST_ENTER arguments in C2PMSG_56..59, + GetGfxFrequency 0x37) - smu-mbox v0.3 (15.11.2026: ARG+4 cleared before MSG and resp & 0xff as PS5 Linux ps5-gpufreq.c L66-69/L82,
 * wait bounded to 200 ms) - v0.2 (15.11.2026: + RequestGfxclk 500..2230 MHz as PS5 Linux gpufreq.c) - v0.1 (07.11.2026) - PS5 SMU (MP1) mailbox, the exact protocol of PS5 Linux mp1fw_sendmsg_locked()
 * (ps5-ultra-power/build/linux/drivers/ps5/mp1.c L230-282, same registers as ps5-gpufreq.c / ps5-smu-snapshot.c):
 *   wait RESP!=0 (10 us steps) -> RESP=0 -> ARG=arg -> ARG+4=0 -> MSG=msg -> wait RESP!=0 -> (RESP & 0xff)==1 ok, value=ARG.
 * SMN access goes through caller-supplied ops (Windows: host bridge 00:00.0 config 0x60/0x64 via the HAL; host tests:
 * a simulated SMU). Only allow-listed messages are ever sent; a wait timeout poisons the mailbox for the rest of the
 * session (no further SMU traffic) because a wedged MP1 must not be hammered. Read-only build: 0x02, 0x03, 0x0F. */
#ifndef PS5_SMU_MBOX_H
#define PS5_SMU_MBOX_H
#include <stdint.h>

#define PS5_SMU_MSG  0x03b10a08u
#define PS5_SMU_ARG  0x03b10a48u
#define PS5_SMU_ARG2 0x03b10a4cu   /* C2PMSG_83, cleared before every message (ps5-gpufreq.c: PS5_SMU_ARG + 4) */
#define PS5_SMU_RESP 0x03b10a68u
#define PS5_SMU_TRIES 20000u       /* x 10 us = 200 ms of stalls (plus the SMN accesses): the caller spins under a
                                       * fast mutex at APC_LEVEL; an answer normally takes microseconds */

#define PS5_SMU_MSG_GET_VERSION    0x02u   /* arg 0 -> firmware version (Linux: 0x004a1400) */
#define PS5_SMU_MSG_GET_IF_VERSION 0x03u   /* arg 0 -> driver interface version (Linux: 8) */
#define PS5_SMU_MSG_QUERY_GFXCLK   0x0fu   /* arg 0 -> current GFXCLK in MHz */
#define PS5_SMU_MSG_REQUEST_GFXCLK 0x0eu   /* arg MHz -> SMU sets GFXCLK (PS5 Linux ps5_mp1_request_gfxclk) */
#define PS5_SMU_GFX_MIN_MHZ 500u          /* PS5 Linux PS5_GPU_MIN_MHZ */
#define PS5_SMU_GFX_MAX_MHZ 2230u         /* PS5 Linux PS5_GPU_MAX_MHZ (non-Pro) */
#define PS5_SMU_MSG_UNIVERSAL_ENTRY 0x22u   /* arg 0 after C2PMSG_56..59 (PS5 Linux mp1_universal_mode_enter) */
#define PS5_SMU_MSG_UNIVERSAL_EXIT  0x23u   /* arg 0 (mp1_universal_mode_exit) */
#define PS5_SMU_MSG_GET_GFX_FREQ    0x37u   /* arg 0 -> GFX frequency (MP1_GET_GFX_FREQ) */
/* MP1_BOOST_ENTER (PS5 Linux mp1.c): fixed, never caller-supplied. Only these four registers are ever written besides
 * the mailbox itself. params[1] and params[4] are copied verbatim (meaning not documented). */
#define PS5_SMU_C2PMSG_56 0x03b109e0u
#define PS5_SMU_C2PMSG_57 0x03b109e4u
#define PS5_SMU_C2PMSG_58 0x03b109e8u
#define PS5_SMU_C2PMSG_59 0x03b109ecu
#define PS5_SMU_BOOST_P1  0x000449cdu
#define PS5_SMU_BOOST_P2  ((2000u << 16) | 3200u)   /* GPU_MHZ_MIN << 16 | CPU_MHZ_MIN */
#define PS5_SMU_BOOST_P3  ((2230u << 16) | 3500u)   /* GPU_MHZ_MAX << 16 | CPU_MHZ_MAX */
#define PS5_SMU_BOOST_P4  0x25c6a3c8u
#define PS5_SMU_BOOST_P2_HOLD ((2230u << 16) | 3200u)   /* v0.6 HOLD: GPU_MHZ_MIN raised to the max -> the window holds 2230 */
#define PS5_SMU_GFX_OC_MAX_MHZ 2400u                    /* v0.6: RequestGfxclk ceiling of the explicit above-stock path */
/* v0.9: boost window with a caller-chosen ceiling P3 = gpu_max << 16 | cpu_max (op 21); P1/P2/P4 stay the Linux values */
#define PS5_SMU_WIN_GPU_MAX_LO 2230u
#define PS5_SMU_WIN_GPU_MAX_HI 2400u
#define PS5_SMU_WIN_CPU_MAX_LO 3500u
#define PS5_SMU_WIN_CPU_MAX_HI 3800u
#define PS5_SMU_WIN_P3_OK(v) (((v) >> 16) >= PS5_SMU_WIN_GPU_MAX_LO && ((v) >> 16) <= PS5_SMU_WIN_GPU_MAX_HI && \
                              ((v) & 0xffffu) >= PS5_SMU_WIN_CPU_MAX_LO && ((v) & 0xffffu) <= PS5_SMU_WIN_CPU_MAX_HI)
/* v0.8 queue 0: forced GFX clock and voltage (cyan-skillfish-governor queue0.rs) */
#define PS5_SMU_MSG_GET_GFX_VID      0x38u   /* arg 0 -> SVI2 VID (mV = 1550 - 6.25 * VID) */
#define PS5_SMU_MSG_FORCE_GFX_FREQ   0x39u   /* arg MHz */
#define PS5_SMU_MSG_UNFORCE_GFX_FREQ 0x3Au   /* arg 0 */
#define PS5_SMU_MSG_FORCE_GFX_VID    0x3Bu   /* arg VID */
#define PS5_SMU_MSG_UNFORCE_GFX_VID  0x3Cu   /* arg 0; the governor ignores its status (check_status false) */
#define PS5_SMU_MSG_GET_FEATURES     0x3Du   /* arg 0 -> enabled feature mask */
#define PS5_SMU_FORCE_MIN_MHZ 500u
#define PS5_SMU_FORCE_MAX_MHZ 2600u
#define PS5_SMU_VDDC_MIN_MV 700u             /* Linux cyan_skillfish_ppt.c CYAN_SKILLFISH_VDDC_MIN */
#define PS5_SMU_VDDC_MAX_MV 1129u            /* Linux cyan_skillfish_ppt.c CYAN_SKILLFISH_VDDC_MAX - the hard voltage cap */
/* mV -> VID rounded UP (higher VID = lower voltage), so the programmed voltage is never above the request */
#define PS5_SMU_MV_TO_VID(mv) ((((1550u - (mv)) * 160u) + 999u) / 1000u)
#define PS5_SMU_VID_MIN PS5_SMU_MV_TO_VID(PS5_SMU_VDDC_MAX_MV)   /* 68 = 1125 mV */
#define PS5_SMU_VID_MAX PS5_SMU_MV_TO_VID(PS5_SMU_VDDC_MIN_MV)   /* 136 = 700 mV */
/* VID -> voltage in 1/100 mV (1550 mV - 6.25 mV per step) */
#define PS5_SMU_VID_TO_MV100(vid) (155000u - 625u * (vid))
/* v0.8 queue 3 (bc250_smu_oc api_q3.py; transport = ps5_smu_test_send) */
#define PS5_SMU_Q3_GET_CPU_VOLTAGE 0x36u     /* arg 0 */
#define PS5_SMU_Q3_GET_CORE_FREQ   0x43u     /* arg core 0..7 */
#define PS5_SMU_Q3_SCALE_FVID      0x50u     /* arg signed 16 bit in the low half, -50..0 here */
#define PS5_SMU_Q3_SET_CPU_TMAX    0x8Bu     /* arg degrees C, 70..95 here */
#define PS5_SMU_Q3_SET_GPU_TMAX    0x8Cu
#define PS5_SMU_Q3_SET_CPU_BOOST   0x8Fu     /* arg MHz, 3200..4000 here; 3500 = bc250_detect.py revert_defaults */
#define PS5_SMU_CPU_BOOST_MIN 3200u
#define PS5_SMU_CPU_BOOST_MAX 4000u
#define PS5_SMU_CPU_BOOST_DEFAULT 3500u
#define PS5_SMU_TMAX_MIN 70u
#define PS5_SMU_TMAX_MAX 95u
int ps5_smu_test_allowed(uint32_t msg, uint32_t arg);
/* v0.8: IOCTL_PS5PWR_GFXCLK op -> queue (0 or 3), message, argument. ops 0..6 as ps5_smu_gfxclk_decode; 7 GetGfxVid,
 * 8 GetEnabledSmuFeatures, 9 ForceGfxFreq (val MHz), 10 UnforceGfxFreq, 11 ForceGfxVid (val mV -> VID), 12 UnforceGfxVid,
 * 13 GPU unforce (10 + 12, composite), 14 GetCpuVoltage, 15 GetCoreFreq (val core), 16 SetMaxCpuBoostClk (val MHz),
 * 17 ScaleFVidCurve (val signed), 18 SetCpuMaxTemperature, 19 SetGpuMaxTemperature (val C), 20 CPU OC reset (boost 3500,
 * scale 0; composite). Returns 0 for an unknown op or a value outside its range (nothing is sent then). For composite ops
 * the first message is returned. */
uint32_t ps5_smu_oc_decode(uint32_t op, uint32_t val, uint32_t *arg, uint32_t *queue);

struct ps5_smn_ops {
    void *ctx;
    int (*rd)(void *ctx, uint32_t addr, uint32_t *val);   /* 0 = ok */
    int (*wr)(void *ctx, uint32_t addr, uint32_t val);
    void (*stall_us)(void *ctx, uint32_t us);
};
struct ps5_smu_state {
    int poisoned;                  /* set by any wait timeout or SMN access error */
    uint32_t last_wait_before, last_wait_after;
};
/* returns 0 ok (resp 1), -1 not allow-listed (nothing accessed), -2 poisoned (nothing accessed), -3 busy before send
 * (timeout, now poisoned, nothing written), -4 no response after send (now poisoned), -5 SMU answered resp != 1
 * (*resp holds it, mailbox stays usable), -6 SMN access error (now poisoned) */
int ps5_smu_send(const struct ps5_smn_ops *o, struct ps5_smu_state *s, uint32_t msg, uint32_t arg,
                 uint32_t *resp, uint32_t *value);
int ps5_smu_allowed(uint32_t msg, uint32_t arg);
/* v0.4: boost window enter = write C2PMSG_56..59 with the constants above, then UniversalModeEntry(0). Same return
 * codes as ps5_smu_send; nothing is written if the mailbox is poisoned or busy. */
int ps5_smu_boost_enter(const struct ps5_smn_ops *o, struct ps5_smu_state *s, uint32_t *resp);
/* v0.6: hold = 0 -> exactly ps5_smu_boost_enter; hold = 1 -> C2PMSG_57 = PS5_SMU_BOOST_P2_HOLD, everything else the same */
int ps5_smu_boost_enter_ex(const struct ps5_smn_ops *o, struct ps5_smu_state *s, int hold, uint32_t *resp);
/* v0.9: same as ps5_smu_boost_enter with C2PMSG_58 = p3; -1 (nothing written) if PS5_SMU_WIN_P3_OK(p3) fails */
int ps5_smu_boost_enter_p3(const struct ps5_smn_ops *o, struct ps5_smu_state *s, uint32_t p3, uint32_t *resp);
/* v0.6: RequestGfxclk 500..2400 MHz (above 2230 only here), then QueryGfxclk and GetGfxFrequency; the readback values go
 * to *query_mhz / *freq_mhz (0 when that message failed; GetGfxFrequency answers 0xFE on some consoles). Returns the
 * RequestGfxclk result (ps5_smu_send codes; -1 = out of range, nothing sent). Readback failures do not change it. */
/* v0.7: IOCTL_PS5PWR_GFXCLK op -> mailbox message and argument. ops: 0 QueryGfxclk, 1 RequestGfxclk (500..2230),
 * 2 boost enter, 3 boost exit, 4 GetGfxFrequency, 5 boost enter HOLD, 6 RequestGfxclk 500..2400 + readback.
 * Returns 0 for an unknown op (the driver answers STATUS_INVALID_PARAMETER), else the message. */
uint32_t ps5_smu_gfxclk_decode(uint32_t op, uint32_t mhz, uint32_t *arg);
int ps5_smu_request_gfxclk_oc(const struct ps5_smn_ops *o, struct ps5_smu_state *s, uint32_t mhz, uint32_t *resp,
                              uint32_t *query_mhz, uint32_t *freq_mhz);
/* v0.5: BAPM init (PS5 Linux drivers/ps5/mp1.c mp1_init + mp1_set_bapm_param_set_all, base PS5 parameter set 2) */
#define PS5_SMU_C2PMSG_81 0x03b10a44u    /* extra argument of the weight messages (mp1fw_sendmsg msg->extra) */
#define PS5_SMU_TEST_MSG  0x03b10a20u    /* C2PMSG_72 */
#define PS5_SMU_TEST_RESP 0x03b10a80u    /* C2PMSG_96 */
#define PS5_SMU_TEST_ARG  0x03b10a88u    /* C2PMSG_98 */
#define PS5_SMU_MSG_GFX_CACW   0x2fu     /* arg index|0x20000, extra weight */
#define PS5_SMU_MSG_L3_CACW    0x30u     /* arg index|0x20000, extra weight */
#define PS5_SMU_MSG_BAPM_PARAM 0x33u     /* arg index, extra param */
#define PS5_SMU_TEST_SET_WC_CPU 0x9bu    /* test mailbox, arg 1 */
#define PS5_SMU_TEST_GET_WC_CPU 0x9cu    /* test mailbox, arg 0 -> mode */
struct ps5_bapm_report {
    int status;                    /* 0 ok, else the first failing ps5_smu_* code */
    uint32_t step;                 /* 1 universal exit, 2 wc get, 3 wc set, 4 l3, 5 gfx, 6 param */
    uint32_t index, resp;          /* table index and SMU answer of the failure */
    uint32_t wc_before, wc_after, l3_sent, gfx_sent, param_sent;
};
/* test mailbox, only GET_WC_CPU (arg 0) and SET_WC_CPU (arg 1); same return codes as ps5_smu_send */
int ps5_smu_test_send(const struct ps5_smn_ops *o, struct ps5_smu_state *s, uint32_t msg, uint32_t arg,
                      uint32_t *resp, uint32_t *value);
/* step A (before the per-core DPM_WAC MSR writes, which the caller does on every processor): UniversalModeExit,
 * WC CPU get, set to 1 if it is not 1. Step B (after the MSR writes): 80 L3, gfx_cacw_len GFX weights, every active
 * BAPM param - all values from the fixed table. Both stop at the first error and fill *r. */
int ps5_smu_bapm_prepare(const struct ps5_smn_ops *o, struct ps5_smu_state *s, struct ps5_bapm_report *r);
int ps5_smu_bapm_weights(const struct ps5_smn_ops *o, struct ps5_smu_state *s, struct ps5_bapm_report *r);
/* v0.9: same with every GFX CAC weight scaled to gfx_pct % (ps5_bapm_scale_gfx), 85..100; 100 = the table bit for bit.
 * The SMU then estimates less GPU power and holds higher clocks inside its unchanged budget. -1 if out of range. */
#define PS5_BAPM_GFX_PCT_MIN 85u
int ps5_smu_bapm_weights_scaled(const struct ps5_smn_ops *o, struct ps5_smu_state *s, struct ps5_bapm_report *r, uint32_t gfx_pct);
uint32_t ps5_bapm_scale_gfx(uint32_t w, uint32_t pct);
/* the 21 DPM_WAC values (MSR C001_1077 via index C001_1076, DPM_CFG C001_1074 bit 62 cleared around) */
#define PS5_MSR_DPM_CFG 0xC0011074u
#define PS5_MSR_DPM_WAC_INDEX 0xC0011076u
#define PS5_MSR_DPM_WAC_DATA 0xC0011077u
#define PS5_MSR_ACCESS_DIS (1ull << 62)
const uint64_t *ps5_bapm_dpm_wac(void);
/* v0.8 composite: GPU unforce = UnforceGfxFreq then UnforceGfxVid (its status ignored); CPU reset = SetMaxCpuBoostClk
 * 3500 then ScaleFVidCurve 0. Return the first hard error (ps5_smu_* codes) or 0. */
int ps5_smu_gpu_unforce(const struct ps5_smn_ops *o, struct ps5_smu_state *s, uint32_t *resp);
int ps5_smu_cpu_oc_reset(const struct ps5_smn_ops *o, struct ps5_smu_state *s, uint32_t *resp);
#endif
