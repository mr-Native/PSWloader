import importlib.util
from pathlib import Path
import struct
import unittest

spec=importlib.util.spec_from_file_location('acpi_candidate',Path(__file__).with_name('build-acpi-candidate.py'))
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)

class AcpiCandidateTests(unittest.TestCase):
    def tables(self):
        return {name:bytes(m.checksum(m.header(name.encode(),size,revision)))
                for name,size,revision in [('FACP',276,6),('DSDT',48,2),('APIC',44,3),('MCFG',60,1)]}
    def test_pointer_hierarchy_checksums_and_padding(self):
        for base in (0x40200000,0x100200000):
            image,layout=m.assemble(self.tables(),base)
            self.assertEqual(len(image)%4096,0)
            self.assertEqual(image[:8],b'RSD PTR ')
            self.assertEqual(sum(image[:20])%256,0);self.assertEqual(sum(image[:36])%256,0)
            xsdt=struct.unpack_from('<Q',image,24)[0]-base
            self.assertEqual(image[xsdt:xsdt+4],b'XSDT')
            addresses=struct.unpack_from('<QQQ',image,xsdt+36)
            self.assertEqual(addresses,tuple(layout['addresses'][n] for n in ('FACP','APIC','MCFG')))
            for name,address in layout['addresses'].items():
                off=address-base;length=struct.unpack_from('<I',image,off+4)[0]
                self.assertEqual(image[off:off+4],name.encode());self.assertEqual(sum(image[off:off+length])%256,0)
            f=addresses[0]-base;dsdt=layout['addresses']['DSDT']
            self.assertEqual(struct.unpack_from('<Q',image,f+140)[0],dsdt)
            self.assertEqual(struct.unpack_from('<I',image,f+40)[0],dsdt if dsdt<1<<32 else 0)
            self.assertEqual(struct.unpack_from('<Q',image,f+132)[0],0)
    def test_invalid_bases(self):
        for base in (0,0x40200001,1<<44,-4096):
            with self.assertRaises(ValueError):m.assemble(self.tables(),base)
    def test_corrupt_table_rejected(self):
        tables=self.tables();tables['APIC']=tables['APIC'][:-1]+b'\x01'
        with self.assertRaises(ValueError):m.assemble(tables,0x40200000)
    def test_oversize_arena_rejected(self):
        tables=self.tables();tables['DSDT']=bytes(m.checksum(m.header(b'DSDT',65536,2)))
        with self.assertRaises(ValueError):m.assemble(tables,0x40200000)
    def test_phantom_madt_moves_only_ioapic(self):
        madt=m.header(b'APIC',44+8+12+6,3);madt[44:52]=bytes([0,8,0,0,1,0,0,0])
        madt[52:64]=bytes([1,12,0x10,0])+struct.pack('<II',0xFEC00000,0);madt[64:70]=bytes([4,6,0xff,5,0,1])
        out=m.phantom_madt(bytes(m.checksum(madt)))
        self.assertEqual(struct.unpack_from('<BBBxII',out,52),(1,12,0x10,0xFEE003F0,24))
        self.assertEqual(out[:9]+out[10:52],bytes(madt[:9]+madt[10:52]));self.assertEqual(out[64:],bytes(madt[64:]))
        self.assertEqual(sum(out)%256,0)
        with self.assertRaises(ValueError):m.phantom_madt(bytes(m.checksum(m.header(b'APIC',44,3))))
    def test_generated_ids_are_neutral(self):
        image,_=m.assemble(self.tables(),0x40200000)
        self.assertEqual(image[9:15],b'PSWLDR');self.assertNotIn(bytes.fromhex('474c58'),image)   # the lab's old OEM prefix must be gone

if __name__=='__main__':unittest.main()

class AcpiRootIoContractTests(unittest.TestCase):
    def test_compiled_word_io_matches_extended_hob_aperture(self):
        import shutil,subprocess,tempfile
        if not shutil.which('iasl'):self.skipTest('iasl required')
        with tempfile.TemporaryDirectory() as work:
            prefix=Path(work)/'ps5'
            subprocess.run(['iasl','-p',str(prefix),str(Path(__file__).parent/'acpi/ps5-dsdt.asl')],check=True,capture_output=True)
            aml=prefix.with_suffix('.aml').read_bytes()
            needle=bytes.fromhex('880d0001')
            self.assertEqual(aml.count(needle),1)
            offset=aml.index(needle)
            self.assertEqual(struct.unpack_from('<5H',aml,offset+6),(0,0x1000,0x2fff,0,0x2000))
