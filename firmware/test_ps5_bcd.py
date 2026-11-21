"""r5.22 host test for ps5-bcd.h: testsigning (element 16000049) in real BCD stores. A store without the element gets
it; the result must open in hivex with every other key/value unchanged, and a second run finds it set. Stores that
have the element are left byte-identical; a dirty hive or a damaged checksum is refused. Fixtures (real BCDs from
the console) are private: testdata/bcd/*.bin; without them (public export) the test is skipped."""
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
# private fixtures: testdata/bcd here, or PSW_BCD_FIXTURES, or the maintainers' lab checkout next to this folder
_BCD = [Path(p) for p in (os.environ.get('PSW_BCD_FIXTURES', ''),) if p] + [HERE / 'testdata' / 'bcd', HERE.parent.parent / 'windows-native-lab' / 'testdata' / 'bcd']
FIX = sorted(next((d for d in _BCD if d.is_dir() and any(d.glob('*.bin'))), HERE / 'testdata' / 'bcd').glob('*.bin'))
TOOL = r'''
#include <stdio.h>
#include <stdlib.h>
#include "ps5-bcd.h"
int main(int c, char **v) { (void)c;
  FILE *f = fopen(v[1], "rb"); if (!f) return 90; fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  uint8_t *b = calloc(1, (size_t)n + 0x1000); if (fread(b, 1, (size_t)n, f) != (size_t)n) return 91; fclose(f);
  size_t out = 0; int r = ps5_bcd_enable_testsigning(b, (size_t)n, (size_t)n + 0x1000, &out);
  printf("%d\n", r);
  if (r == 1) { f = fopen(v[2], "wb"); fwrite(b, 1, out, f); fclose(f); }
  return 0;
}
'''


def hx(path, *cmds):
    return subprocess.run(['hivexsh', str(path)], input='\n'.join(cmds) + '\n', capture_output=True, text=True).stdout


def dump(path):
    """every key path and its values, recursively (hivexsh), for an order-independent comparison"""
    out, todo = [], ['']
    while todo:
        k = todo.pop()
        cd = ['cd ' + k] if k else []
        subs = [s for s in hx(path, *cd, 'ls').splitlines() if s]
        vals = sorted(v for v in hx(path, *cd, 'lsval').splitlines() if v)
        out.append((k, tuple(vals)))
        todo += [(k + '\\' if k else '\\') + s for s in subs]
    return sorted(out)


def default_obj(path):
    return hx(path, 'cd \\Objects\\{9dea862c-5cdd-4e70-acc1-f32b344d4795}\\Elements\\23000003', 'lsval').split('"')[3]


@unittest.skipUnless(FIX and shutil.which('hivexsh') and shutil.which('gcc'), 'private BCD fixtures / hivexsh / gcc missing')
class BcdTestsigningTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp())
        (cls.tmp / 't.c').write_text(TOOL)
        subprocess.run(['gcc', '-Wall', '-Wextra', '-Werror', '-I', str(HERE), '-o', str(cls.tmp / 't'), str(cls.tmp / 't.c')], check=True)

    def run_tool(self, src, dst):
        return int(subprocess.run([str(self.tmp / 't'), str(src), str(dst)], capture_output=True, text=True, check=True).stdout)

    def element(self, path):
        d = default_obj(path)
        return hx(path, f'cd \\Objects\\{d}\\Elements\\16000049', 'lsval').strip()

    def test_add_where_missing_and_idempotent(self):
        for fix in FIX:
            with self.subTest(fix=fix.name):
                src = self.tmp / ('in-' + fix.name)
                shutil.copy(fix, src)
                if self.element(src):   # has testsigning: strip it with hivex to get a store without it
                    d = default_obj(src)
                    subprocess.run(['hivexsh', '-w', str(src)], input=f'cd \\Objects\\{d}\\Elements\\16000049\ndel\ncommit\n',
                                   text=True, check=True, capture_output=True)
                self.assertEqual(self.element(src), '')
                before = dump(src)
                out = self.tmp / ('out-' + fix.name)
                self.assertEqual(self.run_tool(src, out), 1)
                self.assertEqual(self.element(out), '"Element"=hex(3):01')
                after = dump(out)
                added = [k for k in after if k not in before]
                self.assertEqual(len(added), 1)
                self.assertTrue(added[0][0].endswith('\\Elements\\16000049'))
                self.assertEqual([k for k in before if k not in after], [])
                out2 = self.tmp / ('out2-' + fix.name)
                self.assertEqual(self.run_tool(out, out2), 0)        # already set: nothing written

    def test_refuse_dirty_and_damaged(self):
        fix = FIX[0]
        b = bytearray(fix.read_bytes())
        b[8] ^= 1                                                   # sequence numbers differ (checksum fixed below)
        x = 0
        for i in range(0, 508, 4):
            x ^= int.from_bytes(b[i:i + 4], 'little')
        b[0x1FC:0x200] = x.to_bytes(4, 'little')
        (self.tmp / 'dirty.bin').write_bytes(b)
        self.assertEqual(self.run_tool(self.tmp / 'dirty.bin', self.tmp / 'x'), -2)
        b = bytearray(fix.read_bytes())
        b[0x30] ^= 0xFF                                             # checksum no longer matches
        (self.tmp / 'bad.bin').write_bytes(b)
        self.assertEqual(self.run_tool(self.tmp / 'bad.bin', self.tmp / 'x'), -1)


if __name__ == '__main__':
    unittest.main()
