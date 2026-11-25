"""r5.26.2: the firmware's quiet servo table (ps5-fan-quiet.h, BSD) equals ps5pwr's backup table (ps5pwr-fancurve.c) byte for byte."""
import ctypes as C, os, random, subprocess, tempfile, unittest
from pathlib import Path
HERE = Path(__file__).resolve().parent
PWR = HERE.parent / 'drivers' / 'ps5pwr'
SRC = r'''
#include "ps5-fan-quiet.h"
#include "ps5pwr-fancurve.h"
void fw_prepare(unsigned char *q, const unsigned char *o) { ps5_fq_prepare(q, o); }
int fw_quiet(const unsigned char *t) { return ps5_fq_is_quiet(t); }
void pwr_prepare(unsigned char *q, const unsigned char *o) { ps5_fc_prepare_backup(q, o); }
int pwr_quiet(const unsigned char *t) { return ps5_fc_quiet_left(t); }
'''
class QuietTableTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(); w = Path(cls.tmp.name)
        (w / 't.c').write_text(SRC)
        subprocess.run(['gcc', '-shared', '-fPIC', '-O1', '-I' + str(HERE), '-I' + str(PWR), str(w / 't.c'),
                        str(PWR / 'ps5pwr-fancurve.c'), '-o', str(w / 't.so')], check=True)
        cls.lib = C.CDLL(str(w / 't.so'))
    def run_both(self, orig):
        a = (C.c_ubyte * 0x6ae)(); b = (C.c_ubyte * 0x6ae)(); o = (C.c_ubyte * 0x6ae)(*orig)
        self.lib.fw_prepare(a, o); self.lib.pwr_prepare(b, o)
        return bytes(a), bytes(b)
    def test_same_table_for_random_and_board_like_inputs(self):
        rnd = random.Random(2611)
        for n in range(200):
            orig = bytes(rnd.randrange(256) for _ in range(0x6ae)) if n % 2 else bytes([0] * 0x6ae)
            a, b = self.run_both(orig)
            self.assertEqual(a, b)
            q = (C.c_ubyte * 0x6ae)(*a)
            self.assertEqual(self.lib.fw_quiet(q), 1); self.assertEqual(self.lib.pwr_quiet(q), 1)
            o = (C.c_ubyte * 0x6ae)(*orig)
            self.assertEqual(self.lib.fw_quiet(o), self.lib.pwr_quiet(o))
    def test_header_is_bsd_and_free_of_gpl_includes(self):
        text = (HERE / 'ps5-fan-quiet.h').read_text()
        self.assertTrue(text.startswith('/* SPDX-License-Identifier: BSD-2-Clause-Patent'))
        self.assertNotIn('#include', text.split('#define PS5_FAN_QUIET_H_')[1])
        driver = (HERE / 'Ps5PlatformPkg/HdmiProbeDxe/HdmiProbeDxe.c').read_text()
        self.assertIn('#include "ps5-fan-quiet.h"', driver)
        self.assertNotIn('ps5pwr-fancurve', driver)


class GameOsTableOnEspTests(unittest.TestCase):
    def test_original_kept_and_used_when_the_quiet_table_survived(self):   # r5.26.4
        src = (HERE / 'Ps5BootWindows.c').read_text()
        body = src[src.index('STATIC VOID Ps5ApplyFanTarget(CONST CHAR8 *When,EFI_HANDLE Esp) {'):src.index('STATIC VOID Ps5InstallPStateSsdt(VOID) {')]
        self.assertIn('L"\\\\PSWloader\\\\fan-gameos.bin"', body)
        self.assertIn('!ps5_fq_is_quiet(Original))Flags|=0x101;', body)          # only a non-quiet file counts
        self.assertLess(body.index('if((Flags & 2) && Esp)'), body.index('if(Flags & 1) {'))   # file read before publishing
        self.assertIn("'ps5-fan-quiet.h'", (HERE / 'ps5_boot_manager.py').read_text())

if __name__ == '__main__':
    unittest.main()
