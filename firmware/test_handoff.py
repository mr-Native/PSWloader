"""Compile and execute the actual freestanding HOB producer in a host process.

Only caller-owned buffers are accessed; no privileged instructions or devices.
"""
import ctypes
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from test_payload import fixture, packer


class Result(ctypes.Structure):
    _fields_ = [('entry', ctypes.c_uint64), ('image_bytes', ctypes.c_uint64),
                ('hob_bytes', ctypes.c_uint64), ('resource_count', ctypes.c_uint32),
                ('fv_count', ctypes.c_uint32)]


def boot_params(regions):
    bp = bytearray(4096)
    bp[0x1e8] = len(regions)
    struct.pack_into('<Q', bp, 0x70, 0x7fd8e014)
    for i, (start, size, kind) in enumerate(regions):
        struct.pack_into('<QQI', bp, 0x2d0 + i * 20, start, size, kind)
    return bp


class HandoffTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        lib = Path(cls.temp.name) / 'handoff.so'
        subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2',
                        '-fPIC', '-shared', str(Path(__file__).with_name('handoff.c')),
                        '-o', str(lib)], check=True)
        cls.library = ctypes.CDLL(str(lib))
        cls.call = cls.library.ps5_prepare_handoff
        cls.call.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t,
                             ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint, ctypes.POINTER(Result)]
        cls.call.restype = ctypes.c_int

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def prepare(self, regions=None, container=None, capacity=32768):
        if regions is None:
            regions = [(0x1000, 0x6f000, 1), (0x100000, 0xf00000, 1),
                       (0x40000000, 0x20000000, 1), (0x7fd6f000, 0x20000, 3),
                       (0x100000000, 0x350000000, 1)]
        bp = ctypes.create_string_buffer(bytes(boot_params(regions)))
        blob = bytes(container) if container is not None else packer.pack(fixture())[0]
        image = ctypes.create_string_buffer(blob)
        out = ctypes.create_string_buffer(capacity)
        result = Result()
        code = self.call(bp, 4096, image, len(blob), out, capacity, 48, ctypes.byref(result))
        return code, out.raw, result

    def test_pi_hob_structure_and_reservations(self):
        code, raw, result = self.prepare()
        self.assertEqual(code, 0)
        self.assertEqual(result.entry, packer.BASE + 0x200)
        offset, types, resources, allocations = 0, [], [], []
        while offset < result.hob_bytes:
            kind, size, reserved = struct.unpack_from('<HHI', raw, offset)
            self.assertEqual(size % 8, 0)
            self.assertGreaterEqual(size, 8)
            self.assertEqual(reserved, 0)
            types.append(kind)
            if kind == 3:
                typ, attr, base, length = struct.unpack_from('<IIQQ', raw, offset + 24)
                resources.append((typ, attr, base, length))
            if kind == 2:
                allocations.append(struct.unpack_from('<QQI', raw, offset + 24))
            if kind == 0xffff:
                self.assertEqual(struct.unpack_from('<Q', raw, 48)[0], 0x40000000 + offset)
            offset += size
        self.assertEqual(offset, result.hob_bytes)
        self.assertEqual(types[0], 1)
        self.assertEqual(types[-1], 0xffff)
        self.assertEqual(types.count(6), 1)
        self.assertEqual(types.count(4), 2)
        self.assertEqual(struct.unpack_from('<Q', raw, 40)[0], 0x40000000 + result.hob_bytes)
        self.assertIn((5, 0x401, 0x100000000, 0x12000000), resources)
        self.assertIn((0x7fd6f000, 0x20000, 9), allocations)
        self.assertIn((0x40000000, 0x4000000, 4), allocations)
        for left, right in zip(resources, resources[1:]):
            self.assertLessEqual(left[2] + left[3], right[2])

    def test_reserved_resources_cannot_be_promoted_to_free_ram(self):
        # Exercise exact HOB bytes for the measured Windows preferred-image
        # reservation and the loader arena, ACPI and unknown transport types.
        regions=[(0x1000,0x5f000,1),(0x100000,0xf00000,1),
                 (0x1000000,0x3f000000,2),(0x40000000,0x20000000,1),
                 (0x7fd67000,0x8000,4),(0x7fd6f000,0x20000,3),
                 (0x80000000,0x100000,0xfeed),(0x100000000,0x350000000,1)]
        code,raw,result=self.prepare(regions)
        self.assertEqual(code,0);offset=0;reserved=[]
        while offset<result.hob_bytes:
            kind,size,_=struct.unpack_from('<HHI',raw,offset)
            if kind==3:
                typ,attr,base,length=struct.unpack_from('<IIQQ',raw,offset+24)
                if typ==5:
                    reserved.append((base,length))
                    # Exact eligibility predicate used by pinned DXE promotion.
                    self.assertNotEqual(attr & 7,3)
                    self.assertFalse(attr & 2)
                else:self.assertEqual(attr & 7,7)
            offset+=size
        self.assertIn((0x1000000,0x3f000000),reserved)
        self.assertIn((0x100000000,0x12000000),reserved)

    def test_overlapping_tmr_blocks_workspace(self):
        code, _, _ = self.prepare([(0x40000000, 0x20000000, 1), (0x42000000, 4096, 2)])
        self.assertEqual(code, -6)

    def test_gap_is_not_free_ram(self):
        code, _, _ = self.prepare([(0x40000000, 0x1000000, 1), (0x42000000, 0x6000000, 1)])
        self.assertEqual(code, -6)

    def test_unknown_type_is_not_ram(self):
        code, _, _ = self.prepare([(0x40000000, 0x20000000, 1), (0x42000000, 4096, 0xfeed)])
        self.assertEqual(code, -6)

    def test_range_overflow(self):
        code, _, _ = self.prepare([(0xfffffffffffff000, 8192, 1)])
        self.assertEqual(code, -5)

    def test_image_corruption(self):
        blob = bytearray(packer.pack(fixture())[0]); blob[4096 + 0x200] ^= 1
        code, _, _ = self.prepare(container=blob)
        self.assertEqual(code, -3)

    def test_header_corruption(self):
        blob = bytearray(packer.pack(fixture())[0]); blob[32] ^= 1
        code, _, _ = self.prepare(container=blob)
        self.assertEqual(code, -2)

    def test_small_hob_buffer(self):
        code, _, _ = self.prepare(capacity=16)
        self.assertEqual(code, -1)

    def test_unaligned_acpi_rejected(self):
        code, _, _ = self.prepare([(0x40000000, 0x20000000, 1), (0x7fd6f001, 4096, 3)])
        self.assertEqual(code, -8)


if __name__ == '__main__':
    unittest.main()
