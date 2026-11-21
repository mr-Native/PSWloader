import ctypes as C
from pathlib import Path
import subprocess,tempfile,unittest

class FlavaVideoTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory();p=Path(cls.tmp.name);r=Path(__file__).parent
        (p/'stub.c').write_text('#include "icc-power.h"\nint ps5_icc_query_events(const struct ps5_icc_io*a,uint8_t b,uint16_t c,const uint8_t*d,unsigned e,uint8_t*f,unsigned g,unsigned*h,ps5_icc_notification i,void*j){f[0]=f[1]=0;*h=2;return 0;}')
        subprocess.run(['gcc','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-shared','-fPIC','-I',str(r),str(r/'flava3-video.c'),str(r/'flava3-prepare.c'),str(r/'hdmi-commands.c'),str(p/'stub.c'),'-o',str(p/'video.so')],check=True)
        cls.lib=C.CDLL(str(p/'video.so'));cls.lib.ps5_flava3_video_stage.argtypes=[C.c_void_p,C.c_uint,C.c_void_p]
        cls.lib.ps5_hdmi_commands_submit.argtypes=[C.c_void_p]*4
    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()
    def packet(self,stage,swap=0):
        c=(C.c_uint8*13)(*([0]*8+[2,0,swap,255,255]));out=(C.c_uint8*2024)()
        self.assertEqual(self.lib.ps5_flava3_video_stage(c,stage,out),0)
        self.assertEqual(self.lib.ps5_hdmi_commands_submit(None,out,None,None),0)
        raw=bytes(out);end=raw[1]|raw[2]<<8;at=4;ops=[];writes=[]
        while at<end:
            size=raw[at+1];self.assertGreaterEqual(size,4);o=raw[at:at+size];ops.append(o)
            if o[0]==2 and o[2]==2:
                index=4;count=0
                while index<len(o):
                    n=o[index];writes.append((int.from_bytes(o[index+1:index+3],'big'),o[index+3:index+3+n]));index+=3+n;count+=1
                self.assertEqual(count,o[3]);self.assertEqual(index,len(o))
            at+=size
        self.assertEqual(at,end);self.assertEqual(len(ops),raw[3]);return raw,ops,writes
    def test_preconfig_and_training_wait_order(self):
        raw,ops,writes=self.packet(0);self.assertEqual(raw[0],1);self.assertEqual(len(ops),1)
        self.assertEqual(writes,[(0x6224,b'\0\1'),(0x1047,b'\0'),(0x1050,b'\0'*4),(0x7215,b'\0'),(0x7077,b'\0'),(0x7079,b'\x80')])
        _,ops,writes=self.packet(1);self.assertEqual(len(ops),12)
        self.assertEqual(writes,[(0x600c,b'\1\0'),(0x6c00,b'\x1e\x84\0'),(0x6a03,b'\x47'),(0x6a01,b'\x4d')])
        self.assertEqual([o for o in ops if o[0]==3 and o[2]==2],[bytes([3,8,2,1,0,0x60,0xf8,0xff]),bytes([3,8,2,1,0,0x60,0xf9,1]),bytes([3,8,2,1,0,0x60,0xf9,0x1a]),bytes([3,8,2,1,0,0x60,0x83,2])])
        self.assertEqual([int.from_bytes(o[5:7],'little') for o in ops if o[0]==3 and o[2]==1],[2,2,10])
    def test_rgb8_vic16_and_output_wait(self):
        for swap in (0,1):
            _,ops,writes=self.packet(2,swap)
            self.assertEqual(len(ops),6 if swap else 4)
            self.assertIn((0x7227,b'\x10\0'),writes);self.assertIn((0x7070,b'\x10\x10'+b'\0'*4),writes)
            for pair in [(0x70c4,b'\x08\x08'),(0x70c3,b'\0'),(0x7019,b'\0')]:self.assertIn(pair,writes)
            self.assertEqual(ops[-1],bytes([3,8,3,1,0,0x70,0x11,0xff]))
            self.assertEqual(ops[1],bytes([3,8,1,1,0,0xf4,1,0]))
    def test_phy_banked_writes_retain_order(self):
        for stage,first,last in [(3,(0x7022,b'\3'),(0x7025,b'\x77')),(4,(0x7030,b'\x47'),(0x7026,b'\xef'))]:
            raw,ops,writes=self.packet(stage);self.assertEqual(raw[0],0);self.assertEqual(len(ops),1)
            self.assertEqual(writes[0],first);self.assertEqual(writes[-1],last)
        _,_,writes=self.packet(3)
        self.assertEqual([v for a,v in writes if a==0x7030][:4],[b'\x40',b'\x41',b'\x42',b'\x43'])
    def test_reject_invalid_stage_and_converter_unchanged(self):
        for converter,stage in [(3,0),(2,8)]:
            c=(C.c_uint8*13)(*([0]*8+[converter,0,0,255,255]));out=(C.c_uint8*2024)(*([0xa5]*2024))
            self.assertEqual(self.lib.ps5_flava3_video_stage(c,stage,out),-1);self.assertEqual(bytes(out),b'\xa5'*2024)

    def test_output_start_and_hdcp_policy(self):
        raw,ops,writes=self.packet(5)
        self.assertEqual(raw[0],1);self.assertEqual(ops[-1],bytes([5,5,3,1,1]))
        self.assertIn((0x7a8b,b'\5'),writes)
        self.assertNotIn((0x7c00,b'\0'),writes)
        self.assertEqual([int.from_bytes(o[5:7],'little') for o in ops if o[0]==3 and o[2]==1],[10,700])
        raw,ops,_=self.packet(6)
        self.assertEqual(raw[:4],bytes([4,18,0,2]));self.assertEqual(ops[0],bytes([5,5,1,1,1]))
    def test_encoder_quiesce_matches_linux_packet(self):
        raw,_,_=self.packet(7)
        expected=bytes.fromhex('04 55 00 0a 02 09 03 01 01 70 5f 80 80 05 05 03 01 00 02 09 03 01 01 70 21 00 f0 02 08 01 01 01 7a 88 ff 03 08 03 01 00 7a 84 01 02 08 01 01 01 74 0a ff 03 08 03 01 00 74 0a ff 02 09 03 01 01 74 19 05 07 02 09 03 01 01 7a 8b 00 07 03 08 01 01 00 64 00 00')
        self.assertEqual(raw[:len(expected)],expected)

    def test_stage11_code4_unmutes_and_stage5_keeps_mute(self):
        raw5,_,writes5=self.packet(5)
        self.assertEqual(raw5[0],1)
        self.assertIn((0x705f,b'\x80'),writes5)
        raw,ops,writes=self.packet(11)
        self.assertEqual(raw[0],4)
        self.assertEqual(raw[:12],bytes([4,12,0,1,2,8,2,1,1,0x70,0x5f,0]))
        self.assertEqual(writes,[(0x705f,b'\x00')])
        self.assertEqual(len(ops),1)
        self.assertFalse(any(o[0]==2 and o[2]==3 for o in ops))
        c=(C.c_uint8*13)(*([0]*8+[2,0,0,255,255]));out=(C.c_uint8*2024)(*([0xa5]*2024))
        self.assertEqual(self.lib.ps5_flava3_video_stage(c,14,out),-1)
        self.assertEqual(bytes(out),b'\xa5'*2024)

class FlavaAudioStageTests(unittest.TestCase):
    def test_v241_audio_stages_follow_linux(self):
        src=(Path(__file__).resolve().parent/'flava3-video.c').read_text()
        a=src[src.index('if(stage==12)'):src.index('if(stage==13)')]
        for frag in ('W(0x62a0,0x06)','W(0x7011,0xa2)','WAIT(0x10e7,0xa2,1)','M(0x7203,0x10,0x10)','W(0x70a8,0xc0)','M(0x70ae,0x01,0x07)'):
            self.assertIn(frag,a)
        self.assertIn('M(0x70a8,0x00,0x04)',src[src.index('if(stage==13)'):])
        self.assertIn('stage==12?2:',src)
