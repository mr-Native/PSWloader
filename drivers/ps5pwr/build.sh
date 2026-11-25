#!/bin/sh
# v1.6 build ps5pwr.sys (+ icc-power.c) with clang + lld-link against the official WDK/SDK NuGet headers;
# /tsaware:no, /fixed:no (relocatable), .retplne merged, embedded test signature (see ps5gbe/build.sh).
# v1.4: + hotplug core and the shared FLAVA3/HDMI builders; ICC transport with PS5_ICC_LINUX_NOTIFY (EMC
# notifications carry magic 0x24). v1.5: fan curset control and read-only SMN Tctl; no startup writes.
# v1.6: longer fan ICC timeout, larger bounded reply and precise transport status mapping.
# Reproducible: PS5PWR_EPOCH=<unix time> pins the PE timestamp and the
# signing time (v1.2 deployed 676b3369... = PS5PWR_EPOCH=1794020714 with the v1.2 sources).
set -eu
W=${WDK:-${PSW_WORK:-/opt/pswloader-work}/wdk}
V=10.0.28000.0
KI=$W/microsoft.windows.wdk.x64/c/Include/$V
SI=$W/microsoft.windows.sdk.cpp/c/Include/$V
KL=$W/microsoft.windows.wdk.x64/c/Lib/$V/km/x64
D=$(dirname "$0"); OUT=${OUT:-$D/out}
mkdir -p "$OUT"
EP=${PS5PWR_EPOCH:-}
OBJS=""
for f in "$D/ps5pwr.c" "$D/ps5pwr-sensors.c" "$D/smu-mbox.c" "$D/../../firmware/icc-power.c" "$D/ps5pwr-hotplug.c" "$D/../../firmware/icc-hdmi.c" "$D/../../firmware/hdmi-commands.c" \
         "$D/../../firmware/flava3-prepare.c" "$D/../../firmware/flava3-video.c" "$D/../../firmware/ps5-display-mode.c" "$D/../../firmware/button-keys.c" "$D/ps5pwr-fancurve.c"; do
OBJS="$OBJS $OUT/$(basename "$f" .c).obj"
clang --target=x86_64-pc-windows-msvc -c -O2 -Wall -Werror -Wno-unused-function -Wno-microsoft-anon-tag \
  -fms-extensions -fms-compatibility -fno-stack-protector -mno-red-zone -mcmodel=small -ffreestanding -fno-builtin \
  -D_AMD64_ -D_WIN64 -DAMD64 -D_KERNEL_MODE=1 -DPOOL_NX_OPTIN=0 -DNTDDI_VERSION=0x0A000000 -D_WIN32_WINNT=0x0A00 \
  -DPS5_ICC_LINUX_NOTIFY=1 \
  -isystem "$KI/km" -isystem "$W/case-shim" -isystem "$KI/km/crt" -isystem "$KI/shared" -isystem "$SI/shared" \
  -o "$OUT/$(basename "$f" .c).obj" "$f"
done
lld-link /nologo /driver /tsaware:no /fixed:no /dynamicbase /subsystem:native /entry:DriverEntry /nodefaultlib /machine:x64 \
  /merge:.retplne=.rdata /release /opt:ref /integritycheck:no ${EP:+/timestamp:$EP} /out:"$OUT/ps5pwr.sys" $OBJS \
  "$KL/ntoskrnl.lib" "$KL/hal.lib" "$KL/wdmsec.lib" "$KL/bufferoverflowfastfailk.lib"
C=${PSW_WORK:-/opt/pswloader-work}/certs
rm -f "$OUT/ps5pwr.signed.sys"
cp -f "$OUT/ps5pwr.sys" "$OUT/ps5pwr.unsigned.sys"   # 17.11.2026: third parties reproduce this one (the test key is not shipped)
osslsigncode sign ${EP:+-time $EP} -certs $C/ps5-test.crt -key $C/ps5-test.key -h sha256 -n ps5pwr -in "$OUT/ps5pwr.sys" -out "$OUT/ps5pwr.signed.sys" > /dev/null && mv -f "$OUT/ps5pwr.signed.sys" "$OUT/ps5pwr.sys"
python3 "$D/../tools/check-pe-sections.py" "$OUT/ps5pwr.sys"
sha256sum "$OUT/ps5pwr.sys" "$OUT/ps5pwr.unsigned.sys"
