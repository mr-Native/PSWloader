#!/bin/sh
# ps5cmos v1.2 build (clang + lld-link, WDK/SDK NuGet headers, test signature) - pattern of drivers/ps5pwr/build.sh
set -eu
W=${WDK:-${PSW_WORK:-/opt/pswloader-work}/wdk}
V=10.0.28000.0
KI=$W/microsoft.windows.wdk.x64/c/Include/$V
SI=$W/microsoft.windows.sdk.cpp/c/Include/$V
KL=$W/microsoft.windows.wdk.x64/c/Lib/$V/km/x64
D=$(dirname "$0"); OUT=${OUT:-$D/out}
mkdir -p "$OUT"
EP=${PS5CMOS_EPOCH:-}
clang --target=x86_64-pc-windows-msvc -c -O2 -Wall -Werror -Wno-unused-function -Wno-deprecated-declarations -Wno-microsoft-anon-tag \
  -fms-extensions -fms-compatibility -fno-stack-protector -mno-red-zone -mcmodel=small -ffreestanding -fno-builtin \
  -D_AMD64_ -D_WIN64 -DAMD64 -D_KERNEL_MODE=1 -DPOOL_NX_OPTIN=0 -DNTDDI_VERSION=0x0A000000 -D_WIN32_WINNT=0x0A00 \
  -isystem "$KI/km" -isystem "$W/case-shim" -isystem "$KI/km/crt" -isystem "$KI/shared" -isystem "$SI/shared" \
  -o "$OUT/ps5cmos.obj" "$D/ps5cmos.c"
lld-link /nologo /driver /tsaware:no /fixed:no /dynamicbase /subsystem:native /entry:DriverEntry /nodefaultlib /machine:x64 \
  /merge:.retplne=.rdata /release /opt:ref /integritycheck:no ${EP:+/timestamp:$EP} /out:"$OUT/ps5cmos.sys" "$OUT/ps5cmos.obj" \
  "$KL/ntoskrnl.lib" "$KL/hal.lib" "$KL/bufferoverflowfastfailk.lib"
C=${PSW_WORK:-/opt/pswloader-work}/certs
rm -f "$OUT/ps5cmos.signed.sys"
osslsigncode sign ${EP:+-time $EP} -certs $C/ps5-test.crt -key $C/ps5-test.key -h sha256 -n ps5cmos -in "$OUT/ps5cmos.sys" -out "$OUT/ps5cmos.signed.sys" > /dev/null && mv -f "$OUT/ps5cmos.signed.sys" "$OUT/ps5cmos.sys"
sha256sum "$OUT/ps5cmos.sys"
