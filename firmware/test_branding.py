"""r5.24 gate (user: one visible name "PSWloader" / "PSWloader v1.0"): no old project names in strings the user can see
in the firmware - "1360uefi", "native UEFI", "beta", "PS5 native". Checked: wide string literals (L"...", the UEFI
console/HII/system-table strings) in the firmware C sources, every HII string (.uni), the LVGL chrome PCDs and firmware
PCDs set by build-uefi.py, and the C string literals the LVGL patch scripts insert (DEBUG log lines excepted - the log
is not shown on screen). Comments and DEBUG text may keep the history."""
import re
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
BAD = re.compile(r'(?i)1360uefi|native\s+uefi|\bbeta\b|ps5\s+native')
WIDE = re.compile(r'L"((?:[^"\\]|\\.)*)"')
NARROW = re.compile(r'"((?:[^"\\]|\\.)*)"')


def firmware_c():
    for p in sorted(list(HERE.glob('*.c')) + list(HERE.glob('*.h')) + list((HERE / 'Ps5PlatformPkg').rglob('*.[ch]'))):
        if p.name.startswith('test_'):
            continue
        yield p


class BrandingTests(unittest.TestCase):
    def hits(self, p, lines, rx):
        out = []
        for n, line in enumerate(lines, 1):
            if 'DEBUG' in line:
                continue
            for m in rx.finditer(line):
                if BAD.search(m.group(1)):
                    out.append(f'{p.relative_to(HERE)}:{n}: {m.group(0)[:90]}')
        return out

    def test_wide_strings(self):
        bad = []
        for p in firmware_c():
            bad += self.hits(p, p.read_text(errors='replace').splitlines(), WIDE)
        self.assertEqual(bad, [])

    def test_hii_strings(self):
        bad = []
        for p in sorted((HERE / 'Ps5PlatformPkg').rglob('*.uni')):
            raw = p.read_bytes()
            text = raw.decode('utf-16') if raw[:2] in (b'\xff\xfe', b'\xfe\xff') else raw.decode('utf-8', errors='replace')
            bad += self.hits(p, text.splitlines(), NARROW)
        self.assertEqual(bad, [])

    def test_pcds_and_lvgl_patches(self):
        bad = []
        b = HERE / 'build-uefi.py'
        bad += self.hits(b, [l for l in b.read_text().splitlines() if 'Pcd' in l and '|' in l], NARROW)
        for p in sorted(HERE.glob('lvgl-*.py')):
            bad += self.hits(p, p.read_text().splitlines(), NARROW)
        self.assertEqual(bad, [])

    def test_pattern(self):
        for s in ('1360uefi Boot Manager', 'v1.0-beta  |  PS5 native UEFI', 'Native UEFI Payload - PS5'):
            self.assertTrue(BAD.search(s), s)
        for s in ('UEFI Boot Manager', 'PSWloader v1.0', 'AMD Oberon (PS5 Custom GPU)', 'alphabet'):
            self.assertFalse(BAD.search(s), s)


if __name__ == '__main__':
    unittest.main()
