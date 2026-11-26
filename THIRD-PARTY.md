# Third-party code and licenses

Our own code is BSD-2-Clause-Patent (see [LICENSE](LICENSE)) unless a file says otherwise. This page lists everything that comes from somewhere else or carries a different license.

## In this repo

| Path | Origin | License |
|---|---|---|
| `firmware/atom-host/atom-host.c` and headers | Port of the ATOM BIOS interpreter from the Linux `amdgpu` driver (`atom.c`, Copyright 2008 Advanced Micro Devices) | MIT, notice kept in the files |
| `firmware/atom-host/kmd-*.h` | Glue for using the interpreter inside a Windows driver | GPL-2.0-only, as marked |
| `firmware/dcn201-*.c` | Register sequences following the Linux `amdgpu` display code (DCN 2.0.x, link encoder, command tables; that code is MIT-licensed). Each file names its sources in the header | BSD-2-Clause-Patent |
| `firmware/flava3-*.c`, `firmware/icc-*.c`, `firmware/hdmi-commands.c` | Command and register facts taken from the PS5 Linux `hdmi.c` and ICC code | BSD-2-Clause-Patent |
| `drivers/ps5gbe/` | Windows NDIS driver for Sony's Ethernet MAC, following the PS5 Linux `mts` driver | GPL-2.0-only, as marked |
| `drivers/ps5pwr/` | Windows driver for the ICC mailbox (power, LED), following PS5 Linux `spcie.c` and `ps5.c` | GPL-2.0-only, as marked |
| `firmware/acpi/ps5-dsdt.asl` | Our own DSDT with the measured PCI windows, compiled into the stage 1 ACPI tables. The console's MADT and MCFG are read from your console when you build; they are not shipped | BSD-2-Clause-Patent |
| `firmware/edk2/` | Changes to EDK2 | BSD-2-Clause-Patent, like EDK2 |
| `firmware/lvgl-fonts/` | LVGL bitmap fonts generated with lv_font_conv (LVGL project, MIT) from Montserrat (Copyright The Montserrat Project Authors) and Font Awesome Free (Fonticons, Inc.) | SIL Open Font License 1.1 (fonts), MIT (LVGL font format) |
| `pswloader/` | PSWloader, the GameOS payload that starts the firmware: a patch against [ps5-linux-loader](https://github.com/ps5-linux/ps5-linux-loader) commit badfe48. The released `PSWloader.elf` is built from that commit plus this patch | GPL-3.0 |
| `gpu/` | Port of [amdgpu-wddm](https://github.com/D-Ogi/amdgpu-wddm) (kernel driver, D3D11 front-end, package); license, notices and third-party texts in `gpu/LICENSE.md`, `gpu/NOTICE`, `gpu/licenses/` | PolyForm Noncommercial 1.0.0 (decided 17 November 2026); Copyright (c) 2026 D-Ogi and PSWloader contributors |

## Not in this repo, fetched or provided by you

- **EDK2** (TianoCore), BSD-2-Clause-Patent. Build against the revision named in `docs/building.md`.
- **LVGL** (MIT) and **LvglPkg** (MIT, hamitcan99/LvglPkg, used as a fork) for the firmware setup screens. Fetched at build time, not in this repo.
- **PS5 Linux loader** (ps5-linux-loader, GPL-3.0). Its source is not in this repo; `pswloader/` holds our patch against it (see above), and you fetch the upstream source at the pinned commit to rebuild the payload. The released `pswloader.elf` is a build of upstream plus that patch.
- **The jailbreak.** Not included and not linked.
- **GPU microcode** (`cyan_skillfish2_*.bin`). Comes from `linux-firmware` under its own license. Not in this repo; the installer payload ships it with `licenses/LICENSE.amdgpu`.
- **Windows 11**. You need your own ISO and license. No Microsoft files are in this repo.
- **WDK and Windows SDK** from Microsoft's NuGet packages, for building the drivers.
- **amdgpu-wddm** upstream sources and its tester release 0.7.216.109-tester.37. The installer ships its user-mode drivers (Mesa/RADV, DXVK, vkd3d-proton, dxil-spirv, LLVM parts) unchanged; their licenses are in `gpu/licenses/`.

## Required notices for the GPU port

These lines come from the amdgpu-wddm `NOTICE` file and must travel with any copy of `gpu/`:

> Required Notice: Copyright (c) 2026 D-Ogi. Licensed under the PolyForm Noncommercial License 1.0.0 (https://polyformproject.org/licenses/noncommercial/1.0.0).
>
> Required Notice: Unofficial community project, not affiliated with or endorsed by AMD, ASRock or Microsoft. Genuine releases are published only by D-Ogi, as source code with signed checksums (see SECURITY.md). Tester pre-releases are binary zips published by D-Ogi on the GitHub releases page of D-Ogi/amdgpu-wddm, with their SHA256 in the release notes and in manifest.json. A binary obtained anywhere else does not come from this project.
