/* SPDX-License-Identifier: GPL-2.0-only
 * ps5pwr v1.4 hotplug core (UNTESTED ON HARDWARE, 14.11.2026). See ps5pwr-hotplug.h. */
#include "ps5pwr-hotplug.h"
#include "../../firmware/flava3-prepare.h"
#include "../../firmware/flava3-video.h"
#include "../../firmware/ps5-display-mode.h"
#include "../../firmware/ps5-hdmi-mode-var.h"

static void copy(uint8_t *d, const uint8_t *s, unsigned n) { for (unsigned i = 0; i < n; i++) d[i] = s[i]; }
static void zero(void *p, unsigned n) { for (unsigned i = 0; i < n; i++) ((uint8_t *)p)[i] = 0; }
static int same_bytes(const uint8_t *a, const uint8_t *b, unsigned n)
{
    for (unsigned i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

void ps5_hp_edid_fix(uint8_t *edid)
{
    uint32_t sum = 0;
    edid[0] = 0x00;                       /* "For some reason, PS5 sets 0x01." (Linux fix_edid) */
    for (unsigned i = 0; i < 0x7f; i++) sum += edid[i];
    edid[0x7f] = (uint8_t)(0u - sum);
}

/* ------------------------------------------------------------------ collector */
void ps5_hp_note(void *context, uint8_t service, uint16_t type, const uint8_t *payload, unsigned bytes)
{
    struct ps5_hp *hp = context;
    struct ps5_hp_event ev = { service, 0, 0, 0, type, 0, bytes };
    if (!hp) return;
    if (hp->extra_note) hp->extra_note(hp->extra_ctx, service, type, payload, bytes);   /* v1.10 */
    hp->c.notes++;
    if (service != PS5_HP_SERVICE_HDMI) { hp->c.other_service++; return; }
    hp->c.hdmi++;
    if (bytes < 4 || !payload) { hp->c.short_note++; return; }   /* Linux: length < sizeof(msg) + 4 */
    ev.event = payload[1];
    if (ev.event == PS5_HP_EV_UNPLUG) hp->unplug_seq++;
    if (ev.event == PS5_HP_EV_EDID) {
        unsigned size = payload[2] | (unsigned)payload[3] << 8;
        ev.size = (uint16_t)size;
        if (size >= 128 && size <= bytes - 4) {                    /* Linux: else "bad EDID size" */
            unsigned keep = size < PS5_HP_EDID_MAX ? size : PS5_HP_EDID_MAX;
            copy(hp->edid, payload + 4, keep);
            hp->edid_bytes = size;
            ev.edid_ok = 1;
        }
    }
    if (hp->q_count >= PS5_HP_QUEUE) { hp->c.queue_lost++; return; }
    hp->q[(hp->q_head + hp->q_count++) % PS5_HP_QUEUE] = ev;
}

void ps5_hp_arm_reset(struct ps5_hp *hp, uint64_t now_ms)
{
    hp->q_head = hp->q_count = 0;
    hp->pending = hp->unplugged = hp->baseline_done = hp->held = 0;
    hp->attempt = hp->ok_n = hp->window_msgs = 0;
    hp->due_ms = hp->guard_until_ms = hp->quiet_until_ms = 0;
    hp->armed_ms = hp->window_ms = now_ms;
    hp->last_result = 0; hp->last_stage = 0;
}

int ps5_hp_events(struct ps5_hp *hp, const struct ps5_icc_io *io, struct ps5_hdmi_commands *p, int on)
{
    if (ps5_hdmi_commands_init(p, 4) || ps5_hdmi_commands_event(p, 4, (uint8_t)(on != 0))) return PS5_HP_BUILD_FAIL;
    return ps5_hdmi_commands_submit(io, p, ps5_hp_note, hp);
}

/* ------------------------------------------------------------------ poll */
int ps5_hp_poll(struct ps5_hp *hp, const struct ps5_icc_io *io, uint64_t now_ms)
{
    unsigned n = 0;
    if (now_ms < hp->quiet_until_ms) return 0;
    if (now_ms - hp->window_ms >= PS5_HP_STORM_WINDOW_MS) { hp->window_ms = now_ms; hp->window_msgs = 0; }
    while (n < PS5_HP_POLL_MAX_MSGS) {
        int r = ps5_icc_poll(io, ps5_hp_note, hp);
        if (r <= 0) { if (r < 0) hp->c.poll_err++; break; }
        if (r == 2) hp->c.stale++;
        n++;
    }
    hp->window_msgs += n;
    if (hp->window_msgs > PS5_HP_STORM_MSGS) {
        hp->c.storms++;
        ps5_hp_log(hp->log_ctx, "hotplug: %u ICC messages within %u ms -> polling paused %u ms",
                   hp->window_msgs, (unsigned)(now_ms - hp->window_ms), PS5_HP_STORM_PAUSE_MS);
        hp->quiet_until_ms = now_ms + PS5_HP_STORM_PAUSE_MS;
        hp->window_ms = hp->quiet_until_ms; hp->window_msgs = 0;
    }
    return (int)n;
}

/* ------------------------------------------------------------------ state machine */
static void edid_event(struct ps5_hp *hp, const struct ps5_hp_event *ev, uint64_t now)
{
    unsigned size = ev->size, keep = size < PS5_HP_EDID_MAX ? size : PS5_HP_EDID_MAX;
    uint8_t raw0 = hp->edid[0];
    copy(hp->pub_edid, hp->edid, keep);
    ps5_hp_edid_fix(hp->pub_edid);
    int same = hp->boot_edid_bytes && size == hp->boot_edid_bytes && size <= PS5_HP_EDID_MAX &&
               same_bytes(hp->pub_edid, hp->boot_edid, size);
    unsigned v = (unsigned)hp->pub_edid[8] << 8 | hp->pub_edid[9];
    hp->pub_bytes = keep; hp->pub_same = same; hp->pub_new = 1;
    hp->c.edid++;
    if (same) hp->c.edid_same++; else hp->c.edid_diff++;
    ps5_hp_log(hp->log_ctx, "hotplug: EDID %u B %c%c%c/%04x byte0=%02x crc=%08x: %s", size,
               '@' + ((v >> 10) & 31), '@' + ((v >> 5) & 31), '@' + (v & 31),
               hp->pub_edid[10] | (unsigned)hp->pub_edid[11] << 8, raw0,
               ps5_hdmi_mode_crc32(hp->pub_edid, keep),
               same ? "same as boot" : (hp->boot_edid_bytes ? "DIFFERENT from boot" : "no boot EDID"));
    /* v1.13: pick + publish; the replay below still restores the current mode. v1.13.1: after a first pick the boot sink
     * is picked too, so the KMD gets its EDID back (Windows otherwise keeps the other monitor's descriptor). */
    if ((!same || hp->pick.seq) && hp->mode == PS5_HP_PICK) {
        uint32_t seq = hp->pick.seq + 1;
        ps5_hp_pick_from_edid(hp->pub_edid, keep, PS5_HP_PICK_MASK, &hp->pick);
        hp->pick.seq = seq; hp->pick_new = 1;
        ps5_hp_log(hp->log_ctx, "hotplug: other sink: picked mode %u (%s, %u Hz, offered %03x, edid_ok %u) seq %u",
                   hp->pick.mode_id, ps5_display_modes[hp->pick.mode_id].name, hp->pick.refresh_hz, hp->pick.offered_mask,
                   hp->pick.edid_ok, seq);
    } else if (!same) ps5_hp_log(hp->log_ctx, "hotplug: other sink: mode change not supported, current mode kept");
    if (hp->mode == PS5_HP_LOG) {
        hp->c.log_only++; hp->baseline_done = 1;
        ps5_hp_log(hp->log_ctx, "hotplug: log-only mode (HotplugMode=1): no replay");
        return;
    }
    if (hp->source == PS5_HP_SRC_NONE) {
        ps5_hp_log(hp->log_ctx, "hotplug: current mode unknown: no replay (firmware should publish Ps5HdmiMode)");
        return;
    }
    if (hp->held) {
        hp->c.held++;
        ps5_hp_log(hp->log_ctx, "hotplug: on hold after %u replays in %u s: waiting for an unplug event",
                   PS5_HP_RATE_MAX, PS5_HP_RATE_WINDOW_MS / 1000);
        return;
    }
    if (now < hp->guard_until_ms && !hp->unplugged) {
        hp->c.guard_ignored++;
        ps5_hp_log(hp->log_ctx, "hotplug: EDID %u ms after the last replay without unplug: ignored",
                   (unsigned)(now + PS5_HP_GUARD_MS - hp->guard_until_ms));
        return;
    }
    if (!hp->baseline_done && !hp->unplugged && same && now - hp->armed_ms < PS5_HP_ARM_MS) {
        hp->baseline_done = 1; hp->c.baseline++;
        ps5_hp_log(hp->log_ctx, "hotplug: initial EDID report %u ms after events-on: no replay",
                   (unsigned)(now - hp->armed_ms));
        return;
    }
    hp->baseline_done = 1;
    if (!hp->pending) hp->c.scheduled++;
    hp->pending = 1; hp->attempt = 0; hp->due_ms = now + PS5_HP_DEBOUNCE_MS;
    ps5_hp_log(hp->log_ctx, "hotplug: replay scheduled in %u ms", PS5_HP_DEBOUNCE_MS);
}

void ps5_hp_process(struct ps5_hp *hp, uint64_t now_ms)
{
    while (hp->q_count) {
        struct ps5_hp_event ev = hp->q[hp->q_head];
        hp->q_head = (hp->q_head + 1) % PS5_HP_QUEUE; hp->q_count--;
        if (ev.event == PS5_HP_EV_UNPLUG) {
            hp->c.unplug++; hp->unplugged = 1; hp->held = 0;
            ps5_hp_log(hp->log_ctx, "hotplug: unplug (event 01, %u B)%s", ev.bytes,
                       hp->pending ? ": pending replay cancelled" : "");
            hp->pending = 0;
        } else if (ev.event == PS5_HP_EV_EDID) {
            if (!ev.edid_ok) {
                hp->c.edid_bad++;
                ps5_hp_log(hp->log_ctx, "hotplug: EDID event with bad size %u (payload %u B): ignored", ev.size, ev.bytes);
            } else edid_event(hp, &ev, now_ms);
        } else {
            hp->c.other_event++;
            ps5_hp_log(hp->log_ctx, "hotplug: HDMI event %02x type %04x %u B: ignored", ev.event, ev.type, ev.bytes);
        }
    }
}

int ps5_hp_due(const struct ps5_hp *hp, uint64_t now_ms)
{
    return hp->pending && hp->mode >= PS5_HP_REPLAY_BOOT && hp->source != PS5_HP_SRC_NONE &&
           now_ms >= hp->due_ms && now_ms >= hp->quiet_until_ms;
}

/* ------------------------------------------------------------------ replay */
unsigned ps5_hp_stage_list(const struct ps5_hp *hp, unsigned out[PS5_HP_MAX_STAGES])
{
    /* ps5-native-dp-video.h: Ps5NativeDpReset 6,7,8 / Ps5NativeDpVideo 9,10, DCN stream setup (skipped:
     * the DCN keeps running), 0..5, 14 if geometry, 11, 12, 13. PS5_HP_REPLAY_SHORT: PLAN.md Stage 4
     * (0..5, 14, 11 and the audio pair 12/13), i.e. without HDCP-off/quiesce/ISR/DP-input/VSync. */
    unsigned n = 0;
    if (hp->mode != PS5_HP_REPLAY_SHORT) { out[n++] = 6; out[n++] = 7; out[n++] = 8; out[n++] = 9; out[n++] = 10; }
    for (unsigned s = 0; s <= 5; s++) out[n++] = s;
    if (!hp->bridge_null && hp->bridge.uw_geometry) out[n++] = 14;
    out[n++] = 11; out[n++] = 12; out[n++] = 13;
    return n;
}

/* one firmware stage exactly as HdmiProbeDxe VideoStage(): 8..10 = ps5_flava3_prepare_stage(stage-8),
 * otherwise ps5_flava3_video_stage_mode(mode or NULL); stage 6 is preceded by stopHdcpHw (ICC 2/0x1f). */
static int run_stage(struct ps5_hp *hp, const struct ps5_icc_io *io, unsigned stage, struct ps5_hdmi_commands *p)
{
    int r = (stage >= 8 && stage <= 10) ? ps5_flava3_prepare_stage(&hp->cfg, stage - 8, p)
          : ps5_flava3_video_stage_mode(&hp->cfg, stage, hp->bridge_null ? 0 : &hp->bridge, p);
    if (r) return PS5_HP_BUILD_FAIL;
    if (stage == 6) {
        r = ps5_icc_query_events(io, 2, 0x1f, 0, 0, 0, 0, 0, ps5_hp_note, hp);
        if (r) return r;
    }
    return ps5_hdmi_commands_submit(io, p, ps5_hp_note, hp);
}

int ps5_hp_replay(struct ps5_hp *hp, const struct ps5_icc_io *io, struct ps5_hdmi_commands *packet,
                  ps5_hp_gate gate, void *ctx)
{
    unsigned st[PS5_HP_MAX_STAGES], n = ps5_hp_stage_list(hp, st);
    uint32_t seq = hp->unplug_seq;
    int result = 0;
    if (hp->source == PS5_HP_SRC_NONE) return PS5_HP_NO_MODE;
    hp->c.replays++; hp->last_stage = 0;
    ps5_hp_log(hp->log_ctx, "hotplug: replay %u (attempt %u/%u, %s list, %u stages, mode %u%s)",
               hp->c.replays, hp->attempt + 1, PS5_HP_MAX_ATTEMPTS,
               hp->mode == PS5_HP_REPLAY_SHORT ? "short" : "boot", n, hp->mode_id,
               hp->bridge_null ? " VIC16" : "");
    for (unsigned i = 0; i < n; i++) {
        if (gate && gate(ctx, i, st[i], 0)) { result = PS5_HP_ABORT_STOP; break; }
        int r = run_stage(hp, io, st[i], packet);
        int unplug = hp->unplug_seq != seq;
        if (gate) gate(ctx, i, st[i], 1);
        ps5_hp_log(hp->log_ctx, "hotplug: stage %u result %d", st[i], r);
        if (r) { hp->last_stage = st[i]; result = r; break; }
        if (unplug) { hp->last_stage = st[i]; result = PS5_HP_ABORT_UNPLUG; break; }
    }
    return result;
}

void ps5_hp_replay_done(struct ps5_hp *hp, int result, uint64_t now_ms)
{
    hp->last_result = result;
    if (result == 0) {
        hp->c.replay_ok++; hp->pending = 0; hp->unplugged = 0; hp->guard_until_ms = now_ms + PS5_HP_GUARD_MS;
        if (hp->ok_n < PS5_HP_RATE_MAX) hp->ok_ms[hp->ok_n++] = now_ms;
        else { for (unsigned i = 1; i < PS5_HP_RATE_MAX; i++) hp->ok_ms[i - 1] = hp->ok_ms[i]; hp->ok_ms[PS5_HP_RATE_MAX - 1] = now_ms; }
        ps5_hp_log(hp->log_ctx, "hotplug: replay ok; EDID events ignored for %u ms unless unplugged", PS5_HP_GUARD_MS);
        if (hp->ok_n == PS5_HP_RATE_MAX && now_ms - hp->ok_ms[0] < PS5_HP_RATE_WINDOW_MS) {
            hp->held = 1;
            ps5_hp_log(hp->log_ctx, "hotplug: %u replays within %u ms: hold until the next unplug event",
                       PS5_HP_RATE_MAX, (unsigned)(now_ms - hp->ok_ms[0]));
        }
        return;
    }
    if (result == PS5_HP_ABORT_UNPLUG || result == PS5_HP_ABORT_STOP || result == PS5_HP_NO_MODE) {
        hp->c.replay_abort++; hp->pending = 0;
        ps5_hp_log(hp->log_ctx, "hotplug: replay aborted (%d) at stage %u", result, hp->last_stage);
        return;
    }
    hp->attempt++;
    if (hp->attempt < PS5_HP_MAX_ATTEMPTS) {
        unsigned wait = 1000u << (hp->attempt - 1);
        hp->c.retries++; hp->due_ms = now_ms + wait;
        ps5_hp_log(hp->log_ctx, "hotplug: replay failed at stage %u (%d): retry %u/%u in %u ms",
                   hp->last_stage, result, hp->attempt + 1, PS5_HP_MAX_ATTEMPTS, wait);
    } else {
        hp->c.replay_fail++; hp->pending = 0; hp->guard_until_ms = now_ms + PS5_HP_GUARD_MS;
        ps5_hp_log(hp->log_ctx, "hotplug: replay failed %u times (last %d at stage %u): waiting for the next event",
                   PS5_HP_MAX_ATTEMPTS, result, hp->last_stage);
    }
}

/* ------------------------------------------------------------------ v1.13 pick + program for a mode id */
void ps5_hp_pick_from_edid(const uint8_t *edid, unsigned bytes, uint32_t mask, struct ps5_hp_pick *out)
{
    struct ps5_edid_info info;
    uint32_t seq = out->seq;
    unsigned keep = bytes < PS5_HP_EDID_MAX ? bytes : PS5_HP_EDID_MAX;
    for (unsigned i = 0; i < sizeof *out; i++) ((uint8_t *)out)[i] = 0;
    out->seq = seq;
    copy(out->edid, edid, keep); out->edid_bytes = keep;
    out->mode_id = PS5_MODE_1080P60;
    if (ps5_edid_parse(edid, bytes, &info) == 0) {
        out->edid_ok = 1;
        for (unsigned id = 0; id < PS5_MODE_COUNT; id++)
            if ((mask & (1u << id)) && ps5_display_offered(&info, (enum ps5_mode_id)id) == 1) out->offered_mask |= 1u << id;
        out->mode_id = (uint32_t)ps5_display_select_refresh_max(&info, mask);
    }
    if (out->mode_id >= PS5_MODE_COUNT || !(mask & (1u << out->mode_id))) out->mode_id = PS5_MODE_1080P60;
    out->t = ps5_display_modes[out->mode_id].t;
    out->refresh_hz = ps5_display_modes[out->mode_id].refresh_hz;
}

int ps5_hp_program_mode(struct ps5_hp *hp, const struct ps5_icc_io *io, struct ps5_hdmi_commands *packet, uint32_t mode_id,
                        ps5_hp_gate gate, void *ctx, unsigned stages[PS5_HP_MAX_STAGES], int results[PS5_HP_MAX_STAGES],
                        unsigned *n)
{
    struct ps5_flava3_mode_x mx;
    uint32_t seq = hp->unplug_seq;
    unsigned count = 0, done = 0;
    int result = 0;
    *n = 0;
    if (mode_id >= PS5_MODE_COUNT || !(PS5_HP_PICK_MASK & (1u << mode_id)) || ps5_flava3_modex_for((enum ps5_mode_id)mode_id, &mx))
        return PS5_HP_NO_MODE;
    /* v1.13.2: the caller (KMD mode change) owns the bridge now: a replay of the old mode that is still scheduled would
     * put the old recipe back under the new DCN timing */
    if (hp->pending) ps5_hp_log(hp->log_ctx, "hotplug: program: scheduled replay cancelled");
    hp->pending = 0;
    for (unsigned s = 6; s <= 10; s++) stages[count++] = s;
    for (unsigned s = 0; s <= 5; s++) stages[count++] = s;
    if (mx.m.uw_geometry) stages[count++] = 14;
    stages[count++] = 11; stages[count++] = 12; stages[count++] = 13;
    ps5_hp_log(hp->log_ctx, "hotplug: program bridge for mode %u (%s, %u stages%s)", mode_id, ps5_display_modes[mode_id].name,
               count, mx.manual ? ", manual timing" : "");
    for (unsigned i = 0; i < count; i++) {
        unsigned stage = stages[i];
        if (gate && gate(ctx, i, stage, 0)) { result = PS5_HP_ABORT_STOP; break; }
        int r = (stage >= 8 && stage <= 10) ? ps5_flava3_prepare_stage(&hp->cfg, stage - 8, packet)
              : ps5_flava3_video_stage_modex(&hp->cfg, stage, &mx, packet);
        if (r) r = PS5_HP_BUILD_FAIL;
        else {
            if (stage == 6) r = ps5_icc_query_events(io, 2, 0x1f, 0, 0, 0, 0, 0, ps5_hp_note, hp);   /* stopHdcpHw */
            if (!r) r = ps5_hdmi_commands_submit(io, packet, ps5_hp_note, hp);
        }
        int unplug = hp->unplug_seq != seq;
        if (gate) gate(ctx, i, stage, 1);
        results[i] = r; done = i + 1;
        ps5_hp_log(hp->log_ctx, "hotplug: program stage %u result %d", stage, r);
        if (r) { hp->last_stage = stage; result = r; break; }
        if (unplug) { hp->last_stage = stage; result = PS5_HP_ABORT_UNPLUG; break; }
    }
    *n = done;
    if (!result && done == count) {   /* v1.13.2: the programmed mode is the current mode of later hotplug replays */
        hp->mode_id = mode_id;
        hp->bridge_null = mode_id == PS5_MODE_1080P60;          /* the firmware runs 1080p60 with VIC16 packets */
        if (hp->bridge_null) zero(&hp->bridge, sizeof(hp->bridge)); else copy((uint8_t *)&hp->bridge, (const uint8_t *)&mx.m, sizeof(hp->bridge));
        hp->source = mx.manual ? PS5_HP_SRC_NONE : PS5_HP_SRC_PROGRAM;   /* manual timing: recognised, never replayed (v1.10.1) */
        ps5_hp_log(hp->log_ctx, "hotplug: mode %u is now the current mode%s", mode_id, mx.manual ? " (manual timing: no replay)" : "");
    }
    return result;
}

/* ------------------------------------------------------------------ current mode */
/* v1.10.1: the replay builds stage 2 from the base recipe only; a manual-timing mode (r5.13 720p144: m7042/m7063/m7078)
 * would come back with the wrong bridge timing, so it is recognised but never replayed */
static int manual_mode(uint32_t id)
{
    struct ps5_flava3_mode_x x;
    return id < PS5_MODE_COUNT && !ps5_flava3_modex_for((enum ps5_mode_id)id, &x) && x.manual;
}

int ps5_hp_mode_from_var(struct ps5_hp *hp, const uint8_t *var, unsigned bytes)
{
    struct ps5_hdmi_mode_var v;
    int r = ps5_hdmi_mode_check(var, bytes, &v);
    if (r) return r;
    if (manual_mode(v.mode_id)) {
        ps5_hp_log(hp->log_ctx, "hotplug: Ps5HdmiMode: mode %u %ux%u@%u uses manual bridge timing: replay not supported",
                   v.mode_id, v.width, v.height, v.refresh_hz);
        return 1;
    }
    hp->bridge_null = !(v.flags & PS5_HDMI_MODE_FLAG_BRIDGE);
    if (hp->bridge_null) zero(&hp->bridge, sizeof(hp->bridge));
    else copy((uint8_t *)&hp->bridge, (const uint8_t *)&v.bridge, sizeof(hp->bridge));
    hp->mode_id = v.mode_id; hp->source = PS5_HP_SRC_VAR;
    ps5_hp_log(hp->log_ctx, "hotplug: Ps5HdmiMode: mode %u %ux%u@%u %u kHz 7227=%02x 7070[5]=%02x high=%u geometry=%u%s",
               v.mode_id, v.width, v.height, v.refresh_hz, v.clock_khz, v.bridge.reg7227[0], v.bridge.reg7070[5],
               v.bridge.high_tmds, v.bridge.uw_geometry, hp->bridge_null ? " (VIC16 packets)" : "");
    return 0;
}

void ps5_hp_mode_default(struct ps5_hp *hp)
{
    zero(&hp->bridge, sizeof(hp->bridge));
    hp->bridge_null = 1; hp->mode_id = PS5_MODE_1080P60; hp->source = PS5_HP_SRC_DEFAULT;
}

/* Which whitelist recipe does the bridge run? 0x7227 discriminates all of them (VIC16 16, VIC63 63,
 * 1440 stock 4, VIC97 6 masked to 97 by stage 5, 3440 5); for 3440 the vertical total 0x7056 (written by
 * stage 14) tells 60 from 100 Hz. Identical parameter sets (1440p60 and the UGREEN 1440p60 DTD) count
 * as one. 0x7228 (stage 5 sets 0x30) and 0x7072/0x7074 (high-TMDS writes) are not compared. */
int ps5_hp_mode_from_bridge(struct ps5_hp *hp, const struct ps5_icc_io *io)
{
    uint8_t r7227 = 0, r7070[6] = { 0 }, r7056[2] = { 0 }, r7063[11] = { 0 };
    int have7056 = 0, have7063 = 0, found = -1, r;
    struct ps5_flava3_mode m, hit;
    struct ps5_flava3_mode_x x;
    zero(&hit, sizeof(hit));
    if ((r = ps5_hdmi_register_read(io, 0x7227, 1, &r7227, ps5_hp_note, hp))) return r;
    if ((r = ps5_hdmi_register_read(io, 0x7070, 6, r7070, ps5_hp_note, hp))) return r;
    for (int id = 0; id < PS5_MODE_COUNT; id++) {
        if (ps5_flava3_modex_for((enum ps5_mode_id)id, &x)) continue;
        m = x.m;
        if (r7227 != (m.vic97_mask ? 97 : m.reg7227[0])) continue;
        if (m.uw_geometry) {
            if (!have7056) {
                if ((r = ps5_hdmi_register_read(io, 0x7056, 2, r7056, ps5_hp_note, hp))) return r;
                have7056 = 1;
            }
            if (!same_bytes(r7056, m.uw_vtotal, 2)) continue;
        }
        if (x.manual) {   /* v1.10.1: manual-timing recipes (r5.13 720p144) share 0x7227=5 with the ultrawide modes */
            if (!have7063) {
                if ((r = ps5_hdmi_register_read(io, 0x7063, 11, r7063, ps5_hp_note, hp))) return r;
                have7063 = 1;
            }
            if (!same_bytes(r7063, x.m7063, 11)) continue;
        }
        if (found >= 0 && !same_bytes((const uint8_t *)&hit, (const uint8_t *)&m, sizeof(m))) {
            ps5_hp_log(hp->log_ctx, "hotplug: bridge 7227=%02x matches modes %d and %d: ambiguous", r7227, found, id);
            return 1;
        }
        if (found < 0) { found = id; hit = m; }
    }
    ps5_hp_log(hp->log_ctx, "hotplug: bridge readback 7227=%02x 7070=%02x %02x %02x %02x %02x %02x 7056=%02x%02x -> %s",
               r7227, r7070[0], r7070[1], r7070[2], r7070[3], r7070[4], r7070[5], r7056[1], r7056[0],
               found >= 0 ? ps5_display_modes[found].name : "no whitelist recipe");
    if (found < 0) return 1;
    if (manual_mode((uint32_t)found)) {
        ps5_hp_log(hp->log_ctx, "hotplug: bridge runs %s (manual timing): replay not supported", ps5_display_modes[found].name);
        return 1;
    }
    hp->bridge = hit; hp->bridge_null = found == PS5_MODE_1080P60;
    if (hp->bridge_null) zero(&hp->bridge, sizeof(hp->bridge));
    hp->mode_id = (uint32_t)found; hp->source = PS5_HP_SRC_BRIDGE;
    return 0;
}
