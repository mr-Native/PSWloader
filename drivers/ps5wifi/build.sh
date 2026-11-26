#!/bin/sh
# build.sh - Build and sign ps5wifi.sys + ps5wifi.cat (NDIS 6.20 Native 802.11 miniport for IW620 1B4B:2B56)
set -eu
W=${WDK:-${PSW_WORK:-/opt/pswloader-work}/wdk}
V=10.0.28000.0
KI=$W/microsoft.windows.wdk.x64/c/Include/$V
SI=$W/microsoft.windows.sdk.cpp/c/Include/$V
KL=$W/microsoft.windows.wdk.x64/c/Lib/$V/km/x64
DIR=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-$DIR/out}
mkdir -p "$OUT"

clang --target=x86_64-pc-windows-msvc -c -O2 -Wall -Werror -Wno-unused-function -Wno-microsoft-anon-tag \
  -fms-extensions -fms-compatibility -fno-stack-protector -mno-red-zone -mcmodel=small -ffreestanding \
  -D_AMD64_ -D_WIN64 -DAMD64 -D_KERNEL_MODE=1 -DNDIS620_MINIPORT=1 -DNDIS_MINIPORT_DRIVER=1 -DNDIS_WDM=1 \
  -DPOOL_NX_OPTIN=0 -DNTDDI_VERSION=0x0A000000 -D_WIN32_WINNT=0x0A00 \
  -isystem "$KI/km" -isystem "$W/case-shim" -isystem "$KI/km/crt" -isystem "$KI/shared" -isystem "$SI/shared" \
  -o "$OUT/ps5wifi.obj" "$DIR/ps5wifi.c"

lld-link /nologo /driver /fixed:no /dynamicbase /subsystem:native /entry:DriverEntry /nodefaultlib /machine:x64 \
  /release /opt:ref /integritycheck:no /merge:.retplne=.rdata /tsaware:no \
  /out:"$OUT/ps5wifi.sys" "$OUT/ps5wifi.obj" \
  "$KL/ndis.lib" "$KL/ntoskrnl.lib" "$KL/hal.lib"

CERT=${SIGN_CERT:-}
KEY=${SIGN_KEY:-}
cp "$DIR/ps5wifi.inf" "$OUT/ps5wifi.inf"
rm -f "$OUT/ps5wifi.signed.sys" "$OUT/ps5wifi.cat" "$OUT/ps5wifi.unsigned.cat"
if [ -n "$CERT" ] && [ -n "$KEY" ]; then
  osslsigncode sign -certs "$CERT" -key "$KEY" -h sha256 -n "PS5 Wi-Fi 6 Adapter (ps5wifi)" \
    -in "$OUT/ps5wifi.sys" -out "$OUT/ps5wifi.signed.sys" >/dev/null
  mv -f "$OUT/ps5wifi.signed.sys" "$OUT/ps5wifi.sys"
  python3 "$DIR/../make-cat.py" --name ps5wifi --pe "$OUT/ps5wifi.sys" --flat "$OUT/ps5wifi.inf" --out "$OUT/ps5wifi.unsigned.cat"
  osslsigncode sign -certs "$CERT" -key "$KEY" -h sha256 -n "ps5wifi" \
    -in "$OUT/ps5wifi.unsigned.cat" -out "$OUT/ps5wifi.cat" >/dev/null
  rm -f "$OUT/ps5wifi.unsigned.cat"
fi

sha256sum "$OUT/ps5wifi.sys" "$OUT/ps5wifi.cat"
python3 - "$OUT/ps5wifi.sys" <<'PY'
import struct, sys
b = open(sys.argv[1], 'rb').read(); pe = struct.unpack_from('<I', b, 0x3c)[0]
rva, size = struct.unpack_from('<II', b, pe + 24 + 112 + 5 * 8)
assert size > 0, 'no base relocation directory'
print('reloc directory', hex(rva), size)
PY
