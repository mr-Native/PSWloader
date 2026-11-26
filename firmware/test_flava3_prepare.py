import ctypes as C
from pathlib import Path
import subprocess,tempfile,unittest

class FlavaPrepareTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory();p=Path(cls.tmp.name);r=Path(__file__).parent
        (p/'stub.c').write_text('#include "icc-power.h"\nunsigned calls=0,fail_at=0; int ps5_icc_query_events(const struct ps5_icc_io*a,uint8_t b,uint16_t c,const uint8_t*d,unsigned e,uint8_t*f,unsigned g,unsigned*h,ps5_icc_notification i,void*j){calls++;if(calls==fail_at)return -3;f[0]=0;f[1]=0;*h=2;return 0;}')
        subprocess.run(['gcc','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-shared','-fPIC','-I',str(r),str(r/'flava3-prepare.c'),str(r/'hdmi-commands.c'),str(p/'stub.c'),'-o',str(p/'test.so')],check=True)
        cls.lib=C.CDLL(str(p/'test.so'));cls.lib.ps5_flava3_prepare_stage.argtypes=[C.c_void_p,C.c_uint,C.c_void_p]
        cls.lib.ps5_flava3_prepare.argtypes=[C.c_void_p,C.c_void_p,C.c_void_p,C.c_void_p,C.c_void_p]
    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()
    def build(self,stage,pol=255,swap=255,tmds=0):
        config=(C.c_uint8*13)(*([0]*8+[2,1,tmds,pol,swap]));out=(C.c_uint8*2024)()
        self.assertEqual(self.lib.ps5_flava3_prepare_stage(config,stage,out),0)
        raw=bytes(out);n=raw[1]|raw[2]<<8;ops=[];at=4
        while at<n:
            size=raw[at+1];self.assertGreaterEqual(size,4);ops.append(raw[at:at+size]);at+=size
        self.assertEqual(at,n);self.assertEqual(raw[3],len(ops))
        # Expand grouped register commands for semantic sequence assertions.
        expanded=[]
        for o in ops:
            if o[0]==2 and o[2]==2:
                at=4;count=0
                while at<len(o):
                    size=o[at];expanded.append(bytes([2,7+size,2,1])+o[at:at+3+size]);at+=3+size;count+=1
                self.assertEqual(count,o[3])
            else:expanded.append(o)
        self.assertEqual(raw[3],{0:2 if tmds else 1,1:5,2:1}[stage])
        return raw,expanded
    def test_dp_order_delays_and_default_wiring(self):
        raw,ops=self.build(1)
        writes=[(int.from_bytes(o[5:7],'big'),o[7:]) for o in ops if o[0]==2]
        self.assertEqual(writes[:8],[(0x6a03,b'\x04'),(0x60a2,b'\xf1'),(0x60bf,b'\x03'),(0x60c0,b'\xef'),(0x60c3,b'\x4d'),(0x60c7,b'\x85'),(0x60bf,b'\x04'),(0x60c7,b'\x84')])
        self.assertNotIn(0x600e,[x[0] for x in writes]);self.assertNotIn(0x600b,[x[0] for x in writes])
        self.assertEqual(ops[-4:], [bytes([3,8,1,1,0,2,0,0]),bytes([2,8,2,1,1,0x60,5,1]),bytes([3,8,1,1,0,2,0,0]),bytes([2,8,2,1,1,0x60,8,0])])
    def test_wiring_and_mask(self):
        _,ops=self.build(1,pol=5,swap=0xe4)
        self.assertIn(bytes([2,8,2,1,1,0x60,0x0e,0x50]),ops)
        self.assertIn(bytes([2,8,2,1,1,0x60,0x0b,0xe4]),ops)
        _,ops=self.build(0,tmds=1)
        self.assertEqual(ops[-1],bytes([2,9,3,1,1,0x70,0x1a,0xb1,0xff]))
    def test_vsync_code_and_register_values(self):
        raw,ops=self.build(2);self.assertEqual(raw[0],1)
        self.assertEqual([(int.from_bytes(o[5:7],'big'),o[7:]) for o in ops],[(0x1047,b'\0'),(0x6064,b'\1'),(0x600c,b'\1\0'),(0x6c07,b'\0'),(0x7214,b'\0\0')])
    def test_reject_other_converter_invalid_stage_and_wiring_unchanged(self):
        for converter,stage,pol,swap in [(3,0,255,255),(2,3,255,255),(2,0,16,255),(2,1,255,0)]:
            c=(C.c_uint8*13)(*([0]*8+[converter,0,0,pol,swap]));out=(C.c_uint8*2024)(*([0xa5]*2024))
            self.assertEqual(self.lib.ps5_flava3_prepare_stage(c,stage,out),-1);self.assertEqual(bytes(out),b'\xa5'*2024)

    def test_submit_fail_fast_and_counts_completed_stages(self):
        config=(C.c_uint8*13)(*([0]*8+[2,0,0,255,255]));io=C.c_uint8(0)
        for fail in (0,1,2,3):
            C.c_uint.in_dll(self.lib,'calls').value=0;C.c_uint.in_dll(self.lib,'fail_at').value=fail
            completed=C.c_uint(99)
            self.assertEqual(self.lib.ps5_flava3_prepare(C.byref(io),config,C.byref(completed),None,None),-3 if fail else 0)
            self.assertEqual(completed.value,fail-1 if fail else 3)
            self.assertEqual(C.c_uint.in_dll(self.lib,'calls').value,fail or 3)
        config[8]=3;C.c_uint.in_dll(self.lib,'calls').value=0
        self.assertEqual(self.lib.ps5_flava3_prepare(C.byref(io),config,None,None,None),-1)
        self.assertEqual(C.c_uint.in_dll(self.lib,'calls').value,0)
