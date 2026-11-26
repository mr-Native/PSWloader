#!/bin/sh
# build.sh - Build and sign ps5btx.sys + ps5btx.cat (KMDF 1.31 bus driver: Salina xHCI 104D:9108 + IW620 Bluetooth as BthX transport)
set -eu
W=${WDK:-${PSW_WORK:-/opt/pswloader-work}/wdk}
V=10.0.28000.0
KI=$W/microsoft.windows.wdk.x64/c/Include/$V
SI=$W/microsoft.windows.sdk.cpp/c/Include/$V
KL=$W/microsoft.windows.wdk.x64/c/Lib/$V/km/x64
WI=$W/microsoft.windows.wdk.x64/c/Include/wdf/kmdf/1.31
WL=$W/microsoft.windows.wdk.x64/c/Lib/wdf/kmdf/x64/1.31
DIR=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-$DIR/out}
mkdir -p "$OUT/shim"
# the KMDF and pack headers are included with other letter case than the NuGet files have
ln -sf "$SI/shared/pshpack1.h" "$OUT/shim/PSHPACK1.H"; ln -sf "$SI/shared/poppack.h" "$OUT/shim/POPPACK.H"
for inc in $(grep -ho '#include "[A-Za-z]*\.h"' "$WI"/*.h | sed 's/#include "//;s/"//' | sort -u); do
  lc=$(echo "$inc" | tr A-Z a-z); [ -f "$WI/$inc" ] || { [ -f "$WI/$lc" ] && ln -sf "$WI/$lc" "$OUT/shim/$inc"; }; done

clang --target=x86_64-pc-windows-msvc -c -O2 -Wall -Werror -Wno-unused-function -Wno-microsoft-anon-tag \
  -fms-extensions -fms-compatibility -fno-stack-protector -mno-red-zone -mcmodel=small -ffreestanding \
  -D_AMD64_ -D_WIN64 -DAMD64 -D_KERNEL_MODE=1 -DPOOL_NX_OPTIN=0 -DNTDDI_VERSION=0x0A000000 -D_WIN32_WINNT=0x0A00 \
  -DKMDF_VERSION_MAJOR=1 -DKMDF_VERSION_MINOR=31 \
  -isystem "$KI/km" -isystem "$W/case-shim" -isystem "$OUT/shim" -isystem "$KI/km/crt" -isystem "$KI/shared" -isystem "$SI/shared" -isystem "$WI" \
  -o "$OUT/ps5btx.obj" "$DIR/ps5btx.c"

lld-link /nologo /driver /fixed:no /dynamicbase /subsystem:native /entry:FxDriverEntry /nodefaultlib /machine:x64 \
  /release /opt:ref /integritycheck:no /merge:.retplne=.rdata /tsaware:no \
  /out:"$OUT/ps5btx.sys" "$OUT/ps5btx.obj" \
  "$WL/wdfdriverentry.lib" "$WL/wdfldr.lib" "$KL/ntoskrnl.lib" "$KL/hal.lib" "$KL/bufferoverflowfastfailk.lib"

CERT=${SIGN_CERT:-}
KEY=${SIGN_KEY:-}
cp "$DIR/ps5btx.inf" "$OUT/ps5btx.inf"
rm -f "$OUT/ps5btx.signed.sys" "$OUT/ps5btx.cat" "$OUT/ps5btx.unsigned.cat"
if [ -n "$CERT" ] && [ -n "$KEY" ]; then
  osslsigncode sign -certs "$CERT" -key "$KEY" -h sha256 -n "PS5 Bluetooth (ps5btx)" \
    -in "$OUT/ps5btx.sys" -out "$OUT/ps5btx.signed.sys" >/dev/null
  mv -f "$OUT/ps5btx.signed.sys" "$OUT/ps5btx.sys"
  python3 "$DIR/../make-cat.py" --name ps5btx --pe "$OUT/ps5btx.sys" --flat "$OUT/ps5btx.inf" --out "$OUT/ps5btx.unsigned.cat"
  osslsigncode sign -certs "$CERT" -key "$KEY" -h sha256 -n "ps5btx" \
    -in "$OUT/ps5btx.unsigned.cat" -out "$OUT/ps5btx.cat" >/dev/null
  rm -f "$OUT/ps5btx.unsigned.cat"
fi
sha256sum "$OUT/ps5btx.sys"
