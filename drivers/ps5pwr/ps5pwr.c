/*
 * ps5pwr v1.19.2 (26.11.2026, user: the fan went up briefly at every restart / on short peaks): a restart through the PSWloader
 *       warm re-entry keeps the quiet table under EMC auto (GameOS table only at a real power-off); the curve goes up only after
 *       10 s of asking for more (from 80 C at once, emergency maximum at 90 C unchanged).
 * ps5pwr v1.19.1 (26.11.2026): QUERY_STOP/QUERY_REMOVE refused + PNP_DEVICE_NOT_DISABLEABLE (new DriverVer: the Windows driver
 *       store keys packages by their INF - 1.19.0 without the refusal was already on a test console).
 * ps5pwr v1.19.0 (26.11.2026, user: fan off or as quiet as in GameOS on the desktop): quiet curve keeps the minimum (10 %)
 *       up to 66 C, 30 % at 70 C, 55 % at 75 C, from 80 C the PS5 Linux values (ps5pwr-fancurve.c); QUERY_STOP/QUERY_REMOVE
 *       refused and PNP_DEVICE_NOT_DISABLEABLE - 104D:9107 is never stopped while Windows runs (a live reinstall reset the
 *       USB boot disk and ended in BUGCODE_USB3_DRIVER, HW 26.11.).
 * ps5pwr v1.18.4 (26.11.2026): the firmware (r5.28.10) puts up the quiet fan table with the power preset's servo target
 *       (50..80 C) instead of 50 C; that table still counts as the quiet one (ps5_fc_quiet_left), so the GameOS table from
 *       PswFanOriginal is what goes back at shutdown.
 * ps5pwr v1.18.3 (25.11.2026): IOCTL_PS5PWR_BRIDGE_READ (0x90F, read only, diagnostics) - bridge registers for the fast-to-slow
 *       mode change analysis (ps5pwr.ps1 -BridgeRead "7072:1,7c00:1").
 * ps5pwr v1.18.2 (25.11.2026, HW UGREEN -> 1280x720@144 "Not Supported"): the pick for a new monitor follows the firmware
 *       default refresh=native (highest refresh at the native resolution: Dell 3440x1440@100, UGREEN 1080p60), not refresh=max.
 * ps5pwr v1.18.1 (25.11.2026, HW UGREEN -> Dell): a new sink with a fresh pick (HotplugMode 4) waits 6 s for the KMD mode change
 *       before the old mode is replayed (the Dell showed 1080p for ~5 s before the KMD switched to 3440x1440@100).
 * ps5pwr v1.18.0 (25.11.2026, GitHub issue #13 "no picture after the TV was off"): HDMI hotplug like PS5 Linux, generic -
 *       FLAVA3 and VERDE (PS5 Slim, was "inactive (converter is not FLAVA3)"), manual-timing modes replayed too, and ON BY
 *       DEFAULT: a missing Parameters\HotplugMode means 4 (replay the current mode after every replug / sink power cycle,
 *       pick the new sink's mode for the KMD). HotplugMode=0 still switches it off. caps 0x4000.
 * ps5pwr v1.17.2 (24.11.2026): firmware r5.26.2 puts the quiet backup servo table on the EMC at power-on; the GameOS table
 *       comes from the volatile variable PswFanOriginal and is restored at stop as before.
 * ps5pwr v1.17.1 (24.11.2026): fan curve really starts (1.17.0 never read FanCurve); registry writes outside icc_lock.
 * ps5pwr v1.17.0 (24.11.2026): PS5 Linux fan control 1:1 (drivers/ps5/fan.c, ps5pwr-fancurve.c): board EMC servo table,
 *       quiet backup table, quiet curve 50..87 C -> PWM 26..255 every 500 ms with smoothing, slew limit, stop below 45 C
 *       and emergency maximum at 90 C; original table + firmware auto at stop/S5/restart. Parameters\FanCurve 0 = off.
 *       caps 0x2000.
 * ps5pwr v1.16.0 (19.11.2026): a restart (PowerActionShutdownReset) is left to Windows when the firmware publishes the
 *       volatile variable PswWarmReentry (PSWloader firmware r5.25.5+ with warm re-entry): no ICC reboot, the S5 IRP goes
 *       down normally and Windows restarts through the firmware's ResetSystem, which starts PSWloader again instead of
 *       GameOS (HW 19.11.: the ICC reboot here sent every Windows restart to GameOS before ResetSystem was called).
 *       Shutdown (power off) keeps the ICC path. caps 0x1000.
 * ps5pwr v1.15.1 (BAPM op 3) / v1.15.0 (18.11.2026, UNTESTED ON HARDWARE; night task "real OC mode"): the PS5 SMU answers 0xFE to every BC-250
 *       OC message of v1.14.0 (0x38/0x39/0x3B/0x3D, queue 3 0x43/0x8F/0x50) - its table is the 15 messages of PS5 Linux mp1.c.
 *       New op 21 = boost window (UniversalModeEntry) with C2PMSG_58 = gpu_max << 16 | cpu_max, gpu_max 2230..2400,
 *       cpu_max 3500..3800 (smu-mbox v0.9 ps5_smu_boost_enter_p3); P1/P2/P4 unchanged, voltage stays with the SMU curve,
 *       op 3 (UniversalModeExit) leaves it. caps 0x400. HW 05:21: the SMU answers 0xFF to any ceiling above 2230/3500
 *       (even 2231 or 3501) - the window cannot go above stock. IOCTL_PS5PWR_BAPM op 3 {op, gfx_pct 85..100} = the BAPM
 *       init of op 1 with every GFX CAC weight scaled (signed 16-bit halves, ps5_bapm_scale_gfx; 100 = table bit for bit):
 *       the SMU estimates less GPU power and holds higher clocks in its unchanged budget. caps 0x800.
 * ps5pwr v1.14.0 (18.11.2026, UNTESTED ON HARDWARE; user night task "real OC mode"): IOCTL_PS5PWR_GFXCLK ops 7..20
 *       (smu-mbox v0.8): GetGfxVid, GetEnabledSmuFeatures, ForceGfxFreq 500..2600, UnforceGfxFreq, ForceGfxVid 700..1129 mV
 *       (never above, VID rounded towards lower voltage), UnforceGfxVid, GPU unforce; queue 3: GetCpuVoltage, GetCoreFreq,
 *       SetMaxCpuBoostClk 3200..4000, ScaleFVidCurve -50..0, SetCpu/GpuMaxTemperature 70..95, CPU OC reset (3500 / 0). Only on
 *       request; whatever was changed is taken back at stop, remove, unload, S5 (PASSIVE, mailbox lock) and in a bugcheck
 *       callback (port 0xCF8 config access, skipped while the mailbox lock is held). VERSION caps |= 0x200;
 * ps5pwr v1.13.2 (17.11.2026; v1.13.2: a successful IOCTL_PS5PWR_HP_PROGRAM makes its mode the current mode of later
 *       hotplug replays and cancels a scheduled replay of the old mode (KMD v10.18 mode change); v1.13.1: in HotplugMode 4 the boot sink is picked again after a first pick (KMD v10.17 K2
 *       gets the boot EDID back); v1.6 14.11.2026) - PS5 platform power/indicator driver for native Windows 11.
 *
 * Function driver for the Salina glue function Sony 104D:9107 (bus 0x40 fn 5), which carries the ICC
 * mailbox (BAR4) and doorbell (BAR2 + 0x108000) - the channel PS5 Linux (drivers/ps5/spcie.c,
 * arch/x86/platform/ps5/ps5.c) uses for power and the front LED. Windows cannot power the console off
 * by itself: ACPI S5 has no effect on the PS5, so a shut down Windows kept drawing ~150 W.
 *
 *  - start:    power LED white/dim like PS5 Linux spcie_icc_init() (otherwise the GameOS hand-off pattern,
 *              pulsing blue = "still booting", stays on)
 *  - shutdown: on the system set-power IRP for PowerSystemShutdown (sent after all IRP_MJ_SHUTDOWN
 *              notifications, i.e. after file systems and disk caches were flushed) the console is switched
 *              off - or rebooted for PowerActionShutdownReset - with the exact ICC sequence of
 *              icc_power_shutdown()/icc_power_reboot() (icc-power.c: ps5_icc_power)
 *
 * v1.1: admin-only control device \\.\Ps5Pwr - IOCTL_PS5PWR_STATUS (LED result, S5 IRPs seen, last ICC power
 * result) and IOCTL_PS5PWR_POWER (0 = off, 1 = reboot, immediately) so the ICC power path can be proven at
 * runtime (fast4: driver started, but the console stayed at ~150 W after shutdown /s).
 *
 * v1.2: fast5 showed led_result -2 (mailbox never idle). The mailbox/doorbell windows are now chosen by the
 * BAR4/BAR2 addresses read from PCI config space (IRP_MN_READ_CONFIG) instead of "first small window", pending
 * ICC messages are drained first, and a mailbox left busy across the firmware hand-off is reset the way PS5
 * Linux icc_send() starts every message (SOR=0 ... SOW=1) before one retry. Raw SOW/SOR go into the status.
 *
 * v1.4 (UNTESTED ON HARDWARE, 14.11.2026): HDMI hotplug. When the sink is unplugged/replugged at runtime the
 * EMC mutes the FLAVA3 output and Windows never programmed it again (picture black until reboot, 13.11. 23:00).
 * A system thread polls ICC notifications every 250 ms (one receive per message under icc_lock, at most 8 per
 * tick) and, on an HDMI EDID notification (service 0x10, event 0x02), replays the firmware's FLAVA3 stages for
 * the CURRENT mode (ps5-native-dp-video.h order without the DCN part: 6,7,8,9,10,0..5,[14],11,12,13); the DCN
 * keeps running, like the PS5 Linux ultrawide reconnect path. The state machine (debounce 500 ms, 3 s guard,
 * bounded retries, rate hold, storm pause) is ps5pwr-hotplug.c, host-tested with a mocked mailbox.
 *  - switch: HKLM\SYSTEM\CurrentControlSet\Services\ps5pwr\Parameters\HotplugMode (DWORD, re-read every
 *    second, no reboot needed): 0 off (DEFAULT, the thread never touches ICC), 1 log only (HDMI events on, no
 *    replay), 2 replay boot stage list, 3 replay short list (0..5,[14],11,12,13)
 *  - preconditions: UEFI variable Ps5HdmiEdid (firmware with --hdmi-edid-request, edid6+) and a FLAVA3 converter
 *  - current mode: UEFI variable Ps5HdmiMode (proposal, ps5-hdmi-mode-var.h), else the bridge's own 0x7227
 *    profile (+0x7056), else 1080p60 if the readback failed; an unknown profile refuses the replay
 *  - log: DbgPrintEx (IHVDRIVER) + 128-line ring, IOCTL_PS5PWR_HOTPLUG (ps5pwr.ps1 -Hotplug), System event
 *    log (ErrorCode 0x40000010 replay, 0x40000011 arm); last EDID event in Parameters\HotplugLastEdid
 *  - the ICC transport is built with PS5_ICC_LINUX_NOTIFY (EMC notifications carry magic 0x24, accepted like
 *    PS5 Linux with a length check only), for every ps5pwr ICC user including the S5 power path
 *
 * The ICC wire protocol lives in ../../firmware/icc-power.c (shared with the UEFI HdmiProbeDxe, unit-tested).
 * v1.5: admin-only MainSoC fan target read/write (ICC service 0x0a, curset GET/SET), plus read-only Tctl
 *       from AMD SMN 0x59800 through host bridge config 0x60/0x64. Fan SET preserves all six curset fields,
 *       bounds targets to 60..100 C, and verifies readback; SMN access restores the previous index.
 * v1.6: match PS5 Linux's 12 s ICC timeout for fan commands (instead of the 2 s LED default), accept up to
 *       128 response payload bytes, and map ICC timeout/buffer/data errors to distinct Windows status codes.
 * v1.13.0 (17.11.2026, UNTESTED ON HARDWARE; G05 hotplug with resolution change, ps5pwr part): HotplugMode 4 = as 2 (replay of
 *       the current mode) plus, for an EDID that differs from the boot EDID, the mode for that sink with the firmware's
 *       refresh=max rule over the release replay set (PS5_HP_PICK_MASK); published as IOCTL_PS5PWR_HP_PICK 0x90D (EDID,
 *       mode id, timing, seq), registry HotplugPickModeId/HotplugPickSeq and the auto-reset event \BaseNamedObjects\PswHotplug
 *       (SYSTEM + Administrators; the KMD waits on it and reads the pick through the IOCTL). IOCTL_PS5PWR_HP_PROGRAM 0x90E
 *       programs the FLAVA3 bridge for a mode id (boot stage list with the mode's parameters incl. manual timing, one ICC
 *       transaction per stage, abort/unplug rules of the replay) and returns every stage result; never called automatically.
 *       Replay and program never interleave (hp_seq). Caps 0x100.
 * v1.12.1 (17.11.2026): IOCTL_PS5PWR_VERSION 0x90A (FILE_READ_ACCESS, no started device needed) = {size 20, major, minor,
 *       patch, caps} in the layout the ThermalManager already reads (v1.5.0+): caps 0x08 boost window, 0x10 fan target 50..100,
 *       0x20 fan status, 0x40 BAPM init, 0x80 boost HOLD + above-stock request. The HW test of v1.12.0 got error 87 for op 5
 *       because the console still ran v1.10.3 (file hash at ImagePath said 1.12.0) - now the LOADED version is asked.
 *       GFXCLK op table moved to smu-mbox v0.7 ps5_smu_gfxclk_decode (host-tested).
 * v1.12.0 (17.11.2026, UNTESTED ON HARDWARE; user-approved OC package a+b; version above the unpublished 1.11.x images
 *       the ThermalManager knows by hash): IOCTL_PS5PWR_GFXCLK op 5 = boost window HOLD (C2PMSG_57 = (2230<<16)|3200, the
 *       window then keeps GPU 2230 / CPU up to 3500), op 6 = RequestGfxclk 500..2400 MHz (above 2230 only here) with an
 *       immediate QueryGfxclk + GetGfxFrequency readback (PS5PWR_GFXCLK_OUT2 when the output buffer is large enough).
 *       Ops 0..4 unchanged (op 1 still capped at 2230). smu-mbox v0.6.
 * v1.10.3 (17.11.2026, UNTESTED ON HARDWARE): IOCTL_PS5PWR_BAPM 0x90C - op 1 = BAPM init exactly as PS5 Linux mp1_init
 *       (UniversalModeExit, WC CPU mode 1 over the test mailbox, per-core DPM_WAC MSRs of parameter set 2, 80 L3 + 64 GFX
 *       CAC weights, 155 BAPM params; smu-mbox v0.5, ps5-bapm-set2.h copied verbatim), op 2 = read the WC CPU mode only.
 *       Nothing runs at load; ThermalManager or ps5pwr.ps1 -BapmInit calls it. Undo: restart (GameOS sets its own values).
 * v1.10.2 (17.11.2026): release build = v1.10.1 + the shared hdmi-commands.c register read with a 128-byte reply
 *       buffer and the Linux echo checks (the EMC pads to 28+count). Reproducible: PS5PWR_EPOCH=1794877200.
 * v1.10.1 (16.11.2026): source synced into the repo and built against the firmware r5.16 shared files; the hotplug
 *       bridge readback tells the r5.13 manual-timing 720p144 (0x7227=5 like 3440) apart by 0x7063 and refuses to replay
 *       manual-timing modes (replay builds stage 2 from the base recipe only). Reproducible: PS5PWR_EPOCH=1794862800.
 * v1.10 (16.11.2026; F20 + listener test passed on hardware 16.11.): power button under Windows. The firmware switches the ICC button
 *       notifications off at ReadyToBoot (F20: a queued press made the next shutdown hang without S5). ps5pwr switches
 *       them on again (service 0x08 type 1 {0,1}, as HdmiProbeDxe) from the existing poll thread (250 ms), takes them
 *       through the ps5_hp_note tap, and on a short press (<2 s, button-keys.c) sets the auto-reset event
 *       \BaseNamedObjects\Ps5PowerButton (Global\Ps5PowerButton, SYSTEM + Administrators only); the user-mode listener
 *       runs the Windows power-button action. A press not consumed within 5 s is cleared. Long presses are ignored.
 *       Off ({0,0}) + drain: leaving S0 (S5/restart/Sx set-power IRP, before the bus), IOCTL power, thread exit
 *       (stop/remove/unload), failed enable, kill switch, and a bugcheck callback (HIGH_LEVEL, skipped while an ICC owner
 *       holds the mailbox). Kill switch: Parameters\EnablePowerButton DWORD (default 1, 0 = off, re-read every second).
 *       Status: IOCTL_PS5PWR_BUTTON (0x909, ps5pwr.ps1 -Button), System log ErrorCode 0x40000012, Parameters\PowerButtonPresses.
 * v1.8.2 (16.11.2026): fan target 50..100 C (was 60..100); 50 C is the TARGETTEMP PS5 Linux writes in its quiet
 *       autoservo override (fan.c/autoservo_param.h {0, 50 * 256}, PS5_TARGETTEMP_SHIFT 8), so the EMC accepts it.
 * v1.8.1 (16.11.2026): Tctl read retried once inside the locked SMN index window (0 / all-ones / failed read).
 *       All SMN index/data pairs and SMU mailbox sequences were already serialised by g_smn_lock (one owner of cfg 0x60/0x64).
 * v1.8 (16.11.2026; UNTESTED ON HARDWARE): IOCTL_PS5PWR_GFXCLK op 2 = boost window enter (PS5 Linux
 *       MP1_BOOST_ENTER: C2PMSG_56..59 fixed, UniversalModeEntry 0x22), op 3 = exit (0x23), op 4 = GetGfxFrequency 0x37.
 *       Lifts the SMU window to GPU 2000..2230 / CPU 3200..3500 MHz for the OC-Mode; RequestGfxclk stays capped at 2230.
 * v1.7.1: smu-mbox v0.3 - ARG2 (C2PMSG_83) cleared before MSG, response checked as (resp & 0xff) == 1, waits 200 ms.
 * v1.7 (15.11.2026; UNTESTED ON HARDWARE): GPU clock through the PS5 SMU (MP1), as PS5 Linux gpufreq.c /
 *       mp1.c: IOCTL_PS5PWR_GFXCLK {op 0 = QueryGfxclk 0x0F, 1 = RequestGfxclk 0x0E with 500..2230 MHz}. The mailbox
 *       (smu-mbox.c, host-tested allow-list, poisoned after a timeout) runs under g_smn_lock, the same lock as the
 *       Tctl read, and every SMN access saves and restores the host bridge index - one owner of cfg 0x60/0x64.
 * GPL-2.0-only like the Linux code it follows.
 */
#include <ntifs.h>   /* v1.10: ZwCreateEvent, RtlCreateAcl, SeExports (superset of ntddk.h) */
#include <wdmsec.h>
#include <ntstrsafe.h>
#include <stdarg.h>
#include "../../firmware/icc-power.h"
#include "../../firmware/ps5-hdmi-mode-var.h"
#include "ps5pwr-hotplug.h"
#include "ps5pwr-sensors.h"
#include "smu-mbox.h"
#include "../../firmware/button-keys.h"
#include "ps5pwr-fancurve.h"

#define PS5PWR_TAG 'rwp5'
#define DOORBELL_OFF 0x108000u
#define LED_TYPE 0x20
static const UCHAR k_white_dim[25] = { 0x03,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x02,0x00,0x11,
                                        0x01,0x00,0x20,0x00,0x01,0x00,0x12,0x01,0x00,0x00,0x00,0x02,0x00 };

#define IOCTL_PS5PWR_STATUS CTL_CODE(0x8002, 0x900, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_PS5PWR_POWER CTL_CODE(0x8002, 0x901, METHOD_BUFFERED, FILE_WRITE_ACCESS)
/* 0x902 stays reserved for the hdmi-audio draft (hdmi-audio draft 03, not applied) */
#define IOCTL_PS5PWR_HOTPLUG CTL_CODE(0x8002, 0x903, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_PS5PWR_FAN_GET CTL_CODE(0x8002, 0x904, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_PS5PWR_FAN_SET CTL_CODE(0x8002, 0x905, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_PS5PWR_TCTL CTL_CODE(0x8002, 0x906, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_PS5PWR_GFXCLK CTL_CODE(0x8002, 0x907, METHOD_BUFFERED, FILE_WRITE_ACCESS)
/* v1.9 (16.11.2026, user: heat/fan): read-only fan duty and EMC temperature zones, exactly PS5 Linux
 * drivers/ps5/fan.c ps5_fan_read_duty (service 0x0a type 1, reply +2 le16 0..1024) and ps5_fan_read_temp (service 0x0b
 * type 1, request {zone}, reply +2 s16/256 C, valid 0..125 C) for zones 0..3 (fan zones, 0 = MainSoC) and sensor 18.
 * There is no tachometer on the PS5 (Linux hwmon has pwm1 and temp1..5 only): duty is the honest fan number. */
#define IOCTL_PS5PWR_FAN_STATUS CTL_CODE(0x8002, 0x908, METHOD_BUFFERED, FILE_READ_ACCESS)
typedef struct { ULONG duty; ULONG valid; LONG zone_milli[5]; } PS5PWR_FAN_STATUS;   /* valid: bit 0 duty, bit 1+i zone i */
C_ASSERT(sizeof(PS5PWR_FAN_STATUS) == 28);
/* v1.10 (16.11.2026): power button state, read-only */
#define IOCTL_PS5PWR_BUTTON CTL_CODE(0x8002, 0x909, METHOD_BUFFERED, FILE_READ_ACCESS)
typedef struct { ULONG version, cfg, on, sx; LONG enable_result, off_result, tries;
                 ULONG notes, presses, longs, drained, stale, offs, event_ok, bugcheck_cb; } PS5PWR_BUTTON;
C_ASSERT(sizeof(PS5PWR_BUTTON) == 60);
typedef struct { ULONG op, mhz; } PS5PWR_GFXCLK_IN;
typedef struct { LONG status; ULONG resp, mhz, wait_before, wait_after, poisoned; } PS5PWR_GFXCLK_OUT;
typedef struct { PS5PWR_GFXCLK_OUT b; ULONG query_mhz, freq_mhz; } PS5PWR_GFXCLK_OUT2;   /* v1.12.0: op 6 readback */
/* v1.10.3: BAPM init exactly as PS5 Linux mp1_init (op 1) / read WC CPU mode only (op 2). Admin control device. */
#define IOCTL_PS5PWR_BAPM CTL_CODE(0x8002, 0x90C, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_PS5PWR_VERSION CTL_CODE(0x8002, 0x90A, METHOD_BUFFERED, FILE_READ_ACCESS)   /* v1.12.1 = 0x80026428 */
typedef struct { ULONG size, major, minor, patch, caps; } PS5PWR_VERSION_OUT;
#define PS5PWR_VER_MAJOR 1u
#define PS5PWR_VER_MINOR 18u
#define PS5PWR_VER_PATCH 4u
#define PS5PWR_CAPS (0x08u | 0x10u | 0x20u | 0x40u | 0x80u | 0x100u | 0x200u | 0x400u | 0x800u | 0x1000u | 0x2000u | 0x4000u)   /* v1.18.0: 0x4000 hotplug FLAVA3+VERDE, on by default */   /* v1.17.0: 0x2000 PS5 Linux fan curve in the driver */   /* v1.16.0: 0x1000 warm restart left to the firmware */   /* v1.15.0: 0x400 op 21 boost window ceiling, 0x800 BAPM op 3 GFX CAC scale */   /* v1.14.0: 0x200 OC ops 7..20 */   /* v1.13: 0x100 hotplug pick + program */
#define IOCTL_PS5PWR_HP_PICK CTL_CODE(0x8002, 0x90D, METHOD_BUFFERED, FILE_READ_ACCESS)      /* v1.13 = 0x80026434 */
#define IOCTL_PS5PWR_HP_PROGRAM CTL_CODE(0x8002, 0x90E, METHOD_BUFFERED, FILE_WRITE_ACCESS)  /* v1.13 = 0x8002a438 */
#define IOCTL_PS5PWR_BRIDGE_READ CTL_CODE(0x8002, 0x90F, METHOD_BUFFERED, FILE_READ_ACCESS)  /* v1.18.3 = 0x8002643c: {USHORT reg, count 1..16} -> {LONG result, UCHAR data[16]} (diagnostics) */
typedef struct { LONG result; ULONG count, last_stage, mode_id; ULONG stages[PS5_HP_MAX_STAGES]; LONG results[PS5_HP_MAX_STAGES]; } PS5PWR_HP_PROGRAM_OUT;
C_ASSERT(sizeof(struct ps5_hp_pick) == 560 && FIELD_OFFSET(struct ps5_hp_pick, edid) == 48);   /* ps5pwr.ps1 -HotplugPick, KMD reader */
C_ASSERT(sizeof(PS5PWR_HP_PROGRAM_OUT) == 144);
typedef struct { ULONG op; } PS5PWR_BAPM_IN;
typedef struct { ULONG op, gfx_pct; } PS5PWR_BAPM_IN2;   /* v1.15.0: op 3 = op 1 with GFX CAC weights at gfx_pct % (85..100) */
typedef struct { LONG status; ULONG step, index, resp, wc_before, wc_after, l3_sent, gfx_sent, param_sent;
                 ULONG cpus_ok, cpus_total, poisoned; ULONG64 dpm_cfg_before, dpm_cfg_after; } PS5PWR_BAPM_OUT;
C_ASSERT(sizeof(PS5PWR_BAPM_OUT) == 64);
typedef struct { LONG led_result, s5_seen, shutdown_type, power_result, started; ULONG sow, sor, mbox_lo, bell_lo, drained; } PS5PWR_STATUS;
typedef struct { ULONG target_c, reply_bytes; uint32_t curset[PS5PWR_FAN_CURSET_COUNT]; } PS5PWR_FAN_STATE;
typedef struct { uint32_t raw; int32_t milli_celsius; } PS5PWR_TCTL;
C_ASSERT(sizeof(PS5PWR_FAN_STATE) == 32 && sizeof(PS5PWR_TCTL) == 8);

/* v1.4 hotplug */
#define HP_TICK_MS 250
#define HP_LOG_LINES 128
#define HP_LOG_TEXT 88
#define HP_ARM_TRIES 5
#define HP_ARM_RETRY_MS 5000
enum { HP_IDLE = 0, HP_ARMED = 1, HP_ARM_FAILED = 2, HP_INACTIVE = 3 };
typedef struct { ULONG seq, ms; CHAR text[HP_LOG_TEXT]; } PS5PWR_HP_LOG;
typedef struct {
    ULONG version, size, cfg_mode, state, mode_source, mode_id, bridge_null, pending;
    ULONG boot_edid_bytes, last_edid_bytes, last_edid_same, log_next, icc_tolerated, icc_dropped, last_result, last_stage;
    ULONG counters[32];
    UCHAR bridge[32];
    UCHAR last_edid[256];
    PS5PWR_HP_LOG log[HP_LOG_LINES];
} PS5PWR_HOTPLUG;
C_ASSERT(sizeof(PS5PWR_HP_LOG) == 96 && sizeof(PS5PWR_HOTPLUG) == 12768);   /* offsets used by ps5pwr.ps1 -Hotplug */
C_ASSERT(sizeof(struct ps5_hp_counters) <= 32 * sizeof(ULONG));
C_ASSERT(sizeof(struct ps5_flava3_mode) <= 32);

typedef struct _EXT {
    PDEVICE_OBJECT lower;
    volatile UCHAR *mbox, *bell;          /* region 0 / region 1 of ps5_icc_io */
    FAST_MUTEX icc_lock;
    LONG led_result, power_sent, s5_seen, shutdown_type, power_result, started;
    ULONG sow, sor, drained;
    PHYSICAL_ADDRESS mbox_pa, bell_pa;
    /* v1.4 hotplug: hp is written by icc_note() under icc_lock and by the hotplug thread */
    PDEVICE_OBJECT self;
    struct ps5_hp hp;
    PKTHREAD hp_thread;
    KEVENT hp_wake;
    volatile LONG hp_stop;
    ULONG hp_cfg, hp_state, relax_n;
    KSPIN_LOCK log_lock;
    ULONG log_next;
    PS5PWR_HP_LOG log[HP_LOG_LINES];
    /* v1.10 power button (state written under icc_lock unless noted) */
    volatile LONG icc_owner;              /* 1 while icc_lock is held: the bugcheck callback leaves ICC alone then */
    volatile LONG btn_on;                 /* notifications (maybe) enabled -> must be switched off + drained */
    LONG btn_sx, btn_enable_result, btn_off_result, btn_tries;   /* btn_sx: system left S0 (dispatch_power) */
    ULONG btn_cfg, btn_notes, btn_presses, btn_long, btn_drained, btn_stale, btn_offs;
    ULONGLONG btn_set_ms;                 /* thread only: when the event was set (stale signal expiry) */
    struct ps5_button_keys keys;
    PKEVENT btn_event; HANDLE btn_handle; /* \BaseNamedObjects\Ps5PowerButton, SYSTEM + Administrators only */
    KBUGCHECK_CALLBACK_RECORD btn_bug;
    LONG btn_bug_reg;
    ULONG btn_event_logged;
    /* v1.13 hotplug pick / program */
    KEVENT hp_seq;                        /* binary semaphore (signalled = free): one FLAVA3 stage sequence at a time */
    PKEVENT pick_event; HANDLE pick_handle;   /* \BaseNamedObjects\PswHotplug */
    /* v1.17 PS5 Linux fan curve (thread + S5 path, always under icc_lock) */
    ULONG fc_cfg, fc_tries, fc_errors, fc_ticks, fc_polls;
    LONG fc_phase, fc_wire_mode, fc_last_temp;
    BOOLEAN fc_original_ok, fc_fault;
    ULONGLONG fc_retry_at;
    struct ps5_fc_state fc;
    KBUGCHECK_CALLBACK_RECORD fc_bug;
    LONG fc_bug_reg;
    UCHAR fc_original[PS5_FC_SERVO_SIZE], fc_backup[PS5_FC_SERVO_SIZE];
    UCHAR fc_fw_original[PS5_FC_SERVO_SIZE]; BOOLEAN fc_fw_original_ok;   /* v1.17.2: PswFanOriginal (firmware r5.26.2+) */
    UCHAR fc_req[4 + PS5_FC_SERVO_SIZE], fc_io[0x7e4];
} EXT, *PEXT;
static PDEVICE_OBJECT g_ctl;                     /* \\.\Ps5Pwr */
static PEXT g_fdo_ext;                           /* the (single) started 9107 FDO */
static FAST_MUTEX g_smn_lock;
static struct ps5_hdmi_commands g_hp_packet;     /* 2 KiB, hotplug thread only (non-paged image data, not stack) */
static UNICODE_STRING g_params;                  /* <service key>\Parameters */
static WCHAR g_params_buf[256];

DRIVER_INITIALIZE DriverEntry;
static PVOID volatile g_reloc_anchor = (PVOID)DriverEntry;
static LARGE_INTEGER g_freq;

/* v1.10: every ICC user takes icc_lock through these, so the bugcheck callback can see an owner */
static void icc_acquire(PEXT e) { ExAcquireFastMutex(&e->icc_lock); InterlockedExchange(&e->icc_owner, 1); }
static void icc_release(PEXT e) { InterlockedExchange(&e->icc_owner, 0); ExReleaseFastMutex(&e->icc_lock); }

/* ---------------------------------------------------------------- ICC io callbacks */
static uint32_t icc_read(void *c, unsigned region, unsigned off, unsigned width)
{
    volatile UCHAR *b = (region ? ((PEXT)c)->bell : ((PEXT)c)->mbox) + off;
    if (width == 1) return READ_REGISTER_UCHAR((volatile UCHAR *)b);
    if (width == 2) return READ_REGISTER_USHORT((volatile USHORT *)b);
    return READ_REGISTER_ULONG((volatile ULONG *)b);
}
static void icc_write(void *c, unsigned region, unsigned off, unsigned width, uint32_t v)
{
    volatile UCHAR *b = (region ? ((PEXT)c)->bell : ((PEXT)c)->mbox) + off;
    if (width == 1) WRITE_REGISTER_UCHAR((volatile UCHAR *)b, (UCHAR)v);
    else if (width == 2) WRITE_REGISTER_USHORT((volatile USHORT *)b, (USHORT)v);
    else WRITE_REGISTER_ULONG((volatile ULONG *)b, v);
}
static uint64_t icc_ticks(void *c) { UNREFERENCED_PARAMETER(c); return (uint64_t)KeQueryPerformanceCounter(NULL).QuadPart; }
static void icc_relax(void *c) { UNREFERENCED_PARAMETER(c); KeStallExecutionProcessor(20); }
/* v1.4: hotplug thread only (PASSIVE, or APC under icc_lock): spin ~1 ms, then give the CPU away for 1 ms, so a
 * FLAVA3 stage with 500/700 ms converter delays does not busy-wait a whole core */
static void icc_relax_yield(void *c)
{
    PEXT e = c;
    if (++e->relax_n % 50) KeStallExecutionProcessor(20);
    else { LARGE_INTEGER d; d.QuadPart = -10000; KeDelayExecutionThread(KernelMode, FALSE, &d); }
}
static void icc_io(PEXT e, struct ps5_icc_io *io, ULONG ms)
{
    io->context = e; io->read = icc_read; io->write = icc_write; io->ticks = icc_ticks; io->relax = icc_relax;
    io->timeout_ticks = (uint64_t)g_freq.QuadPart * ms / 1000;
}

/* ---------------------------------------------------------------- v1.2 robust ICC entry */
static void icc_note(void *c, uint8_t svc, uint16_t type, const uint8_t *p, unsigned n)
{
    ((PEXT)c)->drained++;
    ps5_hp_note(&((PEXT)c)->hp, svc, type, p, n);      /* v1.4: every ICC user feeds the hotplug collector */
}
/* drain pending notifications; if the mailbox is still not idle (SOR!=1 or SOW!=0) after the firmware
 * hand-off, restore the idle state the way icc_send() in PS5 Linux begins a message; caller holds icc_lock */
static void icc_prepare(PEXT e)
{
    struct ps5_icc_io io; icc_io(e, &io, 200);
    for (int i = 0; i < 16 && ps5_icc_poll(&io, icc_note, e) > 0; i++) {}
    e->sow = icc_read(e, 0, 0x7f0, 2); e->sor = icc_read(e, 0, 0x7f4, 2);
    if (e->sor != 1 || e->sow != 0) {
        icc_write(e, 1, 0x14, 4, 3);              /* clear latched transport status */
        icc_write(e, 0, 0x7f0, 2, 0);             /* SOW = 0: no host message pending */
        icc_write(e, 0, 0x7f4, 2, 1);             /* SOR = 1: idle, like after a consumed message */
    }
}
static int icc_query_robust_ms(PEXT e, uint8_t svc, uint16_t type, const uint8_t *payload, unsigned n,
                               uint8_t *output, unsigned capacity, unsigned *output_bytes, ULONG timeout_ms)
{
    struct ps5_icc_io io; icc_io(e, &io, timeout_ms);
    icc_prepare(e);
    int r = ps5_icc_query_events(&io, svc, type, payload, n, output, capacity, output_bytes, icc_note, e);
    if (r == -2 || r == -3) { icc_prepare(e); r = ps5_icc_query_events(&io, svc, type, payload, n,
                                                                         output, capacity, output_bytes, icc_note, e); }
    return r;
}
static int icc_query_robust(PEXT e, uint8_t svc, uint16_t type, const uint8_t *payload, unsigned n,
                            uint8_t *output, unsigned capacity, unsigned *output_bytes)
{
    return icc_query_robust_ms(e, svc, type, payload, n, output, capacity, output_bytes, 2000);
}
static NTSTATUS icc_query_status(int result)
{
    switch (result) {
    case -2: case -3: return STATUS_IO_TIMEOUT;
    case -4: return STATUS_DATA_ERROR;
    case -5: return STATUS_BUFFER_TOO_SMALL;
    default: return STATUS_IO_DEVICE_ERROR;
    }
}
static int icc_power_robust(PEXT e, int reboot)
{
    struct ps5_icc_io io; icc_io(e, &io, 3000);
    icc_prepare(e);
    int r = ps5_icc_power(&io, reboot);
    if (r == -2 || r == -3) { icc_prepare(e); r = ps5_icc_power(&io, reboot); }
    return r;
}

/* MainSoC fan service, following drivers/ps5/fan.c. ICC caller holds icc_lock. */
static NTSTATUS fan_read_curset(PEXT e, uint32_t curset[PS5PWR_FAN_CURSET_COUNT], ULONG *reply_bytes_out)
{
    UCHAR request[PS5PWR_FAN_GET_BYTES], reply[PS5PWR_FAN_REPLY_MAX_BYTES];
    unsigned reply_bytes = 0;
    ps5pwr_fan_get_request(request, PS5PWR_FAN_ZONE_MAINSOC);
    int r = icc_query_robust_ms(e, 0x0a, 0x07, request, sizeof(request), reply, sizeof(reply), &reply_bytes, 12000);
    if (r) return icc_query_status(r);
    if (reply_bytes_out) *reply_bytes_out = reply_bytes;
    return ps5pwr_fan_decode_curset(reply, reply_bytes, curset) ? STATUS_DATA_ERROR : STATUS_SUCCESS;
}

static NTSTATUS fan_set_target(PEXT e, ULONG target_c, uint32_t curset[PS5PWR_FAN_CURSET_COUNT], ULONG *reply_bytes_out)
{
    UCHAR request[PS5PWR_FAN_SET_BYTES];
    NTSTATUS st = fan_read_curset(e, curset, NULL);
    if (!NT_SUCCESS(st)) return st;
    uint32_t original[PS5PWR_FAN_CURSET_COUNT];
    RtlCopyMemory(original, curset, sizeof(original));
    ps5pwr_fan_set_target_c(curset, target_c);
    ps5pwr_fan_set_request(request, PS5PWR_FAN_ZONE_MAINSOC, 0, curset);
    int r = icc_query_robust_ms(e, 0x0a, 0x06, request, sizeof(request), NULL, 0, NULL, 12000);
    if (r) {
        ps5pwr_fan_set_request(request, PS5PWR_FAN_ZONE_MAINSOC, 0, original);
        (void)icc_query_robust_ms(e, 0x0a, 0x06, request, sizeof(request), NULL, 0, NULL, 12000);
        return icc_query_status(r);
    }
    uint32_t verify[PS5PWR_FAN_CURSET_COUNT];
    ULONG verify_bytes = 0;
    st = fan_read_curset(e, verify, &verify_bytes);
    if (!NT_SUCCESS(st) || ps5pwr_fan_target_c(verify) != target_c) {
        ps5pwr_fan_set_request(request, PS5PWR_FAN_ZONE_MAINSOC, 0, original);
        (void)icc_query_robust_ms(e, 0x0a, 0x06, request, sizeof(request), NULL, 0, NULL, 12000);
        return NT_SUCCESS(st) ? STATUS_DATA_ERROR : st;
    }
    RtlCopyMemory(curset, verify, sizeof(verify));
    if (reply_bytes_out) *reply_bytes_out = verify_bytes;
    return STATUS_SUCCESS;
}

/* Read only Zen Tctl via the PS5 Linux k10temp SMN register. Preserve the indirect index register. */
static NTSTATUS tctl_read(PS5PWR_TCTL *out)
{
    PCI_SLOT_NUMBER slot; ULONG id = 0, old_index = 0, raw = 0, smn_addr = 0x59800;
    BOOLEAN index_saved = FALSE, raw_valid = FALSE;
    NTSTATUS st = STATUS_DEVICE_NOT_READY;
    slot.u.AsULONG = 0; /* host bridge 00:00.0 */
    if (!out) return STATUS_INVALID_PARAMETER;
    ExAcquireFastMutex(&g_smn_lock);
    if (HalGetBusDataByOffset(PCIConfiguration, 0, slot.u.AsULONG, &id, 0, sizeof(id)) != sizeof(id) ||
        (id & 0xffff) != 0x1022) goto done;
    if (HalGetBusDataByOffset(PCIConfiguration, 0, slot.u.AsULONG, &old_index, 0x60, sizeof(old_index)) != sizeof(old_index))
        goto done;
    index_saved = TRUE;
    if (HalSetBusDataByOffset(PCIConfiguration, 0, slot.u.AsULONG, &smn_addr, 0x60, sizeof(smn_addr)) != sizeof(smn_addr))
        goto done;
    /* v1.8.1: one immediate retry inside the same locked index window if the data read fails or reads all-ones/zero */
    for (int attempt = 0; attempt < 2 && !raw_valid; attempt++)
        if (HalGetBusDataByOffset(PCIConfiguration, 0, slot.u.AsULONG, &raw, 0x64, sizeof(raw)) == sizeof(raw) &&
            raw != 0 && raw != 0xFFFFFFFFu)
            raw_valid = TRUE;
    st = STATUS_DEVICE_NOT_READY;
done:
    if (index_saved) {
        ULONG restore = old_index;
        if (HalSetBusDataByOffset(PCIConfiguration, 0, slot.u.AsULONG, &restore, 0x60, sizeof(restore)) != sizeof(restore))
            raw_valid = FALSE;
    }
    if (raw_valid && NT_SUCCESS(ps5pwr_tctl_from_raw(raw, &out->milli_celsius))) {
        out->raw = (uint32_t)raw;
        st = STATUS_SUCCESS;
    }
    ExReleaseFastMutex(&g_smn_lock);
    return st;
}

/* v1.7: SMN ops for smu-mbox.c. The caller holds g_smn_lock; each access saves and restores the index register. */
static struct ps5_smu_state g_smu;
static int smn_access(uint32_t addr, uint32_t *rd, uint32_t wr, int write)
{
    PCI_SLOT_NUMBER slot; ULONG old_index = 0, a = addr, v = 0; int bad = 0;
    slot.u.AsULONG = 0;
    if (HalGetBusDataByOffset(PCIConfiguration, 0, slot.u.AsULONG, &old_index, 0x60, sizeof(old_index)) != sizeof(old_index)) return 1;
    if (HalSetBusDataByOffset(PCIConfiguration, 0, slot.u.AsULONG, &a, 0x60, sizeof(a)) != sizeof(a)) bad = 1;
    else if (write) { v = wr; if (HalSetBusDataByOffset(PCIConfiguration, 0, slot.u.AsULONG, &v, 0x64, sizeof(v)) != sizeof(v)) bad = 1; }
    else if (HalGetBusDataByOffset(PCIConfiguration, 0, slot.u.AsULONG, &v, 0x64, sizeof(v)) != sizeof(v)) bad = 1;
    else *rd = v;
    if (HalSetBusDataByOffset(PCIConfiguration, 0, slot.u.AsULONG, &old_index, 0x60, sizeof(old_index)) != sizeof(old_index)) bad = 1;
    return bad;
}
static int smu_rd(void *c, uint32_t a, uint32_t *v) { UNREFERENCED_PARAMETER(c); return smn_access(a, v, 0, 0); }
static int smu_wr(void *c, uint32_t a, uint32_t v) { UNREFERENCED_PARAMETER(c); return smn_access(a, NULL, v, 1); }
static void smu_stall(void *c, uint32_t us) { UNREFERENCED_PARAMETER(c); KeStallExecutionProcessor(us); }
static const struct ps5_smn_ops g_smu_ops = { NULL, smu_rd, smu_wr, smu_stall };

/* v1.14.0: what an OC op changed, so stop / S5 / bugcheck can take it back (best effort) */
static volatile LONG g_oc_gpu, g_oc_cpu, g_oc_bug_reg;
static KBUGCHECK_CALLBACK_RECORD g_oc_bug;
static KBUGCHECK_CALLBACK_ROUTINE oc_bugcheck;

static NTSTATUS gfxclk_call(const PS5PWR_GFXCLK_IN *in, PS5PWR_GFXCLK_OUT *o, ULONG *query_mhz, ULONG *freq_mhz)
{
    PCI_SLOT_NUMBER slot; ULONG id = 0; uint32_t resp = 0, value = 0, q = 0, f = 0, queue = 0;
    /* v1.8: op 2 = boost window enter (UniversalModeEntry, fixed Linux arguments), 3 = boost exit, 4 = GetGfxFrequency */
    /* v1.14.0: ops 7..20 (smu-mbox v0.8 ps5_smu_oc_decode): forced GFX clock/voltage (queue 0), CPU boost/curve/limits (queue 3) */
    uint32_t arg = 0, msg = ps5_smu_oc_decode(in->op, in->mhz, &arg, &queue);
    if (!msg) return STATUS_INVALID_PARAMETER;
    if ((in->op == 9 || in->op == 11 || (in->op >= 16 && in->op <= 19)) &&
        KeGetCurrentIrql() == PASSIVE_LEVEL && !InterlockedExchange(&g_oc_bug_reg, 1)) {   /* first write op: arm the bugcheck net */
        KeInitializeCallbackRecord(&g_oc_bug);
        if (!KeRegisterBugCheckCallback(&g_oc_bug, oc_bugcheck, NULL, 0, (PUCHAR)"ps5pwr-oc")) InterlockedExchange(&g_oc_bug_reg, 0);
    }
    slot.u.AsULONG = 0;
    ExAcquireFastMutex(&g_smn_lock);
    if (HalGetBusDataByOffset(PCIConfiguration, 0, slot.u.AsULONG, &id, 0, sizeof(id)) != sizeof(id) || (id & 0xffff) != 0x1022) {
        ExReleaseFastMutex(&g_smn_lock); return STATUS_DEVICE_NOT_READY;
    }
    o->status = in->op == 2 ? ps5_smu_boost_enter(&g_smu_ops, &g_smu, &resp)
              : in->op == 5 ? ps5_smu_boost_enter_ex(&g_smu_ops, &g_smu, 1, &resp)
              : in->op == 6 ? ps5_smu_request_gfxclk_oc(&g_smu_ops, &g_smu, arg, &resp, &q, &f)
              : in->op == 13 ? ps5_smu_gpu_unforce(&g_smu_ops, &g_smu, &resp)
              : in->op == 20 ? ps5_smu_cpu_oc_reset(&g_smu_ops, &g_smu, &resp)
              : in->op == 21 ? ps5_smu_boost_enter_p3(&g_smu_ops, &g_smu, arg, &resp)   /* v1.15.0 */
              : queue == 3 ? ps5_smu_test_send(&g_smu_ops, &g_smu, msg, arg, &resp, &value)
                            : ps5_smu_send(&g_smu_ops, &g_smu, msg, arg, &resp, &value);
    if (o->status == 0) {                                 /* v1.14.0: remember what is changed */
        if (in->op == 9 || in->op == 11) InterlockedExchange(&g_oc_gpu, 1);
        else if (in->op == 13) InterlockedExchange(&g_oc_gpu, 0);
        else if ((in->op == 16 && arg != PS5_SMU_CPU_BOOST_DEFAULT) || (in->op == 17 && arg != 0)) InterlockedExchange(&g_oc_cpu, 1);
        else if (in->op == 20) InterlockedExchange(&g_oc_cpu, 0);
    }
    if (query_mhz) *query_mhz = q;
    if (freq_mhz) *freq_mhz = f;
    o->resp = resp;
    o->mhz = (in->op == 0 || in->op == 4 || in->op == 7 || in->op == 8 || in->op == 14 || in->op == 15) ? value : arg;
    o->wait_before = g_smu.last_wait_before; o->wait_after = g_smu.last_wait_after; o->poisoned = (ULONG)g_smu.poisoned;
    ExReleaseFastMutex(&g_smn_lock);
    return STATUS_SUCCESS;
}

/* v1.14.0: take forced GFX clock/voltage and the CPU boost/curve back (PASSIVE: stop, remove, unload, S5). Limits
 * (0x8B/0x8C) stay: the allow-list only ever lowers them. */
static void oc_safe_reset(const char *why)
{
    uint32_t resp = 0; int g = -9, c = -9;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || (!g_oc_gpu && !g_oc_cpu)) return;
    ExAcquireFastMutex(&g_smn_lock);
    if (InterlockedExchange(&g_oc_gpu, 0)) g = ps5_smu_gpu_unforce(&g_smu_ops, &g_smu, &resp);
    if (InterlockedExchange(&g_oc_cpu, 0)) c = ps5_smu_cpu_oc_reset(&g_smu_ops, &g_smu, &resp);
    ExReleaseFastMutex(&g_smn_lock);
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "ps5pwr: OC reset (%s): gpu %d cpu %d\n", why, g, c);
}
/* bugcheck: HIGH_LEVEL, no HAL bus calls and no mutex - config mechanism #1 (0xCF8/0xCFC) to 00:00.0 0x60/0x64 directly,
 * skipped when the mailbox lock is held (another thread may be inside a message). Fresh mailbox state, same waits. */
static __forceinline void port_out32(USHORT p, ULONG v) { __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(p)); }
static __forceinline ULONG port_in32(USHORT p) { ULONG v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static int cf8_smn(uint32_t addr, uint32_t *rd, uint32_t wr, int write)
{
    ULONG old;
    port_out32(0xCF8, 0x80000060u); old = port_in32(0xCFC);
    port_out32(0xCFC, addr);
    port_out32(0xCF8, 0x80000064u);
    if (write) port_out32(0xCFC, wr); else *rd = port_in32(0xCFC);
    port_out32(0xCF8, 0x80000060u); port_out32(0xCFC, old);
    return 0;
}
static int cf8_rd(void *c, uint32_t a, uint32_t *v) { UNREFERENCED_PARAMETER(c); return cf8_smn(a, v, 0, 0); }
static int cf8_wr(void *c, uint32_t a, uint32_t v) { UNREFERENCED_PARAMETER(c); return cf8_smn(a, NULL, v, 1); }
static VOID oc_bugcheck(PVOID buf, ULONG len)
{
    static const struct ps5_smn_ops ops = { NULL, cf8_rd, cf8_wr, smu_stall };
    struct ps5_smu_state st = { 0 }; uint32_t resp = 0;
    UNREFERENCED_PARAMETER(buf); UNREFERENCED_PARAMETER(len);
    if ((!g_oc_gpu && !g_oc_cpu) || g_smn_lock.Count != FM_LOCK_BIT) return;   /* nothing changed, or the mailbox is in use */
    if (g_oc_gpu) (void)ps5_smu_gpu_unforce(&ops, &st, &resp);
    if (g_oc_cpu) (void)ps5_smu_cpu_oc_reset(&ops, &st, &resp);
    g_oc_gpu = g_oc_cpu = 0;
}

/* v1.10.3: per-core DPM_WAC weights (PS5 Linux update_bapm_weights_smp): clear DPM_CFG.ACCESS_DIS, write the 21
 * index/data pairs of parameter set 2, restore DPM_CFG. Runs on every processor at IPI level. */
static __forceinline ULONG64 rdmsr64(ULONG m) { ULONG lo, hi; __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(m)); return ((ULONG64)hi << 32) | lo; }
static __forceinline void wrmsr64(ULONG m, ULONG64 v) { __asm__ volatile("wrmsr" : : "c"(m), "a"((ULONG)v), "d"((ULONG)(v >> 32))); }
static volatile LONG g_bapm_cpus_ok;
static ULONG64 g_bapm_cfg_before, g_bapm_cfg_after;
static KIPI_BROADCAST_WORKER bapm_msr_ipi;
static ULONG_PTR bapm_msr_ipi(ULONG_PTR arg)
{
    const uint64_t *wac = ps5_bapm_dpm_wac();
    ULONG64 cfg = rdmsr64(PS5_MSR_DPM_CFG);
    UNREFERENCED_PARAMETER(arg);
    wrmsr64(PS5_MSR_DPM_CFG, cfg & ~PS5_MSR_ACCESS_DIS);
    for (ULONG i = 0; i < 21; i++) { wrmsr64(PS5_MSR_DPM_WAC_INDEX, i); wrmsr64(PS5_MSR_DPM_WAC_DATA, wac[i]); }
    wrmsr64(PS5_MSR_DPM_CFG, cfg);
    if (KeGetCurrentProcessorNumberEx(NULL) == 0) { g_bapm_cfg_before = cfg; g_bapm_cfg_after = rdmsr64(PS5_MSR_DPM_CFG); }
    InterlockedIncrement(&g_bapm_cpus_ok);
    return 0;
}

static NTSTATUS bapm_call(const PS5PWR_BAPM_IN2 *in, PS5PWR_BAPM_OUT *o)
{
    PCI_SLOT_NUMBER slot; ULONG id = 0; uint32_t resp = 0, mode = 0;
    struct ps5_bapm_report r;
    uint32_t pct = in->op == 3 ? in->gfx_pct : 100u;
    if (in->op != 1 && in->op != 2 && in->op != 3) return STATUS_INVALID_PARAMETER;
    if (pct < PS5_BAPM_GFX_PCT_MIN || pct > 100u) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(&r, sizeof(r));
    slot.u.AsULONG = 0;
    ExAcquireFastMutex(&g_smn_lock);
    if (HalGetBusDataByOffset(PCIConfiguration, 0, slot.u.AsULONG, &id, 0, sizeof(id)) != sizeof(id) || (id & 0xffff) != 0x1022) {
        ExReleaseFastMutex(&g_smn_lock); return STATUS_DEVICE_NOT_READY;
    }
    o->cpus_total = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    if (in->op == 2) {                                    /* read only */
        o->status = ps5_smu_test_send(&g_smu_ops, &g_smu, PS5_SMU_TEST_GET_WC_CPU, 0, &resp, &mode);
        o->resp = resp; o->wc_before = o->wc_after = mode;
    } else {
        o->status = ps5_smu_bapm_prepare(&g_smu_ops, &g_smu, &r);   /* UniversalModeExit, WC CPU = 1 */
        if (!o->status) {
            g_bapm_cpus_ok = 0;
            KeIpiGenericCall(bapm_msr_ipi, 0);                       /* DPM_WAC on every processor */
            o->cpus_ok = (ULONG)g_bapm_cpus_ok; o->dpm_cfg_before = g_bapm_cfg_before; o->dpm_cfg_after = g_bapm_cfg_after;
            o->status = ps5_smu_bapm_weights_scaled(&g_smu_ops, &g_smu, &r, pct);   /* L3 / GFX weights, BAPM params */
        }
        o->step = r.step; o->index = r.index; o->resp = r.resp; o->wc_before = r.wc_before; o->wc_after = r.wc_after;
        o->l3_sent = r.l3_sent; o->gfx_sent = r.gfx_sent; o->param_sent = r.param_sent;
    }
    o->poisoned = (ULONG)g_smu.poisoned;
    ExReleaseFastMutex(&g_smn_lock);
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------- helpers */
static NTSTATUS forward_sync_done(PDEVICE_OBJECT d, PIRP irp, PVOID ctx)
{
    UNREFERENCED_PARAMETER(d);
    if (irp->PendingReturned) KeSetEvent((PKEVENT)ctx, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}
static NTSTATUS forward_sync(PEXT e, PIRP irp)
{
    KEVENT ev;
    KeInitializeEvent(&ev, NotificationEvent, FALSE);
    IoCopyCurrentIrpStackLocationToNext(irp);
    IoSetCompletionRoutine(irp, forward_sync_done, &ev, TRUE, TRUE, TRUE);
    NTSTATUS st = IoCallDriver(e->lower, irp);
    if (st == STATUS_PENDING) { KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, NULL); st = irp->IoStatus.Status; }
    return st;
}
static void unmap(PEXT e)
{
    if (e->mbox) { MmUnmapIoSpace((PVOID)e->mbox, PAGE_SIZE); e->mbox = NULL; }
    if (e->bell) { MmUnmapIoSpace((PVOID)e->bell, PAGE_SIZE); e->bell = NULL; }
}
/* v1.2: read config space through the PCI bus driver (PDO) */
static NTSTATUS read_config(PEXT e, ULONG off, PVOID buf, ULONG len)
{
    KEVENT ev; IO_STATUS_BLOCK io;
    KeInitializeEvent(&ev, NotificationEvent, FALSE);
    PIRP irp = IoBuildSynchronousFsdRequest(IRP_MJ_PNP, e->lower, NULL, 0, NULL, &ev, &io);
    if (!irp) return STATUS_INSUFFICIENT_RESOURCES;
    irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    PIO_STACK_LOCATION s = IoGetNextIrpStackLocation(irp);
    s->MinorFunction = IRP_MN_READ_CONFIG;
    s->Parameters.ReadWriteConfig.WhichSpace = PCI_WHICHSPACE_CONFIG;
    s->Parameters.ReadWriteConfig.Buffer = buf;
    s->Parameters.ReadWriteConfig.Offset = off;
    s->Parameters.ReadWriteConfig.Length = len;
    NTSTATUS st = IoCallDriver(e->lower, irp);
    if (st == STATUS_PENDING) { KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, NULL); st = io.Status; }
    return st;
}
/* mailbox = BAR4 (4 KiB window), doorbell = BAR2 + 0x108000 - exactly the UEFI HdmiProbeDxe constants
 * 0x85400000 / 0x85308000; the BARs come from config space, the window must also be in the resources */
static NTSTATUS map_resources(PEXT e, PCM_RESOURCE_LIST rl)
{
    PHYSICAL_ADDRESS mbox = { 0 }, bell = { 0 };
    ULONG bar[6] = { 0 };
    if (!rl || !rl->Count) return STATUS_DEVICE_CONFIGURATION_ERROR;
    if (!NT_SUCCESS(read_config(e, 0x10, bar, sizeof(bar)))) return STATUS_DEVICE_CONFIGURATION_ERROR;
    ULONG64 bar2 = ((ULONG64)bar[3] << 32) | (bar[2] & ~0xFULL), bar4 = ((ULONG64)bar[5] << 32) | (bar[4] & ~0xFULL);
    PCM_PARTIAL_RESOURCE_LIST pl = &rl->List[0].PartialResourceList;
    for (ULONG i = 0; i < pl->Count; i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR d = &pl->PartialDescriptors[i];
        ULONG64 len, start;
        if (d->Type == CmResourceTypeMemory) len = d->u.Memory.Length;
        else if (d->Type == CmResourceTypeMemoryLarge) len = RtlCmDecodeMemIoResource(d, NULL);
        else continue;
        start = (ULONG64)d->u.Memory.Start.QuadPart;
        if (bar2 && start <= bar2 + DOORBELL_OFF && bar2 + DOORBELL_OFF + PAGE_SIZE <= start + len) bell.QuadPart = (LONGLONG)(bar2 + DOORBELL_OFF);
        if (bar4 && start <= bar4 && bar4 + PAGE_SIZE <= start + len) mbox.QuadPart = (LONGLONG)bar4;
    }
    e->mbox_pa = mbox; e->bell_pa = bell;
    if (!mbox.QuadPart || !bell.QuadPart) return STATUS_DEVICE_CONFIGURATION_ERROR;
    e->mbox = MmMapIoSpaceEx(mbox, PAGE_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    e->bell = MmMapIoSpaceEx(bell, PAGE_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (!e->mbox || !e->bell) { unmap(e); return STATUS_INSUFFICIENT_RESOURCES; }
    return STATUS_SUCCESS;
}
static void log_event(PDEVICE_OBJECT d, ULONG code, LONG a, LONG b)
{
    PIO_ERROR_LOG_PACKET p = IoAllocateErrorLogEntry(d, (UCHAR)(sizeof(IO_ERROR_LOG_PACKET) + 2 * sizeof(ULONG)));
    if (!p) return;
    p->ErrorCode = (NTSTATUS)0x40000000 | code;     /* informational */
    p->DumpDataSize = 2 * sizeof(ULONG);
    p->DumpData[0] = (ULONG)a; p->DumpData[1] = (ULONG)b;
    IoWriteErrorLogEntry(p);
}

/* ---------------------------------------------------------------- v1.4 hotplug (UNTESTED ON HARDWARE) */
static ULONGLONG now_ms(void) { return KeQueryInterruptTime() / 10000; }

/* ps5pwr-hotplug.c log sink: DbgPrintEx + ring read by IOCTL_PS5PWR_HOTPLUG; any IRQL <= DISPATCH, ANSI only */
void ps5_hp_log(void *ctx, const char *fmt, ...)
{
    PEXT e = ctx; CHAR line[HP_LOG_TEXT]; va_list ap; KIRQL irql;
    line[0] = 0;
    va_start(ap, fmt);
    (void)RtlStringCbVPrintfA(line, sizeof(line), fmt, ap);     /* truncates, always terminated */
    va_end(ap);
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "ps5pwr: %s\n", line);
    if (!e) return;
    KeAcquireSpinLock(&e->log_lock, &irql);
    PS5PWR_HP_LOG *l = &e->log[e->log_next % HP_LOG_LINES];
    l->seq = ++e->log_next; l->ms = (ULONG)now_ms();
    RtlCopyMemory(l->text, line, sizeof(line));
    KeReleaseSpinLock(&e->log_lock, irql);
}

/* registry: <service>\Parameters, PASSIVE_LEVEL only */
static ULONG reg_dword(PCWSTR name, ULONG def)
{
    OBJECT_ATTRIBUTES oa; HANDLE h; UNICODE_STRING v; ULONG len = 0;
    ULONG buf[(sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG) + 3) / 4];
    PKEY_VALUE_PARTIAL_INFORMATION p = (PKEY_VALUE_PARTIAL_INFORMATION)buf;
    if (!g_params.Length) return def;
    InitializeObjectAttributes(&oa, &g_params, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwOpenKey(&h, KEY_READ, &oa))) return def;
    RtlInitUnicodeString(&v, name);
    NTSTATUS st = ZwQueryValueKey(h, &v, KeyValuePartialInformation, p, sizeof(buf), &len);
    ZwClose(h);
    return NT_SUCCESS(st) && p->Type == REG_DWORD && p->DataLength == sizeof(ULONG) ? *(ULONG *)p->Data : def;
}
static void reg_set(PCWSTR name, ULONG type, const void *data, ULONG bytes)
{
    OBJECT_ATTRIBUTES oa; HANDLE h; UNICODE_STRING v; ULONG disp;
    if (!g_params.Length) return;
    InitializeObjectAttributes(&oa, &g_params, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateKey(&h, KEY_SET_VALUE, &oa, 0, NULL, REG_OPTION_NON_VOLATILE, &disp))) return;
    RtlInitUnicodeString(&v, name);
    (void)ZwSetValueKey(h, &v, 0, type, (PVOID)data, bytes);
    ZwClose(h);
}
/* volatile runtime variables of our firmware (ps5-edid-info.h / ps5-hdmi-mode-var.h), PASSIVE_LEVEL only */
static GUID g_hp_guid = PS5_HDMI_MODE_VENDOR_GUID_INIT;
static NTSTATUS fw_var(PCWSTR name, PVOID buf, PULONG len)
{
    UNICODE_STRING n; ULONG attr = 0;
    RtlInitUnicodeString(&n, name);
    return ExGetFirmwareEnvironmentVariable(&n, &g_hp_guid, buf, len, &attr);
}

/* boot EDID + current mode, HDMI config, HDMI events on. Returns HP_ARMED, HP_ARM_FAILED (retry) or HP_INACTIVE. */
static ULONG hp_arm(PEXT e, ULONG cfg)
{
    struct ps5_hp *hp = &e->hp; struct ps5_icc_io io;
    UCHAR mode_var[sizeof(struct ps5_hdmi_mode_var)];
    ULONG len = sizeof(hp->boot_edid);
    int var_r, cfg_r = 1, bridge_r = 0, ev_r = 1;
    NTSTATUS st = fw_var(L"Ps5HdmiEdid", hp->boot_edid, &len);
    /* v1.7.2 : the boot EDID is only logged/reported, never compared. Booting without a display (or the
     * firmware EDID phase skipped) left hotplug off for the whole session, so a later Dell plug-in stayed black.
     * Missing EDID now only logs; the real gate is the converter check below (v1.18.0: FLAVA3 or VERDE). */
    if (!NT_SUCCESS(st) || len < 128) {
        ps5_hp_log(e, "hotplug: Ps5HdmiEdid unavailable (%08lx): no boot EDID (no display at boot?), arming anyway", (ULONG)st);
        hp->boot_edid_bytes = 0;
    } else {
        hp->boot_edid_bytes = len;
        ps5_hp_log(e, "hotplug: boot EDID %lu B crc %08x", len, ps5_hdmi_mode_crc32(hp->boot_edid, len));
    }
    hp->source = PS5_HP_SRC_NONE; hp->mode_id = 0xffffffffu;
    len = sizeof(mode_var);
    st = fw_var(PS5_HDMI_MODE_VAR_NAME, mode_var, &len);
    if (!NT_SUCCESS(st)) ps5_hp_log(e, "hotplug: Ps5HdmiMode unavailable (%08lx): bridge readback", (ULONG)st);
    else if ((var_r = ps5_hp_mode_from_var(hp, mode_var, len)) != 0) ps5_hp_log(e, "hotplug: Ps5HdmiMode invalid (%d, %lu B)", var_r, len);
    icc_acquire(e);
    if (!e->hp_stop && e->mbox) {
        icc_prepare(e);
        icc_io(e, &io, 12000); io.relax = icc_relax_yield;
        cfg_r = ps5_hdmi_read_config_events(&io, &hp->cfg, icc_note, e);
        if (!cfg_r && ps5_hp_converter_ok(hp->cfg.converter)) {
            /* v1.18.0: the bridge readback exists on FLAVA3 only (the firmware's VideoReadRegisters refuses VERDE); without
             * Ps5HdmiMode a VERDE console assumes the firmware default 1080p60, like a failed FLAVA3 readback */
            if (hp->source == PS5_HP_SRC_NONE && hp->cfg.converter == PS5_HP_CONVERTER_FLAVA3 && (bridge_r = ps5_hp_mode_from_bridge(hp, &io)) < 0) ps5_hp_mode_default(hp);
            if (hp->source == PS5_HP_SRC_NONE && hp->cfg.converter == PS5_HP_CONVERTER_VERDE) { ps5_hp_mode_default(hp); bridge_r = -1; }
            hp->mode = cfg;
            ps5_hp_arm_reset(hp, now_ms());              /* drop pre-arm events, then events on */
            ev_r = ps5_hp_events(hp, &io, &g_hp_packet, 1);
        }
    }
    icc_release(e);
    ULONG state = cfg_r ? HP_ARM_FAILED : !ps5_hp_converter_ok(hp->cfg.converter) ? HP_INACTIVE : ev_r ? HP_ARM_FAILED : HP_ARMED;
    if (bridge_r < 0) ps5_hp_log(e, "hotplug: %s: assuming 1080p60 (VIC16)", hp->cfg.converter == PS5_HP_CONVERTER_VERDE ?
                                 "VERDE without Ps5HdmiMode" : "bridge readback failed");
    ps5_hp_log(e, "hotplug: arm mode %lu: config %d converter %u events-on %d source %d mode %u -> %s", cfg, cfg_r,
               hp->cfg.converter, ev_r, hp->source, hp->mode_id,
               state == HP_ARMED ? (hp->source == PS5_HP_SRC_NONE ? "armed, replay refused (mode unknown)" : "armed")
               : state == HP_INACTIVE ? "inactive (unknown HDMI converter)" : "failed, retry in 5 s");
    log_event(e->self, 0x11, (LONG)state, hp->source);
    return state;
}
static void hp_disarm(PEXT e)
{
    struct ps5_icc_io io; int r = 1;
    icc_acquire(e);
    if (!e->hp_stop && e->mbox) { icc_io(e, &io, 12000); io.relax = icc_relax_yield; r = ps5_hp_events(&e->hp, &io, &g_hp_packet, 0); }
    e->hp.pending = 0; e->hp.mode = 0;
    icc_release(e);
    ps5_hp_log(e, "hotplug: off, HDMI events off result %d", r);
}
/* one ICC transaction (stage) per gate pair; icc_prepare() once per replay attempt like every other ps5pwr user */
static int hp_gate(void *c, unsigned index, unsigned stage, int after)
{
    PEXT e = c;
    UNREFERENCED_PARAMETER(stage);
    if (after) { icc_release(e); return 0; }
    if (e->hp_stop) return 1;
    icc_acquire(e);
    if (e->hp_stop || !e->mbox) { icc_release(e); return 1; }
    if (!index) icc_prepare(e);
    return 0;
}
/* v1.13: \BaseNamedObjects\PswHotplug, auto-reset, SYSTEM + Administrators (same ACL as Ps5PowerButton) */
static NTSTATUS pick_event_create(PEXT e)
{
    ULONG acl_buf[64]; PACL acl = (PACL)acl_buf; SECURITY_DESCRIPTOR sd; OBJECT_ATTRIBUTES oa; HANDLE h; NTSTATUS st;
    UNICODE_STRING name = RTL_CONSTANT_STRING(L"\\BaseNamedObjects\\PswHotplug");
    if (e->pick_event) return STATUS_SUCCESS;
    if (!NT_SUCCESS(st = RtlCreateAcl(acl, sizeof(acl_buf), ACL_REVISION)) ||
        !NT_SUCCESS(st = RtlAddAccessAllowedAce(acl, ACL_REVISION, EVENT_ALL_ACCESS, SeExports->SeLocalSystemSid)) ||
        !NT_SUCCESS(st = RtlAddAccessAllowedAce(acl, ACL_REVISION, EVENT_ALL_ACCESS, SeExports->SeAliasAdminsSid)) ||
        !NT_SUCCESS(st = RtlCreateSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION)) ||
        !NT_SUCCESS(st = RtlSetDaclSecurityDescriptor(&sd, TRUE, acl, FALSE))) return st;
    InitializeObjectAttributes(&oa, &name, OBJ_KERNEL_HANDLE | OBJ_OPENIF, NULL, &sd);
    if (!NT_SUCCESS(st = ZwCreateEvent(&h, EVENT_ALL_ACCESS, &oa, SynchronizationEvent, FALSE))) return st;
    if (!NT_SUCCESS(st = ObReferenceObjectByHandle(h, EVENT_ALL_ACCESS, *ExEventObjectType, KernelMode, (PVOID *)&e->pick_event, NULL))) {
        ZwClose(h); e->pick_event = NULL; return st;
    }
    e->pick_handle = h;
    return STATUS_SUCCESS;
}
static void pick_event_close(PEXT e)
{
    if (e->pick_event) { ObDereferenceObject(e->pick_event); e->pick_event = NULL; }
    if (e->pick_handle) { ZwClose(e->pick_handle); e->pick_handle = NULL; }
}
static void hp_publish(PEXT e)
{
    ULONG same, n, pick_new, pick_mode, pick_seq;
    icc_acquire(e);                                        /* v1.13: the pick is written by ps5_hp_process under icc_lock */
    pick_new = (ULONG)e->hp.pick_new; e->hp.pick_new = 0; pick_mode = e->hp.pick.mode_id; pick_seq = e->hp.pick.seq;
    icc_release(e);
    if (pick_new && !e->hp_stop) {
        reg_set(L"HotplugPickModeId", REG_DWORD, &pick_mode, sizeof(pick_mode));
        reg_set(L"HotplugPickSeq", REG_DWORD, &pick_seq, sizeof(pick_seq));
        if (NT_SUCCESS(pick_event_create(e))) KeSetEvent(e->pick_event, IO_NO_INCREMENT, FALSE);
        ps5_hp_log(e, "hotplug: pick %lu (mode %lu) published, PswHotplug %s", pick_seq, pick_mode, e->pick_event ? "set" : "missing");
    }
    if (!e->hp.pub_new || e->hp_stop) return;              /* no registry writes once S5 has begun */
    e->hp.pub_new = 0; same = (ULONG)e->hp.pub_same; n = e->hp.c.edid;
    reg_set(L"HotplugLastEdid", REG_BINARY, e->hp.pub_edid, e->hp.pub_bytes);
    reg_set(L"HotplugLastEdidSame", REG_DWORD, &same, sizeof(same));
    reg_set(L"HotplugEdidEvents", REG_DWORD, &n, sizeof(n));
}
static void hp_tick(PEXT e)
{
    struct ps5_icc_io io; ULONGLONG now = now_ms(); int r;
    icc_acquire(e);
    if (!e->hp_stop && e->mbox) { icc_io(e, &io, 200); ps5_hp_poll(&e->hp, &io, now); }
    ps5_hp_process(&e->hp, now);
    icc_release(e);
    hp_publish(e);
    if (e->hp_stop || !ps5_hp_due(&e->hp, now)) return;
    icc_io(e, &io, 12000); io.relax = icc_relax_yield;   /* PS5 Linux ICC_TIMEOUT_MSECS, as HdmiProbeDxe VideoStage */
    KeWaitForSingleObject(&e->hp_seq, Executive, KernelMode, FALSE, NULL);   /* v1.13: never interleave with a program */
    r = ps5_hp_replay(&e->hp, &io, &g_hp_packet, hp_gate, e);
    KeSetEvent(&e->hp_seq, IO_NO_INCREMENT, FALSE);
    now = now_ms();
    icc_acquire(e);
    ps5_hp_replay_done(&e->hp, r, now);
    ps5_hp_process(&e->hp, now);
    icc_release(e);
    if (e->hp_stop) return;
    log_event(e->self, 0x10, r, (LONG)e->hp.last_stage);
    hp_publish(e);
}
/* ---------------------------------------------------------------- v1.10 power button (UNTESTED ON HARDWARE) */
#define BTN_STALE_MS 5000          /* a set event nobody consumed within 5 s is cleared (no shutdown at a later consumer start) */
#define BTN_ENABLE_TRIES 5
/* hook in ps5_hp_note(): every ICC user (icc_note, hotplug poll, replay) passes notifications through here; inside the
 * ICC transaction under icc_lock, never blocks. Messages after btn_off are dropped (the drain must not create presses). */
static void btn_note(void *c, uint8_t svc, uint16_t type, const uint8_t *p, unsigned n)
{
    PEXT e = c;
    UNREFERENCED_PARAMETER(p); UNREFERENCED_PARAMETER(n);
    if (svc != PS5_BUTTON_SERVICE) return;
    e->btn_notes++;
    if (e->btn_on) (void)ps5_button_event(&e->keys, svc, type, now_ms());   /* 0x8010 down / 0x8011 up, <2 s = short */
}
static void btn_drop(void *c, uint8_t svc, uint16_t type, const uint8_t *p, unsigned n)
{
    UNREFERENCED_PARAMETER(c); UNREFERENCED_PARAMETER(svc); UNREFERENCED_PARAMETER(type);
    UNREFERENCED_PARAMETER(p); UNREFERENCED_PARAMETER(n);
}
/* service 0x08 type 1 {0 = power button, on}: the message HdmiProbeDxe sends ({0,1} in ButtonPoll, {0,0} at hand-off) */
static int btn_send(PEXT e, int on)
{
    const uint8_t req[2] = { 0, (uint8_t)(on != 0) };
    return icc_query_robust_ms(e, PS5_BUTTON_SERVICE, 1, req, sizeof(req), NULL, 0, NULL, 2000);
}
/* F20: switch notifications off, then drain what is queued. Idempotent; caller holds icc_lock (PASSIVE/APC). */
static void btn_off_locked(PEXT e, const char *why)
{
    struct ps5_icc_io io; ULONG n = 0;
    if (!e->btn_on || !e->mbox) return;
    InterlockedExchange(&e->btn_on, 0);
    e->btn_off_result = btn_send(e, 0);
    icc_io(e, &io, 200);
    while (n < 16 && ps5_icc_poll(&io, icc_note, e) > 0) n++;
    e->btn_drained += n; e->btn_offs++;
    ps5_hp_log(e, "button: notifications off (%s): result %ld, drained %lu", why, e->btn_off_result, n);
}
static void btn_off(PEXT e, const char *why)
{
    if (!e->btn_on) return;
    icc_acquire(e); btn_off_locked(e, why); icc_release(e);
}
/* HIGH_LEVEL, no locks, no logging (ps5_hp_log takes a spin lock). Skips ICC if an owner was interrupted mid-message. */
static VOID btn_bugcheck(PVOID buf, ULONG len)
{
    PEXT e = buf; struct ps5_icc_io io; const uint8_t req[2] = { 0, 0 };
    UNREFERENCED_PARAMETER(len);
    if (!e || !e->btn_on || !e->mbox || !e->bell || e->icc_owner) return;
    e->btn_on = 0;
    icc_io(e, &io, 200);               /* relax = KeStallExecutionProcessor, ticks = KeQueryPerformanceCounter */
    (void)ps5_icc_query_events(&io, PS5_BUTTON_SERVICE, 1, req, sizeof(req), NULL, 0, NULL, btn_drop, NULL);
    for (int i = 0; i < 16 && ps5_icc_poll(&io, btn_drop, NULL) > 0; i++) {}
}
/* \BaseNamedObjects\Ps5PowerButton (user mode: "Global\Ps5PowerButton"), auto-reset, SYSTEM + Administrators only:
 * a waiter consumes one short press; nobody else can open, wait (and thereby steal) or set it. */
static NTSTATUS btn_event_create(PEXT e)
{
    ULONG acl_buf[64]; PACL acl = (PACL)acl_buf; SECURITY_DESCRIPTOR sd; OBJECT_ATTRIBUTES oa; HANDLE h; NTSTATUS st;
    UNICODE_STRING name = RTL_CONSTANT_STRING(L"\\BaseNamedObjects\\Ps5PowerButton");
    if (e->btn_event) return STATUS_SUCCESS;
    if (!NT_SUCCESS(st = RtlCreateAcl(acl, sizeof(acl_buf), ACL_REVISION)) ||
        !NT_SUCCESS(st = RtlAddAccessAllowedAce(acl, ACL_REVISION, EVENT_ALL_ACCESS, SeExports->SeLocalSystemSid)) ||
        !NT_SUCCESS(st = RtlAddAccessAllowedAce(acl, ACL_REVISION, EVENT_ALL_ACCESS, SeExports->SeAliasAdminsSid)) ||
        !NT_SUCCESS(st = RtlCreateSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION)) ||
        !NT_SUCCESS(st = RtlSetDaclSecurityDescriptor(&sd, TRUE, acl, FALSE))) return st;
    InitializeObjectAttributes(&oa, &name, OBJ_KERNEL_HANDLE | OBJ_OPENIF, NULL, &sd);
    if (!NT_SUCCESS(st = ZwCreateEvent(&h, EVENT_ALL_ACCESS, &oa, SynchronizationEvent, FALSE))) return st;
    if (!NT_SUCCESS(st = ObReferenceObjectByHandle(h, EVENT_ALL_ACCESS, *ExEventObjectType, KernelMode, (PVOID *)&e->btn_event, NULL))) {
        ZwClose(h); e->btn_event = NULL; return st;
    }
    e->btn_handle = h;
    KeClearEvent(e->btn_event);
    return STATUS_SUCCESS;
}
static void btn_event_close(PEXT e)
{
    if (e->btn_event) { ObDereferenceObject(e->btn_event); e->btn_event = NULL; }
    if (e->btn_handle) { ZwClose(e->btn_handle); e->btn_handle = NULL; }
}
/* once per second: Parameters\EnablePowerButton (DWORD, default 1 = on; 0 = kill switch, notifications off) */
static void btn_config(PEXT e)
{
    ULONG want = reg_dword(L"EnablePowerButton", 1) != 0, prev = e->btn_cfg;
    NTSTATUS st;
    e->btn_cfg = want;
    if (want != prev) { ps5_hp_log(e, "button: EnablePowerButton %lu -> %lu", prev, want); e->btn_tries = 0; }
    if (!want || e->btn_sx) { if (e->btn_on) btn_off(e, !want ? "EnablePowerButton=0" : "Sx"); return; }
    if (e->btn_on || e->btn_tries >= BTN_ENABLE_TRIES) return;
    if (!NT_SUCCESS(st = btn_event_create(e))) {   /* no channel to user mode -> leave the notifications off */
        /* early boot: \BaseNamedObjects does not exist before smss -> retry every second, no limit, log once */
        if (!e->btn_event_logged++) ps5_hp_log(e, "button: event Ps5PowerButton not created yet (%08lx): retry each second", (ULONG)st);
        return;
    }
    icc_acquire(e);
    if (!e->hp_stop && e->mbox) {
        ps5_button_init(&e->keys, 1000);
        InterlockedExchange(&e->btn_on, 1);        /* before the send: a half-done enable is switched off as well */
        e->btn_enable_result = btn_send(e, 1);
        if (e->btn_enable_result) btn_off_locked(e, "enable failed");   /* error path: off + drain */
    }
    icc_release(e);
    e->btn_tries++;
    ps5_hp_log(e, "button: notifications on: result %ld (try %ld)", e->btn_enable_result, e->btn_tries);
    if (!e->btn_enable_result) e->btn_tries = 0;
}
/* every tick while on: poll (unless the hotplug tick already did), hand short presses to user mode */
static void btn_tick(PEXT e)
{
    struct ps5_icc_io io; ULONG presses = 0, longs = 0; int k;
    ULONGLONG now;
    icc_acquire(e);
    if (!e->hp_stop && e->mbox && e->btn_on) {
        if (e->hp_state != HP_ARMED) {
            icc_io(e, &io, 200);
            for (int i = 0; i < 8 && ps5_icc_poll(&io, icc_note, e) > 0; i++) {}
        }
        while ((k = ps5_button_pop(&e->keys)) != 0) { if (k == PS5_KEY_DOWN) presses++; else longs++; }
    }
    icc_release(e);
    now = now_ms();
    if (e->hp_stop) return;                         /* S5/stop in progress: no new shutdown request */
    if (longs) { e->btn_long += longs; ps5_hp_log(e, "button: %lu long press(es) (2..8 s) ignored", longs); }
    if (presses && e->btn_event) {
        e->btn_presses += presses;
        KeSetEvent(e->btn_event, IO_NO_INCREMENT, FALSE);
        e->btn_set_ms = now ? now : 1;
        ps5_hp_log(e, "button: short press -> Ps5PowerButton set (total %lu)", e->btn_presses);
        log_event(e->self, 0x12, (LONG)presses, (LONG)e->btn_presses);
        { ULONG n = e->btn_presses; reg_set(L"PowerButtonPresses", REG_DWORD, &n, sizeof(n)); }
    } else if (e->btn_set_ms && now - e->btn_set_ms >= BTN_STALE_MS) {
        e->btn_set_ms = 0;
        if (e->btn_event && KeReadStateEvent(e->btn_event)) {
            KeClearEvent(e->btn_event); e->btn_stale++;
            ps5_hp_log(e, "button: press not consumed within %u ms (no listener?) -> cleared", BTN_STALE_MS);
        }
    }
}
/* ---------------------------------------------------------------- v1.17 fan curve (PS5 Linux drivers/ps5/fan.c 1:1)
 * Start: the board's EMC servo table (icc_fan_update_autoservo_param), snapshot of the servo table and zone modes, quiet
 * backup table (EMC auto at a 50 C target if the host stops driving the fan). Every 500 ms: MainSoC temperature ->
 * quiet curve -> manual duty, emergency maximum at 90 C. Stop/S5/restart: original table back, firmware auto.
 * Switch: Parameters\FanCurve (DWORD, default 1; 0 = firmware auto, re-read every second). Caller holds icc_lock. */
enum { FC_IDLE = 0, FC_RUNNING = 1, FC_FAILED = 2 };
static NTSTATUS fc_msg(PEXT e, uint8_t svc, uint16_t type, const UCHAR *req, unsigned n, UCHAR *reply, unsigned rn, ULONG ms)
{
    unsigned got = 0;
    int r = icc_query_robust_ms(e, svc, type, req, n, e->fc_io, sizeof(e->fc_io), &got, ms);
    if (r) return icc_query_status(r);
    if (got < 4) return STATUS_DATA_ERROR;                       /* PS5 Linux: length < 0x10 -> -EPROTO */
    if (e->fc_io[0] | e->fc_io[1]) return STATUS_IO_DEVICE_ERROR;  /* service status */
    if (rn) { if (got < rn) return STATUS_DATA_ERROR; RtlCopyMemory(reply, e->fc_io, rn); }
    return STATUS_SUCCESS;
}
static NTSTATUS fc_read_temp(PEXT e, UCHAR zone, int *milli)
{
    UCHAR req[4] = { zone, 0, 0, 0 }, reply[4];
    NTSTATUS st = fc_msg(e, PS5_FC_TEMP_SERVICE, 1, req, sizeof(req), reply, sizeof(reply), 2000);
    if (NT_SUCCESS(st) && ps5_fc_decode_temp(reply, milli)) st = STATUS_DEVICE_DATA_ERROR;
    return st;
}
static NTSTATUS fc_read_duty(PEXT e, unsigned *duty)
{
    UCHAR reply[4];
    NTSTATUS st = fc_msg(e, PS5_FC_SERVICE, 1, NULL, 0, reply, sizeof(reply), 2000);
    if (!NT_SUCCESS(st)) return st;
    *duty = reply[2] | reply[3] << 8;
    return *duty <= 1024 ? STATUS_SUCCESS : STATUS_DATA_ERROR;
}
static NTSTATUS fc_set_mode(PEXT e, int mode)
{
    UCHAR req[4] = { 0, (UCHAR)mode, 0, 0 };
    if (e->fc_wire_mode == mode) return STATUS_SUCCESS;
    NTSTATUS st = fc_msg(e, PS5_FC_SERVICE, 2, req, sizeof(req), NULL, 0, 2000);
    if (NT_SUCCESS(st)) e->fc_wire_mode = mode;
    return st;
}
static NTSTATUS fc_get_mode(PEXT e, UCHAR zone, UCHAR *mode)
{
    UCHAR req[4] = { zone, 0, 0, 0 }, reply[4];
    NTSTATUS st = fc_msg(e, PS5_FC_SERVICE, 3, req, sizeof(req), reply, sizeof(reply), 2000);
    if (NT_SUCCESS(st)) *mode = reply[2];
    return st;
}
static NTSTATUS fc_write_pwm(PEXT e, unsigned pwm)
{
    UCHAR req[4] = { 0 };
    unsigned duty = ps5_fc_pwm_to_duty(pwm);
    NTSTATUS st = fc_set_mode(e, PS5_FC_MODE_MANUAL);
    if (!NT_SUCCESS(st)) return st;
    req[2] = (UCHAR)duty; req[3] = (UCHAR)(duty >> 8);
    st = fc_msg(e, PS5_FC_SERVICE, 4, req, sizeof(req), NULL, 0, 2000);
    if (NT_SUCCESS(st)) e->fc.requested_pwm = pwm;
    return st;
}
static NTSTATUS fc_upload(PEXT e, const UCHAR *table)        /* icc_fan_update_param: service 0x0a type 0x0e */
{
    RtlZeroMemory(e->fc_req, 4);
    RtlCopyMemory(e->fc_req + 4, table, PS5_FC_SERVO_SIZE);
    return fc_msg(e, PS5_FC_SERVICE, 0x0e, e->fc_req, sizeof(e->fc_req), NULL, 0, 12000);
}
static void fc_fallback(PEXT e)
{
    e->fc_fault = TRUE; e->fc_wire_mode = -1;
    (void)fc_set_mode(e, PS5_FC_MODE_AUTO);
}
static NTSTATUS fc_tick(PEXT e)                               /* ps5_fan_update_locked, automatic mode */
{
    int t = 0; unsigned pwm;
    NTSTATUS st = fc_read_temp(e, 0, &t);
    if (NT_SUCCESS(st)) {
        e->fc_last_temp = t;
        if (ps5_fc_step(&e->fc, t, &pwm) == PS5_FC_MODE_MAXIMUM) {
            st = fc_set_mode(e, PS5_FC_MODE_MAXIMUM);
            if (NT_SUCCESS(st)) e->fc.requested_pwm = 255;
        } else st = fc_write_pwm(e, pwm);
    }
    if (NT_SUCCESS(st)) { e->fc_fault = FALSE; e->fc_ticks++; return st; }
    if (e->fc_errors++ < 20) ps5_hp_log(e, "fan: control failed 0x%08lx; using firmware auto", st);
    fc_fallback(e);
    return st;
}
static VOID fc_bugcheck(PVOID buf, ULONG len)                /* HIGH_LEVEL: leave the fan to the EMC (backup table) */
{
    PEXT e = buf; struct ps5_icc_io io; const uint8_t req[4] = { 0, PS5_FC_MODE_AUTO, 0, 0 };
    UNREFERENCED_PARAMETER(len);
    if (!e || e->fc_phase != FC_RUNNING || !e->mbox || !e->bell || e->icc_owner) return;
    icc_io(e, &io, 200);
    (void)ps5_icc_query_events(&io, PS5_FC_SERVICE, 2, req, sizeof(req), NULL, 0, NULL, btn_drop, NULL);
}
/* PASSIVE, no lock: board/GDDR6 ids from the GameOS boot parameters (reserved, not Windows RAM) like PS5 Linux */
static const uint8_t *fc_board(PEXT e)
{
    PHYSICAL_ADDRESS pa; pa.QuadPart = 0x9b000;
    ULONG64 board = 0; USHORT gddr6 = 0; const char *name = "?";
    PVOID va = MmMapIoSpaceEx(pa, 0x2000, PAGE_READONLY | PAGE_NOCACHE);
    if (va) {
        board = *(volatile ULONG64 *)((PUCHAR)va + 0x328); gddr6 = *(volatile USHORT *)((PUCHAR)va + 0x1184);
        MmUnmapIoSpace(va, 0x2000);
    }
    const uint8_t *table = ps5_fc_board_table(board, gddr6, &name);
    reg_set(L"FanBoardId", REG_QWORD, &board, sizeof(board));
    { ULONG g = gddr6; reg_set(L"FanGddr6Id", REG_DWORD, &g, sizeof(g)); }
    ps5_hp_log(e, "fan: board %s (id %016I64x gddr6 %04x), servo table %s", name, board, gddr6, table ? "found" : "not updated");
    return table;
}
static NTSTATUS fc_start(PEXT e, const uint8_t *table)
{
    NTSTATUS st; int t; unsigned duty; UCHAR mode = 0;
    if (table && !NT_SUCCESS(st = fc_upload(e, table))) { ps5_hp_log(e, "fan: EMC rejected board table 0x%08lx", st); return st; }
    st = fc_read_temp(e, 0, &t);
    if (!NT_SUCCESS(st)) { ps5_hp_log(e, "fan: MainSoC temperature unavailable 0x%08lx", st); return st; }
    st = fc_msg(e, PS5_FC_SERVICE, 0x0f, NULL, 0, e->fc_req, sizeof(e->fc_req), 12000);   /* snapshot */
    if (!NT_SUCCESS(st)) { ps5_hp_log(e, "fan: servo snapshot failed 0x%08lx", st); return st; }
    RtlCopyMemory(e->fc_original, e->fc_req + 4, PS5_FC_SERVO_SIZE);
    if (ps5_fc_quiet_left(e->fc_original) && e->fc_fw_original_ok) {   /* v1.17.2: the firmware set the quiet table */
        RtlCopyMemory(e->fc_original, e->fc_fw_original, PS5_FC_SERVO_SIZE);
        ps5_hp_log(e, "fan: quiet table set by the firmware, GameOS table from PswFanOriginal");
    }
    for (UCHAR zone = 0; zone < 4; zone++) {
        st = fc_get_mode(e, zone, &mode);
        if (!NT_SUCCESS(st) || mode == PS5_FC_MODE_AUTO) continue;
        if (zone) { ps5_hp_log(e, "fan: zone %u in mode %u", zone, mode); return STATUS_DEVICE_BUSY; }
        ps5_hp_log(e, "fan: MainSoC zone left in mode %u, returning it to auto", mode);
        e->fc_wire_mode = -1;
        st = fc_set_mode(e, PS5_FC_MODE_AUTO);
        if (NT_SUCCESS(st)) st = fc_get_mode(e, 0, &mode);
        if (NT_SUCCESS(st) && mode != PS5_FC_MODE_AUTO) st = STATUS_DEVICE_BUSY;
        if (!NT_SUCCESS(st)) return st;
    }
    if (!NT_SUCCESS(st)) return st;
    e->fc_original_ok = !ps5_fc_quiet_left(e->fc_original);
    if (!e->fc_original_ok) ps5_hp_log(e, "fan: servo table already quiet (earlier run), not restoring it");
    ps5_fc_prepare_backup(e->fc_backup, e->fc_original);
    st = fc_upload(e, e->fc_backup);
    if (!NT_SUCCESS(st)) { (void)fc_upload(e, e->fc_original); ps5_hp_log(e, "fan: backup table rejected 0x%08lx", st); return st; }
    st = fc_read_duty(e, &duty);
    if (!NT_SUCCESS(st)) return st;
    ps5_fc_init(&e->fc, ps5_fc_duty_to_pwm(e->fc_original[2] | e->fc_original[3] << 8), ps5_fc_duty_to_pwm(duty));
    e->fc_phase = FC_RUNNING;
    if (!e->fc_bug_reg) e->fc_bug_reg = KeRegisterBugCheckCallback(&e->fc_bug, fc_bugcheck, e, sizeof(*e), (PUCHAR)"ps5pwr-fan");
    ps5_hp_log(e, "fan: quiet curve on, MainSoC %d mC, duty %u", t, duty);
    return fc_tick(e);
}
static void fc_stop_locked(PEXT e, const char *why, int keep_quiet)
{
    if (e->fc_phase != FC_RUNNING) return;
    e->fc_phase = FC_IDLE;
    /* v1.19.2 (user 26.11.: the fan went up for a moment at every restart): a restart through the PSWloader warm re-entry stays on
     * the quiet backup table under EMC auto - the GameOS table came back here and drove the fan up until the firmware put up its
     * quiet table again. The GameOS table only comes back when the console really powers off (GameOS starts next). */
    NTSTATUS st = keep_quiet ? fc_upload(e, e->fc_backup) : e->fc_original_ok ? fc_upload(e, e->fc_original) : STATUS_SUCCESS;
    e->fc_wire_mode = -1;
    NTSTATUS st2 = fc_set_mode(e, PS5_FC_MODE_AUTO);
    ps5_hp_log(e, "fan: curve off (%s): table 0x%08lx, auto 0x%08lx", why, st, st2);
}
static void fc_stop(PEXT e, const char *why, int keep_quiet)
{
    if (e->fc_phase != FC_RUNNING || !e->mbox) return;
    icc_acquire(e); fc_stop_locked(e, why, keep_quiet); icc_release(e);
}
static void fc_service(PEXT e)                               /* hotplug thread, every 500 ms */
{
    if (!(e->fc_polls++ & 1)) e->fc_cfg = reg_dword(L"FanCurve", 1) ? 1 : 0;   /* once per second */
    if (e->btn_sx) return;                                    /* leaving S0: dispatch_power restored the table */
    if (!e->fc_cfg) { fc_stop(e, "FanCurve 0", 0); e->fc_tries = 0; return; }
    if (e->fc_phase == FC_RUNNING) { icc_acquire(e); (void)fc_tick(e); icc_release(e); return; }
    if (e->fc_tries >= 3 || now_ms() < e->fc_retry_at) return;
    const uint8_t *table = fc_board(e);
    if (!e->fc_fw_original_ok) {   /* v1.17.2: PASSIVE, outside icc_lock */
        ULONG l = PS5_FC_SERVO_SIZE;
        NTSTATUS vs = fw_var(L"PswFanOriginal", e->fc_fw_original, &l);
        e->fc_fw_original_ok = NT_SUCCESS(vs) && l == PS5_FC_SERVO_SIZE && !ps5_fc_quiet_left(e->fc_fw_original);
    }
    icc_acquire(e);
    e->fc_wire_mode = -1;
    NTSTATUS st = fc_start(e, table);
    if (!NT_SUCCESS(st) && e->fc_phase != FC_RUNNING) {
        e->fc_phase = FC_FAILED; e->fc_wire_mode = -1; (void)fc_set_mode(e, PS5_FC_MODE_AUTO);
        e->fc_tries++; e->fc_retry_at = now_ms() + 30000;
        ps5_hp_log(e, "fan: curve start failed 0x%08lx (try %lu of 3), firmware auto", st, e->fc_tries);
    }
    icc_release(e);
    { ULONG v = (ULONG)e->fc_phase; reg_set(L"FanCurveState", REG_DWORD, &v, sizeof(v)); }
}

static VOID hp_thread(PVOID ctx)
{
    PEXT e = ctx; ULONG tick = 0, tries = 0; ULONGLONG retry_at = 0;
    LARGE_INTEGER period;
    while (!e->hp_stop) {
        int second = tick++ % (1000 / HP_TICK_MS) == 0;
        if (second) btn_config(e);                       /* v1.10: EnablePowerButton once per second */
        if (second) {                                    /* HotplugMode once per second */
            ULONG m = reg_dword(L"HotplugMode", PS5_HP_PICK);   /* v1.18.0: on by default (was 0 = off) */
            if (m > PS5_HP_PICK) m = 0;   /* v1.13: 4 = PICK */
            if (m != e->hp_cfg) {
                ps5_hp_log(e, "hotplug: HotplugMode %lu -> %lu", e->hp_cfg, m);
                if (!m && e->hp_state == HP_ARMED) hp_disarm(e);
                if (!m) e->hp_state = HP_IDLE;
                else if (e->hp_state == HP_ARMED) { e->hp.mode = m; e->hp.pending = 0; }
                else { e->hp_state = HP_IDLE; tries = 0; retry_at = 0; }
                e->hp_cfg = m;
            }
        }
        if (e->hp_cfg && (e->hp_state == HP_IDLE || e->hp_state == HP_ARM_FAILED) && tries < HP_ARM_TRIES &&
            now_ms() >= retry_at) {
            e->hp_state = hp_arm(e, e->hp_cfg);
            if (e->hp_state == HP_ARM_FAILED && ++tries == HP_ARM_TRIES)
                ps5_hp_log(e, "hotplug: arm failed %u times: change HotplugMode to retry", HP_ARM_TRIES);
            retry_at = now_ms() + HP_ARM_RETRY_MS;
        }
        if (e->hp_state == HP_ARMED) hp_tick(e);
        if (e->btn_on) btn_tick(e);                      /* v1.10 */
        if (tick & 1) fc_service(e);                     /* v1.17: fan curve every 500 ms */
        period.QuadPart = -10LL * 1000 * HP_TICK_MS;     /* v1.10: always 250 ms (was 1 s while not armed) */
        KeWaitForSingleObject(&e->hp_wake, Executive, KernelMode, FALSE, &period);
    }
    btn_off(e, "thread stops");                          /* v1.10: stop/remove/unload/S5 - mbox still mapped here */
    fc_stop(e, "thread stops", 0);                          /* v1.17: original servo table + firmware auto */
    ps5_hp_log(e, "hotplug: thread stops");
    PsTerminateSystemThread(STATUS_SUCCESS);
}
NTSYSAPI NTSTATUS NTAPI ZwWaitForSingleObject(HANDLE Handle, BOOLEAN Alertable, PLARGE_INTEGER Timeout);   /* ntifs.h */
static void hp_start(PEXT e)
{
    HANDLE h; OBJECT_ATTRIBUTES oa;
    if (e->hp_thread) return;
    e->hp_cfg = 0; e->hp_state = HP_IDLE;            /* (re)start: HotplugMode is read and armed afresh */
    InterlockedExchange(&e->hp_stop, 0);
    KeClearEvent(&e->hp_wake);
    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(PsCreateSystemThread(&h, THREAD_ALL_ACCESS, &oa, NULL, NULL, hp_thread, e))) return;
    if (!NT_SUCCESS(ObReferenceObjectByHandle(h, SYNCHRONIZE, *PsThreadType, KernelMode, (PVOID *)&e->hp_thread, NULL))) {
        e->hp_thread = NULL;
        InterlockedExchange(&e->hp_stop, 1); KeSetEvent(&e->hp_wake, IO_NO_INCREMENT, FALSE);
        ZwWaitForSingleObject(h, FALSE, NULL);
    }
    ZwClose(h);
}
/* PASSIVE_LEVEL; waits at most for the stage in flight (ICC deadline 12 s) */
static void hp_halt(PEXT e)
{
    InterlockedExchange(&e->hp_stop, 1);
    KeSetEvent(&e->hp_wake, IO_NO_INCREMENT, FALSE);
    if (e->hp_thread) {
        KeWaitForSingleObject(e->hp_thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(e->hp_thread);
        e->hp_thread = NULL;
    }
}

static NTSTATUS complete(PIRP irp, NTSTATUS st, ULONG_PTR info);

/* ---------------------------------------------------------------- PnP */
static NTSTATUS dispatch_pnp(PDEVICE_OBJECT d, PIRP irp)
{
    if (d == g_ctl) return complete(irp, STATUS_NOT_SUPPORTED, 0);
    PEXT e = d->DeviceExtension;
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS st;
    switch (s->MinorFunction) {
    case IRP_MN_START_DEVICE:
        st = forward_sync(e, irp);
        if (NT_SUCCESS(st)) {
            st = map_resources(e, s->Parameters.StartDevice.AllocatedResourcesTranslated);
            if (NT_SUCCESS(st)) {
                icc_acquire(e);
                e->led_result = icc_query_robust(e, 0x09, LED_TYPE, k_white_dim, sizeof(k_white_dim), NULL, 0, NULL);
                icc_release(e);
                log_event(d, 1, e->led_result, 0);     /* 1 = started, data0 = LED query result (0 ok) */
                e->started = 1;
                g_fdo_ext = e;
                hp_start(e);                           /* v1.4: idle unless HotplugMode != 0; v1.10 power button */
                if (!e->btn_bug_reg)                   /* v1.10: F20 also after a bugcheck (notifications off) */
                    e->btn_bug_reg = KeRegisterBugCheckCallback(&e->btn_bug, btn_bugcheck, e, sizeof(*e), (PUCHAR)"ps5pwr");
            }
        }
        irp->IoStatus.Status = st;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return st;
    case IRP_MN_REMOVE_DEVICE:
        oc_safe_reset("remove");                       /* v1.14.0 */
        hp_halt(e);                                    /* v1.10: the thread switches the button off on its way out */
        btn_off(e, "remove");
        if (e->btn_bug_reg) { KeDeregisterBugCheckCallback(&e->btn_bug); e->btn_bug_reg = 0; }
        if (e->fc_bug_reg) { KeDeregisterBugCheckCallback(&e->fc_bug); e->fc_bug_reg = 0; }   /* v1.17 */
        btn_event_close(e);
        pick_event_close(e);   /* v1.13 */
        if (g_fdo_ext == e) g_fdo_ext = NULL;
        irp->IoStatus.Status = STATUS_SUCCESS;
        IoSkipCurrentIrpStackLocation(irp);
        st = IoCallDriver(e->lower, irp);
        unmap(e);
        IoDetachDevice(e->lower);
        IoDeleteDevice(d);
        return st;
    case IRP_MN_STOP_DEVICE:
    case IRP_MN_SURPRISE_REMOVAL:
        oc_safe_reset("stop");                         /* v1.14.0 */
        hp_halt(e);                                    /* v1.4: no ICC access after this point */
        btn_off(e, "stop");                            /* v1.10: no-op if the thread already did it */
        unmap(e);
        /* fall through */
    case IRP_MN_QUERY_STOP_DEVICE:
    case IRP_MN_QUERY_REMOVE_DEVICE:
        /* v1.19.0 (HW 26.11.: a driver update reinstalled 104D:9107 in the running Windows - the Realtek boot disk was reset
         * in the same second, then BUGCODE_USB3_DRIVER; a stop/D3 of the Salina glue function cuts more than this driver,
         * up to the whole console): never stopped or removed while Windows runs. A new ps5pwr is taken at the next start. */
        return complete(irp, STATUS_UNSUCCESSFUL, 0);
    case IRP_MN_QUERY_PNP_DEVICE_STATE:                /* v1.19.0: Device Manager cannot disable it either */
        st = forward_sync(e, irp);
        if (st == STATUS_NOT_SUPPORTED) { st = STATUS_SUCCESS; irp->IoStatus.Information = 0; }
        if (NT_SUCCESS(st)) irp->IoStatus.Information |= PNP_DEVICE_NOT_DISABLEABLE;
        irp->IoStatus.Status = st;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return st;
    case IRP_MN_CANCEL_STOP_DEVICE:
    case IRP_MN_CANCEL_REMOVE_DEVICE:
        irp->IoStatus.Status = STATUS_SUCCESS;
        /* fall through */
    default:
        IoSkipCurrentIrpStackLocation(irp);
        return IoCallDriver(e->lower, irp);
    }
}

/* ---------------------------------------------------------------- power: S5 -> ICC off / reboot */
static NTSTATUS dispatch_power(PDEVICE_OBJECT d, PIRP irp)
{
    PEXT e = d->DeviceExtension;
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    if (d == g_ctl) { irp->IoStatus.Status = STATUS_SUCCESS; PoStartNextPowerIrp(irp); IoCompleteRequest(irp, IO_NO_INCREMENT); return STATUS_SUCCESS; }
    if (s->MinorFunction == IRP_MN_SET_POWER && s->Parameters.Power.Type == SystemPowerState &&
        s->Parameters.Power.State.SystemState == PowerSystemShutdown) {
        InterlockedIncrement(&e->s5_seen);
        e->shutdown_type = (LONG)s->Parameters.Power.ShutdownType;
        InterlockedExchange(&e->hp_stop, 1);           /* v1.4: hotplug thread leaves ICC after its stage */
        KeSetEvent(&e->hp_wake, IO_NO_INCREMENT, FALSE);
        oc_safe_reset("S5");                           /* v1.14.0: before the console is switched off or restarted */
    }
    /* v1.10: leaving S0 (sleep/hibernate/shutdown/restart): button notifications off + drained before anything else
     * (F20: a queued press notification made the next shutdown hang without S5); back in S0 the thread re-enables */
    if (s->MinorFunction == IRP_MN_SET_POWER && s->Parameters.Power.Type == SystemPowerState && d != g_ctl) {
        if (s->Parameters.Power.State.SystemState == PowerSystemWorking) InterlockedExchange(&e->btn_sx, 0);
        else {
            InterlockedExchange(&e->btn_sx, 1);
            if (KeGetCurrentIrql() == PASSIVE_LEVEL) {
                btn_off(e, s->Parameters.Power.State.SystemState == PowerSystemShutdown ? "S5" : "Sx");
                UCHAR wr = 0; ULONG wl = sizeof(wr);   /* v1.19.2: restart through the PSWloader warm re-entry -> stay quiet */
                int warm = s->Parameters.Power.State.SystemState == PowerSystemShutdown && s->Parameters.Power.ShutdownType == PowerActionShutdownReset &&
                           NT_SUCCESS(fw_var(L"PswWarmReentry", &wr, &wl)) && wr == 1;
                fc_stop(e, warm ? "restart (quiet table kept)" : s->Parameters.Power.State.SystemState == PowerSystemShutdown ? "S5" : "Sx", warm);   /* v1.17 */
            }
        }
    }
    if (s->MinorFunction == IRP_MN_SET_POWER && s->Parameters.Power.Type == SystemPowerState &&
        s->Parameters.Power.State.SystemState == PowerSystemShutdown && e->mbox && e->bell &&
        KeGetCurrentIrql() == PASSIVE_LEVEL && !InterlockedExchange(&e->power_sent, 1)) {
        int reboot = s->Parameters.Power.ShutdownType == PowerActionShutdownReset;
        UCHAR warm = 0; ULONG wl = sizeof(warm);
        if (reboot && NT_SUCCESS(fw_var(L"PswWarmReentry", &warm, &wl)) && warm == 1) {   /* v1.16.0 */
            e->power_result = 0x57;            /* 'W': restart left to Windows -> firmware ResetSystem */
            PoStartNextPowerIrp(irp);
            IoSkipCurrentIrpStackLocation(irp);
            return PoCallDriver(e->lower, irp);
        }
        NTSTATUS st = forward_sync(e, irp);        /* let the bus finish first */
        /* deeper devnodes (USB storage behind xHCI) get their S5 IRP before this shallow one;
         * give anything still in flight a moment, then hand the console to the ICC power service */
        LARGE_INTEGER wait; wait.QuadPart = -10LL * 1000 * 1000;   /* 1 s */
        KeDelayExecutionThread(KernelMode, FALSE, &wait);
        icc_acquire(e);
        e->power_result = icc_power_robust(e, reboot);  /* on success the power is cut before we return */
        icc_release(e);
        PoStartNextPowerIrp(irp);
        irp->IoStatus.Status = st;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return st;
    }
    PoStartNextPowerIrp(irp);
    IoSkipCurrentIrpStackLocation(irp);
    return PoCallDriver(e->lower, irp);
}

static NTSTATUS complete(PIRP irp, NTSTATUS st, ULONG_PTR info)
{
    irp->IoStatus.Status = st; irp->IoStatus.Information = info;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return st;
}
static NTSTATUS dispatch_ctl(PDEVICE_OBJECT d, PIRP irp)
{
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    UNREFERENCED_PARAMETER(d);
    if (s->MajorFunction != IRP_MJ_DEVICE_CONTROL) return complete(irp, STATUS_SUCCESS, 0);   /* create/close */
    PEXT e = g_fdo_ext;
    ULONG code = s->Parameters.DeviceIoControl.IoControlCode;
    if (code == IOCTL_PS5PWR_STATUS) {
        if (s->Parameters.DeviceIoControl.OutputBufferLength < sizeof(PS5PWR_STATUS)) return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        PS5PWR_STATUS *o = irp->AssociatedIrp.SystemBuffer;
        RtlZeroMemory(o, sizeof(*o));
        if (e) { o->led_result = e->led_result; o->s5_seen = e->s5_seen; o->shutdown_type = e->shutdown_type;
                 o->power_result = e->power_result; o->started = e->started;
                 o->sow = e->sow; o->sor = e->sor; o->mbox_lo = e->mbox_pa.LowPart; o->bell_lo = e->bell_pa.LowPart; o->drained = e->drained; }
        return complete(irp, STATUS_SUCCESS, sizeof(*o));
    }
    if (code == IOCTL_PS5PWR_POWER) {
        if (!e || !e->mbox || !e->bell) return complete(irp, STATUS_DEVICE_NOT_READY, 0);
        if (s->Parameters.DeviceIoControl.InputBufferLength < sizeof(ULONG)) return complete(irp, STATUS_INVALID_PARAMETER, 0);
        ULONG what = *(ULONG *)irp->AssociatedIrp.SystemBuffer;
        if (what > 1) return complete(irp, STATUS_INVALID_PARAMETER, 0);
        icc_acquire(e);
        btn_off_locked(e, "IOCTL power");              /* v1.10: F20 on the direct path as well */
        e->power_result = icc_power_robust(e, (int)what);
        icc_release(e);
        return complete(irp, e->power_result ? STATUS_IO_DEVICE_ERROR : STATUS_SUCCESS, 0);
    }
    if (code == IOCTL_PS5PWR_HOTPLUG) {
        if (s->Parameters.DeviceIoControl.OutputBufferLength < sizeof(PS5PWR_HOTPLUG)) return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        PS5PWR_HOTPLUG *o = irp->AssociatedIrp.SystemBuffer;
        RtlZeroMemory(o, sizeof(*o));
        o->version = 1; o->size = sizeof(*o);
        if (e) {   /* diagnostic snapshot; only the log ring is copied under its lock */
            struct ps5_hp *hp = &e->hp; KIRQL irql;
            o->cfg_mode = e->hp_cfg; o->state = e->hp_state; o->mode_source = (ULONG)hp->source; o->mode_id = hp->mode_id;
            o->bridge_null = (ULONG)hp->bridge_null; o->pending = (ULONG)hp->pending;
            o->boot_edid_bytes = hp->boot_edid_bytes; o->last_edid_bytes = hp->pub_bytes; o->last_edid_same = (ULONG)hp->pub_same;
            o->icc_tolerated = ps5_icc_tolerated.count; o->icc_dropped = ps5_icc_dropped.count;
            o->last_result = (ULONG)hp->last_result; o->last_stage = hp->last_stage;
            RtlCopyMemory(o->counters, &hp->c, sizeof(hp->c));
            RtlCopyMemory(o->bridge, &hp->bridge, sizeof(hp->bridge));
            RtlCopyMemory(o->last_edid, hp->pub_edid, hp->pub_bytes < sizeof(o->last_edid) ? hp->pub_bytes : sizeof(o->last_edid));
            KeAcquireSpinLock(&e->log_lock, &irql);
            o->log_next = e->log_next;
            RtlCopyMemory(o->log, e->log, sizeof(o->log));
            KeReleaseSpinLock(&e->log_lock, irql);
        }
        return complete(irp, STATUS_SUCCESS, sizeof(*o));
    }
    if (code == IOCTL_PS5PWR_FAN_GET || code == IOCTL_PS5PWR_FAN_SET) {
        if (!e || !e->started || !e->mbox || !e->bell) return complete(irp, STATUS_DEVICE_NOT_READY, 0);
        if (s->Parameters.DeviceIoControl.OutputBufferLength < sizeof(PS5PWR_FAN_STATE))
            return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        PS5PWR_FAN_STATE *o = irp->AssociatedIrp.SystemBuffer;
        ULONG target = 0;
        if (code == IOCTL_PS5PWR_FAN_SET) {
            if (s->Parameters.DeviceIoControl.InputBufferLength < sizeof(ULONG))
                return complete(irp, STATUS_INVALID_PARAMETER, 0);
            target = *(ULONG *)irp->AssociatedIrp.SystemBuffer;
            if (target < 50 || target > 100) return complete(irp, STATUS_INVALID_PARAMETER, 0);   /* v1.8.2: 50 = PS5 Linux quiet servo TARGETTEMP (50<<8) */
            if (e->fc_phase == FC_RUNNING) return complete(irp, STATUS_DEVICE_BUSY, 0);   /* v1.17: the fan curve drives the fan */
        }
        RtlZeroMemory(o, sizeof(*o));
        ULONG reply_bytes = 0;
        icc_acquire(e);
        NTSTATUS st = code == IOCTL_PS5PWR_FAN_SET ? fan_set_target(e, target, o->curset, &reply_bytes) : fan_read_curset(e, o->curset, &reply_bytes);
        icc_release(e);
        if (!NT_SUCCESS(st)) return complete(irp, st, 0);
        o->target_c = ps5pwr_fan_target_c(o->curset);
        o->reply_bytes = reply_bytes;
        return complete(irp, STATUS_SUCCESS, sizeof(*o));
    }
    if (code == IOCTL_PS5PWR_BUTTON) {
        if (!e) return complete(irp, STATUS_DEVICE_NOT_READY, 0);
        if (s->Parameters.DeviceIoControl.OutputBufferLength < sizeof(PS5PWR_BUTTON)) return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        PS5PWR_BUTTON *o = irp->AssociatedIrp.SystemBuffer;
        RtlZeroMemory(o, sizeof(*o));
        o->version = 0x010a; o->cfg = e->btn_cfg; o->on = (ULONG)e->btn_on; o->sx = (ULONG)e->btn_sx;
        o->enable_result = e->btn_enable_result; o->off_result = e->btn_off_result; o->tries = e->btn_tries;
        o->notes = e->btn_notes; o->presses = e->btn_presses; o->longs = e->btn_long; o->drained = e->btn_drained;
        o->stale = e->btn_stale; o->offs = e->btn_offs; o->event_ok = e->btn_event != NULL; o->bugcheck_cb = (ULONG)e->btn_bug_reg;
        return complete(irp, STATUS_SUCCESS, sizeof(*o));
    }
    if (code == IOCTL_PS5PWR_FAN_STATUS) {
        static const UCHAR zones[5] = { 0, 1, 2, 3, 18 };
        if (!e || !e->started || !e->mbox || !e->bell) return complete(irp, STATUS_DEVICE_NOT_READY, 0);
        if (s->Parameters.DeviceIoControl.OutputBufferLength < sizeof(PS5PWR_FAN_STATUS))
            return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        PS5PWR_FAN_STATUS *o = irp->AssociatedIrp.SystemBuffer;
        UCHAR req[4], rep[32];
        unsigned bytes = 0;
        RtlZeroMemory(o, sizeof(*o));
        icc_acquire(e);
        if (!icc_query_robust_ms(e, 0x0a, 0x01, NULL, 0, rep, sizeof(rep), &bytes, 12000) && bytes >= 4) {
            ULONG d = (ULONG)rep[2] | ((ULONG)rep[3] << 8);
            if (d <= 1024) { o->duty = d; o->valid |= 1; }
        }
        for (unsigned i = 0; i < 5; i++) {
            RtlZeroMemory(req, sizeof(req)); req[0] = zones[i]; bytes = 0;
            if (!icc_query_robust_ms(e, 0x0b, 0x01, req, sizeof(req), rep, sizeof(rep), &bytes, 12000) && bytes >= 4) {
                LONG raw = (LONG)(SHORT)((USHORT)rep[2] | ((USHORT)rep[3] << 8));
                if (raw >= 0 && raw < 125 * 256) { o->zone_milli[i] = (raw * 1000 + 128) / 256; o->valid |= 2u << i; }
            }
        }
        icc_release(e);
        return complete(irp, o->valid ? STATUS_SUCCESS : STATUS_DEVICE_DATA_ERROR, o->valid ? sizeof(*o) : 0);
    }
    if (code == IOCTL_PS5PWR_TCTL) {
        if (!e || !e->started) return complete(irp, STATUS_DEVICE_NOT_READY, 0);
        if (s->Parameters.DeviceIoControl.OutputBufferLength < sizeof(PS5PWR_TCTL))
            return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        PS5PWR_TCTL *o = irp->AssociatedIrp.SystemBuffer;
        RtlZeroMemory(o, sizeof(*o));
        NTSTATUS st = tctl_read(o);
        return complete(irp, st, NT_SUCCESS(st) ? sizeof(*o) : 0);
    }
    if (code == IOCTL_PS5PWR_HP_PICK) {   /* v1.13: last pick (seq 0 = none yet) */
        if (!e) return complete(irp, STATUS_DEVICE_NOT_READY, 0);
        if (s->Parameters.DeviceIoControl.OutputBufferLength < sizeof(struct ps5_hp_pick)) return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        icc_acquire(e);
        RtlCopyMemory(irp->AssociatedIrp.SystemBuffer, &e->hp.pick, sizeof(struct ps5_hp_pick));
        icc_release(e);
        return complete(irp, STATUS_SUCCESS, sizeof(struct ps5_hp_pick));
    }
    if (code == IOCTL_PS5PWR_BRIDGE_READ) {   /* v1.18.3: read bridge registers (FLAVA3) for diagnostics, nothing written */
        struct ps5_icc_io io; USHORT reg, cnt; LONG r; UCHAR *b = irp->AssociatedIrp.SystemBuffer;
        if (!e || !e->started || !e->mbox || e->hp_state != HP_ARMED || e->hp_stop) return complete(irp, STATUS_DEVICE_NOT_READY, 0);
        if (s->Parameters.DeviceIoControl.InputBufferLength < 4 || s->Parameters.DeviceIoControl.OutputBufferLength < 20) return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        if (KeGetCurrentIrql() != PASSIVE_LEVEL) return complete(irp, STATUS_INVALID_DEVICE_STATE, 0);
        reg = ((USHORT *)b)[0]; cnt = ((USHORT *)b)[1];
        if (cnt < 1 || cnt > 16) return complete(irp, STATUS_INVALID_PARAMETER, 0);
        RtlZeroMemory(b, 20);
        KeWaitForSingleObject(&e->hp_seq, Executive, KernelMode, FALSE, NULL);
        icc_io(e, &io, 2000); io.relax = icc_relax_yield;
        r = ps5_hdmi_register_read(&io, reg, cnt, b + 4, ps5_hp_note, &e->hp);
        KeSetEvent(&e->hp_seq, IO_NO_INCREMENT, FALSE);
        *(LONG *)b = r;
        return complete(irp, STATUS_SUCCESS, 20);
    }
    if (code == IOCTL_PS5PWR_HP_PROGRAM) {   /* v1.13: program the bridge for a mode id; needs armed hotplug (converter config) */
        PS5PWR_HP_PROGRAM_OUT *o; struct ps5_icc_io io; unsigned st[PS5_HP_MAX_STAGES], n = 0; int res[PS5_HP_MAX_STAGES], r; ULONG id;
        if (!e || !e->started || !e->mbox || e->hp_state != HP_ARMED || e->hp_stop) return complete(irp, STATUS_DEVICE_NOT_READY, 0);
        if (s->Parameters.DeviceIoControl.InputBufferLength < sizeof(ULONG) ||
            s->Parameters.DeviceIoControl.OutputBufferLength < sizeof(PS5PWR_HP_PROGRAM_OUT)) return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        if (KeGetCurrentIrql() != PASSIVE_LEVEL) return complete(irp, STATUS_INVALID_DEVICE_STATE, 0);
        id = *(ULONG *)irp->AssociatedIrp.SystemBuffer;
        o = irp->AssociatedIrp.SystemBuffer;
        KeWaitForSingleObject(&e->hp_seq, Executive, KernelMode, FALSE, NULL);
        icc_io(e, &io, 12000); io.relax = icc_relax_yield;
        r = ps5_hp_program_mode(&e->hp, &io, &g_hp_packet, id, hp_gate, e, st, res, &n);
        KeSetEvent(&e->hp_seq, IO_NO_INCREMENT, FALSE);
        RtlZeroMemory(o, sizeof(*o));
        o->result = r; o->count = n; o->last_stage = r ? e->hp.last_stage : 0; o->mode_id = id;
        for (unsigned i = 0; i < n && i < PS5_HP_MAX_STAGES; i++) { o->stages[i] = st[i]; o->results[i] = res[i]; }
        log_event(e->self, 0x13, r, (LONG)id);
        return complete(irp, STATUS_SUCCESS, sizeof(*o));
    }
    if (code == IOCTL_PS5PWR_VERSION) {   /* v1.12.1: the LOADED driver's version, also before the device started */
        if (s->Parameters.DeviceIoControl.OutputBufferLength < sizeof(PS5PWR_VERSION_OUT)) return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        PS5PWR_VERSION_OUT *o = irp->AssociatedIrp.SystemBuffer;
        o->size = sizeof(*o); o->major = PS5PWR_VER_MAJOR; o->minor = PS5PWR_VER_MINOR; o->patch = PS5PWR_VER_PATCH; o->caps = PS5PWR_CAPS;
        return complete(irp, STATUS_SUCCESS, sizeof(*o));
    }
    if (code == IOCTL_PS5PWR_GFXCLK) {
        if (!e || !e->started) return complete(irp, STATUS_DEVICE_NOT_READY, 0);
        if (s->Parameters.DeviceIoControl.InputBufferLength < sizeof(PS5PWR_GFXCLK_IN) ||
            s->Parameters.DeviceIoControl.OutputBufferLength < sizeof(PS5PWR_GFXCLK_OUT))
            return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        PS5PWR_GFXCLK_IN in = *(PS5PWR_GFXCLK_IN *)irp->AssociatedIrp.SystemBuffer;
        PS5PWR_GFXCLK_OUT2 *o = irp->AssociatedIrp.SystemBuffer;
        BOOLEAN big = s->Parameters.DeviceIoControl.OutputBufferLength >= sizeof(PS5PWR_GFXCLK_OUT2);
        RtlZeroMemory(o, big ? sizeof(PS5PWR_GFXCLK_OUT2) : sizeof(PS5PWR_GFXCLK_OUT));
        NTSTATUS st = gfxclk_call(&in, &o->b, big ? &o->query_mhz : NULL, big ? &o->freq_mhz : NULL);
        return complete(irp, st, NT_SUCCESS(st) ? (big ? sizeof(PS5PWR_GFXCLK_OUT2) : sizeof(PS5PWR_GFXCLK_OUT)) : 0);
    }
    if (code == IOCTL_PS5PWR_BAPM) {
        if (!e || !e->started) return complete(irp, STATUS_DEVICE_NOT_READY, 0);
        if (s->Parameters.DeviceIoControl.InputBufferLength < sizeof(PS5PWR_BAPM_IN) ||
            s->Parameters.DeviceIoControl.OutputBufferLength < sizeof(PS5PWR_BAPM_OUT))
            return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
        PS5PWR_BAPM_IN2 in = { 0, 100u };
        RtlCopyMemory(&in, irp->AssociatedIrp.SystemBuffer, s->Parameters.DeviceIoControl.InputBufferLength >= sizeof(in) ? sizeof(in) : sizeof(PS5PWR_BAPM_IN));
        PS5PWR_BAPM_OUT *o = irp->AssociatedIrp.SystemBuffer;
        RtlZeroMemory(o, sizeof(*o));
        NTSTATUS st = bapm_call(&in, o);
        return complete(irp, st, NT_SUCCESS(st) ? sizeof(*o) : 0);
    }
    return complete(irp, STATUS_INVALID_DEVICE_REQUEST, 0);
}

static NTSTATUS dispatch_pass(PDEVICE_OBJECT d, PIRP irp)
{
    if (d == g_ctl) return dispatch_ctl(d, irp);
    PEXT e = d->DeviceExtension;
    IoSkipCurrentIrpStackLocation(irp);
    return IoCallDriver(e->lower, irp);
}

static NTSTATUS add_device(PDRIVER_OBJECT drv, PDEVICE_OBJECT pdo)
{
    PDEVICE_OBJECT d;
    NTSTATUS st = IoCreateDevice(drv, sizeof(EXT), NULL, FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN, FALSE, &d);
    if (!NT_SUCCESS(st)) return st;
    PEXT e = d->DeviceExtension;
    RtlZeroMemory(e, sizeof(*e));
    ExInitializeFastMutex(&e->icc_lock);
    e->self = d; e->hp.log_ctx = e;                 /* v1.4 */
    e->hp.extra_note = btn_note; e->hp.extra_ctx = e;   /* v1.10 power button tap */
    KeInitializeCallbackRecord(&e->btn_bug);
    KeInitializeEvent(&e->hp_wake, SynchronizationEvent, FALSE);
    KeInitializeEvent(&e->hp_seq, SynchronizationEvent, TRUE);   /* v1.13: free */
    KeInitializeSpinLock(&e->log_lock);
    e->lower = IoAttachDeviceToDeviceStack(d, pdo);
    if (!e->lower) { IoDeleteDevice(d); return STATUS_NO_SUCH_DEVICE; }
    d->Flags |= DO_POWER_PAGABLE;                  /* S5 IRP at PASSIVE_LEVEL */
    d->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

static UNICODE_STRING g_link = RTL_CONSTANT_STRING(L"\\DosDevices\\Ps5Pwr");
static VOID unload(PDRIVER_OBJECT drv)
{
    UNREFERENCED_PARAMETER(drv);
    oc_safe_reset("unload");                           /* v1.14.0 */
    if (InterlockedExchange(&g_oc_bug_reg, 0)) KeDeregisterBugCheckCallback(&g_oc_bug);
    if (g_ctl) { IoDeleteSymbolicLink(&g_link); IoDeleteDevice(g_ctl); g_ctl = NULL; }
}

NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
    if (!g_reloc_anchor) return STATUS_UNSUCCESSFUL;
    ExInitializeFastMutex(&g_smn_lock);
    RtlInitEmptyUnicodeString(&g_params, g_params_buf, sizeof(g_params_buf));   /* v1.4 HotplugMode etc. */
    if (!reg || !NT_SUCCESS(RtlAppendUnicodeStringToString(&g_params, reg)) ||
        !NT_SUCCESS(RtlAppendUnicodeToString(&g_params, L"\\Parameters"))) g_params.Length = 0;
    KeQueryPerformanceCounter(&g_freq);
    for (int i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) drv->MajorFunction[i] = dispatch_pass;
    drv->MajorFunction[IRP_MJ_PNP] = dispatch_pnp;
    drv->MajorFunction[IRP_MJ_POWER] = dispatch_power;
    drv->DriverExtension->AddDevice = add_device;
    drv->DriverUnload = unload;
    {   /* v1.1 control device, admins + SYSTEM only */
        UNICODE_STRING name = RTL_CONSTANT_STRING(L"\\Device\\Ps5Pwr");
        static const GUID cls = { 0x5a4c2f31, 0x8d6b, 0x4a1b, { 0x9e, 0x31, 0x50, 0x53, 0x35, 0x50, 0x57, 0x52 } };
        if (NT_SUCCESS(IoCreateDeviceSecure(drv, 0, &name, FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN, FALSE,
                                            &SDDL_DEVOBJ_SYS_ALL_ADM_ALL, &cls, &g_ctl))) {
            if (!NT_SUCCESS(IoCreateSymbolicLink(&g_link, &name))) { IoDeleteDevice(g_ctl); g_ctl = NULL; }
            else g_ctl->Flags &= ~DO_DEVICE_INITIALIZING;
        }
    }
    return STATUS_SUCCESS;
}
