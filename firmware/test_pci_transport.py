"""Execute actual transport C with recorded raw-cycle callbacks."""
import ctypes as C
from pathlib import Path
import subprocess
import tempfile
import unittest

class PciTransportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory()
        library = Path(cls.work.name) / 'pci.so'
        subprocess.run(['gcc', '-shared', '-fPIC', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        str(Path(__file__).with_name('pci-transport.c')), '-o', str(library)], check=True)
        cls.lib = C.CDLL(str(library))
        cls.Enter = C.CFUNCTYPE(C.c_int, C.c_void_p)
        cls.Leave = C.CFUNCTYPE(None, C.c_void_p)
        cls.Read = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_uint32, C.c_uint, C.POINTER(C.c_uint32))
        cls.Write = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_uint32, C.c_uint, C.c_uint32)
        class IO(C.Structure):
            _fields_ = [('context', C.c_void_p), ('enter', cls.Enter), ('leave', cls.Leave),
                        ('read', cls.Read), ('write', cls.Write)]
        cls.IO = IO
        cls.lib.ps5_pci_access.argtypes = [C.POINTER(IO), C.c_uint32, C.c_uint, C.c_int, C.c_int, C.POINTER(C.c_uint32)]
        cls.lib.ps5_pci_access.restype = C.c_int

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def access(self, address=0x040080a4, width=4, ari=1, write=0, rid=0x10b, fail=None):
        calls = []
        def enter(_):
            calls.append(('enter',))
            return -9 if fail == 'enter' else 0
        def leave(_): calls.append(('leave',))
        def read(_, a, w, p):
            calls.append(('read', a, w))
            p[0] = rid if a & 0xfff == 0x150 else 0x12345678
            return -9 if fail == len(calls) else 0
        def put(_, a, w, v):
            calls.append(('write', a, w, v))
            return -9 if fail == len(calls) else 0
        io = self.IO(None, self.Enter(enter), self.Leave(leave), self.Read(read), self.Write(put))
        value = C.c_uint32(0xabcdef01)
        result = self.lib.ps5_pci_access(C.byref(io), address, width, ari, write, C.byref(value))
        return result, calls, value.value

    def test_direct_and_absent_rid(self):
        for ari, rid in [(0, 0x1234), (1, 0), (1, 0xffff)]:
            status, calls, value = self.access(ari=ari, rid=rid)
            self.assertEqual(status, 0)
            self.assertEqual(value, 0x12345678)
            self.assertFalse(any(c[0] == 'write' for c in calls))
            self.assertEqual(calls[-2:], [('read', 0x040080a4, 4), ('leave',)])

    def test_all_supported_rid_buses(self):
        for bus in range(33):
            for write in (0, 1):
                with self.subTest(bus=bus, write=write):
                    status, calls, _ = self.access(rid=(bus << 8) | 0x0b, write=write)
                    self.assertEqual(status, 0)
                    special = bus in (0, 32)
                    self.assertEqual(calls[2], ('write', 0x04000d90 if special else 0x04000d10, 4,
                                                4 if special or bus == 1 else 5))
                    self.assertEqual(calls[3], ('write', 0x04000d80 if special else 0x04000d00, 4,
                                                ((32 if special else bus) << 24) | 0xb0080))
                    target = 0x04000fa4 if special else 0x04000f24
                    self.assertEqual(calls[4], ('write', target, 4, 0xabcdef01) if write else ('read', target, 4))
                    self.assertEqual(calls[-1], ('leave',))

    def test_unsupported_rid_no_selector_writes(self):
        for bus in (33, 64, 127, 255):
            status, calls, _ = self.access(rid=bus << 8)
            self.assertEqual(status, -2)
            self.assertEqual([c[0] for c in calls], ['enter', 'read', 'leave'])

    def test_failures_unlock_and_stop(self):
        for step in (2, 3, 4, 5):
            status, calls, _ = self.access(fail=step)
            self.assertEqual(status, -9)
            self.assertEqual(len(calls), step + 1)
            self.assertEqual(calls[-1], ('leave',))
        self.assertEqual(self.access(fail='enter')[:2], (-9, [('enter',)]))

    def test_width_and_ecam_guards(self):
        for address, width in [(0x08000000, 4), (0x100001, 2), (0x100002, 4), (0, 8), (0, 0)]:
            status, calls, _ = self.access(address=address, width=width)
            self.assertEqual((status, calls), (-1, []))
        for width in (1, 2, 4):
            self.assertEqual(self.access(address=0x0400807c, width=width)[0], 0)

if __name__ == '__main__': unittest.main()
