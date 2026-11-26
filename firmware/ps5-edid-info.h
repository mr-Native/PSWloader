/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * ps5-edid-info v1.0 (edid6): small EDID summary shared by the PS5 firmware log (HdmiProbeDxe),
 * edidread.exe (Windows) and the host tests. No allocation, no libc. Also defines the hand-over to
 * Windows: volatile UEFI variables (BOOTSERVICE|RUNTIME access, never NV) under PS5_HDMI_EDID_VENDOR_GUID:
 *   "Ps5HdmiEdid"     = EDID bytes, block 0 repaired like Linux fix_edid() (byte 0 = 0x00, checksum)
 *   "Ps5HdmiEdidInfo" = struct ps5_hdmi_edid_info (64 bytes, little endian)
 */
#ifndef PS5_EDID_INFO_H
#define PS5_EDID_INFO_H
#ifdef PS5_EDK2
#include <Base.h>
typedef UINT8 uint8_t;
typedef UINT16 uint16_t;
typedef UINT32 uint32_t;
typedef UINT64 uint64_t;
#else
#include <stdint.h>
#endif
/* 5efc5ae0-b753-4f7a-a662-e34d15a6d8e5 */
#define PS5_HDMI_EDID_VENDOR_GUID_TEXT "{5efc5ae0-b753-4f7a-a662-e34d15a6d8e5}"
#define PS5_HDMI_EDID_VENDOR_GUID_INIT {0x5efc5ae0,0xb753,0x4f7a,{0xa6,0x62,0xe3,0x4d,0x15,0xa6,0xd8,0xe5}}
#define PS5_HDMI_EDID_INFO_SIGNATURE 0x49444550u   /* "PEDI" */
#define PS5_EDID_INFO_MAX_DTD 8
#define PS5_EDID_INFO_MAX_VIC 64
#pragma pack(push,1)
struct ps5_hdmi_edid_info {
    uint32_t signature;        /* PS5_HDMI_EDID_INFO_SIGNATURE */
    uint16_t version;          /* 1 */
    uint16_t header_size;      /* sizeof(struct ps5_hdmi_edid_info) = 64 */
    uint32_t edid_bytes;       /* length of "Ps5HdmiEdid" */
    uint32_t collector_flags;  /* icc-edid.c: 1 bad header, 2 bad checksum, 4 bad extension count, 8 PS5 byte-0 quirk */
    uint32_t updates;          /* EDID notifications received */
    uint8_t  raw_byte0;        /* as received; PS5: bit0 HDCP 2.2, bits3:1 trained FRL rate (ref linux.patch) */
    uint8_t  raw_checksum;     /* byte 127 as received */
    uint8_t  fixed_checksum;   /* byte 127 after the repair */
    uint8_t  fixed;            /* 1: block 0 repaired (byte 0 = 0, checksum recomputed) */
    uint64_t tsc_at_capture;   /* RDTSC when the EDID was published */
    uint64_t tsc_hz;           /* TSC frequency the firmware assumes */
    uint8_t  firmware_time[16];/* EFI_TIME from gRT->GetTime at capture (software clock, may be the built-in default) */
    uint32_t time_status;      /* low 32 bits of the GetTime EFI_STATUS */
    uint32_t crc32;            /* CRC32 (IEEE) of the "Ps5HdmiEdid" bytes */
};
#pragma pack(pop)
struct ps5_edid_dtd { uint32_t pclk_khz, refresh_mhz; uint16_t hact, vact, htot, vtot; uint8_t interlaced, block; };
struct ps5_edid_summary {
    char vendor[4]; char name[14];
    uint16_t product; uint32_t serial;
    uint8_t raw_byte0, version, revision, extensions, header_ok, block0_ok, cta;
    unsigned dtd_count; struct ps5_edid_dtd dtd[PS5_EDID_INFO_MAX_DTD];
    unsigned vic_count; uint8_t vic[PS5_EDID_INFO_MAX_VIC];
};
/* Linux drivers/ps5/hdmi.c fix_edid(): byte 0 = 0x00, block-0 checksum recomputed. Returns the new checksum. */
uint8_t ps5_edid_fix(uint8_t *edid);
/* 0 ok, -1 too short. Header check ignores byte 0 (PS5 quirk). */
int ps5_edid_summary(const uint8_t *edid, unsigned bytes, struct ps5_edid_summary *s);
/* CRC32 (IEEE 802.3, as gBS->CalculateCrc32). */
uint32_t ps5_edid_crc32(const uint8_t *data, unsigned bytes);
#endif
