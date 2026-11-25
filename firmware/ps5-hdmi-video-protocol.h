/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PS5_HDMI_VIDEO_PROTOCOL_H
#define PS5_HDMI_VIDEO_PROTOCOL_H
/* Boot-services-only, BSP synchronous service. Stages 0..7 are fixed FLAVA3
 * packets; 8..10 repeat the three preparation stages; 14 = 3440x1440 geometry (draft). No arbitrary register
 * access is exposed. Successful command completion is not a GOP guarantee. */
STATIC EFI_GUID mPs5HdmiVideoGuid={0xf6c38128,0x8513,0x47d1,{0xbd,0x04,0x81,0x3d,0x45,0xdc,0x62,0x1a}};
typedef EFI_STATUS (EFIAPI *PS5_HDMI_VIDEO_STAGE)(UINTN Stage);
typedef EFI_STATUS (EFIAPI *PS5_HDMI_VIDEO_READ)(UINT16 Register,UINTN Count,UINT8 *Out);
/* UNTESTED (display-modes draft): EDID as last received over ICC (raw, PS5 byte-0 quirk not
 * repaired, see icc-edid.c). *Bytes in: capacity, out: length; EFI_NOT_FOUND if none yet. */
typedef EFI_STATUS (EFIAPI *PS5_HDMI_VIDEO_GET_EDID)(UINT8 *Buffer,UINTN *Bytes,UINT32 *Flags,UINT32 *Updates);
/* Select the FLAVA3 parameters for stages 2, 5 and 14 (struct ps5_flava3_mode, flava3-mode.h).
 * NULL = fixed VIC16 packets (today's behaviour). Copied; boot services only. */
typedef EFI_STATUS (EFIAPI *PS5_HDMI_VIDEO_SET_MODE)(CONST VOID *Flava3Mode,UINTN Size);
/* r5.14 (16.11.2026, user: heat/fan): MainSoC fan target over ICC service 0x0a (ps5-fan.h), as ps5pwr.sys
 * SetFanTargetTemp. TargetC 0 = read only; 50..100 = set curset word 0, verify by GET, restore the old curset on any
 * mismatch. Out values (any may be NULL): target before/after in C, fan duty before/after in 0..1024. */
typedef EFI_STATUS (EFIAPI *PS5_HDMI_VIDEO_FAN)(UINT32 TargetC,UINT32 *BeforeC,UINT32 *AfterC,UINT32 *DutyBefore,UINT32 *DutyAfter);
/* r5.20: the ICC part of the read-only BIOS "System check", nothing else. Fan duty (0x0a type 1), fan curset GET (0x0a type 7,
 * never SET), MainSoC temperature (0x0b type 1, zone 0) and the FLAVA3 profile registers 0x7227 (1 byte) and 0x7056 (2 bytes)
 * through the 128-byte register read. Same protection as Fan (mVideoBusy, TPL_HIGH, interrupts off, 12 s ICC deadline). r_* per
 * read: 0 ok, <0 ICC/transport error, -6 bad reply, 1 not attempted. EFI_ALREADY_STARTED while another video ICC transaction runs. */
typedef struct { INT32 r_duty, r_fan, r_temp, r_7227, r_7056; UINT32 fan_duty, fan_target_c; INT32 temp_milli_c;
                 UINT8 bridge_7227, bridge_7056[2], reserved; } PS5_HDMI_DIAG_ICC;
typedef EFI_STATUS (EFIAPI *PS5_HDMI_VIDEO_DIAG_ICC)(PS5_HDMI_DIAG_ICC *Out);
/* r5.26.2 (user 24.11.: the fan still spun up at every Windows start): the EMC fan table as PS5 Linux fan.c leaves it when
 * the host stops driving the fan, from power-on instead of only once ps5pwr runs. Snapshot of the servo table (0x0a type
 * 0x0f, 0x6ae bytes) into *Original, the quiet backup table derived from it (ps5_fc_prepare_backup) uploaded (type 0x0e) and
 * the MainSoC zone set to EMC auto (type 2) - the EMC keeps regulating by itself, nothing depends on a later driver.
 * *Flags: 1 snapshot taken (Original valid), 2 table was already the quiet one (nothing uploaded, Original not valid),
 * 4 backup uploaded, 8 auto set. *DutyBefore and *DutyAfter 0..1024 (0xFFFF unread), *TempMilli MainSoC (0x0b) or -1. */
typedef EFI_STATUS (EFIAPI *PS5_HDMI_VIDEO_FAN_QUIET)(UINT8 *Original,UINTN OriginalSize,UINT32 *Flags,UINT32 *DutyBefore,UINT32 *DutyAfter,INT32 *TempMilli);
typedef struct { PS5_HDMI_VIDEO_STAGE Stage; PS5_HDMI_VIDEO_READ Read;
                 PS5_HDMI_VIDEO_GET_EDID GetEdid; PS5_HDMI_VIDEO_SET_MODE SetMode;
                 PS5_HDMI_VIDEO_FAN Fan;
                 PS5_HDMI_VIDEO_DIAG_ICC DiagIcc;   /* r5.20: appended at the end */
                 PS5_HDMI_VIDEO_FAN_QUIET FanQuiet; } PS5_HDMI_VIDEO_PROTOCOL;   /* r5.26.2: appended at the end */
#endif
