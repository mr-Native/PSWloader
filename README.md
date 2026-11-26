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
- HDMI output with HDMI audio
- All 8 CPU cores / 16 threads with clock control by Windows
- Install to an internal M.2 SSD or a USB drive
- Wired Ethernet, on-board Wi-Fi 6 (2.4 GHz / 5 GHz, WPA2), Bluetooth, USB keyboard and mouse
- Power button shutdown, smart automatic fan control, three power presets
- Firmware setup (English and German) with boot order, display and power settings and a system check
- Restart from Windows boots straight back into Windows, without GameOS
- PSWloader Dashboard in Windows: hardware overview, health check, support zip and updates ([docs/dashboard.md](docs/dashboard.md))

Not fully supported yet: PS5 Pro (Windows starts, but the GPU driver and some other drivers do not work yet).

**Own a jailbroken PS5 Pro and want to help bring PSWloader to it?** Send me a DM on X/Twitter: @mrNative_

Tested on a PS5 Fat with system software 13.60.

**Other system software versions:** PSWloader is built to run on the same versions as PS5 Linux (3.00 to 13.60 on
Fat and Slim): the firmware takes the PCI layout from the console instead of fixed values measured on 13.60. Only
13.60 is tested so far. If you use another version and run into problems, please report them in an issue, with the logs listed
in [Troubleshooting](docs/troubleshooting.md#reporting-a-problem).

## Requirements

- A PS5 (Fat / Slim / Pro) that can run payloads through a Payload Manager
- USB drive (16 GB or more) for the official Windows 11 ISO
- A **second** drive as the target: internal M.2 SSD or another USB drive.\
  Only one external drive? Write the ISO to
  it with Rufus in **Windows To Go** mode instead, extract `pswloader-v*-Install-Files.zip` onto it as in step 2, boot it
  with PSWloader and run `sources\$OEM$\$1\PSWSetup\PSWSetup.exe` once on the desktop (it restarts once and installs
  the PS5 drivers).
- HDMI display and USB keyboard

## Installation

Download the files from the [latest release](../../releases/latest).

1. Write the Windows 11 ISO to the USB drive (using Rufus or another flashing tool).
2. Extract `pswloader-v*-Install-Files.zip` to the root of the USB drive.
3. Copy `PSWloader.elf` into a folder of the Payload Manager on the console.
4. Plug in the Windows USB installer drive and your target drive, then start **PSWloader.elf**. When the orange light stays on, wake the console.
5. On the PSWloader screen press `DEL` and pick the Windows USB installer drive. On an NTFS drive the console restarts once by itself.
6. Install Windows as usual. If Setup asks about an upgrade, choose **No**. On the internal M.2 SSD, ignore the two tiny reserved partitions (~33 MB and ~17 MB) — they belong to PlayStation (GameOS) and must stay untouched; only delete any other existing partitions and install into the unallocated space.
7. Setup restarts come back through PSWloader by themselves and Setup continues automatically (you can unplug the USB installer drive after the first restart). If a restart ever ends in GameOS, start **PSWloader.elf** again.
8. Before the first sign-in Windows shows **"Just a moment..."** while all PS5 drivers and tools install automatically. This usually takes 5–15 minutes; do not switch the console off. On the first sign-in, confirm the restart prompt to activate all drivers.
9. Press `ESC` on the PSWloader screen to open the UEFI menu: under *Boot → Boot order*, move the installed Windows drive to the top if needed. Optionally choose a power preset and turn on **Fast boot** (shorter start screen) and **Fast restart** (a restart from Windows skips the start screen).

Windows runs in test-signing mode because the drivers are not signed by Microsoft. There is no Secure Boot and no TPM 2.0
either. Games whose anti-cheat refuses test mode, or requires Secure Boot or TPM 2.0, will not start.

PSWloader does not write to the console's flash or to GameOS data. On the internal SSD Windows only sees the area
behind GameOS, and writes into the GameOS area are blocked. Back up anything important before installing anyway.

## Troubleshooting

Fixes for common problems, known issues and the logs to attach to an issue are in **[docs/troubleshooting.md](docs/troubleshooting.md)**.

## Updating an existing installation

PSWloader checks for a new release when you sign in and offers to download and install it, so most updates need no
manual steps (see [docs/updating.md](docs/updating.md)). The PSWloader Dashboard can also look for updates. To update by hand:

1. Download and extract `pswloader-v*-Updater.zip`, then run `PSWUpdater-v*.exe` as Administrator. 
2. If prompted that `PSWloader.elf` also needs updating, replace `PSWloader.elf` in your Payload Manager.
3. Confirm the restart prompt when the update completes. Update logs are saved under `C:\PSWSetup\Update\`.

Later firmware updates come with the updater and need no new `PSWloader.elf`; replace the ELF only when a release says
so.

See the [Documentation](docs/README.md) for details on automatic update checks (`updater.json`) and hardware compatibility notes.

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
