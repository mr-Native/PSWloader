import ctypes as C
from pathlib import Path
import subprocess
import tempfile
import unittest


class HdmiConfigTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work=tempfile.TemporaryDirectory()
        work=Path(cls.work.name);here=Path(__file__).parent
        (work/'query.c').write_text('''
#include "icc-power.h"
uint8_t reply[32]; unsigned length=20; int result=0; unsigned calls=0; unsigned emit=0; uint8_t event[132];
int ps5_icc_query_events(const struct ps5_icc_io *io,uint8_t s,uint16_t t,
 const uint8_t *p,unsigned n,uint8_t *o,unsigned cap,unsigned *out,ps5_icc_notification callback,void *context) {
 if(emit && callback) callback(context,0x10,0x8000,event,sizeof(event));
 (void)io; calls++;
 if(s!=2 || t!=0x16 || n!=1 || p[0]!=0x10 || cap<length) return -99;
 if(result) return result;
 for(unsigned i=0;i<length;i++) o[i]=reply[i];
 *out=length;return 0;
}
''')
        subprocess.run(['gcc','-Wall','-Wextra','-Werror','-shared','-fPIC','-I',str(here),str(here/'icc-hdmi.c'),str(here/'icc-edid.c'),str(work/'query.c'),'-o',str(work/'hdmi.so')],check=True)
        cls.lib=C.CDLL(str(work/'hdmi.so'))
        cls.lib.ps5_hdmi_read_config.argtypes=[C.c_void_p,C.c_void_p]
        cls.lib.ps5_hdmi_read_config_events.argtypes=[C.c_void_p]*4

    @classmethod
    def tearDownClass(cls):cls.work.cleanup()

    def setUp(self):
        self.reply=(C.c_uint8*32).in_dll(self.lib,'reply')
        self.reply[:]=bytes(32)
        C.c_uint.in_dll(self.lib,'length').value=20
        C.c_int.in_dll(self.lib,'result').value=0
        C.c_uint.in_dll(self.lib,'calls').value=0
        C.c_uint.in_dll(self.lib,'emit').value=0
        self.output=(C.c_uint8*13)(*([0xa5]*13))

    def test_both_converter_profiles_and_raw_prefix(self):
        for converter in (0,1):
            self.reply[:8]=bytes([0x10,0x20,0x30,converter,1,0,0xa5,0x5a])
            self.assertEqual(self.lib.ps5_hdmi_read_config(None,self.output),0)
            self.assertEqual(bytes(self.output),bytes(self.reply[:8])+bytes([2|converter,1,0,0xa5,0x5a]))

    def test_short_reply_preserves_output(self):
        for length,converter in ((0,0),(7,0)):
            C.c_uint.in_dll(self.lib,'length').value=length
            self.reply[3]=converter
            self.assertEqual(self.lib.ps5_hdmi_read_config(None,self.output),-6)
            self.assertEqual(bytes(self.output),bytes([0xa5])*13)

    def test_transport_error_preserves_output(self):
        C.c_int.in_dll(self.lib,'result').value=-3
        self.assertEqual(self.lib.ps5_hdmi_read_config(None,self.output),-3)
        self.assertEqual(bytes(self.output),bytes([0xa5])*13)

    def test_missing_output_does_not_submit(self):
        self.assertEqual(self.lib.ps5_hdmi_read_config(None,None),-1)
        self.assertEqual(C.c_uint.in_dll(self.lib,'calls').value,0)

    def test_configuration_query_delivers_edid_to_real_collector(self):
        import struct
        event=(C.c_uint8*132).in_dll(self.lib,'event')
        data=bytearray(128);data[:8]=b'\0'+b'\xff'*6+b'\0'
        data[127]=-sum(data[:127])&255
        event[:]=b'\0\x02\x80\0'+data
        C.c_uint.in_dll(self.lib,'emit').value=1
        state=C.create_string_buffer(1920+16)
        self.assertEqual(self.lib.ps5_hdmi_read_config_events(None,self.output,
                            C.cast(self.lib.ps5_edid_notification,C.c_void_p),state),0)
        self.assertEqual(state.raw[:128],data)
        self.assertEqual(struct.unpack_from('=4I',state.raw,1920),(128,0,1,0))

    def test_nonbinary_fields_follow_linux_equality_to_one(self):
        for value in (2,0x80,0xff):
            self.reply[:8]=bytes([0,0,0,value,value,value,0xff,0xff])
            self.assertEqual(self.lib.ps5_hdmi_read_config(None,self.output),0)
            self.assertEqual(bytes(self.output),bytes(self.reply[:8])+bytes([2,0,0,255,255]))
