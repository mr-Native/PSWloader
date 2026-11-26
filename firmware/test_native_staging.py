"""Reject unsafe bundle inputs before the staging tool can mount anything."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec=importlib.util.spec_from_file_location('stage_native',Path(__file__).with_name('stage-native-bundle.py'))
stage=importlib.util.module_from_spec(spec);spec.loader.exec_module(stage)

class NativeStagingTests(unittest.TestCase):
    def setUp(self):
        self.work=tempfile.TemporaryDirectory();self.root=Path(self.work.name)
        self.manifest={'bootstrap_test_only':False,'files':{}}
        for name in stage.FILES:
            path=self.root/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(b'input')
            self.manifest['files'][name]={'bytes':5,'sha256':hashlib.sha256(b'input').hexdigest()}
        self.save()
    def tearDown(self):self.work.cleanup()
    def save(self):(self.root/'manifest.json').write_text(json.dumps(self.manifest))
    def test_exact_bundle_accepts(self):self.assertEqual(stage.verify(self.root),self.manifest)
    def test_modified_file_rejected(self):
        (self.root/stage.FILES[0]).write_bytes(b'other')
        with self.assertRaisesRegex(ValueError,'hash/size'):stage.verify(self.root)
    def test_test_only_or_unexpected_files_rejected(self):
        self.manifest['bootstrap_test_only']=True;self.save()
        with self.assertRaises(ValueError):stage.verify(self.root)
        self.manifest['bootstrap_test_only']=False;self.manifest['files']['../../other']={};self.save()
        with self.assertRaises(ValueError):stage.verify(self.root)
    def test_symlink_input_rejected(self):
        path=self.root/stage.FILES[0];path.unlink();path.symlink_to(self.root/stage.FILES[1])
        with self.assertRaisesRegex(ValueError,'Invalid bundle'):stage.verify(self.root)


class NativeTelemetryPairTests(unittest.TestCase):
    def test_entry_bootstrap_pair_is_bound_to_payload_provenance(self):
        spec=importlib.util.spec_from_file_location('bundle',Path(__file__).with_name('build-native-bundle.py'))
        bundle=importlib.util.module_from_spec(spec);spec.loader.exec_module(bundle)
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            bootstrap={'power_telemetry':True,'defer_success_report':True}
            packed={'source_sha256':'verified-elf'}
            with self.assertRaisesRegex(ValueError,'Deferred'):bundle.telemetry_config(bootstrap,packed,root)
            (root/'build.json').write_text(json.dumps({'sha256':'other','entry_power_telemetry':True}))
            with self.assertRaisesRegex(ValueError,'provenance'):bundle.telemetry_config(bootstrap,packed,root)
            (root/'build.json').write_text(json.dumps({'sha256':'verified-elf','entry_power_telemetry':True}))
            self.assertTrue(bundle.telemetry_config(bootstrap,packed,root)['entry_power_telemetry'])
            for incompatible in ({'power_telemetry':False,'defer_success_report':True}, {'power_telemetry':True}):
                with self.assertRaises(ValueError):bundle.telemetry_config(incompatible,packed,root)
