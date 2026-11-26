#!/bin/sh
# make-release-stick.sh v2.2 (18 November 2026; v2.2: PSW\psw-wim.cmd (corner text out of boot.wim); v2.1: psw-specialize.cmd, PSW\drivers\installdrivers) - builds the folder whose CONTENT a user copies onto a stock Windows 11
# setup stick, and the release zip pswloader-v1.0-Install-Files.zip.
#
#   make-release-stick.sh DRIVERS EXTRA [OUT]
#     DRIVERS  signed driver set (ps5gpu/ ps5pwr/ ps5gbe/ ps5cmos/ ps5m2flt/ ps5nvmsi/ pswdisk/ pswloader-signing.cer),
#              catalogs made and signed on the host (drivers/make-cat.py); only the public certificate is in it
#     EXTRA    files that are not built from this repository: licenses/LICENSE.amdgpu, vcredist/VC_redist.x64.exe,
#              vcredist/VC_redist.x86.exe, scripts/fan-target-startup-fast.exe, scripts/ps5-powerbutton.exe
#     OUT      output folder (default installer/dist/release-stick)
#
# Layout (what the setup stick gets next to the Windows files):
#   autounattend.xml                       Windows PE pass only: Setup requirement bypasses, then PSW\psw-drivers.cmd
#   PSW\psw-drivers.cmd, psw-install.cmd   storage drivers + pswdisk in Windows Setup / optional install without Setup
#   PSW\pswsvc.exe                         service control for Windows Setup's Windows PE (it has no sc.exe)
#   PSW\drivers\{ps5nvmsi,ps5m2flt,pswdisk}, PSW\drivers\pswloader-signing.cer
#   PSW\drivers\installdrivers\{ps5gbe,ps5nvmsi,ps5m2flt}  drivers needed before the desktop (network page "Install driver")
#   sources\$OEM$\$$\Setup\Scripts\SetupComplete.cmd   runs PSWSetup.exe /quiet before the first sign-in
#   sources\$OEM$\$$\Setup\Scripts\psw-specialize.cmd  certificate, network driver, BypassNRO (specialize pass)
#   sources\$OEM$\$1\PSWSetup\             PSWSetup.exe, engine\PSWSetup.ps1, payload\ (+ manifest.json with SHA-256)
# Refuses private keys, test answer files and lab files.
set -eu
here=$(cd "$(dirname "$0")" && pwd); inst=$(dirname "$here"); repo=$(dirname "$inst")
D=${1:?drivers dir}; X=${2:?extra dir}; out=${3:-$inst/dist/release-stick}
need() { [ -f "$1" ] || { echo "MISSING $1" >&2; exit 1; }; }
rm -rf "$out"; mkdir -p "$out/PSW/drivers" "$out/sources/\$OEM\$/\$\$/Setup/Scripts"
S="$out/sources/\$OEM\$/\$1/PSWSetup"; P="$S/payload"; mkdir -p "$S/engine" "$P"

# stick root and PSW\ (Windows Setup side)
cp "$here/autounattend.xml" "$out/"
cp "$here/PSW/psw-drivers.cmd" "$here/PSW/psw-install.cmd" "$here/PSW/psw-wim.cmd" "$out/PSW/"
(cd "$inst/pswsvc" && sh build.sh >/dev/null); cp "$inst/pswsvc/pswsvc.exe" "$out/PSW/"
for d in ps5nvmsi ps5m2flt; do mkdir -p "$out/PSW/drivers/$d"; for e in inf sys cat; do need "$D/$d/$d.$e"; cp "$D/$d/$d.$e" "$out/PSW/drivers/$d/"; done; done
need "$D/pswdisk/pswdisk.sys"; mkdir -p "$out/PSW/drivers/pswdisk"; cp "$D/pswdisk/pswdisk.sys" "$out/PSW/drivers/pswdisk/"
need "$D/pswloader-signing.cer"; cp "$D/pswloader-signing.cer" "$out/PSW/drivers/"
cp "$here/sources/\$OEM\$/\$\$/Setup/Scripts/SetupComplete.cmd" "$here/sources/\$OEM\$/\$\$/Setup/Scripts/psw-specialize.cmd" "$here/sources/\$OEM\$/\$\$/Setup/Scripts/psw-specialize.ps1" "$here/sources/\$OEM\$/\$\$/Setup/Scripts/psw-setupcomplete.ps1" "$here/sources/\$OEM\$/\$\$/Setup/Scripts/psw-firstlogin-status.ps1" "$out/sources/\$OEM\$/\$\$/Setup/Scripts/"   # v1.1: 20-minute limit + support zip, first sign-in notice
# v2.1: PSW\drivers\installdrivers - the drivers the new Windows needs before the desktop (network, internal M.2), for the
# "Install driver" button of the network page; psw-specialize.cmd installs the network driver by itself
for d in ps5gbe ps5nvmsi ps5m2flt; do mkdir -p "$out/PSW/drivers/installdrivers/$d"; for e in inf sys cat; do need "$D/$d/$d.$e"; cp "$D/$d/$d.$e" "$out/PSW/drivers/installdrivers/$d/"; done; done
cp "$D/pswloader-signing.cer" "$out/PSW/drivers/installdrivers/"

# PSWSetup (runs in the new Windows)
(cd "$inst/launcher" && sh build.sh >/dev/null); cp "$inst/launcher/PSWSetup.exe" "$S/"
cp "$inst/engine/PSWSetup.ps1" "$S/engine/"
T=$(mktemp); trap 'rm -f "$T"' EXIT
row() { need "$3"; printf '%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$(sha256sum "$3" | cut -d' ' -f1)" >> "$T"; }
(cd "$D/ps5gpu" && find . -type f | sed 's#^\./##' | sort) | while read -r f; do row Gpu "drivers/bc250kmd/$f" "$D/ps5gpu/$f"; done
row Gpu licenses/LICENSE.amdgpu "$X/licenses/LICENSE.amdgpu"
for e in inf sys cat; do row Power "drivers/ps5pwr/ps5pwr.$e" "$D/ps5pwr/ps5pwr.$e"; done
row Power tools/ps5pwr/ps5pwr.ps1 "$repo/drivers/ps5pwr/ps5pwr.ps1"
for e in inf sys cat; do row Network "drivers/ps5gbe/ps5gbe.$e" "$D/ps5gbe/ps5gbe.$e"; done
for d in ps5m2flt ps5nvmsi; do for e in inf sys cat; do row Storage "drivers/$d/$d.$e" "$D/$d/$d.$e"; done; done
row Cmos drivers/ps5cmos/ps5cmos.sys "$D/ps5cmos/ps5cmos.sys"
for e in inf sys cat; do row Bluetooth "drivers/ps5btx/ps5btx.$e" "$D/ps5btx/ps5btx.$e"; done
row Thermal scripts/ps5-thermal-manager.ps1 "$repo/thermal/ps5-thermal-manager.ps1"
row Thermal scripts/thermal-guard-v17.ps1 "$repo/thermal/thermal-guard-v17.ps1"
row Thermal scripts/thermal-supervisor.ps1 "$repo/thermal/thermal-supervisor.ps1"
row Thermal scripts/fan-target-startup-fast.exe "$X/scripts/fan-target-startup-fast.exe"
row Names scripts/set-hw-names.ps1 "$repo/tools/set-hw-names.ps1"
row Tasks scripts/ps5-postboot.ps1 "$inst/engine/ps5-postboot.ps1"
row PowerButton scripts/ps5-powerbutton.exe "$X/scripts/ps5-powerbutton.exe"
row Watermark scripts/psw-hide-testmode.ps1 "$repo/tools/psw-hide-testmode.ps1"
row BootMenu scripts/psw-uefi-bootmanager.ps1 "$repo/tools/psw-uefi-bootmanager.ps1"
row Display scripts/psw-display.ps1 "$repo/tools/psw-display.ps1"
row Signing certs/pswloader-signing.cer "$D/pswloader-signing.cer"
row VCRedist vcredist/VC_redist.x64.exe "$X/vcredist/VC_redist.x64.exe"
row VCRedist vcredist/VC_redist.x86.exe "$X/vcredist/VC_redist.x86.exe"
python3 "$inst/tools/make-payload.py" "$T" "$P"

if find "$out" \( -iname '*unattend*.xml' ! -path "$out/autounattend.xml" -o -name '*.pfx' -o -name '*.key' -o -name '*.p12' -o -name 'pfx.pass' \
     -o -name 'authorized_keys' -o -name 'static-ip.txt' -o -name 'psw-verify-target.cmd' -o -name '*.log' -o -name '*.flag' \) | grep -q .; then
  echo 'REFUSED: secret, test or lab file in the release stick' >&2; exit 1; fi
grep -q 'DiskConfiguration\|InstallTo\|ProductKey' "$out/autounattend.xml" && { echo 'REFUSED: autounattend.xml partitions disks' >&2; exit 1; }
du -sh "$out"
z="$(dirname "$out")/pswloader-v1.0-Install-Files.zip"; rm -f "$z"
(cd "$out" && TZ=UTC find . -exec touch -h -d '2026-11-18 00:00:00' {} + && find . -type f | sort | zip -X -q -@ "$z")
sha256sum "$z"
