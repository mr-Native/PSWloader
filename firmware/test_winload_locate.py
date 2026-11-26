#!/usr/bin/env python3
"""v1.0: winload-locate.c finds the sti thunk + 9 EBS call sites by signature in BOTH 26100.9444 and 26100.9711,
at the exact RVAs independently known from the PDB-based re-derivation. Proves the version-independent locator."""
import os
import ctypes, struct, shutil, subprocess, tempfile, unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REF = Path(os.environ.get('PSW_WORK', '/opt/pswloader-work') + '/windows-reference/winload.efi')
NEW = Path(os.environ.get('PSW_WORK', '/opt/pswloader-work') + '/update-diag-readback/current/winload.efi')
EXPECT = {  # known-good RVAs (from rederive-winload-sites.py)
    'ref': (0x2a9220, [0x10477,0x104c3,0x105dd,0x10616,0x106b9,0x106e8,0x108db,0x089b1,0x1b605]),
    'new': (0x2a9dc0, [0x10793,0x107df,0x108f9,0x10932,0x109d5,0x10a04,0x10bf7,0x09d59,0x1c68d]),
}

class Loc(ctypes.Structure):
    _fields_ = [('sti_rva', ctypes.c_uint32), ('site_rva', ctypes.c_uint32*9),
                ('sti_ok', ctypes.c_int), ('sites_ok', ctypes.c_int)]

def text_section(fn):
    b = fn.read_bytes(); pe = struct.unpack_from('<I', b, 0x3c)[0]
    so = pe+24+struct.unpack_from('<H', b, pe+20)[0]; n = struct.unpack_from('<H', b, pe+6)[0]
    for i in range(n):
        base = so+i*40; vrva = struct.unpack_from('<I', b, base+12)[0]
        if vrva == 0x1000:
            rptr = struct.unpack_from('<I', b, base+20)[0]; rsz = struct.unpack_from('<I', b, base+16)[0]
            return b[rptr:rptr+rsz], vrva
    raise RuntimeError('no .text')

@unittest.skipUnless(shutil.which('cc') and REF.exists() and NEW.exists(), 'no cc / binaries')
class WinloadLocate(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        so = Path(cls.tmp.name)/'wl.so'
        subprocess.run(['cc','-shared','-fPIC','-O2','-Wall','-Werror','-o',str(so),str(HERE/'winload-locate.c')], check=True)
        cls.lib = ctypes.CDLL(str(so))
        cls.lib.ps5_wl_locate.restype = ctypes.c_int

    def check(self, fn, key):
        text, rva = text_section(fn)
        loc = Loc()
        rc = self.lib.ps5_wl_locate(text, len(text), rva, ctypes.byref(loc))
        sti, sites = EXPECT[key]
        self.assertEqual(rc, 0, f'{key}: rc={rc}')
        self.assertTrue(loc.sti_ok); self.assertEqual(loc.sti_rva, sti, f'{key} sti')
        self.assertEqual(loc.sites_ok, 9)
        self.assertEqual(list(loc.site_rva), sites, f'{key} sites')

    def test_ref_9444(self): self.check(REF, 'ref')
    def test_new_9711(self): self.check(NEW, 'new')

if __name__ == '__main__':
    unittest.main()
