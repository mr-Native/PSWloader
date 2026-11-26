"""Verify bounded payload entry telemetry generation; never launch firmware."""
import importlib.util
import contextlib
import io
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import sys
from unittest.mock import patch

HERE=Path(__file__).resolve().parent
EDK=Path(os.environ.get('PS5_EDK2_ROOT',os.environ.get('PSW_WORK', '/opt/pswloader-work') + '/edk2'))

def load(name,filename):
    spec=importlib.util.spec_from_file_location(name,HERE/filename)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    return module

uefi=load('uefi_entry_power','build-uefi.py')
bootstrap=load('bootstrap_entry_power','build-bootstrap.py')


class ShortMarkerPlacementTests(unittest.TestCase):
    # Only pinned generator anchors: no compiler, firmware build or hardware access.
    source = ('extern VOID  *mHobList;\n'
              '  mHobList = (VOID *)BootloaderParameter;\n'
              '  ProcessLibraryConstructorList ();\n'
              '  DEBUG ((DEBUG_INFO, "Entering Universal Payload...\\n"));\n'
              '  Status = BuildHobs (BootloaderParameter, &DxeFv);\n'
              '  ASSERT_EFI_ERROR (Status);\n'
              '  if (GetFirstGuidHob (&gEfiMemoryTypeInformationGuid) == NULL) {\n'
              '  }\n'
              '  FixUpPcdDatabase (DxeFv);\n'
              '  Status = UniversalLoadDxeCore (DxeFv, &DxeCoreEntryPoint);\n'
              '  ASSERT_EFI_ERROR (Status);\n'
              '  Hob.HandoffInformationTable = (EFI_HOB_HANDOFF_INFO_TABLE *)GetFirstHob (EFI_HOB_TYPE_HANDOFF);\n')
    inf = '[Sources]\n  UniversalPayloadEntry.c\n'

    def generate(self, root, placement, source=None, enabled=True, short=True):
        originals = {'UniversalPayloadEntry.c': self.source if source is None else source,
                     'UniversalPayloadEntry.inf': self.inf}
        with patch.object(uefi.subprocess, 'check_output', side_effect=
                          lambda argv, **kw: originals[argv[-1].rsplit('/', 1)[-1]]):
            uefi.prepare_entry_power(root, enabled, short_marker=short,
                                     short_marker_stage=placement)

    def test_single_call_moves_idempotently_and_encoder_is_unchanged(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);entry=root/uefi.ENTRY_DIRECTORY;entry.mkdir(parents=True)
            snapshots={}
            encoder=None
            for placement in ('entry', 'entry', 'post-hobs', 'post-hobs',
                              'post-dxe-load', 'post-dxe-load', 'post-hobs', 'entry'):
                self.generate(root, placement)
                generated=(entry/'UniversalPayloadEntry.c').read_text()
                self.assertEqual(generated.count('  ps5_short_entry_marker ();'), 1)
                call=generated.index('  ps5_short_entry_marker ();')
                if placement=='entry':
                    self.assertLess(call, generated.index('  mHobList ='))
                elif placement=='post-hobs':
                    self.assertLess(generated.index('  Status = BuildHobs ('), call)
                    self.assertLess(generated.index('  ASSERT_EFI_ERROR (Status);'), call)
                    self.assertLess(generated.index('  if (EFI_ERROR (Status)) {'), call)
                    self.assertLess(call, generated.index('  if (GetFirstGuidHob ('))
                else:
                    self.assertLess(generated.index('  Status = UniversalLoadDxeCore ('),call)
                    self.assertLess(call,generated.index('  Hob.HandoffInformationTable ='))
                    self.assertEqual(generated.count('  if (EFI_ERROR (Status)) {'),2)
                self.assertNotIn('ps5_power_report (0x701)', generated)
                self.assertEqual((entry/'UniversalPayloadEntry.inf').read_text().count('  Ps5EntryPower.c'), 1)
                current_encoder=(entry/'Ps5EntryPower.c').read_bytes()
                if encoder is not None:self.assertEqual(current_encoder, encoder)
                encoder=current_encoder
                if placement in snapshots:self.assertEqual(generated, snapshots[placement])
                snapshots[placement]=generated
            self.generate(root, 'entry', enabled=False, short=False)
            self.assertEqual((entry/'UniversalPayloadEntry.c').read_text(), self.source)
            self.assertEqual((entry/'UniversalPayloadEntry.inf').read_text(), self.inf)

    def test_post_hobs_has_non_assert_error_halt_before_exactly_one_marker(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);entry=root/uefi.ENTRY_DIRECTORY;entry.mkdir(parents=True)
            self.generate(root, 'post-hobs')
            generated=(entry/'UniversalPayloadEntry.c').read_text()
            expected=('  Status = BuildHobs (BootloaderParameter, &DxeFv);\n'
                      '  ASSERT_EFI_ERROR (Status);\n'
                      '  // Diagnostic marker requires successful HOB construction even with ASSERT disabled.\n'
                      '  if (EFI_ERROR (Status)) {\n'
                      '    __asm__ volatile("1: cli; hlt; jmp 1b");\n'
                      '  }\n'
                      '  ps5_short_entry_marker ();\n'
                      '  if (GetFirstGuidHob (&gEfiMemoryTypeInformationGuid) == NULL) {')
            self.assertIn(expected, generated)
            self.assertEqual(generated.count('  ps5_short_entry_marker ();'),1)
            # The explicit branch/halt survives removal of ASSERT, independent of
            # DebugLib assertion-mask settings; binary review is still required.
            without_assert=generated.replace('  ASSERT_EFI_ERROR (Status);\n','')
            self.assertIn('  if (EFI_ERROR (Status)) {\n'
                          '    __asm__ volatile("1: cli; hlt; jmp 1b");\n'
                          '  }\n  ps5_short_entry_marker ();',without_assert)
            self.generate(root, 'entry')
            self.assertNotIn('Diagnostic marker requires successful HOB construction',
                             (entry/'UniversalPayloadEntry.c').read_text())

    def test_post_dxe_load_preserves_both_explicit_error_halts_and_one_marker(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);entry=root/uefi.ENTRY_DIRECTORY;entry.mkdir(parents=True)
            self.generate(root,'post-dxe-load')
            generated=(entry/'UniversalPayloadEntry.c').read_text()
            self.assertEqual(generated.count('  ps5_short_entry_marker ();'),1)
            self.assertEqual(generated.count('  if (EFI_ERROR (Status)) {'),2)
            gate='  if (EFI_ERROR (Status)) {\n    __asm__ volatile("1: cli; hlt; jmp 1b");\n  }'
            hobs=generated.index('  Status = BuildHobs (')
            hobs_gate=generated.index(gate,hobs)
            load=generated.index('  Status = UniversalLoadDxeCore (')
            load_gate=generated.index(gate,load)
            call=generated.index('  ps5_short_entry_marker ();')
            self.assertLess(hobs,hobs_gate);self.assertLess(hobs_gate,load)
            self.assertLess(load,load_gate);self.assertLess(load_gate,call)
            self.assertLess(call,generated.index('  Hob.HandoffInformationTable ='))
            without_assert=generated.replace('  ASSERT_EFI_ERROR (Status);\n','')
            self.assertEqual(without_assert.count(gate),2)
            self.generate(root,'post-hobs')
            reverted=(entry/'UniversalPayloadEntry.c').read_text()
            self.assertEqual(reverted.count(gate),1)
            self.assertNotIn('successful DXE loading',reverted)

    def test_missing_ambiguous_reordered_dxe_load_anchors_refused_before_write(self):
        load='  Status = UniversalLoadDxeCore (DxeFv, &DxeCoreEntryPoint);'
        assertion='  ASSERT_EFI_ERROR (Status);'
        checked=load+'\n'+assertion
        handoff='  Hob.HandoffInformationTable = (EFI_HOB_HANDOFF_INFO_TABLE *)GetFirstHob (EFI_HOB_TYPE_HANDOFF);'
        invalid=(self.source.replace(load,''),self.source+load,
                 self.source.replace(checked,load),
                 self.source.replace(checked,checked+'\n'+assertion),
                 self.source.replace(checked,assertion+'\n'+load),
                 self.source.replace(handoff,''),self.source+handoff,
                 self.source.replace(checked,'DXE_PLACEHOLDER').replace(handoff,checked).replace('DXE_PLACEHOLDER',handoff))
        for source in invalid:
            with self.subTest(source=source),tempfile.TemporaryDirectory() as directory:
                root=Path(directory);entry=root/uefi.ENTRY_DIRECTORY;entry.mkdir(parents=True)
                with self.assertRaisesRegex(RuntimeError,'anchors'):
                    self.generate(root,'post-dxe-load',source)
                self.assertEqual(list(entry.iterdir()),[])

    def test_missing_ambiguous_or_reordered_hob_anchors_refused_before_write(self):
        build='  Status = BuildHobs (BootloaderParameter, &DxeFv);'
        checked=build+'\n  ASSERT_EFI_ERROR (Status);'
        assertion='  ASSERT_EFI_ERROR (Status);'
        next_hob='  if (GetFirstGuidHob (&gEfiMemoryTypeInformationGuid) == NULL) {'
        invalid=(self.source.replace(build,''),self.source+build,
                 self.source.replace(assertion,''),
                 self.source.replace(assertion,assertion+'\n'+assertion),
                 self.source.replace(checked,assertion+'\n'+build),
                 self.source.replace(next_hob,''),self.source+next_hob,
                 self.source.replace(checked,'HOB_PLACEHOLDER').replace(next_hob,checked).replace('HOB_PLACEHOLDER',next_hob))
        for source in invalid:
            with self.subTest(source=source), tempfile.TemporaryDirectory() as directory:
                root=Path(directory);entry=root/uefi.ENTRY_DIRECTORY;entry.mkdir(parents=True)
                with self.assertRaisesRegex(RuntimeError,'anchors'):
                    self.generate(root,'post-hobs',source)
                self.assertEqual(list(entry.iterdir()),[])

    def test_other_later_status_assertions_do_not_make_hob_anchor_ambiguous(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);entry=root/uefi.ENTRY_DIRECTORY;entry.mkdir(parents=True)
            self.generate(root,'post-hobs',self.source+'  ASSERT_EFI_ERROR (Status);\n')
            self.assertEqual((entry/'UniversalPayloadEntry.c').read_text().count('  ps5_short_entry_marker ();'),1)

    def test_direct_api_stage_guards(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            for stage,short in (('unknown',True), ('post-hobs',False), ('post-dxe-load',False)):
                with self.subTest(stage=stage), self.assertRaises(ValueError):
                    self.generate(root, stage, short=short)

    def test_manifest_preserves_entry_and_names_actual_post_hobs_stage(self):
        expected=dict(schema=1, pattern='001110010101', slot_seconds=4,
                      nominal_duration_seconds=48, terminal=True,
                      stage='UniversalPayloadEntry', hardware_verified=False)
        self.assertEqual(uefi.short_marker_manifest('entry'), expected)
        expected['stage']='UniversalPayloadEntry/post-hobs'
        self.assertEqual(uefi.short_marker_manifest('post-hobs'), expected)
        expected['stage']='UniversalPayloadEntry/post-dxe-load'
        self.assertEqual(uefi.short_marker_manifest('post-dxe-load'), expected)
        with self.assertRaises(ValueError):uefi.short_marker_manifest('unknown')

    def test_cli_rejects_explicit_placement_without_opt_in_before_git_or_output(self):
        invalid=(['--entry-short-marker-stage','entry'],
                 ['--entry-short-marker-stage','post-hobs'],
                 ['--entry-short-marker-stage','post-dxe-load'],
                 ['--entry-short-marker','--entry-short-marker-stage','post-hobs'],
                 ['--entry-short-marker','--entry-power-telemetry','--entry-power-stage','dxe-ready',
                  '--entry-short-marker-stage','post-hobs'])
        for flags in invalid:
            with self.subTest(flags=flags), tempfile.TemporaryDirectory() as directory:
                output=Path(directory)/'not-created'
                argv=['build-uefi.py','--edk2',directory,'--output',str(output),*flags]
                with patch.object(sys,'argv',argv), patch.object(uefi.subprocess,'check_output') as git, \
                        contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
                    uefi.main()
                self.assertEqual(error.exception.code,2);git.assert_not_called()
                self.assertFalse(output.exists())

    def test_cli_accepts_default_and_explicit_marker_placements_without_building(self):
        class StopBeforeGit(Exception):pass
        for placement in (None,'entry','post-hobs','post-dxe-load'):
            with self.subTest(placement=placement), tempfile.TemporaryDirectory() as directory:
                output=Path(directory)/'not-created'
                argv=['build-uefi.py','--edk2',directory,'--output',str(output),
                      '--entry-short-marker','--entry-power-telemetry']
                if placement is not None:argv+=['--entry-short-marker-stage',placement]
                with patch.object(sys,'argv',argv), \
                        patch.object(uefi.subprocess,'check_output',side_effect=StopBeforeGit), \
                        self.assertRaises(StopBeforeGit):
                    uefi.main()
                self.assertFalse(output.exists())


class EntryPowerTests(unittest.TestCase):
    def test_deferred_success_requires_telemetry(self):
        with tempfile.TemporaryDirectory() as directory:
            output=Path(directory)/'not-created'
            with self.assertRaisesRegex(ValueError,'requires power telemetry'):
                bootstrap.build(output,defer_success_report=True)
            self.assertFalse(output.exists())

    def test_actual_deferred_bootstrap_build_preserves_failure_encoder(self):
        with tempfile.TemporaryDirectory() as directory:
            result=bootstrap.build(directory,power_telemetry=True,defer_success_report=True)
            self.assertTrue(result['power_telemetry']);self.assertTrue(result['defer_success_report'])
            # The compiled ELF retains failure reporting and calibration symbols.
            # build() itself verifies no unresolved relocations/symbols.
            self.assertGreater((Path(directory)/'bootstrap-body.bin').stat().st_size,0)
        source=(HERE/'bootstrap.c').read_text()
        self.assertEqual(source.count('#ifndef PS5_DEFER_SUCCESS_REPORT\n    ps5_power_report(3);\n#endif'),1)
        self.assertIn('    ps5_power_report(code);',source)
        self.assertIn('    ps5_power_calibrate();',source)

    @unittest.skipUnless((EDK/'UefiPayloadPkg/UefiPayloadEntry/UniversalPayloadEntry.c').exists(),'EDK sources required')
    def test_pinned_entry_patch_and_clean_regeneration(self):
        originals={name:subprocess.check_output(['git','show','HEAD:'+uefi.ENTRY_DIRECTORY+'/'+name],
                    cwd=EDK,text=True) for name in ('UniversalPayloadEntry.c','UniversalPayloadEntry.inf')}
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);entry=root/uefi.ENTRY_DIRECTORY;entry.mkdir(parents=True)
            def original(argv,**kwargs):return originals[argv[-1].rsplit('/',1)[-1]]
            with patch.object(uefi.subprocess,'check_output',side_effect=original):
                uefi.prepare_entry_power(root,True)
                source=(entry/'UniversalPayloadEntry.c').read_text()
                self.assertEqual(source.count('ps5_power_report (0x701);'),1)
                self.assertLess(source.index('ps5_power_report (0x701);'),source.index('  mHobList = (VOID *)BootloaderParameter;'))
                self.assertLess(source.index('ps5_power_report (0x701);'),source.index('  ProcessLibraryConstructorList ();'))
                self.assertEqual((entry/'UniversalPayloadEntry.inf').read_text().count('  Ps5EntryPower.c'),1)
                encoder=entry/'Ps5EntryPower.c'
                self.assertEqual(encoder.read_text(),'#define PS5_POWER_EDK2\n'+(HERE/'bootstrap-power.c').read_text())
                subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-ffreestanding','-fshort-wchar',
                    '-I'+str(EDK/'MdePkg/Include'),'-I'+str(EDK/'MdePkg/Include/X64'),
                    '-c',str(encoder),'-o',str(root/'entry-power.o')],check=True)
                uefi.prepare_entry_power(root,True,'dxe-ready')
                deeper=(entry/'UniversalPayloadEntry.c').read_text()
                self.assertNotIn('ps5_power_report (0x701);',deeper)
                self.assertLess(deeper.index('  Status = UniversalLoadDxeCore'),deeper.index('ps5_power_report (0x703);'))
                self.assertLess(deeper.index('ps5_power_report (0x703);'),deeper.index('  HandOffToDxeCore (DxeCoreEntryPoint, Hob);'))
                with self.assertRaisesRegex(ValueError,'Unknown'):uefi.prepare_entry_power(root,True,'unknown')
                uefi.prepare_entry_power(root,False)
                for name,expected in originals.items():self.assertEqual((entry/name).read_text(),expected)
                bad=dict(originals);bad['UniversalPayloadEntry.c']='no pinned anchors'
                with patch.object(uefi.subprocess,'check_output',side_effect=lambda argv,**kw:bad[argv[-1].rsplit('/',1)[-1]]):
                    with self.assertRaisesRegex(RuntimeError,'anchors'):uefi.prepare_entry_power(root,True)

    @unittest.skipUnless((EDK/'UefiPayloadPkg/UefiPayloadEntry/UniversalPayloadEntry.c').exists(),'EDK sources required')
    def test_short_marker_is_terminal_opt_in_and_normal_generation_recovers(self):
        originals={name:subprocess.check_output(['git','show','HEAD:'+uefi.ENTRY_DIRECTORY+'/'+name],
                    cwd=EDK,text=True) for name in ('UniversalPayloadEntry.c','UniversalPayloadEntry.inf')}
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);entry=root/uefi.ENTRY_DIRECTORY;entry.mkdir(parents=True)
            with patch.object(uefi.subprocess,'check_output',side_effect=lambda argv,**kw: originals[argv[-1].rsplit('/',1)[-1]]):
                for enabled,stage in ((False,'entry'),(True,'dxe-ready')):
                    with self.assertRaisesRegex(ValueError,'Short marker'):
                        uefi.prepare_entry_power(root,enabled,stage,short_marker=True)
                uefi.prepare_entry_power(root,True,short_marker=True)
                source=(entry/'UniversalPayloadEntry.c').read_text()
                self.assertEqual(source.count('  ps5_short_entry_marker ();'),1)
                self.assertNotIn('ps5_power_report (0x701);',source)
                self.assertLess(source.index('  ps5_short_entry_marker ();'),source.index('  mHobList ='))
                encoder=entry/'Ps5EntryPower.c'
                self.assertIn('__asm__ volatile("1: cli; hlt; jmp 1b");',encoder.read_text())
                # Execute the generated slot sequence in a host harness. Escape from
                # the twelfth slot before privileged instructions can execute.
                harness=root/'marker-host.c'
                harness.write_text('#include <setjmp.h>\n#undef NULL\nstatic jmp_buf done;\n'
                    '#define PS5_POWER_TEST\n#include "'+str(encoder)+'"\n'
                    'static unsigned calls; static const unsigned expected[12]={0,0,1,1,1,0,0,1,0,1,0,1};\n'
                    'void ps5_power_test_slot(unsigned active,unsigned seconds) {\n'
                    ' if(calls>=12 || seconds!=4 || active!=expected[calls]) longjmp(done,2);\n'
                    ' if(++calls==12) longjmp(done,1); }\n'
                    'int main(void) { int result=setjmp(done); if(!result) ps5_short_entry_marker();\n'
                    ' return result==1 && calls==12 ? 0 : 1; }\n')
                executable=root/'marker-host'
                subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-fshort-wchar',
                    '-I'+str(EDK/'MdePkg/Include'),'-I'+str(EDK/'MdePkg/Include/X64'),
                    str(harness),'-o',str(executable)],check=True)
                subprocess.run([str(executable)],check=True)
                uefi.prepare_entry_power(root,True)
                self.assertEqual(encoder.read_text(),'#define PS5_POWER_EDK2\n'+(HERE/'bootstrap-power.c').read_text())
                self.assertNotIn('ps5_short_entry_marker',(entry/'UniversalPayloadEntry.c').read_text())

if __name__=='__main__':unittest.main()
