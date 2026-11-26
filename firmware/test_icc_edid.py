import ctypes as C
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

class EdidTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work=tempfile.TemporaryDirectory();p=Path(cls.work.name)/'edid.so'
        subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-shared','-fPIC',str(Path(__file__).with_name('icc-edid.c')),'-o',str(p)],check=True)
        cls.lib=C.CDLL(str(p));cls.lib.ps5_edid_notification.argtypes=[C.c_void_p,C.c_uint8,C.c_uint16,C.c_void_p,C.c_uint]
        class State(C.Structure):
            _fields_=[('raw',C.c_uint8*1920),('bytes',C.c_uint),('flags',C.c_uint),('updates',C.c_uint),('rejected',C.c_uint)]
        cls.State=State
    @classmethod
    def tearDownClass(cls):cls.work.cleanup()
    def setUp(self):self.state=self.State()
    def edid(self,blocks=2):
        data=bytearray(blocks*128);data[:8]=b'\0'+b'\xff'*6+b'\0';data[126]=blocks-1
        for offset in range(0,len(data),128):data[offset+127]=-sum(data[offset:offset+127])&255
        return data
    def send(self,data,service=0x10,type=0x8000,declared=None):
        payload=b'\0\x02'+struct.pack('<H',len(data) if declared is None else declared)+data
        self.lib.ps5_edid_notification(C.byref(self.state),service,type,C.create_string_buffer(payload),len(payload))
    def test_copy_survives_borrowed_buffer_and_clears_old_tail(self):
        data=self.edid(15);self.send(data);data[20]=0xa5
        self.assertEqual(self.state.raw[20],0)
        self.assertEqual((self.state.bytes,self.state.flags),(1920,0))
        self.send(self.edid(1));self.assertEqual(bytes(self.state.raw[128:]),bytes(1792))
        self.assertEqual(self.state.updates,2)
    def test_malformed_lengths_preserve_previous_record(self):
        self.send(self.edid());before=bytes(self.state.raw)
        for length in (0,127,129,256,2048):self.send(self.edid(1),declared=length)
        self.assertEqual(bytes(self.state.raw),before);self.assertEqual(self.state.rejected,5)
        self.assertEqual(self.state.updates,1)
    def test_ps5_header_quirk_is_retained_and_flagged_not_repaired(self):
        data=self.edid();data[0]=1;self.send(data)
        self.assertEqual(self.state.flags,1|2|8);self.assertEqual(bytes(self.state.raw[:256]),data)
    def test_bad_extension_checksum_and_count(self):
        data=self.edid();data[140]=1;self.send(data);self.assertEqual(self.state.flags,2)
        data=self.edid();data[126]=0;data[127]=(-sum(data[:127]))&255
        self.send(data);self.assertEqual(self.state.flags,4)
    def test_unrelated_service_reply_or_notification_ignored(self):
        self.send(self.edid(),service=2);self.send(self.edid(),type=0x4000)
        self.lib.ps5_edid_notification(C.byref(self.state),0x10,0x8000,C.create_string_buffer(b'\0\x03'),2)
        self.assertEqual((self.state.updates,self.state.rejected),(0,0))
    def test_truncated_event_prefix(self):
        for payload in (b'',b'\0',b'\0\x02',b'\0\x02\x80'):
            self.lib.ps5_edid_notification(C.byref(self.state),0x10,0x8000,C.create_string_buffer(payload),len(payload))
        self.assertEqual(self.state.rejected,4)

    def test_captured_linux_drm_edid(self):
        import json,hashlib
        path=Path(__file__).parent.parent/'evidence/windows-linux-edid-20261102.json'
        capture=json.loads(path.read_text())
        data=bytes.fromhex(next(c['hex'] for c in capture['connectors'] if c['status']=='connected'))
        self.assertEqual(hashlib.sha256(data).hexdigest(),'5ce7f2a4d7fc47493a10a5e06ce7591b2af2b9046c4710313643a8f1364a7af5')
        self.send(data)
        self.assertEqual((self.state.bytes,self.state.flags),(256,0))
        self.assertEqual(bytes(self.state.raw[:256]),data)
