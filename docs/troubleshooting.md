# Troubleshooting & Logs

## Reporting a problem

Please open an issue and attach the logs that fit your case:

- **Windows reaches the desktop:** open the **PSWloader Dashboard**. *Health* shows what is wrong and *Troubleshoot* can
  fix common problems. For an issue, create a support zip under *Tools* and attach it. See [selfcheck.md](selfcheck.md).
- **Windows does not start:** run the firmware's system check. Press `ESC` on the PSWloader Boot Manager screen, open
  *Diagnostics → Run system check*, and attach the file it writes, `\PSWloader\logs\PS5DIAG.LOG`. It goes to a FAT32 USB
  drive if one is plugged in, otherwise to the EFI partition of the Windows drive.
- **The console goes back to GameOS before the PSWloader screen:** attach `pswloader-elf.log` from `/data/pswloader/` on
  the console (or from `PSW\` on the USB installer drive).
- **Windows Setup shows no disks:** attach the folder `PSW\logs` from the USB installer drive (one log per Setup start).

**System software other than 13.60:** PSWloader is built to run on the same versions as PS5 Linux (3.00 to 13.60,
Fat and Slim), but only 13.60 is tested. If you use another version, please report in an issue whether it works, with
the logs above, even when everything works.

## Known issues

### Windows Setup shows "Install driver to show hardware" (no keyboard and mouse)
Fixed in v1.2. With v1.1: if Windows Setup asks for a driver and keyboard and mouse do not work, do one **cold restart**:
switch the console off, start `PSWloader.elf` again from GameOS and pick the USB installer drive with `DEL` on the
PSWloader screen.

This only happens when a Windows installation already existed (the firmware found it) and Windows Setup was started
through *Boot once from ...* in the Boot Manager, usually after the restart an NTFS USB installer drive asks for.

## Common issues

### No disks listed in Windows Setup
The PS5 storage drivers in Windows Setup need test signing in the boot entry of the USB installer drive. On a FAT32
drive PSWloader sets it by itself. On an NTFS drive (Rufus) PSWloader sets it in the first start and restarts the console once
by itself; after that the disks are listed. A drive written as a read-only image (for example Etcher with the
ISO as is) cannot be changed: write it again with Rufus (FAT32 or NTFS) and extract `pswloader-v*-Install-Files.zip`
onto it. Windows cannot be installed onto the drive that holds the installer. You need a second, empty drive (or the Windows To Go
path in the README). Connect it before you start PSWloader; a drive added while Setup runs needs a cold start through
GameOS. If the disks are still missing, see *Reporting a problem* above.

### Windows stays on "Just a moment..." for a long time
Before the first sign-in PSWloader installs the PS5 drivers. This usually takes 5–10 minutes. After 20 minutes it is
stopped, and Windows continues to the sign-in. If the installation fails or is stopped, a support zip is written to
`C:\PSWloader\selfcheck\results` and a notice at the first sign-in shows where it is. Attach that zip to an issue.
Logs: `C:\PSWSetup\logs` and `C:\Windows\Setup\Scripts\psw-setupcomplete.log`.

### Windows Update broke a driver
PSWloader keeps Windows Update from replacing PS5 drivers. If a big Windows update resets something, open the PSWloader Dashboard, run a health check and use *Troubleshoot*, or run the updater again.
