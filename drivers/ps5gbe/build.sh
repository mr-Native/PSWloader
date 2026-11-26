#!/bin/sh
# v1.1.0 build (/fixed:no + reloc anchor, /merge:.retplne, /tsaware:no: MiObtainSectionForDriver rejects native images with DllCharacteristics TERMINAL_SERVER_AWARE -> 0xC0000018, code 39) ps5gbe.sys with clang + lld-link against the official WDK/SDK NuGet headers.
set -eu
W=${WDK:-${PSW_WORK:-/opt/pswloader-work}/wdk}
V=10.0.28000.0
KI=$W/microsoft.windows.wdk.x64/c/Include/$V
SI=$W/microsoft.windows.sdk.cpp/c/Include/$V
KL=$W/microsoft.windows.wdk.x64/c/Lib/$V/km/x64
OUT=${OUT:-$(dirname "$0")/out}
mkdir -p "$OUT"
clang --target=x86_64-pc-windows-msvc -c -O2 -Wall -Werror -Wno-unused-function -Wno-microsoft-anon-tag \
  -fms-extensions -fms-compatibility -fno-stack-protector -mno-red-zone -mcmodel=small -ffreestanding \
  -D_AMD64_ -D_WIN64 -DAMD64 -D_KERNEL_MODE=1 -DNDIS620_MINIPORT=1 -DNDIS_MINIPORT_DRIVER=1 -DNDIS_WDM=1 \
  -DPOOL_NX_OPTIN=0 -DNTDDI_VERSION=0x0A000000 -D_WIN32_WINNT=0x0A00 \
  -isystem "$KI/km" -isystem "$W/case-shim" -isystem "$KI/km/crt" -isystem "$KI/shared" -isystem "$SI/shared" \
  -o "$OUT/ps5gbe.obj" "$(dirname "$0")/ps5gbe.c"
lld-link /nologo /driver /fixed:no /dynamicbase /subsystem:native /entry:DriverEntry /nodefaultlib /machine:x64 \
  /release /opt:ref /integritycheck:no /dynamicbase /merge:.retplne=.rdata /tsaware:no /out:"$OUT/ps5gbe.sys" "$OUT/ps5gbe.obj" \
  "$KL/ndis.lib" "$KL/ntoskrnl.lib" "$KL/hal.lib"
# v1.1.0: embedded test signature (the DriverStore copy is replaced in place; testsigning is on)
C=${PSW_WORK:-/opt/pswloader-work}/certs
rm -f "$OUT/ps5gbe.signed.sys"
osslsigncode sign -certs $C/ps5-test.crt -key $C/ps5-test.key -h sha256 -n ps5gbe -in "$OUT/ps5gbe.sys" -out "$OUT/ps5gbe.signed.sys" > /dev/null && mv -f "$OUT/ps5gbe.signed.sys" "$OUT/ps5gbe.sys"
sha256sum "$OUT/ps5gbe.sys"
# v1.0.1: kernel images must be relocatable (no .reloc -> code 39 / STATUS_CONFLICTING_ADDRESSES)
python3 - "$OUT/ps5gbe.sys" <<'PY'
import struct, sys
b = open(sys.argv[1], 'rb').read(); pe = struct.unpack_from('<I', b, 0x3c)[0]
rva, size = struct.unpack_from('<II', b, pe + 24 + 112 + 5 * 8)
assert size > 0, 'no base relocation directory'
print('reloc directory', hex(rva), size)
PY
