import ctypes as C
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from test_acpi_candidate import m

class AcpiValidatorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work=tempfile.TemporaryDirectory();lib=Path(cls.work.name)/'acpi.so'
        subprocess.run(['gcc','-shared','-fPIC','-std=c11','-Wall','-Wextra','-Werror',str(Path(__file__).with_name('acpi-validate.c')),'-o',str(lib)],check=True)
        cls.lib=C.CDLL(str(lib));cls.call=cls.lib.ps5_validate_acpi
        cls.call.argtypes=[C.c_void_p,C.c_size_t,C.c_uint64]
    @classmethod
    def tearDownClass(cls):cls.work.cleanup()
    def fixture(self,base=0x40200000):
        tables={n:m.header(n.encode(),s,r) for n,s,r in [('FACP',276,6),('DSDT',48,2),('APIC',52,3),('MCFG',60,1)]}
        struct.pack_into('<I',tables['FACP'],112,1<<20)
        struct.pack_into('<I',tables['APIC'],36,0xfee00000)
        tables['APIC'][44:52]=b'\0\x08\0\0\x01\0\0\0'
        struct.pack_into('<QHBBI',tables['MCFG'],44,0xf0000000,0,0,127,0)
        return m.assemble({n:bytes(m.checksum(t)) for n,t in tables.items()},base)
    def check(self,image,base=0x40200000):return self.call(C.create_string_buffer(bytes(image)),len(image),base)
    def fix_table(self,image,offset):
        length=struct.unpack_from('<I',image,offset+4)[0]
        image[offset:offset+length]=m.checksum(image[offset:offset+length])
    def test_valid_below_and_above_4g(self):
        for base in (0x40200000,0x100200000):self.assertEqual(self.check(self.fixture(base)[0],base),0)
    def test_invalid_checksums_and_bounds(self):
        data,layout=self.fixture()
        for offset in (8,32,layout['addresses']['DSDT']-0x40200000+12):
            image=bytearray(data);image[offset]^=1;self.assertNotEqual(self.check(image),0)
        self.assertEqual(self.check(data[:100]),-1)
    def test_xsdt_pointer_outside_arena_with_valid_checksum(self):
        data,layout=self.fixture();image=bytearray(data);off=layout['addresses']['XSDT']-0x40200000
        struct.pack_into('<Q',image,off+36,0xfffffffffffff000);self.fix_table(image,off)
        self.assertEqual(self.check(image),-3)
    def test_wrong_mcfg_with_valid_checksum(self):
        data,layout=self.fixture();image=bytearray(data);off=layout['addresses']['MCFG']-0x40200000
        image[off+55]=255;self.fix_table(image,off)
        self.assertEqual(self.check(image),-6)
    def test_zero_length_madt_record_rejected(self):
        data,layout=self.fixture();image=bytearray(data);off=layout['addresses']['APIC']-0x40200000
        image[off+45]=0;self.fix_table(image,off)
        self.assertEqual(self.check(image),-7)
    def test_legacy_dsdt_pointer_mismatch(self):
        data,layout=self.fixture();image=bytearray(data);off=layout['addresses']['FACP']-0x40200000
        struct.pack_into('<I',image,off+40,0);self.fix_table(image,off)
        self.assertEqual(self.check(image),-4)

if __name__=='__main__':unittest.main()
