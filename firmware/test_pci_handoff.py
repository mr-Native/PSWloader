import base64
import ctypes as C
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from test_handoff import Result, boot_params, fixture, packer

HERE=Path(__file__).resolve().parent
GUID=bytes.fromhex('cbba4eec38266e41be80e5fa4b511901')

class PciHandoffTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work=tempfile.TemporaryDirectory()
        lib=Path(cls.work.name)/'pci-hobs.so'
        subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-shared','-fPIC',
                        str(HERE/'handoff.c'),str(HERE/'pci-handoff.c'),'-o',str(lib)],check=True)
        cls.lib=C.CDLL(str(lib))
        cls.lib.ps5_prepare_handoff.argtypes=[C.c_void_p,C.c_size_t,C.c_void_p,C.c_size_t,
                                            C.c_void_p,C.c_size_t,C.c_uint,C.POINTER(Result)]
        cls.lib.ps5_add_pci_handoff.argtypes=[C.c_void_p,C.c_size_t,C.c_void_p,C.c_void_p,C.c_size_t,
                                            C.c_void_p,C.c_size_t,C.POINTER(Result)]
        cls.lib.ps5_pci_profile_mismatch.argtypes=[C.c_void_p,C.c_void_p,C.c_size_t,
            C.POINTER(C.c_uint),C.POINTER(C.c_uint),C.POINTER(C.c_uint8),
            C.POINTER(C.c_uint8),C.POINTER(C.c_uint8)]
        audit=json.loads((HERE.parent/'evidence/windows-platform-live-20261102.json').read_text())
        cls.sony=base64.b64decode(audit['detail']['0000:00:01.1']['config_base64'])[:64]
        cls.amd=base64.b64decode(audit['detail']['0000:00:08.1']['config_base64'])[:64]

    @classmethod
    def tearDownClass(cls):cls.work.cleanup()

    def prepared(self,extra=None,replace_windows=False):
        regions=[(0x100000,0xf00000,1),(0x40000000,0x20000000,1),
                 (0x100000000,0x350000000,1)]
        if not replace_windows:
            regions += [(0x7fd90000,0xc4400000-0x7fd90000,2),(0xd0000000,0x10700000,2)]
        if extra: regions+=extra
        bp=C.create_string_buffer(bytes(boot_params(regions)))
        data=packer.pack(fixture())[0]; image=C.create_string_buffer(data)
        out=C.create_string_buffer(32768);result=Result()
        code=self.lib.ps5_prepare_handoff(bp,4096,image,len(data),out,32768,44,C.byref(result))
        self.assertEqual(code,0)
        return out.raw[:result.hob_bytes],result

    def augment(self,raw,result,sony=None,amd=None,capacity=32768):
        src=C.create_string_buffer(raw)
        s=C.create_string_buffer(self.sony if sony is None else bytes(sony))
        a=C.create_string_buffer(self.amd if amd is None else bytes(amd))
        out=C.create_string_buffer(capacity)
        status=self.lib.ps5_add_pci_handoff(src,len(raw),s,a,64,out,capacity,C.byref(result))
        return status,out.raw[:result.hob_bytes]

    def hobs(self,raw):
        off=0
        while off<len(raw):
            kind,length=struct.unpack_from('<HH',raw,off)
            self.assertGreaterEqual(length,8);self.assertEqual(length%8,0)
            yield kind,raw[off:off+length],off
            off+=length
        self.assertEqual(off,len(raw))

    def test_live_profile_and_reserved_resource_split(self):
        raw,result=self.prepared()
        status,out=self.augment(raw,result)
        self.assertEqual(status,0)
        roots=[];resources=[]
        for kind,hob,offset in self.hobs(out):
            if kind==3: resources.append(struct.unpack_from('<IIQQ',hob,24))
            if kind==4 and hob[8:24]==GUID: roots.append(hob)
            if kind==0xffff:self.assertEqual(struct.unpack_from('<Q',out,48)[0],0x40000000+offset)
        self.assertEqual(len(roots),1)
        h=roots[0]
        self.assertEqual(h[24:30],b'\x01\x00\xbc\x00\x01\x01')
        self.assertEqual(struct.unpack_from('<QQQ',h,30+30),(0,127,0))
        self.assertEqual(struct.unpack_from('<QQQ',h,30+54),(0x1000,0x2fff,0))
        self.assertEqual(struct.unpack_from('<QQQ',h,30+78),(0x80000000,0xe06fffff,0))
        self.assertEqual(struct.unpack_from('<QQQ',h,30+150),(0x980000000,0x99fffffff,0))
        self.assertEqual(struct.unpack_from('<II',h,30+174),(0x0a0341d0,0))
        self.assertIn((5,0x401,0x7fd90000,0x270000),resources)
        self.assertIn((1,0x401,0x80000000,0x44400000),resources)
        self.assertIn((1,0x401,0xd0000000,0x10700000),resources)
        self.assertEqual(result.resource_count,len(resources))
        self.assertEqual(struct.unpack_from('<Q',out,40)[0],0x40000000+len(out))

    def mismatch(self,sony=None,amd=None,size=64,null_output=None):
        s=C.create_string_buffer(self.sony if sony is None else bytes(sony))
        a=C.create_string_buffer(self.amd if amd is None else bytes(amd))
        values=[C.c_uint(91),C.c_uint(92),C.c_uint8(93),C.c_uint8(94),C.c_uint8(95)]
        pointers=[C.byref(v) for v in values]
        if null_output is not None:pointers[null_output]=None
        status=self.lib.ps5_pci_profile_mismatch(s,a,size,*pointers)
        return status,tuple(v.value for v in values)

    def test_command_io_optional_memory_and_master_required(self):
        for bridge in ('sony','amd'):
            config=bytearray(getattr(self,bridge));config[4]&=~1
            raw,result=self.prepared()
            self.assertEqual(self.augment(raw,result,**{bridge:config})[0],0)
            self.assertEqual(self.mismatch(**{bridge:config})[0],0)
            for bit in (2,4):
                broken=bytearray(config);broken[4]&=~bit
                raw,result=self.prepared()
                self.assertEqual(self.augment(raw,result,**{bridge:broken})[0],-2)
                self.assertEqual(self.mismatch(**{bridge:broken}),
                    (1,(0 if bridge=='sony' else 1,4,broken[4],6,6)))

    def test_profile_changes_fail_closed(self):
        offsets=[0,1,2,3,0xe,0x18,0x19,0x1a,0x1c,0x1d]+list(range(0x20,0x24))+[0x24,0x26]+list(range(0x30,0x34))
        for bridge in ('sony','amd'):
            for offset in offsets:
                with self.subTest(bridge=bridge,offset=offset):
                    raw,result=self.prepared();config=bytearray(getattr(self,bridge))
                    if offset>=0x30:
                        config[0x1c:0x1e]=b'\x11\x11' if bridge=='sony' else b'\x21\x21'
                        # Upper base must not exceed limit: exercise an active window.
                        if offset in (0x30,0x31):config[offset+2]=1
                    config[offset]^=1
                    self.assertEqual(self.augment(raw,result,**{bridge:config})[0],-2)
                    self.assertEqual(self.mismatch(**{bridge:config}),
                        (1,(0 if bridge=='sony' else 1,offset,config[offset],
                            getattr(self,bridge)[offset] & (0x7f if offset==0xe else 0x0f if offset in (0x1c,0x1d,0x24,0x26) else 0xff),
                            0x7f if offset==0xe else 0x0f if offset in (0x1c,0x1d,0x24,0x26) else 0xff)))

    def test_sony_bounded_io_windows(self):
        for base,limit in ((0xf1,0x01),(0x11,0x01),(0x11,0x11),(0x21,0x11)):
            with self.subTest(base=base,limit=limit):
                config=bytearray(self.sony);config[0x1c]=base;config[0x1d]=limit
                self.assertEqual(self.mismatch(sony=config)[0],0)
                raw,result=self.prepared()
                self.assertEqual(self.augment(raw,result,sony=config)[0],0)
        # Exhaust the 4-bit window bounds: only disabled or the assigned
        # single 4KiB window may pass, including disabled bounds above AMD.
        for base in range(16):
            for limit in range(16):
                config=bytearray(self.sony)
                config[0x1c]=(base << 4) | 1;config[0x1d]=(limit << 4) | 1
                self.assertEqual(self.mismatch(sony=config)[0],
                    0 if base > limit or base == limit == 1 else 1)
        for base,limit,offset,expected,mask in (
                (0x10,0x11,0x1c,1,0x0f),(0x11,0x10,0x1d,1,0x0f),
                (0x11,0x21,0x1d,0x11,0xff),(0x21,0x21,0x1c,0x11,0xff),
                (0x01,0x01,0x1c,0x11,0xff),(0x01,0x11,0x1c,0x11,0xff),
                (0xf1,0xf1,0x1c,0x11,0xff)):
            with self.subTest(base=base,limit=limit):
                config=bytearray(self.sony);config[0x1c]=base;config[0x1d]=limit
                self.assertEqual(self.mismatch(sony=config),
                    (1,(0,offset,config[offset],expected,mask)))
                raw,result=self.prepared()
                self.assertEqual(self.augment(raw,result,sony=config)[0],-2)
    def test_sony_full_32bit_io_window(self):
        cases=[
            (0x11,0x01,0xffff,0,True),
            (0xf1,0x01,0xffff,0xffff,True),
            (0x11,0x11,0xffff,0xffff,False),
            (0xf1,0x01,0,1,False),
            (0x11,0x11,1,0,True),
            (0x11,0x11,0,1,False),
        ]
        for base,limit,upper_base,upper_limit,accepted in cases:
            with self.subTest(base=base,limit=limit,upper_base=upper_base,upper_limit=upper_limit):
                config=bytearray(self.sony);config[0x1c]=base;config[0x1d]=limit
                struct.pack_into('<HH',config,0x30,upper_base,upper_limit)
                self.assertEqual(self.mismatch(sony=config)[0],0 if accepted else 1)
                raw,result=self.prepared()
                self.assertEqual(self.augment(raw,result,sony=config)[0],0 if accepted else -2)
        for offset in range(0x30,0x34):
            config=bytearray(self.sony);config[0x1c:0x1e]=b'\x11\x11'
            config[offset]=1
            if offset in (0x30,0x31):config[offset+2]=1
            self.assertEqual(self.mismatch(sony=config),(1,(0,offset,1,0,0xff)))
        config=bytearray(self.sony);struct.pack_into('<HH',config,0x30,0xffff,0)
        for offset in (0x1c,0x1d):
            broken=bytearray(config);broken[offset]&=0xf0
            self.assertEqual(self.mismatch(sony=broken),(1,(0,offset,broken[offset],1,0x0f)))

    def test_amd_full_32bit_io_window(self):
        cases=[(0x21,0x21,0,0,True),(0xf1,0x01,0,0,True),
               (0x21,0x11,0,0,True),(0x21,0x21,0xffff,0,True),
               (0xf1,0x01,0xffff,0xffff,True),
               (0x21,0x21,0xffff,0xffff,False),
               (0xf1,0x01,0,1,False),(0x11,0x11,0,0,False),
               (0x11,0x21,0,0,False),(0x21,0x31,0,0,False),
               (0x31,0x31,0,0,False)]
        for base,limit,upper_base,upper_limit,accepted in cases:
            with self.subTest(base=base,limit=limit,upper_base=upper_base,upper_limit=upper_limit):
                config=bytearray(self.amd);config[0x1c]=base;config[0x1d]=limit
                struct.pack_into('<HH',config,0x30,upper_base,upper_limit)
                self.assertEqual(self.mismatch(amd=config)[0],0 if accepted else 1)
                raw,result=self.prepared()
                self.assertEqual(self.augment(raw,result,amd=config)[0],0 if accepted else -2)
        for offset in (0x1c,0x1d):
            config=bytearray(self.amd);config[offset]=0x11 if offset==0x1c else 0x31
            self.assertEqual(self.mismatch(amd=config),(1,(1,offset,config[offset],0x21,0xff)))
            config=bytearray(self.amd);struct.pack_into('<HH',config,0x30,0xffff,0)
            config[offset]&=0xf0
            self.assertEqual(self.mismatch(amd=config),(1,(1,offset,config[offset],1,0x0f)))

    def test_full_64bit_prefetch_windows(self):
        cases=[
            ('sony',0x980000000,0x99fffffff,True),
            ('sony',0xfffffffffff00000,0xfffff,True),
            ('amd',0xfffffffffff00000,0xfffff,True),
            ('amd',0xfffffffffff00000,0xffffffff000fffff,True),
            ('amd',0xfff00000,0x1000fffff,False),
            ('amd',0x980000000,0x99fffffff,False),
            ('sony',0x970000000,0x99fffffff,False),
            ('sony',0x980000000,0x9a00fffff,False),
            ('sony',0x10000000,0x100fffff,False),
        ]
        for bridge,base,limit,accepted in cases:
            with self.subTest(bridge=bridge,base=base,limit=limit):
                config=bytearray(getattr(self,bridge))
                struct.pack_into('<HHII',config,0x24,((base>>16)&0xfff0)|1,
                                 ((limit>>16)&0xfff0)|1,base>>32,limit>>32)
                self.assertEqual(self.mismatch(**{bridge:config})[0],0 if accepted else 1)
                raw,result=self.prepared()
                self.assertEqual(self.augment(raw,result,**{bridge:config})[0],0 if accepted else -2)
                if accepted:
                    for offset in (0x24,0x26):
                        broken=bytearray(config);broken[offset]&=0xf0
                        self.assertEqual(self.mismatch(**{bridge:broken}),
                            (1,(0 if bridge=='sony' else 1,offset,broken[offset],1,0x0f)))
        for offset in range(0x24,0x30):
            config=bytearray(self.sony);config[offset]^=0x10 if offset in (0x24,0x26) else 2 if offset==0x2c else 1
            # A high base would disable the window; keep the upper limit equal.
            if offset in (0x29,0x2a,0x2b):config[offset+4]=config[offset]
            self.assertEqual(self.mismatch(sony=config),
                (1,(0,offset,config[offset],self.sony[offset],0xff)))

    def test_helper_ignores_unobserved_bits_and_preserves_outputs(self):
        for bridge in ('sony','amd'):
            config=bytearray(getattr(self,bridge))
            for offset in range(64):
                if offset not in [0,1,2,3,4,0xe,0x18,0x19,0x1a,0x1c,0x1d]+list(range(0x20,0x34)):
                    config[offset]^=0xff
            config[4]^=0xf9;config[0xe]^=0x80
            self.assertEqual(self.mismatch(**{bridge:config}),(0,(91,92,93,94,95)))
        self.assertEqual(self.mismatch(size=63),(-1,(91,92,93,94,95)))
        for index in range(5):
            self.assertEqual(self.mismatch(null_output=index),(-1,(91,92,93,94,95)))
        for null_bridge in (0,1):
            buffers=[C.create_string_buffer(self.sony),C.create_string_buffer(self.amd)]
            buffers[null_bridge]=None
            values=[C.c_uint(91),C.c_uint(92),C.c_uint8(93),C.c_uint8(94),C.c_uint8(95)]
            self.assertEqual(self.lib.ps5_pci_profile_mismatch(*buffers,64,*[C.byref(v) for v in values]),-1)
            self.assertEqual(tuple(v.value for v in values),(91,92,93,94,95))

    def test_helper_reports_first_bridge_then_first_offset(self):
        s=bytearray(self.sony);a=bytearray(self.amd)
        s[0x30]=1;s[0x19]^=1;a[0]^=1
        self.assertEqual(self.mismatch(s,a),(1,(0,0x19,s[0x19],0x40,0xff)))

    def test_ram_and_nvs_cannot_become_mmio(self):
        for kind in (1,3,4):
            raw,result=self.prepared([(0x80000000,0x44400000,kind),(0xd0000000,0x10700000,2)],True)
            self.assertEqual(self.augment(raw,result)[0],-4)

    def test_unknown_reserved_gap_rejected(self):
        raw,result=self.prepared([(0xc5000000,4096,2)])
        self.assertEqual(self.augment(raw,result)[0],-4)

    def test_high_prefetch_ram_overlap_rejected(self):
        raw,result=self.prepared([(0x980000000,0x100000,1)])
        self.assertEqual(self.augment(raw,result)[0],-4)

    def test_missing_window_coverage_rejected(self):
        raw,result=self.prepared(replace_windows=True)
        self.assertEqual(self.augment(raw,result)[0],-4)

    def test_capacity_and_malformed_input(self):
        raw,result=self.prepared();before=result.hob_bytes
        self.assertEqual(self.augment(raw,result,capacity=len(raw))[0],-5)
        self.assertEqual(result.hob_bytes,before)
        broken=bytearray(raw);struct.pack_into('<H',broken,58,0)
        self.assertEqual(self.augment(bytes(broken),result)[0],-3)

    def test_repeat_annotation_rejected(self):
        raw,result=self.prepared();status,out=self.augment(raw,result)
        self.assertEqual(status,0)
        self.assertNotEqual(self.augment(out,result)[0],0)

if __name__=='__main__':unittest.main()
