# Troubleshooting & Logs

## Log locations

If something fails during installation, boot, or updating, grab the relevant log before opening an issue:

- **UEFI diagnostics:** `\PS5DIAG.LOG` on the EFI System Partition (generated via *UEFI Menu → Diagnostics → Run system check*).
- **Initial setup logs:** `C:\PSWSetup\logs\setup_psw*.log` and `C:\Windows\Setup\Scripts\psw-specialize.log`.
- **Updater logs:** `C:\PSWSetup\Update\PSWUpdate-*.log` and `C:\PSWSetup\Update\PSWUpdateCheck.log`.

## Common issues

### No disks listed in Windows Setup (NTFS USB stick)
If your flashing tool formatted the Windows installer USB stick as NTFS instead of FAT32, the first boot enables test-signing in the stick's BCD. Restart once back into `PSWloader.elf` and boot the USB stick again — your target drives will appear.

### Wi-Fi adapter installed, but no networks shown
1. Make sure you booted at least once with the matching `PSWloader.elf` (v1.0.3 or newer) so `\PS5WIFIFW.BIN` is extracted from your console.
2. Check Device Manager → *Network adapters* → **PS5 Wi-Fi 6 Adapter (ps5wifi)** to confirm there is no yellow warning icon.
3. If your console has an unknown network controller instead, check its Hardware ID in Device Manager (`Details → Hardware Ids`).

### Windows Setup asks for a network or Microsoft account
Starting with v1.0.3, both Ethernet and Wi-Fi drivers are installed before the Windows 11 OOBE screen, and `BypassNRO` is enabled automatically so you can also select **I don't have internet** for a local account.

### Windows Update broke a driver after an OS update
PSWloader sets `ExcludeWUDriversInQualityUpdate = 1` to keep Windows Update from overwriting PS5 drivers. If a major Windows update resets something, simply re-run the latest `PSWUpdater-vX.Y.Z.exe` to repair all drivers and BCD flags.
