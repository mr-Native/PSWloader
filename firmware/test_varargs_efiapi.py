"""r5.20.5 (17.11.2026): every firmware function with '...' must be EFIAPI. With GCC X64 the EDK2 VA_LIST is
__builtin_ms_va_list; VA_START in a System V function reads the arguments from the wrong registers (r5.20.1-r5.20.4
diag=once hang: %a dereferenced garbage). Scans the UEFI sources of this tree."""
import re
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
SKIP = ('research/', 'drivers/', 'tools/', 'installer/', 'public-release/', 'nagi-port/', 'ops/', 'docs/')
DEF = re.compile(r'^(?:STATIC\s+|static\s+)?(?:inline\s+)?[A-Za-z_][\w\s\*]*?\b(\w+)\s*\(([^;{)]*\.\.\.)\)\s*[;{]?', re.M)


def firmware_sources():
    for p in sorted(HERE.rglob('*')):
        rel = p.relative_to(HERE).as_posix()
        if p.suffix in ('.c', '.h') and not rel.startswith(SKIP) and not p.name.startswith('test_'):
            yield p, rel


class VarargsEfiapiTests(unittest.TestCase):
    def test_variadic_functions_are_efiapi(self):
        bad = []
        for p, rel in firmware_sources():
            text = p.read_text(errors='replace')
            if 'VA_START' not in text and '...)' not in text:
                continue
            for m in DEF.finditer(text):
                head = text[m.start():m.end()]
                if 'typedef' in head or '#define' in head:
                    continue
                uses_va = re.search(r'\bVA_START\b', text[m.end():m.end() + 2000]) is not None
                if uses_va and 'EFIAPI' not in head:
                    bad.append(f'{rel}: {m.group(1)}')
        self.assertEqual(bad, [], 'variadic functions using VA_START without EFIAPI')

    def test_known_offenders_fixed(self):
        self.assertIn('STATIC VOID EFIAPI Ps5DiagSetF(', (HERE / 'ps5-diag-uefi.h').read_text())
        self.assertIn('STATIC VOID EFIAPI Ps5DiagMark(CONST CHAR8 *Fmt,...)\n{', (HERE / 'Ps5BootWindows.c').read_text())


if __name__ == '__main__':
    unittest.main()
