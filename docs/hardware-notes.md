# Hardware Notes & Compatibility

Practical notes on PS5 hardware behavior under native Windows 11.

## PS5 revisions

- **PS5 Phat & PS5 Slim (CFI-1xxx / CFI-2xxx on 13.60):** Supported. HDMI output, GPU acceleration, Ethernet, Wi-Fi 6, USB, power/fan control, and internal/external storage all work.
- **Wi-Fi chipsets:** PS5 consoles ship with the NXP/Marvell IW620 Wi-Fi 6 module (`PCI\VEN_1B4B&DEV_2B56`), which is supported by `ps5wifi.sys` on both 2.4 GHz and 5 GHz. Newer hardware revisions with a different PCI vendor/device ID (for example MediaTek `14C3:...`) are not covered by `ps5wifi.sys` yet — if your adapter shows up as an unknown network controller in Device Manager, open an issue with its Hardware ID.

## Wi-Fi firmware handoff

PSWloader does not bundle any proprietary Sony or NXP firmware binaries. Instead, `PSWloader.elf` extracts the Wi-Fi firmware directly from your own console's GameOS memory at boot, passes it to UEFI, and caches `\PS5WIFIFW.BIN` on your EFI System Partition (and `C:\Windows\System32\drivers\ps5wifi_fw.bin`) so warm restarts from Windows keep working.

## Internal M.2 SSD vs. USB drive

- **Internal M.2 SSD:** Windows sees the usable space behind GameOS; writes to the GameOS region are blocked by `ps5m2flt.sys`. During Windows Setup, leave the two small reserved partitions (~33 MB and ~17 MB) untouched and install Windows into the remaining unallocated space.
- **USB SSD / NVMe enclosure:** Works as a full boot target. USB selective suspend and USB 3 link power management are turned off automatically during setup to prevent disconnects.

## Display & HDMI

- Display resolution is configured by the UEFI firmware at boot (via the `ESC` setup menu under *Display*). Changing resolution from inside Windows is not supported yet.
- If a monitor shows no signal on the first boot, press `ESC` during startup or check `\PS5DIAG.LOG` on the EFI partition for the detected EDID modes.

## Test-signing & anti-cheat

Because the PS5 drivers are self-signed rather than WHQL-signed by Microsoft, Windows runs with `testsigning` enabled. Kernel-level anti-cheat systems that reject test-signing mode will refuse to launch games.
