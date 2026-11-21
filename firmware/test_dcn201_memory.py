import ctypes as C
from pathlib import Path
import subprocess,tempfile,unittest
class Memory(C.Structure):
    _fields_=[(n,C.c_uint64) for n in ('fb_base','fb_end','uma_base','uma_end')]
class AperturePlan(C.Structure):
    _fields_=[(n,C.c_uint64) for n in ('cpu','surface','bytes')]
class DcnMemoryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory();p=Path(cls.tmp.name);r=Path(__file__).parent
        subprocess.run(['gcc','-Wall','-Wextra','-Werror','-shared','-fPIC',str(r/'dcn201-memory.c'),'-o',str(p/'m.so')],check=True)
        cls.lib=C.CDLL(str(p/'m.so'));cls.lib.ps5_dcn201_memory_decode.argtypes=[C.c_uint32]*3+[C.POINTER(Memory)]
        cls.lib.ps5_dcn201_uma_contains.argtypes=[C.POINTER(Memory),C.c_uint64,C.c_uint64]
        cls.lib.ps5_dcn201_gpu_to_uma.argtypes=[C.POINTER(Memory),C.c_uint64,C.c_uint64,C.c_void_p]
        cls.lib.ps5_dcn201_framebuffer_candidate.argtypes=[C.POINTER(Memory),C.c_uint64,C.c_uint64,C.c_uint64,C.c_void_p]
        cls.lib.ps5_dcn201_aperture_framebuffer.argtypes=[C.POINTER(Memory),C.c_uint64,C.c_uint64,C.c_uint32,C.c_uint32,C.c_uint64,C.POINTER(AperturePlan)]
    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()
    def test_decode_units_inclusive_top_and_gpu_translation(self):
        m=Memory();self.assertEqual(self.lib.ps5_dcn201_memory_decode(0x400,0x7ff,0,m),0)
        self.assertEqual((m.fb_base,m.fb_end,m.uma_base,m.uma_end),(0x400000000,0x800000000,0,0x400000000))
        out=C.c_uint64();self.assertEqual(self.lib.ps5_dcn201_gpu_to_uma(m,0x46e000000,14745600,C.byref(out)),0)
        self.assertEqual(out.value,0x6e000000)
        self.assertEqual(self.lib.ps5_dcn201_gpu_to_uma(m,0x6e000000,14745600,C.byref(out)),0)
        self.assertEqual(out.value,0x6e000000)
    def test_range_crossing_zero_and_wrap_are_rejected(self):
        m=Memory();self.assertEqual(self.lib.ps5_dcn201_memory_decode(0x400,0x7ff,0,m),0)
        for address,size in [(0x7ffffffff,2),(0x3ffffffff,2),(0,0),(0xffffffffffffffff,2),(0x800000000,1)]:
            out=C.c_uint64(0xabc);self.assertEqual(self.lib.ps5_dcn201_gpu_to_uma(m,address,size,C.byref(out)),-1);self.assertEqual(out.value,0xabc)
        self.assertEqual(self.lib.ps5_dcn201_uma_contains(m,0x3ffffffff,1),1)
        self.assertEqual(self.lib.ps5_dcn201_uma_contains(m,0x400000000,1),0)
    def test_invalid_registers_and_uma_overflow_preserve_output(self):
        for base,top,offset in [(2,1,0),(0xff000000,0xff000001,0),(0,0xffffffff,0),(0,0xffffff,1)]:
            m=Memory(1,2,3,4);self.assertEqual(self.lib.ps5_dcn201_memory_decode(base,top,offset,m),-1);self.assertEqual(tuple(getattr(m,n) for n,_ in m._fields_),(1,2,3,4))
        m=Memory();self.assertEqual(self.lib.ps5_dcn201_memory_decode(0,0xffffff,0,m),0);self.assertEqual(m.fb_end,1<<48)
    def test_overlapping_ranges_match_linux_gpu_first_order(self):
        m=Memory();self.assertEqual(self.lib.ps5_dcn201_memory_decode(1,3,2,m),0)
        out=C.c_uint64();self.assertEqual(self.lib.ps5_dcn201_gpu_to_uma(m,0x2000000,4096,C.byref(out)),0);self.assertEqual(out.value,0x3000000)

    def test_framebuffer_intersection_alignment_and_failure_preserves_output(self):
        m=Memory();self.assertEqual(self.lib.ps5_dcn201_memory_decode(0x400,0x7ff,0,m),0)
        out=C.c_uint64();size=1920*1080*4
        self.assertEqual(self.lib.ps5_dcn201_framebuffer_candidate(m,0x10000000,0x1000000,size,C.byref(out)),0)
        self.assertEqual(out.value&65535,0);self.assertGreaterEqual(out.value,0x10000000);self.assertLessEqual(out.value+size,0x11000000)
        for base,length,n in [(0x400000000,0x1000000,size),(0,0x100000,size),(0x10000000,size-1,size),(0xffffffffffffffff,2,size),(0x10000000,0x1000000,1)]:
            out.value=0xabc;self.assertEqual(self.lib.ps5_dcn201_framebuffer_candidate(m,base,length,n,C.byref(out)),-1);self.assertEqual(out.value,0xabc)

    def test_aperture_plan_actual_mc_window_with_fixture_cpu_bar(self):
        m=Memory();self.assertEqual(self.lib.ps5_dcn201_memory_decode(0xf400,0xf41f,0x450,m),0)
        out=AperturePlan();size=1920*1080*4
        self.assertEqual(self.lib.ps5_dcn201_aperture_framebuffer(m,0x800000000,0x10000000,0xf4000000,0,size,out),0)
        self.assertEqual((out.cpu,out.surface,out.bytes),(0x80f810000,0x45f810000,size))
        # A remapped aperture can start in the second half of FB.
        self.assertEqual(self.lib.ps5_dcn201_aperture_framebuffer(m,0x800000000,0x10000000,0xf4100000,0,size,out),0)
        self.assertEqual(out.surface,0x46f810000)
    def test_aperture_rejections_preserve_output(self):
        m=Memory();self.lib.ps5_dcn201_memory_decode(0xf400,0xf41f,0x450,m)
        for base,span,lo,hi,size in [(0,0x10000000,0xf4000000,0,4096),(0x800000000,4096,0xf4000000,0,8192),(0x800000000,0x10000000,0,0,4096),(0x800000000,0x10000000,0xf4200000,0,4096),(0x800000000,0x10000000,0xf4000000,256,4096),(0xffffffffffffffff,4096,0xf4000000,0,4096)]:
            out=AperturePlan(1,2,3);self.assertEqual(self.lib.ps5_dcn201_aperture_framebuffer(m,base,span,lo,hi,size,out),-1);self.assertEqual((out.cpu,out.surface,out.bytes),(1,2,3))
