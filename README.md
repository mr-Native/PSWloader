<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/img/pswloader-banner-dark.svg">
  <img alt="PSWloader - Windows 11 on the PS5" src="docs/img/pswloader-banner.svg">
</picture>

# PSWloader

Native Windows 11 on the PlayStation 5.

PSWloader is a payload that boots a custom UEFI firmware on the PS5. The firmware starts the normal Windows Boot
Manager, and Windows runs directly on the console's hardware, no virtual machine.
The Windows kernel is used as Microsoft ships it and PatchGuard stays on. The PS5-specific parts are the firmware, the
ACPI tables and a set of Windows drivers.

## Features

- Latest stock Windows 11 from the official Microsoft ISO
- GPU driver (*AMD Oberon*): DirectX 11, DirectX 12, Vulkan and OpenGL 4.6
- HDMI output
- All 8 CPU cores / 16 threads with clock control by Windows
- Install to an internal M.2 SSD or a USB drive
- Wired Ethernet, on-board Wi-Fi 6 (2.4 GHz / 5 GHz), USB keyboard and mouse
- Power button shutdown, fan control by CPU temperature, three power presets
- Firmware setup (English and German) with boot order, display and power settings and a system check
- Restart from Windows goes straight back to Windows, without GameOS

Not supported (yet): Bluetooth, audio, changing the resolution from Windows.

Tested on system software 13.60.

## Requirements

- A PS5 that can run payloads through a Payload Manager
- USB drive (16 GB or more) for the official Windows 11 ISO
- Internal M.2 SSD or USB drive as the target drive
- HDMI display and USB keyboard

## Installation

Download the files from the [latest release](../../releases/latest).

1. Write the Windows 11 ISO to the USB drive (using Rufus or another flashing tool).
2. Extract `pswloader-v*-USB-Stick.zip` to the root of the USB drive.
3. Copy `PSWloader.elf` and `PSWloader.elf.json` into a folder of the Payload Manager on the console.
4. Plug in the Windows USB installer drive and your target drive, then start **PSWloader.elf**. When the orange light stays on, wake the console.
5. On the PSWloader screen press `DEL` and pick the Windows USB installer drive. *(FAT32 works on the first boot; if your flashing tool formatted the drive as NTFS, Setup enables test-signing in the drive's BCD on the first start — restart once into PSWloader if disks are not listed yet.)*
6. Install Windows as usual. If Setup asks about an upgrade, choose **No**. On the internal M.2 SSD, ignore the two tiny reserved partitions (~33 MB and ~17 MB) — they belong to PlayStation (GameOS) and must stay untouched; only delete any other existing partitions and install into the unallocated space.
7. Each Setup restart returns to GameOS; start **PSWloader.elf** again and Setup continues automatically (you can unplug the USB installer drive after the first restart).
8. Before the first sign-in, all PS5 drivers and tools are installed automatically. On the first Desktop sign-in, confirm the restart prompt to activate all drivers.
9. Press `ESC` on the PSWloader screen to open the UEFI menu: under *Boot → Boot order*, move the installed Windows drive to the top if needed, and optionally configure power presets and enable **Fast Boot** for faster startups.

Windows runs in test-signing mode because the drivers are not signed by Microsoft. Games whose anti-cheat refuses test
mode will not start.

PSWloader does not write to the console's flash or to GameOS data. On the internal SSD Windows only sees the area
behind GameOS, and writes into the GameOS area are blocked. Back up anything important before installing anyway.

If something goes wrong, open an issue and attach `\PS5DIAG.LOG` (UEFI Bootmanager → Diagnostics → *Run system check*)
and the newest `C:\PSWSetup\logs\setup_psw*.log`.

## Updating an existing installation

1. Download and extract `pswloader-v*-Updater.zip`, then run `PSWUpdater-v*.exe` as Administrator.
2. If prompted that `PSWloader.elf` also needs updating, replace `PSWloader.elf` and `PSWloader.elf.json` in your Payload Manager after the reboot.
3. Confirm the restart prompt when the update completes. Update logs are saved under `C:\PSWSetup\Update\`.

See the [Documentation](docs/README.md) for details on automatic update checks (`updater.json`), hardware compatibility notes, and troubleshooting.

## Support

If you enjoy this project and want to support my work, I'd be really happy if you bought me a coffee: https://buymeacoffee.com/mrnative

If you're a content creator showcasing PSWloader, it would be great if you could link this GitHub repository as well as my [Buy Me a Coffee](https://buymeacoffee.com/mrnative) page and give it a quick mention.

## Building from source

- **Firmware**: EDK2 at `ec82c96152507d64055a8965fe261fb9aaca3279` with `firmware/edk2/*.diff` applied, then
  `EDK2=/path/to/edk2 firmware/build-vN.sh <version>`. Stage 1: `firmware/build-bootstrap.py`.
- **Payload**: [ps5-linux-loader](https://github.com/ps5-linux/ps5-linux-loader) at `badfe48` plus
  `pswloader/pswloader-v1.0-from-upstream-badfe48.patch`, built with the ps5-payload-sdk.
- **Drivers**: `drivers/<name>/build.sh` (clang and lld-link against the WDK NuGet packages).
- **Installer**: `installer/launcher/build.sh` and `installer/tools/make-payload.py`.

## Credits

- [PS5 Linux](https://github.com/ps5-linux) and [ps5-linux-loader](https://github.com/ps5-linux/ps5-linux-loader)
- [amdgpu-wddm](https://github.com/D-Ogi/amdgpu-wddm) by D-Ogi, the base of the GPU driver
- [ps5-hackintosh](https://github.com/rodri0x/ps5-hackintosh)
- [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk)
- [EDK2](https://github.com/tianocore/edk2), [LVGL](https://github.com/lvgl/lvgl) and
  [LvglPkg](https://github.com/hamitcan99/LvglPkg)
- The Linux `amdgpu` driver, Mesa, DXVK and vkd3d-proton

### How it was made

This project was developed with Claude doing the heavy lifting. Claude had full control of a hardware lab around the
console: it could cut the PS5's power through a smart plug and read its power draw live, press the console's power
button with a Fingerbot, watch the HDMI output through a capture card, start payloads in GameOS remotely and log into
Windows over SSH. With no serial port on the PS5, early firmware stages reported their progress as patterns in the
power draw. Every hardware detail in this repository was measured on the console this way.

## License

BSD-2-Clause-Patent, see [LICENSE](LICENSE). Exceptions are marked in the files: `drivers/ps5gbe`, `drivers/ps5pwr`, `drivers/ps5wifi`
and `firmware/atom-host/kmd-*.h` are GPL-2.0-only, the payload patch is GPL-3.0, and the GPU driver keeps the PolyForm
Noncommercial license of amdgpu-wddm. Third-party components and their licenses are listed in
[THIRD-PARTY.md](THIRD-PARTY.md).

Windows is a trademark of Microsoft. PlayStation and PS5 are trademarks of Sony Interactive Entertainment. This project
is not affiliated with either.
