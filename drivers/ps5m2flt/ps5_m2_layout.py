#!/usr/bin/env python3
"""PS5 internal M.2: nested GPT layout for Windows + synthetic GPT overlay (N3).

Layout follows ps5-linux (m2_init.c / patched block/partitions/efi.c):
  * LBA [0, SONY_HEAD) and the last SONY_TAIL sectors belong to GameOS and are never written.
  * Protective MBR at LBA SONY_HEAD, primary GPT header at SONY_HEAD+1, entries right after.
  * Backup GPT header at last_lba = N-1-SONY_TAIL, backup entries just before it.
  * All LBAs stored in the nested GPT are ABSOLUTE (that is what the patched Linux kernel reads).

The overlay (for the planned UEFI BlockIo filter N4 and Windows disk filter N5) synthesises a
standard GPT view (PMBR@0, header@1, backup@N-1) that points at the SAME absolute partitions and
hides the Sony areas by making first/last usable LBA equal the partition extent.

SAFETY: this tool only operates on regular image files. Block devices are refused.
"""
import argparse, binascii, hashlib, os, stat, struct, sys, uuid

SECTOR = 512
SONY_HEAD = 65536          # PS5_M2_MBR_LBA
SONY_TAIL = 34224          # PS5_M2_PART44_LBA_OFFSET
ENTRIES = 128
ENTRY_SIZE = 128
ENTRY_SECTORS = ENTRIES * ENTRY_SIZE // SECTOR   # 32
ALIGN = 2048

GUID_MSR = "E3C9E316-0B5C-4DB8-817D-F92DF00215AE"
GUID_BASIC_DATA = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"
GUID_ESP = "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"


def guid_bytes(s):
    return uuid.UUID(s).bytes_le


def crc32(b):
    return binascii.crc32(b) & 0xFFFFFFFF


class Disk:
    """Image file with a write guard that refuses the GameOS areas."""

    def __init__(self, path, writable=False):
        st = os.stat(path)
        if not stat.S_ISREG(st.st_mode):
            raise SystemExit(f"refused: {path} is not a regular image file")
        if st.st_size % SECTOR:
            raise SystemExit("image size is not a multiple of 512")
        self.n = st.st_size // SECTOR
        if self.n <= SONY_HEAD + SONY_TAIL + 4 * ALIGN:
            raise SystemExit("image too small for the PS5 M.2 layout")
        self.f = open(path, "r+b" if writable else "rb")
        self.last_lba = self.n - 1 - SONY_TAIL

    def read(self, lba, count=1):
        self.f.seek(lba * SECTOR)
        return self.f.read(count * SECTOR)

    def write(self, lba, data):
        count = (len(data) + SECTOR - 1) // SECTOR
        if lba < SONY_HEAD or lba + count - 1 > self.last_lba:
            raise RuntimeError(f"write guard: LBA {lba}+{count} touches the GameOS area")
        self.f.seek(lba * SECTOR)
        self.f.write(data)

    def protected_hashes(self):
        h_head = hashlib.sha256()
        for i in range(0, SONY_HEAD, 2048):
            h_head.update(self.read(i, min(2048, SONY_HEAD - i)))
        h_tail = hashlib.sha256(self.read(self.n - SONY_TAIL, SONY_TAIL))
        return h_head.hexdigest(), h_tail.hexdigest()


def entry(type_guid, uniq, first, last, name, attrs=0):
    nm = name.encode("utf-16-le")[:72].ljust(72, b"\0")
    return guid_bytes(type_guid) + guid_bytes(uniq) + struct.pack("<QQQ", first, last, attrs) + nm


def header(my_lba, alt_lba, first_usable, last_usable, disk_guid, entries_lba, entries_crc):
    h = bytearray(struct.pack("<8sIIIIQQQQ16sQIII", b"EFI PART", 0x00010000, 92, 0, 0,
                              my_lba, alt_lba, first_usable, last_usable, disk_guid,
                              entries_lba, ENTRIES, ENTRY_SIZE, entries_crc))
    struct.pack_into("<I", h, 16, crc32(bytes(h)))
    return bytes(h).ljust(SECTOR, b"\0")


def pmbr(start_lba, size):
    m = bytearray(SECTOR)
    rec = struct.pack("<B3sB3sII", 0, b"\x00\x02\x00", 0xEE, b"\xff\xff\xff", start_lba,
                      min(size, 0xFFFFFFFF))
    m[446:462] = rec
    m[510:512] = b"\x55\xaa"
    return bytes(m)


def plan(n, esp_mib=0, msr_mib=16):
    """Return partition list [(type, uniq, first, last, name)] inside the nested area."""
    last_lba = n - 1 - SONY_TAIL
    first_usable = SONY_HEAD + 2 + ENTRY_SECTORS          # PMBR + header + entries
    last_usable = last_lba - ENTRY_SECTORS - 1
    cur = (first_usable + ALIGN - 1) // ALIGN * ALIGN
    parts = []
    if esp_mib:
        size = esp_mib * 1024 * 1024 // SECTOR
        parts.append((GUID_ESP, str(uuid.uuid4()), cur, cur + size - 1, "PS5M2ESP"))
        cur += size
    size = msr_mib * 1024 * 1024 // SECTOR
    parts.append((GUID_MSR, str(uuid.uuid4()), cur, cur + size - 1, "Microsoft reserved partition"))
    cur += size
    end = (last_usable + 1) // ALIGN * ALIGN - 1
    parts.append((GUID_BASIC_DATA, str(uuid.uuid4()), cur, end, "PS5WINDOWS-M2"))
    return parts, first_usable, last_usable


def build_entries(parts):
    arr = b"".join(entry(t, u, a, b, nm) for t, u, a, b, nm in parts)
    return arr.ljust(ENTRIES * ENTRY_SIZE, b"\0")


def write_nested(disk, parts, first_usable, last_usable, disk_guid=None):
    disk_guid = disk_guid or uuid.uuid4().bytes_le
    arr = build_entries(parts)
    ecrc = crc32(arr)
    p_hdr_lba = SONY_HEAD + 1
    b_hdr_lba = disk.last_lba
    disk.write(SONY_HEAD, pmbr(p_hdr_lba, disk.last_lba - SONY_HEAD))
    disk.write(p_hdr_lba, header(p_hdr_lba, b_hdr_lba, first_usable, last_usable, disk_guid,
                                 p_hdr_lba + 1, ecrc))
    disk.write(p_hdr_lba + 1, arr)
    disk.write(b_hdr_lba - ENTRY_SECTORS, arr)
    disk.write(b_hdr_lba, header(b_hdr_lba, p_hdr_lba, first_usable, last_usable, disk_guid,
                                 b_hdr_lba - ENTRY_SECTORS, ecrc))
    return disk_guid


def parse_header(sec):
    f = struct.unpack("<8sIIIIQQQQ16sQIII", sec[:92])
    sig, rev, hsize, hcrc, _, my, alt, fu, lu, dg, elba, ne, esz, ecrc = f
    tmp = bytearray(sec[:hsize]); struct.pack_into("<I", tmp, 16, 0)
    return dict(sig=sig, crc_ok=(crc32(bytes(tmp)) == hcrc), my=my, alt=alt, first_usable=fu,
                last_usable=lu, disk_guid=dg, entries_lba=elba, n=ne, esz=esz, ecrc=ecrc)


def parse_entries(arr, ne, esz):
    out = []
    for i in range(ne):
        e = arr[i * esz:(i + 1) * esz]
        if e[:16] == b"\0" * 16:
            continue
        t = str(uuid.UUID(bytes_le=e[:16])).upper(); u = str(uuid.UUID(bytes_le=e[16:32])).upper()
        a, b = struct.unpack("<QQ", e[32:48])
        out.append((t, u, a, b, e[56:128].decode("utf-16-le").rstrip("\0")))
    return out


def verify_linux_view(disk):
    """Emulate the patched Linux efi.c path for nvme* disks."""
    m = disk.read(SONY_HEAD)
    assert m[510:512] == b"\x55\xaa", "PMBR signature missing at LBA 65536"
    os_type = m[446 + 4]; start = struct.unpack_from("<I", m, 446 + 8)[0]
    assert os_type == 0xEE and start == SONY_HEAD + 1, "PMBR record not EE@65537 (pmbr_part_valid)"
    h = parse_header(disk.read(SONY_HEAD + 1))
    assert h["sig"] == b"EFI PART" and h["crc_ok"], "primary GPT header invalid"
    assert h["my"] == SONY_HEAD + 1 and h["alt"] == disk.last_lba, "header LBAs do not match patched last_lba()"
    arr = disk.read(h["entries_lba"], ENTRY_SECTORS)
    assert crc32(arr[:h["n"] * h["esz"]]) == h["ecrc"], "entry array CRC mismatch"
    b = parse_header(disk.read(disk.last_lba))
    assert b["crc_ok"] and b["my"] == disk.last_lba and b["alt"] == SONY_HEAD + 1, "backup header invalid"
    parts = parse_entries(arr, h["n"], h["esz"])
    for _, _, a, z, _ in parts:
        assert h["first_usable"] <= a <= z <= h["last_usable"], "partition outside usable range"
    return h, parts


def overlay_sectors(disk):
    """Synthetic standard-GPT sectors {lba: bytes} for the filter view (N4/N5)."""
    h, parts = verify_linux_view(disk)
    arr = disk.read(h["entries_lba"], ENTRY_SECTORS)
    ecrc = crc32(arr)
    first = min(a for _, _, a, _, _ in parts)
    last = max(z for _, _, _, z, _ in parts)
    n = disk.n
    sec = {0: pmbr(1, n - 1),
           1: header(1, n - 1, first, last, h["disk_guid"], 2, ecrc),
           n - 1: header(n - 1, 1, first, last, h["disk_guid"], n - 1 - ENTRY_SECTORS, ecrc)}
    for i in range(ENTRY_SECTORS):
        sec[2 + i] = arr[i * SECTOR:(i + 1) * SECTOR]
        sec[n - 1 - ENTRY_SECTORS + i] = arr[i * SECTOR:(i + 1) * SECTOR]
    return sec, parts


def write_overlay_view(disk, out_path):
    """Test helper: materialise the filter view as a sparse image file (for sgdisk -v)."""
    sec, _ = overlay_sectors(disk)
    with open(out_path, "wb") as o:
        o.truncate(disk.n * SECTOR)
        for lba, data in sec.items():
            o.seek(lba * SECTOR); o.write(data)
    return out_path


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    w = sub.add_parser("layout", help="write the nested Windows GPT into an IMAGE file")
    w.add_argument("image"); w.add_argument("--esp-mib", type=int, default=0)
    w.add_argument("--msr-mib", type=int, default=16)
    v = sub.add_parser("verify", help="verify Linux view + print partitions")
    v.add_argument("image")
    o = sub.add_parser("overlay", help="write the synthetic standard-GPT view to a sparse file")
    o.add_argument("image"); o.add_argument("out")
    a = ap.parse_args()
    if a.cmd == "layout":
        d = Disk(a.image, writable=True)
        before = d.protected_hashes()
        parts, fu, lu = plan(d.n, a.esp_mib, a.msr_mib)
        write_nested(d, parts, fu, lu)
        d.f.flush(); os.fsync(d.f.fileno())
        after = d.protected_hashes()
        if before != after:
            raise SystemExit("ABORT: GameOS areas changed!")
        print("GameOS areas unchanged:", before[0][:16], before[1][:16])
        for p in parts:
            print(" ", p[4], p[2], p[3])
    elif a.cmd == "verify":
        h, parts = verify_linux_view(Disk(a.image))
        print("ok, first/last usable", h["first_usable"], h["last_usable"])
        for p in parts:
            print(" ", p[4], p[0], p[2], p[3])
    else:
        write_overlay_view(Disk(a.image), a.out); print("overlay view:", a.out)


if __name__ == "__main__":
    main()
