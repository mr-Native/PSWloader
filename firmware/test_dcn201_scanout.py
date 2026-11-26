"""Validate real PS5 snapshot decoding and reject unsafe GOP candidates."""
import ctypes as c
import subprocess,tempfile,unittest
from pathlib import Path
HERE=Path(__file__).parent
class State(c.Structure):
 _fields_=[(n,c.c_uint32) for n in ['hubp','otg','config','tiling','control','pitch','low','high','hblank','vblank']]
class Scanout(c.Structure):
 _fields_=[('surface_address',c.c_uint64),('bytes',c.c_uint64)]+[(n,c.c_uint32) for n in ['width','height','pixels','stride']]
class Dcn201Tests(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.work=tempfile.TemporaryDirectory();p=Path(cls.work.name)/'scanout.so'
  subprocess.run(['gcc','-Wall','-Wextra','-Werror','-shared','-fPIC',str(HERE/'dcn201-scanout.c'),'-o',str(p)],check=True)
  cls.fn=c.CDLL(str(p)).ps5_dcn201_linear_scanout;cls.fn.argtypes=[c.POINTER(State),c.POINTER(Scanout)]
 @classmethod
 def tearDownClass(cls):cls.work.cleanup()
 def base(self):return State(0xf0102,0x80011311,8,0x80,0,0x9ff,0x6e000000,4,0x700a70,0x2705c7)
 def test_actual_live_linux_snapshot(self):
  s=self.base();out=Scanout();self.assertEqual(self.fn(c.byref(s),c.byref(out)),0)
  self.assertEqual((out.surface_address,out.width,out.height,out.stride,out.bytes),(0x46e000000,2560,1440,10240,14745600))
 def test_reject_blank_disabled_tiled_compressed_rotated_and_bad_extents_without_output_write(self):
  for field,value in [('hubp',1),('hubp',4),('otg',0),('tiling',1),('control',2),('config',0x108),('config',9),('pitch',0),('hblank',0),('vblank',0),('low',0x6e000001),('low',0)]:
   s=self.base();setattr(s,field,value)
   if field=='low' and value==0:s.high=0
   out=Scanout(123,456,7,8,9,10);before=bytes(out)
   self.assertLess(self.fn(c.byref(s),c.byref(out)),0,(field,value));self.assertEqual(bytes(out),before)
