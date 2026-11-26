# Hardware Notes & Compatibility

Practical notes on PS5 hardware behavior under native Windows 11.

## PS5 revisions

- **PS5 Fat & PS5 Slim (CFI-1xxx / CFI-2xxx on 13.60):** Supported. HDMI output, GPU acceleration, Ethernet, Wi-Fi 6, USB, power/fan control, and internal/external storage all work.
- **System software versions:** tested on 13.60. Like PS5 Linux, the firmware takes the PCI layout from the console, so the same versions (3.00 to 13.60) should work, but they are unconfirmed. Reports from other versions (with logs) are welcome in the issues.
- **Wi-Fi chipsets:** PS5 consoles ship with the NXP/Marvell IW620 Wi-Fi 6 module (`PCI\VEN_1B4B&DEV_2B56`), which is supported by `ps5wifi.sys` on both 2.4 GHz and 5 GHz. Newer hardware revisions with a different PCI vendor/device ID (for example MediaTek `14C3:...`) are not covered by `ps5wifi.sys` yet — if your adapter shows up as an unknown network controller in Device Manager, open an issue with its Hardware ID.

## Wi-Fi firmware handoff

PSWloader does not bundle any proprietary Sony or NXP firmware binaries. Instead, `PSWloader.elf` extracts the Wi-Fi firmware directly from your own console's GameOS memory at boot, passes it to UEFI, and caches `\PS5WIFIFW.BIN` on your EFI System Partition (and `C:\Windows\System32\drivers\ps5wifi_fw.bin`) so warm restarts from Windows keep working.

## Internal M.2 SSD vs. USB drive

- **Internal M.2 SSD:** Windows sees the usable space behind GameOS; writes to the GameOS region are blocked by `ps5m2flt.sys`. During Windows Setup, leave the two small reserved partitions (~33 MB and ~17 MB) untouched and install Windows into the remaining unallocated space.
- **USB SSD / NVMe enclosure:** Works as a full boot target. USB selective suspend and USB 3 link power management are turned off automatically during setup to prevent disconnects.

## Display & HDMI

- HDMI audio works (stereo): Windows shows a *Digital Output* audio device. Tested on a PS5 with the FLAVA3 HDMI bridge; the Slim's VERDE bridge is untested.
- Display resolution: choose it in Windows under Settings → System → Display, which lists the modes of the connected monitor or TV. Plug in another monitor or TV while Windows runs and Windows switches to its resolution by itself. The `ESC` setup menu of the firmware under *Display* sets the mode the console starts with.
- After the TV was off or the HDMI cable was unplugged, the picture comes back in the mode that was running.
- Video memory: Windows gets 3 GB by default. A file `vram.txt` in `/data/pswloader/` or `PSW/` on the USB drive changes it (hex bytes, 512 MB to 4 GB in 16 MB steps, e.g. `80000000` = 2 GB).
- If a monitor shows no signal on the first boot, press `ESC` during startup or check `\PS5DIAG.LOG` on the EFI partition for the detected EDID modes.

## Test-signing & anti-cheat

Because the PS5 drivers are self-signed rather than WHQL-signed by Microsoft, Windows runs with `testsigning` enabled. Kernel-level anti-cheat systems that reject test-signing mode will refuse to launch games.

Secure Boot and TPM 2.0 are not available:

- **Secure Boot** would need every boot driver signed by Microsoft (or keys the user enrolls) - test-signed drivers and Secure Boot exclude each other, so it stays off.
- **TPM 2.0:** the PS5 has no TPM that Windows can use, and the firmware does not emulate one. Windows 11 is installed with the TPM check skipped.

Games that require Secure Boot or TPM 2.0 (for example some titles with Vanguard, FACEIT or similar anti-cheat, or Steam games that show "Secure Boot & TPM 2.0 Not Detected") do not run.
