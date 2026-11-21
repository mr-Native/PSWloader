#!/usr/bin/env python3
"""v1.0 Flatten/relocate a pinned-build X64 Universal Payload for a PS5 shim.

Output is a data container, NOT a bootable Linux kernel or a Windows proof.
Memory placement must still be checked against the actual loader E820 map.
The host does ELF parsing so the privileged shim can use a small bounded format.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib

BASE = 0x41000000
MAX_IMAGE = 32 * 1024**2
HEADER = 4096
MAGIC = b'PS5UPL01'
EHDR = struct.Struct('<16sHHIQQQIHHHHHH')
SHDR = struct.Struct('<IIQQQQIIQQ')
PHDR = struct.Struct('<IIQQQQQQ')


def bounded(data, offset, size):
    if offset < 0 or size < 0 or offset > len(data) or size > len(data) - offset:
        raise ValueError('ELF range outside file')
    return data[offset:offset + size]


def align(value, boundary=4096):
    return (value + boundary - 1) & -boundary


def pack(data):
    h = EHDR.unpack(bounded(data, 0, EHDR.size))
    ident, kind, machine, version, entry, phoff, shoff, flags, ehsize, phsize, phnum, shsize, shnum, stridx = h
    if (ident[:7] != b'\x7fELF\x02\x01\x01' or kind != 3 or machine != 62 or version != 1
            or ehsize != 64 or phsize != 56 or shsize != 64
            or not 0 < phnum < 32 or not 0 < shnum < 256 or not 0 < stridx < shnum):
        raise ValueError('Expected bounded X64 little-endian ET_DYN payload')
    raw_sections = [SHDR.unpack(bounded(data, shoff + i * shsize, shsize)) for i in range(shnum)]
    strtab = raw_sections[stridx]
    names = bounded(data, strtab[4], strtab[5])
    sections = {}
    for s in raw_sections[1:]:
        if s[0] >= len(names) or b'\0' not in names[s[0]:]:
            raise ValueError('Invalid section name')
        name = names[s[0]:].split(b'\0', 1)[0].decode('ascii')
        if name in sections:
            # EDK II can retain repeated non-loadable GNU debug links.
            if s[2] & 2 or name.startswith('.upld') or name in ('.rela', '.dynamic'):
                raise ValueError('Duplicate load-relevant section')
            continue
        sections[name] = s
        if s[1] != 8:
            bounded(data, s[4], s[5])
    segments = [PHDR.unpack(bounded(data, phoff + i * phsize, phsize)) for i in range(phnum)]
    loads = [p for p in segments if p[0] == 1]
    if not loads or any(p[3] != p[4] or p[6] < p[5] or p[3] + p[6] > MAX_IMAGE for p in loads):
        raise ValueError('Invalid load segment')
    for p in loads:
        bounded(data, p[2], p[5])
    allocated = [(name, s) for name, s in sections.items() if s[2] & 2 and s[5]]
    end = max(p[3] + p[6] for p in loads)
    if not any(s[2] & 4 and s[3] <= entry < s[3] + s[5] for _, s in allocated):
        raise ValueError('Entry outside executable section')
    image = bytearray(align(end))
    ranges = []
    mismatches = []
    for name, s in allocated:
        _, stype, attributes, address, offset, size, *_ = s
        if stype not in (1, 6, 8) or address + size > end:
            raise ValueError('Unsupported allocated section')
        containing = [p for p in loads if p[3] <= address and address + size <= p[3] + p[6]]
        if len(containing) != 1 or any(address < b and a < address + size for a, b in ranges):
            raise ValueError('Overlapping or uncovered allocated section')
        ranges.append((address, address + size))
        if stype != 8:
            image[address:address + size] = bounded(data, offset, size)
            p = containing[0]
            if offset - p[2] != address - p[3]:
                mismatches.append(name)
    # EDK II's ELF/FV repacker compacts section file offsets while preserving
    # virtual addresses. Copy each SHF_ALLOC section to its own sh_addr and
    # zero gaps (including omitted BSS), rather than copying packed debug data
    # into those gaps with the now-inconsistent PT_LOAD file extent.
    dynamic = sections.get('.dynamic')
    reloc = sections.get('.rela')
    if not dynamic or not reloc or dynamic[1] != 6 or dynamic[5] % 16:
        raise ValueError('Missing dynamic relocation metadata')
    tags = dict(struct.iter_unpack('<qQ', bounded(data, dynamic[4], dynamic[5])))
    if (reloc[1] != 4 or reloc[9] != 24 or reloc[5] % 24 or tags.get(7) != reloc[3]
            or tags.get(8) != reloc[5] or tags.get(9) != 24
            or tags.get(0x6ffffff9) != reloc[5] // 24 or 1 in tags):
        raise ValueError('Unsupported dynamic relocation model')
    destinations = set()
    for target, info, addend in struct.iter_unpack('<QQq', bounded(data, reloc[4], reloc[5])):
        if (info != 8 or target in destinations or target % 8
                or not any(a <= target and target + 8 <= b for a, b in ranges)
                or not 0 <= addend < end):
            raise ValueError('Invalid or non-relative relocation')
        destinations.add(target)
        struct.pack_into('<Q', image, target, BASE + addend)
    fvs = []
    for name in sorted(n for n in sections if n.startswith('.upld.') and n.endswith('_fv')):
        s = sections[name]
        fv = bounded(data, s[4], s[5])
        if len(fv) < 56 or fv[40:44] != b'_FVH' or struct.unpack_from('<Q', fv, 32)[0] != len(fv):
            raise ValueError('Invalid firmware-volume length/signature')
        hdrlen = struct.unpack_from('<H', fv, 48)[0]
        if hdrlen < 56 or hdrlen > len(fv) or hdrlen % 2 or sum(struct.unpack('<' + 'H' * (hdrlen // 2), fv[:hdrlen])) & 0xffff:
            raise ValueError('Invalid firmware-volume header checksum')
        boundary = max(4096, 1 << ((struct.unpack_from('<I', fv, 44)[0] >> 16) & 31))
        if boundary > 16 * 1024**2:
            raise ValueError('Unsupported firmware-volume alignment')
        position = align(len(image), boundary)
        if position + len(fv) > MAX_IMAGE:
            raise ValueError('Payload exceeds reserved image arena')
        image.extend(bytes(position - len(image)))
        image.extend(fv)
        fvs.append(dict(identifier=name[6:], base=BASE + position, size=len(fv)))
    fvs.sort(key=lambda f: (f['identifier'] != 'uefi_fv', f['identifier']))
    if not fvs or fvs[0]['identifier'] != 'uefi_fv' or len(fvs) > 8:
        raise ValueError('Missing DXE FV or too many FVs')
    image.extend(bytes(align(len(image)) - len(image)))
    header = bytearray(HEADER)
    struct.pack_into('<8sIIQQQII', header, 0, MAGIC, 1, HEADER, BASE, len(image), BASE + entry,
                     len(fvs), zlib.crc32(image))
    for i, fv in enumerate(fvs):
        identifier = fv['identifier'].encode('ascii')
        if len(identifier) > 15:
            raise ValueError('FV identifier too long')
        struct.pack_into('<16sQQ', header, 48 + i * 32, identifier, fv['base'], fv['size'])
    struct.pack_into('<I', header, HEADER - 4, zlib.crc32(header[:-4]))
    report = dict(format='PS5UPL01', load_base=BASE, entry=BASE + entry, image_bytes=len(image),
                  relocation_count=len(destinations), firmware_volumes=fvs,
                  section_segment_offset_mismatches=mismatches,
                  source_sha256=hashlib.sha256(data).hexdigest(),
                  source_repack_gaps_zero_initialized=True, hardware_execution_verified=False)
    return bytes(header + image), report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('elf', type=Path)
    p.add_argument('output', type=Path)
    a = p.parse_args()
    blob, report = pack(a.elf.read_bytes())
    with a.output.open('xb') as f:
        f.write(blob)
    report['container_sha256'] = hashlib.sha256(blob).hexdigest()
    a.output.with_suffix(a.output.suffix + '.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
