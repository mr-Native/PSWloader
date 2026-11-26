"""Verify the probe is before DXE handlers, and disabled builds are pristine."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import ps5_dxe_probe as probe

HERE = Path(__file__).resolve().parent
EDK = Path(os.environ.get('PS5_EDK2_ROOT', os.environ.get('PSW_WORK', '/opt/pswloader-work') + '/edk2'))


@unittest.skipUnless((EDK / probe.DXE_C).exists(), 'Pinned EDK source required')
class DxeProbeTests(unittest.TestCase):
    def test_actual_core_patch_restores_pristine_disabled_source(self):
        originals = {name: subprocess.check_output(['git', 'show', 'HEAD:' + name],
                     cwd=EDK, text=True) for name in (probe.DXE_C, probe.DXE_INF)}
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name in originals:
                (root / name).parent.mkdir(parents=True, exist_ok=True)
            encoder = (HERE / 'bootstrap-power.c').read_text()
            with patch.object(probe.subprocess, 'check_output',
                              side_effect=lambda argv, **kw: originals[argv[-1][5:]]):
                probe.prepare_dxe_power(root, True, encoder)
                source = (root / probe.DXE_C).read_text()
                self.assertEqual(source.count('ps5_power_report (0x704);'), 1)
                self.assertLess(source.index('ps5_power_report (0x704);'),
                                source.index('  Status = InitializeCpuExceptionHandlers'))
                self.assertLess(source.index('ps5_power_report (0x704);'),
                                source.index('  ProcessLibraryConstructorList'))
                self.assertEqual((root / probe.DXE_INF).read_text().count('DxeMain/Ps5DxePower.c'), 1)
                generated = (root / probe.DXE_C).with_name('Ps5DxePower.c')
                self.assertEqual(generated.read_text(), '#define PS5_POWER_EDK2\n' + encoder)
                subprocess.run(['gcc', '-Wall', '-Wextra', '-Werror', '-ffreestanding', '-fshort-wchar',
                    '-I' + str(EDK / 'MdePkg/Include'), '-I' + str(EDK / 'MdePkg/Include/X64'),
                    '-c', str(generated), '-o', str(root / 'probe.o')], check=True)
                probe.prepare_dxe_power(root, True, encoder, 'dxe-exceptions')
                later=(root / probe.DXE_C).read_text()
                self.assertNotIn('ps5_power_report (0x704);', later)
                self.assertLess(later.index('  Status = InitializeSeparateExceptionStacks'), later.index('ps5_power_report (0x705);'))
                self.assertLess(later.index('ps5_power_report (0x705);'), later.index('  InitializeDebugAgent ('))
                probe.prepare_dxe_power(root, True, encoder, 'dxe-memory')
                memory=(root / probe.DXE_C).read_text()
                self.assertNotIn('ps5_power_report (0x705);', memory)
                self.assertLess(memory.index('  CoreInitializeMemoryServices ('), memory.index('ps5_power_report (0x706);'))
                self.assertLess(memory.index('ps5_power_report (0x706);'), memory.index('  MemoryProfileInit (HobStart);'))
                probe.prepare_dxe_power(root, True, encoder, 'dxe-dispatch')
                dispatch=(root / probe.DXE_C).read_text()
                self.assertEqual(dispatch.count('ps5_power_report (0x707);'), 1)
                self.assertLess(dispatch.index('  ProcessLibraryConstructorList'), dispatch.index('ps5_power_report (0x707);'))
                self.assertLess(dispatch.index('ps5_power_report (0x707);'), dispatch.index('  CoreDispatcher ();'))
                with self.assertRaisesRegex(ValueError, 'Unknown'):probe.prepare_dxe_power(root, True, encoder, 'unknown')
                probe.prepare_dxe_power(root, False, encoder)
                for name, original in originals.items():
                    self.assertEqual((root / name).read_text(), original)
            with patch.object(probe.subprocess, 'check_output', return_value='changed source'):
                with self.assertRaisesRegex(RuntimeError, 'anchors'):
                    probe.prepare_dxe_power(root, True, encoder)
