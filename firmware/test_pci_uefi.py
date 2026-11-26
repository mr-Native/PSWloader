"""Compile the real UEFI PCI adapter against pinned EDK headers, mock raw IO."""
import ctypes as C
import base64
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
EDK = Path(os.environ.get('PS5_EDK2_ROOT', os.environ.get('PSW_WORK', '/opt/pswloader-work') + '/edk2'))

@unittest.skipUnless((EDK / 'MdePkg/Include/Base.h').is_file(), 'EDK headers required')
class UefiPciTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory()
        work = Path(cls.work.name)
        (work/'override.h').write_text('#include <Base.h>\nextern volatile UINT32 TestLock;\n#define SHARED_LOCK (&TestLock)\n')
        (work/'mock.c').write_text(r'''
#include <Base.h>
#include <Library/BaseLib.h>
#include <Library/IoLib.h>
#include <Library/SynchronizationLib.h>
volatile UINT32 TestLock;
static BOOLEAN Interrupts = TRUE;
static UINT32 (*Read)(UINTN,UINTN);
static VOID (*Write)(UINTN,UINTN,UINT32);
void MockSetup(UINT32 (*R)(UINTN,UINTN), VOID (*W)(UINTN,UINTN,UINT32)) { Read=R; Write=W; Interrupts=TRUE; TestLock=0; }
int MockState(void) { return (TestLock ? 2 : 0) | (Interrupts ? 1 : 0); }
UINT8 EFIAPI MmioRead8(UINTN A) { return (UINT8)Read(A,1); }
UINT16 EFIAPI MmioRead16(UINTN A) { return (UINT16)Read(A,2); }
UINT32 EFIAPI MmioRead32(UINTN A) { return Read(A,4); }
UINT8 EFIAPI MmioWrite8(UINTN A,UINT8 V) { Write(A,1,V); return V; }
UINT16 EFIAPI MmioWrite16(UINTN A,UINT16 V) { Write(A,2,V); return V; }
UINT32 EFIAPI MmioWrite32(UINTN A,UINT32 V) { Write(A,4,V); return V; }
BOOLEAN EFIAPI SaveAndDisableInterrupts(VOID) { BOOLEAN Old=Interrupts; Interrupts=FALSE; return Old; }
BOOLEAN EFIAPI SetInterruptState(BOOLEAN V) { Interrupts=V; return V; }
VOID EFIAPI CpuPause(VOID) { }
VOID EFIAPI MemoryFence(VOID) { }
UINT64 EFIAPI LShiftU64(UINT64 V,UINTN N) { return V << N; }
UINT32 EFIAPI InterlockedCompareExchange32(volatile UINT32 *V,UINT32 Cmp,UINT32 New) { UINT32 Old=*V; if(Old==Cmp)*V=New; return Old; }
''')
        library=work/'adapter.so'
        subprocess.run(['gcc','-shared','-fPIC','-fshort-wchar','-DPS5_EDK2','-DMDEPKG_NDEBUG',
                        '-include',str(work/'override.h'),'-I'+str(EDK/'MdePkg/Include'),
                        '-I'+str(EDK/'MdePkg/Include/X64'),'-I'+str(HERE),
                        str(HERE/'Ps5PlatformPkg/Library/Ps5PciExpressLib/Ps5PciExpressLib.c'),
                        str(HERE/'pci-transport.c'),str(work/'mock.c'),'-o',str(library)],check=True)
        cls.lib=C.CDLL(str(library))
        cls.Read=C.CFUNCTYPE(C.c_uint32,C.c_size_t,C.c_size_t)
        cls.Write=C.CFUNCTYPE(None,C.c_size_t,C.c_size_t,C.c_uint32)
        cls.lib.MockSetup.argtypes=[cls.Read,cls.Write]
        for bits,typ in [(8,C.c_uint8),(16,C.c_uint16),(32,C.c_uint32)]:
            for name,args in [('Read',[C.c_size_t]),('Write',[C.c_size_t,typ]),
                              ('BitFieldAndThenOr',[C.c_size_t,C.c_size_t,C.c_size_t,typ,typ])]:
                f=getattr(cls.lib,f'PciExpress{name}{bits}')
                f.argtypes=args; f.restype=typ
        for name in ('ReadBuffer','WriteBuffer'):
            f=getattr(cls.lib,'PciExpress'+name)
            f.argtypes=[C.c_size_t,C.c_size_t,C.c_void_p]; f.restype=C.c_size_t

    @classmethod
    def tearDownClass(cls): cls.work.cleanup()

    def setUp(self):
        self.memory={};self.calls=[]
        def read(address,width):
            self.calls.append(('r',address,width,self.lib.MockState()))
            return sum(self.memory.get(address+i,255) << (8*i) for i in range(width))
        def write(address,width,value):
            self.calls.append(('w',address,width,self.lib.MockState()))
            self.put(address,width,value)
        self.read=self.Read(read);self.write=self.Write(write)
        self.lib.MockSetup(self.read,self.write)

    def put(self,address,width,value):
        for i in range(width): self.memory[address+i]=(value>>(8*i))&255

    def bridge(self,enabled=True,identity=0x13df1022,cap_next=0):
        base=0xf0009000
        self.put(base,4,identity);self.put(base+0x19,1,0x40)
        self.put(base+6,2,0x10);self.put(base+0x34,1,0x40)
        self.put(base+0x40,1,0x10);self.put(base+0x42,2,0x42)
        self.put(base+0x68,2,0x20 if enabled else 0)
        self.put(base+0x41,1,cap_next)

    def assert_serialized(self):
        self.assertTrue(all(c[3]==2 for c in self.calls))
        self.assertEqual(self.lib.MockState(),1)

    def test_direct_amd_usb_no_invented_ari(self):
        a=0x02004010
        self.put(0xf0000000+a,4,0xdeadbeef)
        self.assertEqual(self.lib.PciExpressRead32(a),0xdeadbeef)
        self.assertFalse(any(c[0]=='w' for c in self.calls))
        self.assert_serialized()

    def test_sfc_rmw_and_bridge_checks(self):
        a=0x040080a4
        self.bridge();self.put(0xf4008150,2,0x010b)
        self.put(0xf4000f24,4,0x12345678)
        result=self.lib.PciExpressBitFieldAndThenOr32(a,8,15,0x0f,0xa0)
        self.assertEqual(result,0x1234a678)
        self.assertEqual([c[1] for c in self.calls if c[0]=='w'],
                         [0xf4000d10,0xf4000d00,0xf4000d10,0xf4000d00,0xf4000f24])
        self.assert_serialized()
        for enabled,identity in [(False,0x13df1022),(True,0xffffffff)]:
            self.setUp();self.bridge(enabled,identity);self.put(0xf0000000+a,4,0x1234)
            self.assertEqual(self.lib.PciExpressRead32(a),0x1234)
            self.assertFalse(any(c[0]=='w' for c in self.calls))

    def test_captured_live_root_ports_route_sony_and_usb_differently(self):
        audit=json.loads((HERE.parent/'evidence/windows-platform-live-20261102.json').read_text())
        for bdf, address in [('0000:00:01.1',0xf0009000),('0000:00:08.1',0xf0041000)]:
            config=base64.b64decode(audit['detail'][bdf]['config_base64'])
            for i,byte in enumerate(config): self.put(address+i,1,byte)
        # Same synthetic RID on both endpoints: only Sony's captured upstream
        # ARI-forwarding bit authorizes interpreting it as an indirect route.
        self.put(0xf4008150,2,0x010b);self.put(0xf2004150,2,0x010b)
        self.put(0xf4000f24,4,0x11223344);self.put(0xf20040a4,4,0x55667788)
        self.assertEqual(self.lib.PciExpressRead32(0x040080a4),0x11223344)
        self.assertTrue(any(c[0]=='w' and c[1]==0xf4000d00 for c in self.calls))
        self.calls.clear()
        self.assertEqual(self.lib.PciExpressRead32(0x020040a4),0x55667788)
        self.assertFalse(any(c[0]=='w' for c in self.calls))
        self.assert_serialized()

    def test_full_width_and_boundary_fields(self):
        for bits in (8,16,32):
            self.put(0xf0000100,bits//8,0)
            f=getattr(self.lib,f'PciExpressBitFieldAndThenOr{bits}')
            self.assertEqual(f(0x100,0,bits-1,0,(1<<bits)-1),(1<<bits)-1)
            self.assertEqual(f(0x100,bits-1,bits-1,0,0),(1<<(bits-1))-1)
        self.assert_serialized()

    def test_unaligned_buffer_and_range_refusal(self):
        data=(C.c_uint8*11)(*range(11));result=(C.c_uint8*11)()
        self.assertEqual(self.lib.PciExpressWriteBuffer(0x101,11,data),11)
        self.assertEqual(self.lib.PciExpressReadBuffer(0x101,11,result),11)
        self.assertEqual(list(result),list(data))
        calls=len(self.calls)
        self.assertEqual(self.lib.PciExpressReadBuffer(0xfff,2,result),0)
        self.assertEqual(self.lib.PciExpressRead32(0x08000000),0xffffffff)
        self.assertEqual(len(self.calls),calls)
        self.assert_serialized()

    def test_cyclic_capability_list_is_bounded(self):
        self.bridge();self.put(0xf0009040,1,1);self.put(0xf0009041,1,0x40)
        self.put(0xf40080a4,4,123)
        self.assertEqual(self.lib.PciExpressRead32(0x040080a4),123)
        self.assertLess(len(self.calls),110)
        self.assert_serialized()

if __name__=='__main__':unittest.main()
