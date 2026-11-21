"""Read-only x64 EFI/ACPI observation from stopped test-machine memory.

The caller supplies bounded physical reads. No inference about native PS5 or
Windows compatibility is made. EFI system tables are found by signature AND
header CRC, not by treating arbitrary pointer-shaped values as tables.
"""
import hashlib
import struct
import uuid
import zlib

ACPI_GUID = uuid.UUID('8868e871-e4f1-11d3-bc22-0080c73c8881').bytes_le


def observe(read, scan_base, scan_bytes):
    data = read(scan_base, scan_bytes)
    candidates = []
    offset = 0
    while True:
        offset = data.find(b'IBI SYST', offset)
        if offset < 0:
            break
        address = scan_base + offset
        offset += 1
        header = bytearray(read(address, 24))
        size, crc = struct.unpack_from('<II', header, 12)
        if size != 120:
            continue
        table = bytearray(read(address, size))
        struct.pack_into('<I', table, 16, 0)
        if zlib.crc32(table) != crc:
            continue
        count, configs = struct.unpack_from('<QQ', table, 104)
        if not 0 < count <= 256:
            continue
        entries = read(configs, count * 24)
        for n in range(count):
            if entries[n*24:n*24+16] == ACPI_GUID:
                rsdp = struct.unpack_from('<Q', entries, n*24+16)[0]
                candidates.append((address, rsdp))
    if len(candidates) != 1:
        raise ValueError(f'Expected one CRC-valid EFI table with ACPI 2.0, found {len(candidates)}')
    system, rsdp_address = candidates[0]
    rsdp = read(rsdp_address, 36)
    if (rsdp[:8] != b'RSD PTR ' or rsdp[15] < 2 or
            struct.unpack_from('<I', rsdp, 20)[0] != 36 or
            sum(rsdp[:20]) % 256 or sum(rsdp) % 256):
        raise ValueError('Invalid published RSDP')

    def table_at(address, expected=None):
        header = read(address, 36)
        length = struct.unpack_from('<I', header, 4)[0]
        if not 36 <= length <= 1024*1024:
            raise ValueError('Invalid published ACPI table size')
        body = read(address, length)
        if sum(body) % 256 or (expected and body[:4] != expected):
            raise ValueError('Invalid published ACPI signature/checksum')
        return body

    xsdt_address = struct.unpack_from('<Q', rsdp, 24)[0]
    xsdt = table_at(xsdt_address, b'XSDT')
    if (len(xsdt)-36) % 8:
        raise ValueError('Misaligned XSDT entries')
    tables = []
    for pos in range(36, len(xsdt), 8):
        address = struct.unpack_from('<Q', xsdt, pos)[0]
        body = table_at(address)
        record = {'signature': body[:4].decode('ascii'), 'address': address,
                  'bytes': len(body), 'sha256': hashlib.sha256(body).hexdigest()}
        if body[:4] == b'FACP':
            if len(body) < 148:
                raise ValueError('Short FADT')
            dsdt_address = struct.unpack_from('<Q', body, 140)[0] or struct.unpack_from('<I', body, 40)[0]
            dsdt = table_at(dsdt_address, b'DSDT')
            record.update(flags=struct.unpack_from('<I', body, 112)[0],
                          iapc_boot_arch=struct.unpack_from('<H', body, 109)[0],
                          dsdt_address=dsdt_address, dsdt_bytes=len(dsdt),
                          dsdt_sha256=hashlib.sha256(dsdt).hexdigest())
        tables.append(record)
    return {'scope': 'Published EFI configuration table in stopped TCG firmware; no native/Windows validation',
            'scan_base': scan_base, 'scan_bytes': scan_bytes,
            'system_table': system, 'rsdp': rsdp_address, 'xsdt': xsdt_address,
            'tables': tables}
