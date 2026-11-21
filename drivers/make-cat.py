#!/usr/bin/env python3
"""make-cat.py v1.0 - builds an UNSIGNED Windows security catalog (.cat) on Linux, then sign it with
osslsigncode (see make-winpe-drivers.sh). The layout matches makecat.exe output byte for byte except for
the list identifier and the time stamp (reference: makecat on Windows 11 for ps5dod.inf + ps5dod.sys):
  - member tag: SHA1 hash as upper-case hex, UTF-16LE with terminating NUL (what Windows looks up)
  - member digest: sha1; .inf = flat file hash (SIP DE351A42), .sys/.dll = Authenticode PE hash (SIP C689AAB8)
  - member attribute 1.3.6.1.4.1.311.12.2.2 {SIP GUID, 0x0200}; catalog attribute OSAttr "2:10.0"
  - subject algorithm 1.3.6.1.4.1.311.12.1.2

Usage: make-cat.py --name ps5gbe.cat --pe ps5gbe.sys --flat ps5gbe.inf --out ps5gbe.unsigned.cat
  --pe   <file> : PE member (.sys/.dll/.exe), Authenticode SHA1 (repeatable)
  --flat <file> : non-PE member (.inf), flat SHA1 (repeatable)
"""
import argparse
import hashlib
import struct
import uuid
from datetime import datetime, timezone
from pathlib import Path

# ---------------- minimal DER encoder ----------------

def _len(n: int) -> bytes:
    if n < 0x80:
        return bytes([n])
    b = n.to_bytes((n.bit_length() + 7) // 8, 'big')
    return bytes([0x80 | len(b)]) + b

def tlv(tag: int, body: bytes) -> bytes:
    return bytes([tag]) + _len(len(body)) + body

def der_seq(*parts: bytes) -> bytes:   return tlv(0x30, b''.join(parts))
def der_set(*parts: bytes) -> bytes:   return tlv(0x31, b''.join(parts))
def der_oct(b: bytes) -> bytes:        return tlv(0x04, b)
def der_null() -> bytes:               return tlv(0x05, b'')
def der_bmp(s: str) -> bytes:          return tlv(0x1E, s.encode('utf-16-be'))
def der_int(n: int) -> bytes:          return tlv(0x02, n.to_bytes(max(1, (n.bit_length() + 8) // 8), 'big'))
def der_ctx(num: int, body: bytes, constructed=True) -> bytes:
    return tlv(0xA0 + num if constructed else 0x80 + num, body)

def der_oid(dotted: str) -> bytes:
    parts = [int(x) for x in dotted.split('.')]
    out = bytearray([40 * parts[0] + parts[1]])
    for n in parts[2:]:
        if n == 0:
            out.append(0); continue
        stack = []
        while n:
            stack.append(n & 0x7f); n >>= 7
        for i in range(len(stack) - 1, -1, -1):
            out.append(stack[i] | (0x80 if i else 0))
    return tlv(0x06, bytes(out))

def der_utctime(dt: datetime) -> bytes:
    return tlv(0x17, dt.strftime('%y%m%d%H%M%SZ').encode('ascii'))

# ---------------- Authenticode PE hash ----------------

def authenticode_hash(path: Path, algo='sha1') -> bytes:
    """PE image hash as CryptCATAdminCalcHashFromFileHandle/signtool compute it: skips the optional
    header CheckSum (4 B), the Certificate Table directory entry (8 B) and an appended certificate table."""
    data = path.read_bytes()
    if data[:2] != b'MZ':
        raise ValueError(f'{path}: not a PE file')
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    if data[pe:pe + 4] != b'PE\0\0':
        raise ValueError(f'{path}: no PE header')
    opt = pe + 24
    magic = struct.unpack_from('<H', data, opt)[0]
    checksum_off = opt + 64
    if magic == 0x20b:      # PE32+
        dd_off = opt + 112
    elif magic == 0x10b:    # PE32
        dd_off = opt + 96
    else:
        raise ValueError(f'{path}: unknown PE magic {magic:#x}')
    sec_dir_off = dd_off + 4 * 8          # Certificate Table = data directory index 4
    cert_off, cert_size = struct.unpack_from('<II', data, sec_dir_off)
    h = hashlib.new(algo)
    h.update(data[:checksum_off])
    h.update(data[checksum_off + 4:sec_dir_off])
    if cert_size and cert_off:
        h.update(data[sec_dir_off + 8:cert_off])
        h.update(data[cert_off + cert_size:])
    else:
        h.update(data[sec_dir_off + 8:])
    return h.digest()

# ---------------- catalog members ----------------

SIP_FLAT = '{DE351A42-8E59-11D0-8C47-00C04FC295EE}'
SIP_PE = '{C689AAB8-8E78-11D0-8C47-00C04FC295EE}'
OSATTR = bytes.fromhex('30241E0C004F00530041007400740072020410010001040E32003A00310030002E0030000000')

def _member(digest: bytes, pe: bool) -> bytes:
    if pe:   # SpcPeImageData: flags BIT STRING (makecat: 05 a0) + [0]{[2]{[0]}} empty SpcLink
        inner = der_seq(der_oid('1.3.6.1.4.1.311.2.1.15'),
                        der_seq(tlv(0x03, b'\x05\xa0'), der_ctx(0, der_ctx(2, der_ctx(0, b'', constructed=False)))))
    else:    # flat file: SPC_CAB_DATA-style link [2]{[0]}
        inner = der_seq(der_oid('1.3.6.1.4.1.311.2.1.25'), der_ctx(2, der_ctx(0, b'', constructed=False)))
    digest_info = der_seq(der_seq(der_oid('1.3.14.3.2.26'), der_null()), der_oct(digest))
    indirect = der_seq(der_oid('1.3.6.1.4.1.311.2.1.4'), der_set(der_seq(inner, digest_info)))
    sipinfo = der_seq(der_oid('1.3.6.1.4.1.311.12.2.2'),
                      der_set(der_seq(der_bmp(SIP_PE if pe else SIP_FLAT), der_int(0x200))))
    tag = (digest.hex().upper() + '\0').encode('utf-16-le')
    return tag, der_seq(der_oct(tag), der_set(indirect, sipinfo))

def build_ctl(pe_files, flat_files) -> bytes:
    members = [_member(authenticode_hash(Path(p)), True) for p in pe_files]
    members += [_member(hashlib.sha1(Path(p).read_bytes()).digest(), False) for p in flat_files]
    members.sort(key=lambda m: m[0])          # makecat lists members sorted by tag
    return der_seq(der_seq(der_oid('1.3.6.1.4.1.311.12.1.1')),
                   der_oct(uuid.uuid4().bytes),
                   der_utctime(datetime.now(timezone.utc)),
                   der_seq(der_oid('1.3.6.1.4.1.311.12.1.2'), der_null()),
                   der_seq(*[m[1] for m in members]),
                   der_ctx(0, der_seq(der_seq(der_oid('1.3.6.1.4.1.311.12.2.1'), der_oct(OSATTR)))))

def build_unsigned_cat(pe_files, flat_files) -> bytes:
    encap = der_seq(der_oid('1.3.6.1.4.1.311.10.1'), der_ctx(0, build_ctl(pe_files, flat_files)))
    signed_data = der_seq(der_int(1), der_set(), encap, der_set())  # v1, empty digestAlgorithms + signerInfos
    return der_seq(der_oid('1.2.840.113549.1.7.2'), der_ctx(0, signed_data))

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--name', required=True, help='catalog name from the .cdf (log only)')
    ap.add_argument('--pe', action='append', default=[], help='PE member (Authenticode hash)')
    ap.add_argument('--flat', action='append', default=[], help='flat member such as .inf (flat SHA1)')
    ap.add_argument('--out', required=True, type=Path)
    a = ap.parse_args()
    if not a.pe and not a.flat:
        ap.error('at least one --pe or --flat required')
    blob = build_unsigned_cat(a.pe, a.flat)
    a.out.write_bytes(blob)
    print(f'{a.name}: unsigned catalog {len(blob)} B -> {a.out} '
          f'({len(a.pe)} PE + {len(a.flat)} flat member)')

if __name__ == '__main__':
    main()
