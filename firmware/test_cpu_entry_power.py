"""Pinned CPU-entry probe generation tests; no firmware execution."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch
from test_log_entry_power import uefi,bundle
import json
EDK=Path(os.environ.get('PS5_EDK2_ROOT',os.environ.get('PSW_WORK', '/opt/pswloader-work') + '/edk2'))

class CpuPowerStageTests(unittest.TestCase):
    def test_cpu_stage_is_exclusive_and_requires_power(self):
        self.assertEqual(uefi.power_targets(True,'cpu-entry',False),(False,False,False,True))
        with self.assertRaises(ValueError):uefi.power_targets(False,'cpu-entry',True)
        for stage in uefi.POWER_STAGES:
            targets=uefi.power_targets(True,stage,True)
            self.assertEqual(sum(targets),0 if stage=='exception-only' else 1)
            if stage not in ('cpu-entry','cpu-before-mp','cpu-before-gcd','cpu-after-paging','cpu-after-gdt','cpu-after-idt'):self.assertFalse(targets[3])
    def test_bundle_accepts_bound_cpu_stage(self):
        with tempfile.TemporaryDirectory() as work:
            root=Path(work)
            (root/'build.json').write_text(json.dumps({'sha256':'bound','entry_power_telemetry':True,
                'entry_power_stage':'cpu-entry','log_export_enabled':False}))
            result=bundle.telemetry_config({'power_telemetry':True,'defer_success_report':True},
                {'source_sha256':'bound'},root)
            self.assertEqual(result['entry_power_stage'],'cpu-entry')

@unittest.skipUnless((EDK/'UefiCpuPkg/CpuDxe/CpuDxe.c').exists(),'EDK required')
class CpuPowerGenerationTests(unittest.TestCase):
    def test_pinned_single_probe_before_page_initialization_and_clean_reset(self):
        originals={name:subprocess.check_output(['git','show','HEAD:'+uefi.CPU_DIRECTORY+'/'+name],
            cwd=EDK,text=True) for name in ('CpuDxe.c','CpuDxe.inf')}
        with tempfile.TemporaryDirectory() as work:
            root=Path(work);directory=root/uefi.CPU_DIRECTORY;directory.mkdir(parents=True)
            def pristine(argv,**kwargs):return originals[argv[-1].rsplit('/',1)[-1]]
            with patch.object(uefi.subprocess,'check_output',side_effect=pristine):
                uefi.prepare_cpu_power(root,True)
                source=(directory/'CpuDxe.c').read_text();inf=(directory/'CpuDxe.inf').read_text()
                self.assertEqual(source.count('  ps5_power_report (0x709);'),1)
                self.assertLess(source.index('InitializeCpu ('),source.index('  ps5_power_report (0x709);'))
                self.assertLess(source.index('  ps5_power_report (0x709);'),source.index('  InitializePageTableLib ();'))
                self.assertEqual(inf.count('\n  Ps5CpuEntryPower.c\n'),1)
                encoder=directory/'Ps5CpuEntryPower.c'
                self.assertEqual(encoder.read_text(),'#define PS5_POWER_EDK2\n'+(uefi.HERE/'bootstrap-power.c').read_text())
                subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-ffreestanding','-fshort-wchar',
                    '-I'+str(EDK/'MdePkg/Include'),'-I'+str(EDK/'MdePkg/Include/X64'),
                    '-c',str(encoder),'-o',str(root/'cpu-power.o')],check=True)
                (directory/'CpuDxe.c').write_text('stale')
                uefi.prepare_cpu_power(root,True)
                self.assertEqual((directory/'CpuDxe.c').read_text(),source)
                uefi.prepare_cpu_power(root,True,'cpu-before-mp')
                later=(directory/'CpuDxe.c').read_text()
                self.assertNotIn('ps5_power_report (0x709);',later)
                self.assertLess(later.index('  AddLocalApicMemorySpace ('),later.index('ps5_power_report (0x70a);'))
                self.assertLess(later.index('ps5_power_report (0x70a);'),later.index('  InitializeMpSupport ();'))
                self.assertEqual(uefi.power_targets(True,'cpu-before-mp',True),(False,False,False,True))
                uefi.prepare_cpu_power(root,True,'cpu-before-gcd')
                middle=(directory/'CpuDxe.c').read_text()
                self.assertNotIn('ps5_power_report (0x70a);',middle)
                self.assertLess(middle.index('  InstallEfiMemoryAttributeProtocol ('),middle.index('ps5_power_report (0x70b);'))
                self.assertLess(middle.index('ps5_power_report (0x70b);'),middle.index('  RefreshGcdMemoryAttributes ();'))
                self.assertEqual(uefi.power_targets(True,'cpu-before-gcd',True),(False,False,False,True))
                uefi.prepare_cpu_power(root,True,'cpu-after-paging')
                paging=(directory/'CpuDxe.c').read_text()
                self.assertNotIn('ps5_power_report (0x70b);',paging)
                self.assertLess(paging.index('  InitializePageTableLib ();'),paging.index('ps5_power_report (0x70c);'))
                self.assertLess(paging.index('ps5_power_report (0x70c);'),paging.index('  InitializeFloatingPointUnits ();'))
                self.assertEqual(uefi.power_targets(True,'cpu-after-paging',True),(False,False,False,True))
                uefi.prepare_cpu_power(root,True,'cpu-after-gdt')
                gdt=(directory/'CpuDxe.c').read_text()
                self.assertNotIn('ps5_power_report (0x70c);',gdt)
                self.assertLess(gdt.index('  InitGlobalDescriptorTable ();'),gdt.index('ps5_power_report (0x70d);'))
                self.assertLess(gdt.index('ps5_power_report (0x70d);'),gdt.index('  InitInterruptDescriptorTable ();'))
                self.assertEqual(uefi.power_targets(True,'cpu-after-gdt',True),(False,False,False,True))
                uefi.prepare_cpu_power(root,True,'cpu-after-idt')
                idt=(directory/'CpuDxe.c').read_text()
                self.assertNotIn('ps5_power_report (0x70d);',idt)
                self.assertLess(idt.index('  InitInterruptDescriptorTable ();'),idt.index('ps5_power_report (0x70e);'))
                self.assertLess(idt.index('ps5_power_report (0x70e);'),idt.index('  Status = gBS->InstallMultipleProtocolInterfaces ('))
                self.assertEqual(uefi.power_targets(True,'cpu-after-idt',True),(False,False,False,True))
                uefi.prepare_cpu_power(root,False)
                self.assertFalse(encoder.exists())
                for name in originals:self.assertEqual((directory/name).read_text(),originals[name])
                bad=dict(originals);bad['CpuDxe.c']=bad['CpuDxe.c'].replace('  InitializePageTableLib ();','')
                with patch.object(uefi.subprocess,'check_output',side_effect=lambda argv,**kw:bad[argv[-1].rsplit('/',1)[-1]]):
                    with self.assertRaisesRegex(RuntimeError,'anchors'):uefi.prepare_cpu_power(root,True)

if __name__=='__main__':unittest.main()


class ImageProtectionComparisonTests(unittest.TestCase):
    def test_default_preserves_policy_and_comparison_is_explicit(self):
        self.assertEqual(uefi.image_protection_comparison(False,False,'entry'),[])
        self.assertEqual(uefi.image_protection_comparison(True,True,'log-entry'),
            ['-p','gEfiMdeModulePkgTokenSpaceGuid.PcdImageProtectionPolicy=0'])
        for enabled,stage in ((False,'log-entry'),(True,'cpu-entry')):
            with self.assertRaisesRegex(ValueError,'requires'):uefi.image_protection_comparison(True,enabled,stage)

    def test_bundle_carries_bound_experimental_policy(self):
        with tempfile.TemporaryDirectory() as work:
            root=Path(work)
            d={'sha256':'bound','entry_power_telemetry':True,'entry_power_stage':'log-entry',
               'log_export_enabled':True,'experimental_no_image_protection':True}
            (root/'build.json').write_text(json.dumps(d))
            b={'power_telemetry':True,'defer_success_report':True}
            self.assertTrue(bundle.telemetry_config(b,{'source_sha256':'bound'},root)['experimental_no_image_protection'])
            d['entry_power_stage']='cpu-entry';(root/'build.json').write_text(json.dumps(d))
            with self.assertRaisesRegex(ValueError,'comparison'):bundle.telemetry_config(b,{'source_sha256':'bound'},root)
