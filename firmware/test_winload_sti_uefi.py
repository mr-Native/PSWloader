#!/usr/bin/env python3
"""v2.0 (v3.36): host test of the REAL UEFI code in ps5-native-gop.h (Ps5IsWinloadPeAt, Ps5WinloadBase,
Ps5PatchStiBySig): the functions are extracted verbatim, compiled with EDK-type stubs, and run against winload
26100.9444 and 26100.9711 mapped at UEFI-like physical addresses with a PROT_NONE guard page directly below the image
(any read outside winload = SIGSEGV = test failure). Checks: the base is found from an ExitBootServices return address
anywhere inside the image (first .text page .. last page), no return address -> 0, a return address outside the image
is rejected, the result is cached; exactly the sti `fb` byte becomes `c3`, nothing else changes, a second run writes
nothing. v1.0 tested the v3.31 fast path/stack scan, which HW trial v339 proved to hang on 9711."""
import os
import ctypes, re, shutil, struct, subprocess, tempfile, unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
IMGS = {'9444': (Path(os.environ.get('PSW_WORK', '/opt/pswloader-work') + '/windows-reference/winload.efi'), 0x2a9220),
        '9711': (Path(os.environ.get('PSW_WORK', '/opt/pswloader-work') + '/update-diag-readback/current/winload.efi'), 0x2a9dc0)}
STUB = r'''
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
typedef uint8_t UINT8; typedef uint16_t UINT16; typedef uint32_t UINT32; typedef uint64_t UINT64;
typedef uint64_t UINTN; typedef unsigned char BOOLEAN;
#define STATIC static
#define CONST const
#define TRUE 1
#define FALSE 0
#define VOID void
#define BIT16 (1u<<16)
static UINTN AsmReadCr0(void){return 0;} static void AsmWriteCr0(UINTN x){(void)x;} static void AsmWbinvd(void){}
static uint32_t mPs5WbCall; static volatile uint64_t mPs5EbsRet; static uint64_t mPs5WlBaseCache;
static void Ps5DiagWb(UINT32 c,UINTN s,UINT32 col){(void)c;(void)s;(void)col;}
static void Ps5DiagBaseBits(UINT32 c,UINT64 b){(void)c;(void)b;}
#include "extracted.inc"
int map_at(uint64_t a, uint64_t len){void*p=mmap((void*)a,len,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);return p==(void*)a?0:-1;}
int guard_at(uint64_t a){void*p=mmap((void*)a,4096,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);return p==(void*)a?0:-1;}
int unmap_at(uint64_t a, uint64_t len){return munmap((void*)a,len);}
int t_ispe(uint64_t b, uint64_t ret){return Ps5IsWinloadPeAt(b,ret);}
void t_set_ret(uint64_t r){mPs5EbsRet=r;mPs5WlBaseCache=0;}
void t_anchor_only(uint64_t r){mPs5EbsRet=r;}
uint64_t t_base(void){return Ps5WinloadBase();}
uint32_t t_patch(uint64_t b){return Ps5PatchStiBySig(b);}
'''

def memimage(fn):
    b = fn.read_bytes(); pe = struct.unpack_from('<I', b, 0x3c)[0]
    soi = struct.unpack_from('<I', b, pe+24+56)[0]; hdr = struct.unpack_from('<I', b, pe+24+60)[0]
    img = bytearray(soi); img[:hdr] = b[:hdr]
    so = pe+24+struct.unpack_from('<H', b, pe+20)[0]; n = struct.unpack_from('<H', b, pe+6)[0]
    for i in range(n):
        base = so+i*40; v = struct.unpack_from('<I', b, base+12)[0]
        rp = struct.unpack_from('<I', b, base+20)[0]; rs = struct.unpack_from('<I', b, base+16)[0]
        img[v:v+rs] = b[rp:rp+rs]
    return bytes(img)

@unittest.skipUnless(shutil.which('cc') and all(p.exists() for p, _ in IMGS.values()), 'no cc / images')
class StiUefi(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        src = (HERE/'ps5-native-gop.h').read_text()
        a = src.index('STATIC BOOLEAN Ps5IsWinloadPeAt(UINT64 B,UINT64 Ret)')
        e = src.index('return 1;\n}', src.index('STATIC UINT32 Ps5PatchStiBySig')) + len('return 1;\n}')
        cls.tmp = tempfile.TemporaryDirectory(); d = Path(cls.tmp.name)
        (d/'extracted.inc').write_text(src[a:e] + '\n')
        (d/'h.c').write_text(STUB)
        subprocess.run(['cc','-shared','-fPIC','-O1','-Wall','-Werror','-Wno-unused-function','-o',str(d/'h.so'),str(d/'h.c')], check=True, cwd=d)
        cls.lib = ctypes.CDLL(str(d/'h.so'))
        cls.lib.t_base.restype = ctypes.c_uint64; cls.lib.t_patch.restype = ctypes.c_uint32
        for f in ('map_at','unmap_at','guard_at','t_ispe','t_patch','t_set_ret','t_anchor_only'): getattr(cls.lib, f).argtypes = None

    def run_at(self, key, addr):
        fn, sti = IMGS[key]; img = memimage(fn); n = len(img); L = self.lib; u = ctypes.c_uint64
        self.assertEqual(L.guard_at(u(addr - 0x1000)), 0, 'guard mmap')
        self.assertEqual(L.map_at(u(addr), u(n)), 0, 'mmap')
        try:
            ctypes.memmove(addr, img, n)
            self.assertEqual(L.t_ispe(u(addr), u(addr + sti)), 1)
            self.assertEqual(L.t_ispe(u(addr + 0x1000), u(addr + sti)), 0)          # not a PE base
            self.assertEqual(L.t_ispe(u(addr), u(addr + n)), 0)                     # caller outside the image
            for ret in (addr + 0x1000, addr + sti, addr + n - 1):                    # first .text page .. last byte
                L.t_set_ret(u(ret)); self.assertEqual(L.t_base(), addr, hex(ret))
            L.t_set_ret(u(0)); self.assertEqual(L.t_base(), 0)                       # no EBS caller -> no guessing
            L.t_set_ret(u(addr + sti)); self.assertEqual(L.t_base(), addr)
            L.t_anchor_only(u(0x1000))                                                # cache: later bogus anchor
            self.assertEqual(L.t_base(), addr)                                         # ... keeps the resolved base
            self.assertEqual(L.t_patch(u(addr)), 1)
            after = ctypes.string_at(addr, n)
            diff = [i for i in range(n) if after[i] != img[i]]
            self.assertEqual(diff, [sti], f'{key}: changed bytes {[hex(x) for x in diff[:5]]}')
            self.assertEqual(after[sti:sti+2], b'\xc3\xc3')
            self.assertEqual(L.t_patch(u(addr)), 0)                                  # idempotent: nothing found again
        finally:
            L.unmap_at(u(addr), u(n)); L.unmap_at(u(addr - 0x1000), u(0x1000))

    def test_9444_logged_base(self): self.run_at('9444', 0x44dec000)
    def test_9711_logged_base(self): self.run_at('9711', 0x44dec000)
    def test_9711_moved_base(self): self.run_at('9711', 0x45200000)
    def test_9711_high_base(self): self.run_at('9711', 0x5e104000)

if __name__ == '__main__':
    unittest.main()
