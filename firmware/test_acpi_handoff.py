import ctypes as C
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import test_pci_handoff as pci
import test_acpi_validate as validator
from test_handoff import Result

class AcpiHandoffTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work=tempfile.TemporaryDirectory();out=Path(cls.work.name)/'handoff.so';here=Path(__file__).parent
        subprocess.run(['gcc','-shared','-fPIC','-Wall','-Wextra','-Werror',str(here/'acpi-handoff.c'),str(here/'acpi-validate.c'),'-o',str(out)],check=True)
        cls.lib=C.CDLL(str(out));cls.call=cls.lib.ps5_add_acpi_handoff
        cls.call.argtypes=[C.c_void_p,C.c_size_t,C.c_void_p,C.c_size_t,C.c_void_p,C.c_size_t,C.POINTER(Result)]
        pci.PciHandoffTests.setUpClass()
    @classmethod
    def tearDownClass(cls):pci.PciHandoffTests.tearDownClass();cls.work.cleanup()
    def fixture(self):
        helper=pci.PciHandoffTests();raw,r=helper.prepared();status,raw=helper.augment(raw,r)
        self.assertEqual(status,0)
        arena,_=validator.AcpiValidatorTests().fixture()
        return raw,r,arena
    def run_handoff(self,raw,r,arena,capacity=32768):
        inp=C.create_string_buffer(raw);a=C.create_string_buffer(arena);out=C.create_string_buffer(capacity)
        status=self.call(inp,len(raw),a,len(arena),out,capacity,C.byref(r))
        return status,out.raw[:r.hob_bytes]
    def test_allocation_split_new_rsdp_and_pci_hid(self):
        raw,r,arena=self.fixture();status,out=self.run_handoff(raw,r,arena);self.assertEqual(status,0)
        allocations=[];acpi=[];roots=[]
        for kind,h,off in pci.PciHandoffTests().hobs(out):
            if kind==2:allocations.append(struct.unpack_from('<QQI',h,24))
            if kind==4 and h[8:24]==bytes.fromhex('06959a9f97551545bab68bcde784ba87'):acpi.append(struct.unpack_from('<Q',h,28)[0])
            if kind==4 and h[8:24]==pci.GUID:roots.append(struct.unpack_from('<I',h,204)[0])
        self.assertEqual(acpi,[0x40200000]);self.assertEqual(roots,[0x0a0841d0])
        # Default build: unchanged 3-segment split (no IO-APIC scratch).
        self.assertIn((0x40000000,0x200000,4),allocations)
        self.assertIn((0x40200000,4096,9),allocations)
        self.assertIn((0x40201000,0x3dff000,4),allocations)
        self.assertNotIn((0x40000000,0x4000000,4),allocations)
        self.assertEqual(struct.unpack_from('<Q',out,48)[0],0x40000000+len(out)-8)
    def test_ioapic_scratch_optin_reserves_one_page_and_requires_single_page_arena(self):
        # Opt-in firmware IO-APIC: compile with -DPS5_IOAPIC_SCRATCH and expect the 4-segment split
        # with a dedicated EfiReserved (Type 0) page at 0x40201000, plus a strict one-page-arena guard.
        here=Path(__file__).parent
        with tempfile.TemporaryDirectory() as w:
            so=Path(w)/'scratch.so'
            subprocess.run(['gcc','-shared','-fPIC','-Wall','-Wextra','-Werror','-DPS5_IOAPIC_SCRATCH',
                            str(here/'acpi-handoff.c'),str(here/'acpi-validate.c'),'-o',str(so)],check=True)
            lib=C.CDLL(str(so));call=lib.ps5_add_acpi_handoff
            call.argtypes=self.call.argtypes
            raw,r,arena=self.fixture()
            self.assertEqual(len(arena),4096)
            inp=C.create_string_buffer(raw);a=C.create_string_buffer(arena);out=C.create_string_buffer(32768)
            self.assertEqual(call(inp,len(raw),a,len(arena),out,32768,C.byref(r)),0)
            data=out.raw[:r.hob_bytes];allocs=[]
            off=0
            while off<len(data)-8:
                t,l=struct.unpack_from('<HH',data,off)
                if t==0xffff:break
                if t==2:allocs.append(struct.unpack_from('<QQI',data,off+24))
                off+=l
            self.assertIn((0x40200000,4096,9),allocs)          # arena, ACPI reclaim
            self.assertIn((0x40201000,4096,0),allocs)          # dedicated IO-APIC scratch, EfiReserved
            self.assertIn((0x40202000,0x3dfe000,4),allocs)
            self.assertNotIn((0x40201000,0x3dff000,4),allocs)  # old 3-segment tail is gone
            # one-page-arena guard: a validate-passing 2-page arena must hit the explicit -2 guard.
            # Reset r.hob_bytes to len(raw) first (the entry guard requires hob_bytes==size; the prior
            # successful call left it at the output size). On error the result is left unchanged.
            r.hob_bytes=len(raw);a2=C.create_string_buffer(arena+bytes(4096))
            self.assertEqual(call(inp,len(raw),a2,8192,out,32768,C.byref(r)),-2)
            self.assertEqual(r.hob_bytes,len(raw))
    def test_corrupt_arena_does_not_change_result(self):
        raw,r,arena=self.fixture();before=r.hob_bytes;a=bytearray(arena);a[8]^=1
        self.assertEqual(self.run_handoff(raw,r,bytes(a))[0],-1);self.assertEqual(r.hob_bytes,before)
    def test_capacity_failure_preserves_result(self):
        raw,r,arena=self.fixture();before=r.hob_bytes
        self.assertEqual(self.run_handoff(raw,r,arena,len(raw))[0],-4);self.assertEqual(r.hob_bytes,before)
    def test_existing_allocation_collision(self):
        raw,r,arena=self.fixture();data=bytearray(raw)
        for kind,h,off in pci.PciHandoffTests().hobs(raw):
            if kind==2 and struct.unpack_from('<Q',h,24)[0]==0x40000000:
                struct.pack_into('<I',data,off+40,10);break
        self.assertEqual(self.run_handoff(bytes(data),r,arena)[0],-3)

if __name__=='__main__':unittest.main()
