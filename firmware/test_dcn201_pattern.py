import ctypes as C
from pathlib import Path
import subprocess,tempfile,unittest
class Write(C.Structure):
 _fields_=[(n,C.c_uint32) for n in ('offset','mask','value')]
class PatternTests(unittest.TestCase):
 def test_real_dpg_register_plan_and_invalid_input(self):
  with tempfile.TemporaryDirectory() as work:
   r=Path(__file__).parent;p=Path(work)/'pattern.so'
   subprocess.run(['gcc','-Wall','-Wextra','-Werror','-shared','-fPIC',str(r/'dcn201-test-pattern.c'),'-o',str(p)],check=True)
   lib=C.CDLL(str(p));lib.ps5_dcn201_solid_pattern.argtypes=[C.c_uint]*5+[C.POINTER(Write)]
   out=(Write*6)();self.assertEqual(lib.ps5_dcn201_solid_pattern(1920,1080,1023,0,0,out),0)
   self.assertEqual([(x.offset,x.mask,x.value) for x in out],[(0x13458,0xffffffff,0x07800438),(0x13468,0xffffffff,0),(0x13464,0xffffffff,0),(0x13460,0xffffffff,0),(0x1345c,0xffffffff,0xffc0ffc0),(0x13450,0x71,0x41)])
   before=bytes(out)
   for args in [(0,1080,0,0,0),(1920,0x4000,0,0,0),(1920,1080,1024,0,0)]:
    self.assertEqual(lib.ps5_dcn201_solid_pattern(*args,out),-1);self.assertEqual(bytes(out),before)
