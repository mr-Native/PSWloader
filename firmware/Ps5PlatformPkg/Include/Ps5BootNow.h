/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * r5.4 (16.11.2026): "boot Windows now" from inside the setup browser.
 * Installed by the BDS (Ps5BootWindows.c) before UiApp starts; BootWindows() runs the normal post-menu path
 * (UTC seed, time contract, console/post-WIM taps, bootx64.efi on the already selected ESP) and returns only
 * if that boot failed. Must be called at TPL_APPLICATION (browser callback / display-engine input poll).
 * Reason: EFI_BROWSER_ACTION_REQUEST_EXIT from a front-page sub-formset only returns to the front page
 * (SetupBrowser FindNextMenu, FormSetLevel) - UiApp never returns to the BDS. */
#ifndef PS5_BOOT_NOW_H_
#define PS5_BOOT_NOW_H_
#define PS5_BOOT_NOW_PROTOCOL_GUID {0x9c1e5f2a,0x1360,0x4e1b,{0xb0,0x5e,0x13,0x60,0x0e,0xf1,0x00,0x06}}
typedef struct {
  UINT32 Revision;                          /* 1; r5.21: 2 = SaveSettings */
  EFI_STATUS (EFIAPI *BootWindows)(VOID);
  EFI_STATUS (EFIAPI *SaveSettings)(VOID);  /* r5.21 (Revision >= 2): write \PS5UEFI.CFG now (before a restart from the menu) */
} PS5_BOOT_NOW_PROTOCOL;
/* r5.21: bootable disks found when the setup / boot menu was entered, in display order (volatile variable
 * "Ps5BootDevs", PS5_POWER_PRESET_GUID). Ps5BootListSel (PS5_BOOT_LIST, Ps5PowerPreset.h) holds 1-based indices into
 * it in the order the user arranged. */
#define PS5_BOOTDEV_MAX  16
#define PS5_BOOTDEV_NAME 48
typedef struct {
  UINT32 Count;
  UINT32 Id[PS5_BOOTDEV_MAX];
  UINT8  Usb[PS5_BOOTDEV_MAX];
  CHAR16 Name[PS5_BOOTDEV_MAX][PS5_BOOTDEV_NAME];
} PS5_BOOT_DEVS;
#endif
