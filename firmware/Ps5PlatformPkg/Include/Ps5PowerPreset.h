/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * r5.6 (16.11.2026, user request): power preset efficiency / standard / oc (shown as "Power" since r5.25.3, was "OC-Mode").
 * Volatile UEFI variable "Ps5PowerPreset" (BOOTSERVICE|RUNTIME, never NV: the payload variable store is RAM-only)
 * under the Ps5HdmiEdid vendor GUID, value PS5_POWER_PRESET (UINT32). Persistence: text file \PS5POWER.CFG on the
 * boot ESP ("preset=standard"), read by the BDS before the menu and rewritten before bootmgr when changed.
 * Windows (ThermalManager) reads the variable or the file. Header is also included by the VFR: defines and
 * typedefs only. */
#ifndef PS5_POWER_PRESET_H_
#define PS5_POWER_PRESET_H_
#define PS5_POWER_PRESET_GUID {0x5efc5ae0,0xb753,0x4f7a,{0xa6,0x62,0xe3,0x4d,0x15,0xa6,0xd8,0xe5}}
#define PS5_POWER_EFFICIENCY 0
#define PS5_POWER_STANDARD   1
#define PS5_POWER_OC         2   /* shown as "Power" (r5.25.3, user 18.11.; was "OC-Mode") - setting value stays "oc" */
#define PS5_POWER_KEY        0x1364
typedef struct { UINT32 Preset; } PS5_POWER_PRESET;
#define PS5_LANG_KEY         0x1366   /* r5.7: Ps5Lang 0 = en-US, 1 = de-DE (volatile, mirrors PlatformLang) */
typedef struct { UINT32 Lang; } PS5_LANG;
#define PS5_FAST_KEY         0x1367   /* r5.9: Ps5FastBoot 0 = countdown, 1 = fast boot (volatile, persisted in \PS5UEFI.CFG) */
typedef struct { UINT32 Value; } PS5_FASTBOOT;
#define PS5_PSTATE_KEY       0x1368   /* r5.12: Ps5PStates 0 = off, 1 = ACPI CPU P-state SSDT (volatile, persisted in \PS5UEFI.CFG) */
typedef struct { UINT32 Value; } PS5_PSTATES;
#define PS5_BOOTORDER_KEY    0x1369   /* r5.15: Ps5BootOrder 0 = internal first, 1 = USB first (volatile, persisted in \PS5UEFI.CFG) */
typedef struct { UINT32 Value; } PS5_BOOTORDER;
#define PS5_REFRESH_KEY      0x136A   /* r5.16: Ps5Refresh 0 = native mode, 1 = highest refresh (volatile, persisted in \PS5UEFI.CFG) */
typedef struct { UINT32 Value; } PS5_REFRESH;
#define PS5_BOOTLIST_KEY     0x136C   /* r5.21: Ps5BootListSel - ordered list of the bootable disks (1-based, 0 = end) */
typedef struct { UINT8 Order[16]; } PS5_BOOT_LIST;
#define PS5_USBCYCLE_KEY     0x136D   /* r5.25.1: Ps5UsbCycle 1 = power-cycle the USB ports once at start (default), 0 = off (volatile, persisted in \PS5UEFI.CFG) */
typedef struct { UINT32 Value; } PS5_USBCYCLE;
#define PS5_FASTRESTART_KEY  0x136E   /* r5.25.12: Ps5FastRestart 0 = a Windows restart shows the start screen (default), 1 = skips it (volatile, persisted in \PS5UEFI.CFG) */
typedef struct { UINT32 Value; } PS5_FASTRESTART;
#define PS5_RESOLUTION_KEY   0x136F   /* r5.27: Ps5Resolution 0 = automatic, 1..8 = resolution (ps5_set_res order) (volatile, persisted in \PS5UEFI.CFG) */
typedef struct { UINT32 Value; } PS5_RESOLUTION;
#endif
