import importlib.util
from pathlib import Path
import subprocess,tempfile,unittest
R=Path(__file__).parent
class DeferredCalibrationTests(unittest.TestCase):
    def test_compiled_normal_boot_skips_calibration_errors_keep_it(self):
        spec=importlib.util.spec_from_file_location('bootstrap_calibration_test',R/'build-bootstrap.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
        with tempfile.TemporaryDirectory() as work:
            for deferred in (False,True):
                output=Path(work)/str(deferred)
                manifest=m.build(output,power_telemetry=True,defer_success_report=True,defer_power_calibration=deferred)
                self.assertEqual(manifest['defer_power_calibration'],deferred)
                main=subprocess.check_output(['objdump','-d','--disassemble=ps5_bootstrap_main',str(output/'bootstrap.elf')],text=True)
                stop=subprocess.check_output(['objdump','-d','--disassemble=stop',str(output/'bootstrap.elf')],text=True)
                self.assertEqual('<ps5_power_calibrate>' in main,not deferred)
                if deferred:self.assertIn('<ps5_power_calibrate>',stop)
            with self.assertRaises(ValueError):m.build(Path(work)/'invalid',defer_power_calibration=True)

            with self.assertRaises(ValueError):m.build(Path(work)/'invalid-success',power_telemetry=True,defer_power_calibration=True)

    def test_bundle_rejects_deferred_calibration_with_normal_path_power_packets(self):
        import json
        spec=importlib.util.spec_from_file_location('bundle_calibration_test',R/'build-native-bundle.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
        boot={'defer_power_calibration':True,'defer_success_report':True,'power_telemetry':True}
        with tempfile.TemporaryDirectory() as work:
            root=Path(work);provenance={'sha256':'test','entry_power_telemetry':True,'entry_power_stage':'entry','exception_power_telemetry':True}
            (root/'build.json').write_text(json.dumps(provenance))
            with self.assertRaises(ValueError):m.telemetry_config(boot,{'source_sha256':'test'},root)
            provenance['entry_power_stage']='exception-only';(root/'build.json').write_text(json.dumps(provenance))
            self.assertTrue(m.telemetry_config(boot,{'source_sha256':'test'},root)['bootstrap_calibration_deferred'])
