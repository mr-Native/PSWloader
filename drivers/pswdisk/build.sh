#!/bin/sh
# pswdisk v1.1 build (17.11.2026) - clang + lld-link against the WDK/SDK NuGet headers (pattern of g21/ps5cpuclk),
# signed with the PSWloader Driver Signing certificate (SIGN_CRT/SIGN_KEY, key outside the repo). Reproducible: fixed PE timestamp and
# signing time (PSWDISK_EPOCH, default 2026-11-17 00:00 UTC).
set -eu
W=${WDK:-${PSW_WORK:-/opt/pswloader-work}/wdk}
V=10.0.28000.0
KI=$W/microsoft.windows.wdk.x64/c/Include/$V
SI=$W/microsoft.windows.sdk.cpp/c/Include/$V
KL=$W/microsoft.windows.wdk.x64/c/Lib/$V/km/x64
D=$(dirname "$0"); OUT=${OUT:-$D/out}
CRT=${SIGN_CRT:-${PSW_WORK:-/opt/pswloader-work}/signing-private/pswloader-signing.crt}
KEY=${SIGN_KEY:?set SIGN_KEY to the private key of the signing certificate (never in the repo)}
EP=${PSWDISK_EPOCH:-1794873600}
mkdir -p "$OUT"
clang --target=x86_64-pc-windows-msvc -c -O2 -Wall -Werror -Wno-unused-function -Wno-deprecated-declarations -Wno-microsoft-anon-tag \
  -fms-extensions -fms-compatibility -fno-stack-protector -mno-red-zone -mcmodel=small -ffreestanding -fno-builtin \
  -D_AMD64_ -D_WIN64 -DAMD64 -D_KERNEL_MODE=1 -DPOOL_NX_OPTIN=0 -DNTDDI_VERSION=0x0A000000 -D_WIN32_WINNT=0x0A00 \
  -isystem "$KI/km" -isystem "$W/case-shim" -isystem "$KI/km/crt" -isystem "$KI/shared" -isystem "$SI/shared" \
  -o "$OUT/pswdisk.obj" "$D/pswdisk.c"
lld-link /nologo /driver /tsaware:no /fixed:no /dynamicbase /subsystem:native /entry:DriverEntry /nodefaultlib /machine:x64 \
  /merge:.retplne=.rdata /release /opt:ref /integritycheck:no /timestamp:$EP /out:"$OUT/pswdisk.unsigned.sys" "$OUT/pswdisk.obj" \
  "$KL/ntoskrnl.lib" "$KL/hal.lib" "$KL/bufferoverflowfastfailk.lib"
rm -f "$OUT/pswdisk.sys"
osslsigncode sign -time $EP -certs "$CRT" -key "$KEY" -h sha256 -n pswdisk -in "$OUT/pswdisk.unsigned.sys" -out "$OUT/pswdisk.sys" > /dev/null
PE=$D/../tools/check-pe-sections.py; [ ! -f "$PE" ] || python3 "$PE" "$OUT/pswdisk.sys"
osslsigncode verify -CAfile "$CRT" -in "$OUT/pswdisk.sys" | grep -q Succeeded && echo "signature ok"
sha256sum "$OUT/pswdisk.sys" "$OUT/pswdisk.unsigned.sys"
