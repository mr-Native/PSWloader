#!/usr/bin/env python3
"""v1.0: hive-edit.py round trip on a real (clean) SYSTEM hive copy; skips when absent."""
import os
import importlib.util
import struct
import tempfile
import unittest
from pathlib import Path

HIVE = Path(os.environ.get('PSW_WORK', '/opt/pswloader-work') + '/hive-v308/SYSTEM.v307.orig')
DIRTY = Path(os.environ.get('PSW_WORK', '/opt/pswloader-work') + '/hive-v308/SYSTEM.after308')
spec = importlib.util.spec_from_file_location('he', Path(__file__).resolve().parent / 'hive-edit.py')
he = importlib.util.module_from_spec(spec)
try:
    spec.loader.exec_module(he)
    import hivex
except ImportError:  # pragma: no cover
    hivex = None


@unittest.skipUnless(hivex and HIVE.exists(), 'hivex or sample hive missing')
class HiveEdit(unittest.TestCase):
    def test_set_and_verify(self):
        out = Path(tempfile.mkdtemp()) / 'h'
        self.assertEqual(he.main([str(HIVE), str(out), 'set', 'ControlSet001\\Services\\t', 'Start', 'dword', '3',
                                  'set', 'ControlSet001\\Services\\t', 'N', 'sz', 'x']), 0)
        h = hivex.Hivex(str(out))
        self.assertEqual(he.get_value(h, 'ControlSet001\\Services\\t', 'Start'), (4, struct.pack('<I', 3)))
        self.assertEqual(HIVE.read_bytes()[:4], b'regf')

    @unittest.skipUnless(DIRTY.exists(), 'dirty sample missing')
    def test_refuses_dirty(self):
        with self.assertRaises(SystemExit):
            he.main([str(DIRTY), '/tmp/never', 'set', 'X', 'Y', 'dword', '1'])

    def test_refuses_in_place(self):
        with self.assertRaises(SystemExit):
            he.main([str(HIVE), str(HIVE), 'set', 'X', 'Y', 'dword', '1'])

    def test_check_rejects_bad_checksum_even_with_equal_sequences(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'bad-checksum'
            data = bytearray(HIVE.read_bytes())
            data[508] ^= 1
            path.write_bytes(data)
            with self.assertRaisesRegex(SystemExit, 'checksum mismatch'):
                he.main([str(path), '-', 'check'])

    def test_check_rejects_broken_descendant_in_clean_hive(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'bad-values'
            he.main([str(HIVE), str(path), 'set', 'ControlSet001\\Services\\validation-fixture', 'Start', 'dword', '3'])
            h = hivex.Hivex(str(path))
            node = he.walk(h, 'ControlSet001\\Services\\validation-fixture', False)
            data = bytearray(path.read_bytes())
            struct.pack_into('<I', data, node + 44, 0xfffffff0)
            path.write_bytes(data)
            with self.assertRaisesRegex(SystemExit, 'structure validation failed'):
                he.main([str(path), '-', 'check'])


if __name__ == '__main__':
    unittest.main()
