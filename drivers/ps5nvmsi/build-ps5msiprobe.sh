#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
W=${PSW_WORK:-/opt/pswloader-work}/wdk
V=10.0.28000.0
KI="$W/microsoft.windows.wdk.x64/c/Include/$V"
SI="$W/microsoft.windows.sdk.cpp/c/Include/$V"
KL="$W/microsoft.windows.wdk.x64/c/Lib/$V/km/x64"
CERTS=${PSW_WORK:-/opt/pswloader-work}/certs
OUT=${PSW_WORK:-/opt/pswloader-work}/nvme-msi-out
mkdir -p "$OUT"

clang --driver-mode=cl --target=x86_64-pc-windows-msvc \
  /nologo /c /kernel /GS- /W4 /WX /O2 /Zp8 /GF /Gy /Brepro \
  -Wno-microsoft-anon-tag -Wno-unused-command-line-argument -Wno-ignored-pragma-intrinsic \
  /D_AMD64_ /DAMD64 /D_WIN64 /DWINNT=1 /DNTDDI_VERSION=0x0A00000C /D_WIN32_WINNT=0x0A00 /DNDEBUG \
  /imsvc "$KI/km" /imsvc "$W/case-shim" /imsvc "$KI/km/crt" /imsvc "$KI/shared" /imsvc "$SI/shared" /imsvc "$SI/um" \
  /Fo"$OUT/ps5msiprobe.obj" "$HERE/ps5msiprobe.c"

clang --driver-mode=cl --target=x86_64-pc-windows-msvc \
  /nologo /c /kernel /GS- /W3 /O2 /Zp8 /GF /Gy /Brepro \
  -Wno-unused-command-line-argument \
  /D_AMD64_ /DAMD64 /D_WIN64 /DWINNT=1 \
  /imsvc "$KI/km" /imsvc "$W/case-shim" /imsvc "$KI/km/crt" /imsvc "$KI/shared" /imsvc "$SI/shared" /imsvc "$SI/um" \
  /Fo"$OUT/clang-intrin-shim.obj" /Tc"${PSW_WORK:-/opt/pswloader-work}/nagi-port/ps5/clang-intrin-shim.c"   # /Tc: an /opt/... path would parse as /o

rm -f "$OUT/ps5msiprobe.unsigned.sys" "$OUT/ps5msiprobe.sys"
lld-link /nologo /driver:wdm /subsystem:native,10.00 /entry:DriverEntry /nodefaultlib /release /opt:ref /opt:icf \
  /machine:x64 /tsaware:no /integritycheck:no /Brepro /merge:.retplne=.rdata \
  /libpath:"$KL" ntoskrnl.lib hal.lib bufferoverflowfastfailk.lib libcntpr.lib \
  /out:"$OUT/ps5msiprobe.unsigned.sys" /map:"$OUT/ps5msiprobe.map" \
  "$OUT/ps5msiprobe.obj" "$OUT/clang-intrin-shim.obj"

osslsigncode sign \
  -certs "$CERTS/ps5-test.crt" \
  -key "$CERTS/ps5-test.key" \
  -h sha256 \
  -in "$OUT/ps5msiprobe.unsigned.sys" \
  -out "$OUT/ps5msiprobe.sys" >/dev/null

sha256sum "$HERE/ps5msiprobe.c" "$OUT/ps5msiprobe.unsigned.sys" "$OUT/ps5msiprobe.sys"
ls -la "$OUT/ps5msiprobe.sys"
