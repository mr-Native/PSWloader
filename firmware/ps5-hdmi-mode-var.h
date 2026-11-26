/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * PROPOSAL (UNTESTED, 14.11.2026): hand the boot display mode from the firmware to Windows.
 *
 * Volatile UEFI variable "Ps5HdmiMode", same vendor GUID as "Ps5HdmiEdid" (ps5-edid-info.h),
 * attributes BOOTSERVICE_ACCESS | RUNTIME_ACCESS (no NV), exactly 64 bytes, little endian.
 * Written by the BDS (ps5-native-dp-video.h, Ps5NativeDpVideo) right after the FLAVA3 mode was
 * handed to HdmiProbeDxe (Video->SetMode). Read by ps5pwr.sys (hotplug replay of the FLAVA3
 * stages for the CURRENT mode) with ExGetFirmwareEnvironmentVariable.
 *
 * bridge is exactly the struct ps5_flava3_mode given to Video->SetMode(); flags bit 0 clear means
 * SetMode(NULL), i.e. the VIC16 packets of ps5_flava3_video_stage() (1080p60, today's default).
 * The GUID is repeated here because ps5-edid-info.h and ps5-display-mode.h cannot be included
 * together (both define PS5_EDID_INFO_MAX_DTD).
 */
#ifndef PS5_HDMI_MODE_VAR_H
#define PS5_HDMI_MODE_VAR_H
#include "flava3-mode.h"

#define PS5_HDMI_MODE_VENDOR_GUID_INIT {0x5efc5ae0,0xb753,0x4f7a,{0xa6,0x62,0xe3,0x4d,0x15,0xa6,0xd8,0xe5}}
#define PS5_HDMI_MODE_VAR_NAME   L"Ps5HdmiMode"
#define PS5_HDMI_MODE_SIGNATURE  0x444d4850u   /* "PHMD" */
#define PS5_HDMI_MODE_VERSION    1u
#define PS5_HDMI_MODE_FLAG_BRIDGE 1u           /* bridge valid (SetMode(&bridge)); clear: SetMode(NULL) */

struct ps5_hdmi_mode_var {
    uint32_t signature;            /* PS5_HDMI_MODE_SIGNATURE                                       */
    uint16_t version;              /* PS5_HDMI_MODE_VERSION                                         */
    uint16_t size;                 /* sizeof(struct ps5_hdmi_mode_var) = 64                         */
    uint32_t mode_id;              /* enum ps5_mode_id chosen by the BDS (informational)            */
    uint32_t flags;                /* PS5_HDMI_MODE_FLAG_*                                          */
    uint16_t width, height, refresh_hz, reserved0;
    uint32_t clock_khz;
    struct ps5_flava3_mode bridge; /* 30 bytes, offset 28                                           */
    uint8_t reserved1[2];
    uint32_t crc32;                /* CRC-32 (IEEE 802.3, reflected, ~0 in/out) of bytes 0..59      */
};
typedef char ps5_hdmi_mode_var_is_64_bytes[(sizeof(struct ps5_hdmi_mode_var) == 64 &&
                                            sizeof(struct ps5_flava3_mode) == 30) ? 1 : -1];

static inline uint32_t ps5_hdmi_mode_crc32(const uint8_t *p, unsigned n)
{
    uint32_t c = 0xffffffffu;
    for (unsigned i = 0; i < n; i++) {
        c ^= p[i];
        for (unsigned k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
    }
    return ~c;
}
/* bridge NULL = SetMode(NULL) (VIC16). */
static inline void ps5_hdmi_mode_pack(struct ps5_hdmi_mode_var *v, uint32_t mode_id,
                                      const struct ps5_flava3_mode *bridge, uint16_t width,
                                      uint16_t height, uint16_t refresh_hz, uint32_t clock_khz)
{
    uint8_t *b = (uint8_t *)v;
    for (unsigned i = 0; i < sizeof(*v); i++) b[i] = 0;
    v->signature = PS5_HDMI_MODE_SIGNATURE; v->version = PS5_HDMI_MODE_VERSION; v->size = (uint16_t)sizeof(*v);
    v->mode_id = mode_id; v->flags = bridge ? PS5_HDMI_MODE_FLAG_BRIDGE : 0;
    v->width = width; v->height = height; v->refresh_hz = refresh_hz; v->clock_khz = clock_khz;
    if (bridge) for (unsigned i = 0; i < sizeof(*bridge); i++) ((uint8_t *)&v->bridge)[i] = ((const uint8_t *)bridge)[i];
    v->crc32 = ps5_hdmi_mode_crc32(b, 60);
}
/* 0 ok (*out filled), -1 size/signature/version, -2 CRC, -3 implausible bridge flags. */
static inline int ps5_hdmi_mode_check(const uint8_t *buf, unsigned bytes, struct ps5_hdmi_mode_var *out)
{
    struct ps5_hdmi_mode_var v;
    if (!buf || !out || bytes != sizeof(v)) return -1;
    for (unsigned i = 0; i < sizeof(v); i++) ((uint8_t *)&v)[i] = buf[i];
    if (v.signature != PS5_HDMI_MODE_SIGNATURE || v.version != PS5_HDMI_MODE_VERSION || v.size != sizeof(v)) return -1;
    if (v.crc32 != ps5_hdmi_mode_crc32(buf, 60)) return -2;
    if ((v.flags & ~PS5_HDMI_MODE_FLAG_BRIDGE) || v.bridge.high_tmds > 1 || v.bridge.vic97_mask > 1 ||
        v.bridge.uw_geometry > 1) return -3;
    for (unsigned i = 0; i < sizeof(v); i++) ((uint8_t *)out)[i] = ((const uint8_t *)&v)[i];   /* no struct copy: EDK2 has no memcpy */
    return 0;
}
#endif
