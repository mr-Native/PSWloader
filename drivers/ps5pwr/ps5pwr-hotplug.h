/* SPDX-License-Identifier: GPL-2.0-only
 * ps5pwr v1.4 hotplug core (UNTESTED ON HARDWARE, 14.11.2026).
 *
 * v1.18 (25.11.2026, GitHub issue #13): FLAVA3 and VERDE (PS5 Slim), see ps5pwr-hotplug.c.
 * When the HDMI sink is unplugged/replugged at runtime, the EMC runs the "back to unplugged" sequence
 * the firmware registered at boot (flava3-edid.c, hdmiSystemResume) and the FLAVA3 output stays muted
 * until somebody programs it again; Windows has no ICC client, so the picture stayed black until reboot
 * (13.11. 23:00). PS5 Linux re-applies the mode on every EDID notification after the first one
 * (ultrawide reconnect patch: 500 ms delayed work, ignore EDID events for 3 s after a relink;
 * linux.patch:12675-12699 hdmi_notification_handler: event 0x01 unplug, 0x02 EDID).
 *
 * This core does the same for the CURRENT mode only (the DCN keeps running, no mode change):
 *   collector ps5_hp_note()  ICC notification callback (any ICC transaction of ps5pwr feeds it)
 *   ps5_hp_poll()            <= PS5_HP_POLL_MAX_MSGS messages per tick, storm pause
 *   ps5_hp_process()         event queue -> state machine (debounce, guard, baseline, rate hold)
 *   ps5_hp_replay()          the firmware's FLAVA3 stage list for the current mode, one ICC
 *                            transaction per gate() pair, abort on unplug/stop
 *   ps5_hp_replay_done()     bounded retries with backoff 1 s, 2 s
 * Pure C, no allocation, no OS calls: compiled into ps5pwr.sys and into the host test
 * test_ps5pwr_hotplug.c. The caller serializes all ICC users and calls everything except
 * ps5_hp_note() from one thread. Logging goes through ps5_hp_log(), supplied by the caller.
 */
#ifndef PS5PWR_HOTPLUG_H
#define PS5PWR_HOTPLUG_H
#include "../../firmware/icc-power.h"
#include "../../firmware/icc-hdmi.h"
#include "../../firmware/hdmi-commands.h"
#include "../../firmware/flava3-mode.h"
#include "../../firmware/ps5-display-mode.h"

#define PS5_HP_CONVERTER_FLAVA3 2      /* ps5_hdmi_config.converter (ICC 2/0x16), PS5 Linux HDMI_IC_TYPE_*          */
#define PS5_HP_CONVERTER_VERDE  3      /* v1.18: PS5 Slim / late revisions                                         */
#define PS5_HP_SERVICE_HDMI    0x10
#define PS5_HP_EV_UNPLUG       0x01
#define PS5_HP_EV_EDID         0x02
#define PS5_HP_EDID_MAX        512u     /* stored EDID bytes; longer EDIDs compare as different      */
#define PS5_HP_QUEUE           16u
#define PS5_HP_POLL_MAX_MSGS   8u       /* per poll tick                                              */
#define PS5_HP_DEBOUNCE_MS     500u     /* Linux ps5_hdmi_reconnect: mod_delayed_work(500 ms)        */
#define PS5_HP_PICK_GRACE_MS   6000u    /* v1.18.1: other sink + HotplugMode 4: time for the KMD mode change (HW: ~5 s) before the old mode is replayed */
#define PS5_HP_GUARD_MS        3000u    /* Linux: no relink for 3 s after a relink                    */
#define PS5_HP_ARM_MS          3000u    /* identical EDID this soon after events-on = initial report  */
#define PS5_HP_MAX_ATTEMPTS    3u       /* per event; backoff 1 s, 2 s                                */
#define PS5_HP_RATE_WINDOW_MS  60000u   /* more than PS5_HP_RATE_MAX good replays in this window ...  */
#define PS5_HP_RATE_MAX        3u       /* ... -> hold until the next unplug event                    */
#define PS5_HP_STORM_WINDOW_MS 10000u
#define PS5_HP_STORM_MSGS      64u      /* polled messages per window before polling pauses          */
#define PS5_HP_STORM_PAUSE_MS  10000u
#define PS5_HP_MAX_STAGES      16u

/* HotplugMode registry value */
enum ps5_hp_mode { PS5_HP_OFF = 0, PS5_HP_LOG = 1, PS5_HP_REPLAY_BOOT = 2, PS5_HP_REPLAY_SHORT = 3,
                   PS5_HP_PICK = 4 };   /* v1.13: as REPLAY_BOOT + pick the new sink's mode and publish it (KMD, PswHotplug) */

/* v1.13 (G05): modes ps5pwr may pick / program = the DCN replays of the release firmware r5.24 (build-uefi --display-replay
 * 3440p60 3440p100 ugreen1440p60 2560x1440p120 720p144, plus the always present 1080p60) */
#define PS5_HP_PICK_MASK ((1u << PS5_MODE_1080P60) | (1u << PS5_MODE_3440P60) | (1u << PS5_MODE_3440P100) | \
                          (1u << PS5_MODE_UGREEN1440P60) | (1u << PS5_MODE_2560P120) | (1u << PS5_MODE_720P144))
/* v1.13: the mode for a new EDID, same rule as the firmware refresh=max (highest refresh at the native resolution) */
struct ps5_hp_pick {
    uint32_t seq;                          /* 1, 2, ... per picked EDID                                */
    uint32_t mode_id;                      /* enum ps5_mode_id                                         */
    uint32_t offered_mask;                 /* PS5_HP_PICK_MASK modes the EDID offers (bit = mode id)   */
    uint32_t edid_ok;                      /* 0: EDID did not parse -> mode_id = 1080p60 fallback      */
    uint32_t refresh_hz;
    struct ps5_timing t;                   /* timing of mode_id (ps5_display_modes[])                  */
    uint32_t edid_bytes;
    uint8_t edid[PS5_HP_EDID_MAX];         /* fixed EDID (Linux fix_edid) the pick is based on         */
};
/* where the current mode came from; NONE = replay refused */
enum ps5_hp_source { PS5_HP_SRC_NONE = 0, PS5_HP_SRC_VAR = 1, PS5_HP_SRC_BRIDGE = 2, PS5_HP_SRC_DEFAULT = 3,
                      PS5_HP_SRC_PROGRAM = 4 /* v1.13.2: set by a successful ps5_hp_program_mode */ };
/* ps5_hp_replay results besides ICC/service results (-1..-7) */
#define PS5_HP_ABORT_STOP   (-20)
#define PS5_HP_ABORT_UNPLUG (-21)
#define PS5_HP_BUILD_FAIL   (-22)
#define PS5_HP_NO_MODE      (-23)

/* all uint32_t: the driver exposes them as a ULONG array (IOCTL_PS5PWR_HOTPLUG, ps5pwr.ps1 -Hotplug) */
struct ps5_hp_counters {
    uint32_t notes, hdmi, unplug, edid, edid_same, edid_diff, edid_bad, short_note,
             other_event, other_service, stale, poll_err, queue_lost, storms, guard_ignored, baseline,
             scheduled, replays, replay_ok, replay_fail, replay_abort, retries, held, log_only;
};
#define PS5_HP_COUNTER_NAMES "notes hdmi unplug edid edid_same edid_diff edid_bad short_note " \
    "other_event other_service stale poll_err queue_lost storms guard_ignored baseline " \
    "scheduled replays replay_ok replay_fail replay_abort retries held log_only"

struct ps5_hp_event { uint8_t svc, event, edid_ok, pad; uint16_t type, size; uint32_t bytes; };

struct ps5_hp {
    /* configuration, set by the caller (ps5_hp_arm_reset keeps it) */
    unsigned mode;                         /* enum ps5_hp_mode                                     */
    struct ps5_hdmi_config cfg;            /* converter wiring (ICC 2/0x16), stage builders        */
    struct ps5_flava3_mode bridge;         /* FLAVA3 parameters of the current mode                */
    int bridge_null;                       /* 1: VIC16 packets (Video->SetMode(NULL) in firmware)  */
    int source;                            /* enum ps5_hp_source                                   */
    uint32_t mode_id;                      /* informational (enum ps5_mode_id), ~0 unknown         */
    uint8_t boot_edid[PS5_HP_EDID_MAX];    /* "Ps5HdmiEdid" (already fixed by the firmware)        */
    unsigned boot_edid_bytes;
    void *log_ctx;                         /* first argument of ps5_hp_log()                       */
    /* v1.10: optional tap on EVERY ICC notification (power button); called inside the ICC transaction */
    void (*extra_note)(void *ctx, uint8_t service, uint16_t type, const uint8_t *payload, unsigned bytes);
    void *extra_ctx;
    /* collector: written by ps5_hp_note() inside ICC transactions */
    struct ps5_hp_event q[PS5_HP_QUEUE];
    unsigned q_head, q_count;
    uint32_t unplug_seq;
    uint8_t edid[PS5_HP_EDID_MAX];         /* latest EDID notification, raw (byte 0 = PS5 quirk)   */
    unsigned edid_bytes;
    /* state machine */
    uint64_t armed_ms, due_ms, guard_until_ms, quiet_until_ms, window_ms;
    uint64_t ok_ms[PS5_HP_RATE_MAX];
    unsigned ok_n, window_msgs, attempt;
    int pending, unplugged, baseline_done, held;
    int last_result;
    unsigned last_stage;
    /* last processed EDID, fixed (Linux fix_edid), for the caller to publish */
    uint8_t pub_edid[PS5_HP_EDID_MAX];
    unsigned pub_bytes;
    int pub_same, pub_new;
    struct ps5_hp_counters c;
    /* v1.13: last pick (PS5_HP_PICK), set by ps5_hp_process() for a different EDID; pick_new until the caller publishes */
    struct ps5_hp_pick pick;
    int pick_new;
};

/* supplied by the caller (driver: DbgPrintEx + ring; host test: buffer) */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 2, 3)))
#endif
void ps5_hp_log(void *ctx, const char *fmt, ...);

/* ps5_icc_notification; context = struct ps5_hp *. Never blocks, never logs. */
void ps5_hp_note(void *context, uint8_t service, uint16_t type, const uint8_t *payload, unsigned bytes);
/* clear queue + state machine, keep configuration; armed_ms = now (events were just enabled) */
void ps5_hp_arm_reset(struct ps5_hp *hp, uint64_t now_ms);
/* sceControlHdmiEvent(on): packet 5/4 (code 4), the firmware's EDID-phase enable/disable */
int ps5_hp_events(struct ps5_hp *hp, const struct ps5_icc_io *io, struct ps5_hdmi_commands *packet, int on);
/* number of messages taken (0 also while paused); caller owns ICC */
int ps5_hp_poll(struct ps5_hp *hp, const struct ps5_icc_io *io, uint64_t now_ms);
void ps5_hp_process(struct ps5_hp *hp, uint64_t now_ms);
int ps5_hp_due(const struct ps5_hp *hp, uint64_t now_ms);
/* stage order of the firmware (ps5-native-dp-video.h) without the DCN part; returns count */
unsigned ps5_hp_stage_list(const struct ps5_hp *hp, unsigned out[PS5_HP_MAX_STAGES]);
/* gate(ctx, index, stage, 0) before a stage: nonzero aborts (caller then does NOT own ICC);
 * gate(ctx, index, stage, 1) after it. NULL gate: caller owns ICC for the whole replay. */
typedef int (*ps5_hp_gate)(void *ctx, unsigned index, unsigned stage, int after);
int ps5_hp_replay(struct ps5_hp *hp, const struct ps5_icc_io *io, struct ps5_hdmi_commands *packet,
                  ps5_hp_gate gate, void *ctx);
void ps5_hp_replay_done(struct ps5_hp *hp, int result, uint64_t now_ms);
/* current mode: 0 ok. from the "Ps5HdmiMode" variable (ps5-hdmi-mode-var.h) */
int ps5_hp_mode_from_var(struct ps5_hp *hp, const uint8_t *var, unsigned bytes);
/* from the bridge itself (0x7227 profile, 0x7056 for 3440): unique whitelist match; caller owns ICC.
 * <0 ICC error (caller may fall back), 1 read ok but no unique match (refuse). */
int ps5_hp_mode_from_bridge(struct ps5_hp *hp, const struct ps5_icc_io *io);
void ps5_hp_mode_default(struct ps5_hp *hp);   /* 1080p60 / VIC16 (source DEFAULT) */
/* v1.18: is hotplug supported for this converter (FLAVA3 or VERDE, PS5 Linux hdmi_ic_type)? */
static inline int ps5_hp_converter_ok(unsigned converter)
{ return converter == PS5_HP_CONVERTER_FLAVA3 || converter == PS5_HP_CONVERTER_VERDE; }
void ps5_hp_edid_fix(uint8_t *edid);           /* Linux fix_edid(): byte 0 = 0, block-0 checksum */
/* v1.13: pick the mode for an EDID (already fixed); never fails: unparsable EDID -> 1080p60, edid_ok 0. seq untouched. */
void ps5_hp_pick_from_edid(const uint8_t *edid, unsigned bytes, uint32_t mask, struct ps5_hp_pick *out);
/* v1.13: program the FLAVA3 bridge for mode_id (must be in PS5_HP_PICK_MASK): the boot stage list (6..10, 0..5, [14], 11..13)
 * with the mode's parameters incl. manual timing (ps5_flava3_video_stage_modex), one ICC transaction per gate pair, abort on
 * gate/unplug exactly like ps5_hp_replay. results[i] = result of stages[i] (only the first *n are written). Returns 0 or the
 * first failure (ICC/service result, PS5_HP_BUILD_FAIL, PS5_HP_ABORT_*, PS5_HP_NO_MODE for an unknown/unlisted id). */
int ps5_hp_program_mode(struct ps5_hp *hp, const struct ps5_icc_io *io, struct ps5_hdmi_commands *packet, uint32_t mode_id,
                        ps5_hp_gate gate, void *ctx, unsigned stages[PS5_HP_MAX_STAGES], int results[PS5_HP_MAX_STAGES],
                        unsigned *n);
#endif
