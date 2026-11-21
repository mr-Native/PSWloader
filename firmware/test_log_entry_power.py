"""Optional logger-entry protocol snapshot tests, without firmware or hardware."""
import ctypes as C
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
HERE=Path(__file__).resolve().parent
EDK=Path(os.environ.get('PS5_EDK2_ROOT',os.environ.get('PSW_WORK', '/opt/pswloader-work') + '/edk2'))
def load(name,file):
    spec=importlib.util.spec_from_file_location(name,HERE/file)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module
uefi=load('log_power_builder','build-uefi.py')
bundle=load('log_power_bundle','build-native-bundle.py')

class LogPowerBuilderTests(unittest.TestCase):
    def test_exception_only_retains_fatal_reporting_without_success_probe(self):
        self.assertEqual(uefi.power_targets(True,'exception-only',True),(False,False,False,False))
        with tempfile.TemporaryDirectory() as work:
            root=Path(work)
            provenance={'sha256':'bound','entry_power_telemetry':True,
                        'entry_power_stage':'exception-only','exception_power_telemetry':True}
            bootstrap={'power_telemetry':True,'defer_success_report':True}
            (root/'build.json').write_text(json.dumps(provenance))
            result=bundle.telemetry_config(bootstrap,{'source_sha256':'bound'},root)
            self.assertTrue(result['exception_power_telemetry'])
            self.assertEqual(result['entry_power_stage'],'exception-only')
            provenance['exception_power_telemetry']=False
            (root/'build.json').write_text(json.dumps(provenance))
            with self.assertRaisesRegex(ValueError,'Exception-only'):
                bundle.telemetry_config(bootstrap,{'source_sha256':'bound'},root)

    def test_stage_requires_power_and_log_export_and_has_one_target(self):
        self.assertEqual(uefi.power_targets(True,'log-entry',True),(False,False,True,False))
        for power,logger in ((False,True),(True,False),(False,False)):
            with self.assertRaises(ValueError):uefi.power_targets(power,'log-entry',logger)
        for stage in uefi.POWER_STAGES:
            self.assertEqual(sum(uefi.power_targets(True,stage,True)),0 if stage=='exception-only' else 1)
        self.assertEqual(uefi.power_targets(False,'entry',False),(False,False,False,False))
    def test_generated_probe_precedes_events_and_normal_build_is_clean(self):
        with tempfile.TemporaryDirectory() as work:
            package=Path(work)/'Ps5PlatformPkg'
            shutil.copytree(HERE/'Ps5PlatformPkg',package)
            directory=package/'LogExportDxe'
            uefi.prepare_log_power(package,True)
            source=(directory/'LogExportDxe.c').read_text();inf=(directory/'LogExportDxe.inf').read_text()
            self.assertEqual(source.count('  Ps5LogEntryProbe ();'),1)
            self.assertLess(source.index('  Ps5LogEntryProbe ();'),source.index('Status=gBS->CreateEventEx'))
            for name in ('Ps5LogEntryProbe.c','Ps5LogEntryPower.c'):
                self.assertEqual(inf.count('  '+name),1)
            for name in ('Cpu','RealTimeClock','Timer'):
                self.assertIn('gEfi'+name+'ArchProtocolGuid ## CONSUMES',inf)
            encoder=directory/'Ps5LogEntryPower.c'
            self.assertEqual(encoder.read_text(),'#define PS5_POWER_EDK2\n'+(HERE/'bootstrap-power.c').read_text())
            # Corrupt generated copies; regeneration always reads HERE originals.
            (directory/'LogExportDxe.c').write_text('stale')
            uefi.prepare_log_power(package,True)
            self.assertEqual((directory/'LogExportDxe.c').read_text(),source)
            uefi.prepare_log_power(package,False)
            self.assertFalse(encoder.exists())
            for name in ('LogExportDxe.c','LogExportDxe.inf'):
                self.assertEqual((directory/name).read_bytes(),(HERE/'Ps5PlatformPkg/LogExportDxe'/name).read_bytes())
    def test_bundle_requires_bound_logger_build_provenance(self):
        with tempfile.TemporaryDirectory() as work:
            root=Path(work);bootstrap={'power_telemetry':True,'defer_success_report':True}
            provenance={'sha256':'bound','entry_power_telemetry':True,'entry_power_stage':'log-entry'}
            path=root/'build.json';path.write_text(json.dumps(provenance))
            with self.assertRaisesRegex(ValueError,'Log entry'):bundle.telemetry_config(bootstrap,{'source_sha256':'bound'},root)
            provenance['log_export_enabled']=True;path.write_text(json.dumps(provenance))
            self.assertEqual(bundle.telemetry_config(bootstrap,{'source_sha256':'bound'},root)['entry_power_stage'],'log-entry')

@unittest.skipUnless((EDK/'MdePkg/Include/Base.h').exists(),'EDK headers required')
class LogEntryProbeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work=tempfile.TemporaryDirectory();work=Path(cls.work.name)
        source=HERE/'Ps5PlatformPkg/LogExportDxe/Ps5LogEntryProbe.c'
        (work/'mock.c').write_text('#include "'+str(source)+'"\n'+r'''
static EFI_BOOT_SERVICES Bs;
EFI_BOOT_SERVICES *gBS=&Bs;
EFI_GUID gEfiCpuArchProtocolGuid={0},gEfiRealTimeClockArchProtocolGuid={0},gEfiTimerArchProtocolGuid={0};
static unsigned Missing,Calls,Reported,Reports;
static EFI_STATUS EFIAPI Locate(EFI_GUID *Guid,VOID *Registration,VOID **Interface) {
  unsigned Bit=Guid==&gEfiCpuArchProtocolGuid?1:Guid==&gEfiRealTimeClockArchProtocolGuid?2:Guid==&gEfiTimerArchProtocolGuid?4:0;
  (VOID)Registration;Calls|=Bit;*Interface=NULL;
  return Missing&Bit ? EFI_NOT_FOUND : EFI_SUCCESS;
}
VOID ps5_power_report(unsigned Code) {Reported=Code;Reports++;}
unsigned TestProbe(unsigned Mask) {
  Missing=Mask;Calls=0;Reported=0;Reports=0;Bs.LocateProtocol=Locate;
  Ps5LogEntryProbe();return Reported;
}
unsigned TestCalls(void){return Calls;}
unsigned TestReports(void){return Reports;}
''')
        subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-shared','-fPIC','-fshort-wchar',
            '-I'+str(EDK/'MdePkg/Include'),'-I'+str(EDK/'MdePkg/Include/X64'),str(work/'mock.c'),'-o',str(work/'probe.so')],check=True)
        cls.lib=C.CDLL(str(work/'probe.so'))
    @classmethod
    def tearDownClass(cls):cls.work.cleanup()
    def test_all_eight_protocol_presence_masks_null_interfaces_are_valid(self):
        for mask in range(8):
            with self.subTest(mask=mask):
                self.assertEqual(self.lib.TestProbe(mask),0x710|mask if mask else 0x708)
                self.assertEqual(self.lib.TestCalls(),7)
                self.assertEqual(self.lib.TestReports(),1)

if __name__=='__main__':unittest.main()
