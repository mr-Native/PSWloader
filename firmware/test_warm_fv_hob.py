"""r5.26.3: a re-entry into an updated firmware rewrites the FV records of the bootstrap HOB list from the started container
(HW 24.11.: r5.26.0 -> r5.26.2 hung before the picture because the HOBs still described the old, smaller image)."""
import ctypes as C, re, struct, subprocess, tempfile, unittest
from pathlib import Path
HERE = Path(__file__).resolve().parent
SRC = (HERE / 'Ps5PlatformPkg/Library/Ps5ResetSystemLib/Ps5ResetSystemLib.c').read_text()
GUID = bytes([0xf6,0xba,0xa5,0x15,0x91,0x1c,0x7d,0x46,0x9d,0xfb,0x31,0x9d,0x17,0x8d,0x4b,0xb4])
class WarmFvHobTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        body = SRC[SRC.index('STATIC BOOLEAN WarmPatchFvHob'):SRC.index('STATIC VOID WarmPrepare (VOID) {')]
        code = ('#include <stdint.h>\n#include <string.h>\ntypedef uint8_t UINT8; typedef uint16_t UINT16; typedef uint32_t UINT32;'
                'typedef uint64_t UINT64; typedef uint64_t UINTN; typedef int BOOLEAN;\n#define STATIC\n#define CONST const\n'
                '#define TRUE 1\n#define FALSE 0\n#define CompareMem memcmp\n#define CopyMem memcpy\n#define DEBUG(x)\n' + body)
        cls.tmp = tempfile.TemporaryDirectory(); w = Path(cls.tmp.name)
        (w / 'h.c').write_text(code)
        subprocess.run(['gcc', '-shared', '-fPIC', '-Wall', '-Werror', '-Wno-unused-function', str(w / 'h.c'), '-o', str(w / 'h.so')], check=True)
        cls.lib = C.CDLL(str(w / 'h.so')); cls.lib.WarmPatchFvHob.restype = C.c_int
    @staticmethod
    def hobs(records):
        phit = struct.pack('<HHI', 1, 56, 0) + bytes(48)
        ext = struct.pack('<HHI', 4, 32 + 32 * len(records), 0) + GUID + struct.pack('<BBHI', 1, 0, 8 + 32 * len(records), len(records)) + b''.join(records)
        return bytearray(phit + struct.pack('<HHI', 3, 48, 0) + bytes(40) + ext + struct.pack('<HHI', 0xffff, 8, 0))
    @staticmethod
    def rec(base, size, name=b'uefi_fv'): return name.ljust(16, b'\0') + struct.pack('<QQ', base, size)
    def container(self, records):
        h = bytearray(4096); h[0:8] = b'PS5UPL01'; struct.pack_into('<I', h, 40, len(records))
        for i, r in enumerate(records): h[48 + 32 * i:80 + 32 * i] = r
        return (C.c_ubyte * 4096).from_buffer_copy(bytes(h))
    def test_records_rewritten_for_a_larger_image(self):
        old, new = self.rec(0x41001000, 0x4f0000), self.rec(0x41001000, 0x4f3000)
        h = self.hobs([old]); buf = (C.c_ubyte * len(h)).from_buffer_copy(bytes(h)); c = self.container([new])
        self.assertEqual(self.lib.WarmPatchFvHob(buf, C.c_uint64(len(h)), C.c_uint64(C.addressof(c))), 1)
        self.assertIn(new, bytes(buf)); self.assertNotIn(old, bytes(buf))
    def test_other_fv_count_is_refused_unchanged(self):
        h = self.hobs([self.rec(0x41001000, 0x4f0000)]); buf = (C.c_ubyte * len(h)).from_buffer_copy(bytes(h))
        c = self.container([self.rec(0x41001000, 0x200000), self.rec(0x41201000, 0x200000, b'second')])
        self.assertEqual(self.lib.WarmPatchFvHob(buf, C.c_uint64(len(h)), C.c_uint64(C.addressof(c))), 0)
        self.assertEqual(bytes(buf), bytes(h))
    def test_used_on_every_copy_and_on_the_update_switch(self):
        self.assertIn('if (!WarmPatchFvHob (Save, HobLen, Src)) return;', SRC)
        self.assertIn('WarmPatchFvHob ((UINT8 *)(UINTN)P->HobSrc, (UINTN)P->HobLen, PS5W_UPD_IMAGE)) {', SRC)
        sw = SRC[SRC.index('WarmPatchFvHob ((UINT8 *)(UINTN)P->HobSrc'):]
        self.assertLess(sw.index('WarmPatchFvHob'), sw.index('U[3] = 2;'))
if __name__ == '__main__':
    unittest.main()
