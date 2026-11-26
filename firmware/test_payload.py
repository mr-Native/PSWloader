"""Small generated ELF fixtures exercise the privileged loader's input producer."""
import importlib.util
from pathlib import Path
import struct
import unittest
import zlib

spec = importlib.util.spec_from_file_location('packer', Path(__file__).with_name('pack-payload.py'))
packer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packer)


def fixture():
    b = bytearray(0x3400)
    names = b'\0.text\0.data\0.dynamic\0.rela\0.upld.uefi_fv\0.shstrtab\0'
    b[0x500:0x500 + len(names)] = names
    ident = b'\x7fELF\x02\x01\x01' + bytes(9)
    packer.EHDR.pack_into(b, 0, ident, 3, 62, 1, 0x200, 64, 0x3000, 0, 64, 56, 2, 64, 7, 6)
    packer.PHDR.pack_into(b, 64, 1, 5, 0x200, 0x200, 0x200, 16, 16, 4096)
    packer.PHDR.pack_into(b, 120, 1, 6, 0x220, 0x400, 0x400, 16, 0x500, 4096)
    b[0x200:0x210] = b'\xc3' + bytes(15)
    b[0x220:0x230] = bytes(16)
    for i, pair in enumerate([(7, 0), (8, 24), (9, 24), (0x6ffffff9, 1), (0, 0)]):
        struct.pack_into('<qQ', b, 0x240 + i * 16, *pair)
    struct.pack_into('<QQq', b, 0x300, 0x400, 8, 0x200)
    fv = bytearray(4096)
    struct.pack_into('<Q', fv, 32, 4096)
    fv[40:44] = b'_FVH'
    struct.pack_into('<I', fv, 44, 12 << 16)
    struct.pack_into('<H', fv, 48, 72)
    struct.pack_into('<H', fv, 50, -sum(struct.unpack('<36H', fv[:72])) & 0xffff)
    b[0x1000:0x2000] = fv
    values = [('.text', 1, 6, 0x200, 0x200, 16, 0),
              ('.data', 1, 3, 0x400, 0x220, 16, 0),
              ('.dynamic', 6, 3, 0x800, 0x240, 80, 16),
              ('.rela', 4, 0, 0, 0x300, 24, 24),
              ('.upld.uefi_fv', 1, 0, 0, 0x1000, 4096, 0),
              ('.shstrtab', 3, 0, 0, 0x500, len(names), 0)]
    for i, (name, kind, flags, addr, off, size, entsize) in enumerate(values, 1):
        packer.SHDR.pack_into(b, 0x3000 + i * 64, names.index(name.encode()), kind,
                             flags, addr, off, size, 0, 0, 8, entsize)
    return b


class PayloadTests(unittest.TestCase):
    def test_relocation_and_bss_gap(self):
        output, r = packer.pack(fixture())
        self.assertEqual(r['relocation_count'], 1)
        self.assertEqual(r['section_segment_offset_mismatches'], ['.dynamic'])
        image = output[packer.HEADER:]
        self.assertEqual(struct.unpack_from('<Q', image, 0x400)[0], packer.BASE + 0x200)
        self.assertEqual(image[0x410:0x800], bytes(0x3f0))
        self.assertEqual(image[0x800:0x850], fixture()[0x240:0x290])
        self.assertEqual(struct.unpack_from('<I', output, 44)[0], zlib.crc32(image))
        self.assertEqual(r['firmware_volumes'][0]['base'], packer.BASE + 4096)

    def rejected(self, offset, fmt, value):
        b = fixture()
        struct.pack_into(fmt, b, offset, value)
        with self.assertRaises(ValueError):
            packer.pack(b)

    def test_truncated(self):
        for size in (0, 16, 63, 200, 0x2000):
            with self.subTest(size=size), self.assertRaises(ValueError):
                packer.pack(fixture()[:size])

    def test_wrong_architecture(self):
        self.rejected(18, '<H', 183)

    def test_bad_entry(self):
        self.rejected(24, '<Q', 0xdeadbeef)

    def test_oversized_segment(self):
        self.rejected(120 + 40, '<Q', 1 << 63)

    def test_unknown_relocation(self):
        self.rejected(0x308, '<Q', 1)

    def test_relocation_outside_allocation(self):
        self.rejected(0x300, '<Q', 0x600)

    def test_relocation_negative_addend(self):
        self.rejected(0x310, '<q', -1)

    def test_fv_checksum(self):
        self.rejected(0x1000 + 50, '<H', 0)

    def test_fv_length(self):
        self.rejected(0x1000 + 32, '<Q', 8192)

    def test_wrong_relocation_count(self):
        self.rejected(0x240 + 3 * 16 + 8, '<Q', 2)

    def test_overlapping_sections(self):
        self.rejected(0x3000 + 3 * 64 + 16, '<Q', 0x400)


if __name__ == '__main__':
    unittest.main()
