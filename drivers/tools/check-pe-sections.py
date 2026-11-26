#!/usr/bin/env python3
"""v1.1 Reject kernel images the NT loader refuses: sections without characteristics (lld keeps the
WDK '.retplne' RetpolineV1 marker section, characteristics 0 -> code 39 / 0xC000026C) or no .reloc."""
import struct
import sys

bad = 0
for path in sys.argv[1:]:
    b = open(path, 'rb').read(); pe = struct.unpack_from('<I', b, 0x3c)[0]
    nsec, optsz = struct.unpack_from('<H', b, pe + 6)[0], struct.unpack_from('<H', b, pe + 20)[0]
    so = pe + 24 + optsz
    for i in range(nsec):
        name = b[so + 40 * i:so + 40 * i + 8].rstrip(b'\0').decode()
        ch = struct.unpack_from('<I', b, so + 40 * i + 36)[0]
        if ch == 0 or name == '.retplne':
            print(f'{path}: section {name} characteristics {ch:#x} - rejected'); bad = 1
    # v1.1: DllCharacteristics bit 15 (TERMINAL_SERVER_AWARE) - MiObtainSectionForDriver compares the
    # field signed (jl) and returns STATUS_CONFLICTING_ADDRESSES for native images; lld-link sets it by default
    dllch = struct.unpack_from('<H', b, pe + 24 + 70)[0]
    if dllch & 0x8000:
        print(f'{path}: DllCharacteristics {dllch:#x} has TERMINAL_SERVER_AWARE - link with /tsaware:no'); bad = 1
    if struct.unpack_from('<II', b, pe + 24 + 112 + 5 * 8)[1] == 0:
        print(f'{path}: no base relocation directory'); bad = 1
sys.exit(bad)
