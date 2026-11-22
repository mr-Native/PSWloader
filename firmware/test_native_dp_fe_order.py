"""v2.29: preserve inherited HBR3 PHY; no Table 76 DISABLE or 0x17800/0x1780c writes."""
import os, pathlib, subprocess, tempfile, unittest
R = pathlib.Path(__file__).parent.resolve()
EDK = pathlib.Path(os.environ.get('PS5_EDK2_ROOT', os.environ.get('PSW_WORK', '/opt/pswloader-work') + '/edk2'))

def body(text, start, end=None):
    chunk = text[text.index(start):]
    if end:
        chunk = chunk[:chunk.index(end)]
    return chunk

class FeOrderTests(unittest.TestCase):
    def test_source_order_preserves_inherited_phy_and_arms_before_transmitter(self):
        video = (R / 'ps5-native-dp-video.h').read_text()
        source = (R / 'ps5-native-dp-source.h').read_text()
        reset = body(video, 'STATIC EFI_STATUS Ps5NativeDpReset(', 'STATIC EFI_STATUS Ps5NativeDpVideo(')
        self.assertNotIn('Ps5NativeDpArmTiming', reset)
        self.assertNotIn('65536,76,', reset)
        self.assertNotIn('Ps5AtomExecute', reset)
        self.assertIn('inherited PHY preserved readback=%x', reset)
        arm = body(video, 'STATIC EFI_STATUS Ps5NativeDpArmTiming(', 'STATIC EFI_STATUS Ps5NativeDpReset(')
        self.assertIn('UINT32 Clock[4]={M->t.clock_khz*10U,0x1e0b,0,0};', arm)   # pixel clock from the mode (1485000 for CEA16)
        self.assertIn('ps5_dcn201_pixel_dto', arm)
        self.assertIn('ps5_dcn201_start_solid_mode', arm)   # mode-generic since display modes (1080p = one mode)
        self.assertIn('PS5_PAINT?1023:0,0,0', arm)   # v3.30: red DPG only with --diag-graphics, else black
        self.assertIn('0x154a0', arm)
        self.assertIn('65536,12,', arm)
        self.assertNotIn('65536,4,', arm)
        self.assertNotIn('0x580', arm)
        self.assertNotIn('ps5_dcn201_symbol_clock', arm)
        vid = body(video, 'STATIC EFI_STATUS Ps5NativeDpVideo(')
        self.assertNotIn('Ps5AtomExecute', vid)
        self.assertNotIn('ENABLE_DIG', vid)
        self.assertNotIn('STREAM_SETUP', vid)
        self.assertNotIn('ps5_dcn201_start_solid_mode', vid)
        self.assertNotIn('ps5_dcn201_pixel_dto', vid)
        self.assertNotIn('ps5_dcn201_symbol_clock', vid)
        self.assertNotIn('0x580', vid)
        self.assertIn('ps5_dcn201_stream_setup', vid)
        self.assertIn('(FeDomain&0x07000007U)!=0x01000000U', vid)
        self.assertIn('0x154a0', vid)
        self.assertIn('0x155c0', vid)
        self.assertIn('VideoRead(Pci,0x177A0', vid)
        self.assertNotIn('VideoWrite(Pci,0x177A0', vid)
        self.assertNotIn('VideoWrite(Pci,0x155c0', vid)
        self.assertNotIn('FE domain dead after DIG_ENABLE-then-setup', vid)
        self.assertIn('FE domain dead after transmitter ENABLE', vid)
        self.assertNotIn('0x17814', vid)
        state = body(video, 'STATIC VOID VideoState', 'STATIC EFI_STATUS NativeVideoStage')
        for off in ('0x177A0', '0x17800', '0x1780c', '0x17814', '0x17818', '0x1782C', '0x580', '0x154B8', '0x15760', '0x15720'):
            self.assertIn(off, state)
        self.assertEqual(state.count('0x154B8'), 1)
        self.assertEqual(state.count('0x15760'), 1)
        self.assertEqual(state.count('0x15720'), 1)
        self.assertNotIn('VideoWrite', state)
        fn = body(source, 'STATIC EFI_STATUS Ps5NativeDpSource')
        reset_at = fn.index('Status=Ps5NativeDpReset(Pci,Rom,&Link);')
        arm_at = fn.index('Status=Ps5NativeDpArmTiming(Pci,Rom);')
        init_at = fn.index('Ps5AtomExecute(Pci,Rom,65536,76,Params,8)')
        plan_at = fn.index('Plan[N].offset')
        enable_params = fn.index('ps5_dcn201_transmitter_params(&Link,1,4,81000,Params)')
        enable_at = fn.index('Ps5AtomExecute(Pci,Rom,65536,76,Params,8)', enable_params)
        self.assertLess(reset_at, arm_at)
        self.assertLess(arm_at, init_at)
        self.assertLess(init_at, plan_at)
        self.assertLess(plan_at, enable_at)
        self.assertGreater(fn.rfind('#ifdef PS5_NATIVE_DP_VIDEO', 0, arm_at), fn.rfind('#endif', 0, arm_at))
        self.assertNotIn('65536,12,', source)
        self.assertNotIn('65536,4,', source)
        blocks, bare, rest = '', '', fn
        while '#ifdef PS5_NATIVE_DP_VIDEO' in rest:
            pre, rest = rest.split('#ifdef PS5_NATIVE_DP_VIDEO', 1)
            block, rest = rest.split('#endif', 1)
            blocks += block
            bare += pre
            self.assertNotIn('0x17800', pre)
            self.assertNotIn('0x1780c', pre)
        bare += rest
        self.assertNotIn('0x17800', bare)
        self.assertNotIn('0x1780c', bare)
        self.assertIn('Ps5AtomExecute(Pci,Rom,65536,76,Params,8)', bare)
        self.assertNotIn('65536,4,', bare)
        self.assertEqual(blocks.count('Mem.Write(Pci,EfiPciIoWidthUint32,5,0x17800'), 0)
        self.assertEqual(blocks.count('Mem.Write(Pci,EfiPciIoWidthUint32,5,0x1780c'), 0)
        self.assertIn('0x580', blocks)
        self.assertIn('0x155c0', blocks)
        pre_log = fn.index('PHY_CNTL0 17800=')
        after = fn.index('after ENABLE 17800=')
        after_ifdef = fn.rfind('#ifdef PS5_NATIVE_DP_VIDEO', 0, after)
        after_end = fn.index('#endif', after)
        after_block = fn[after_ifdef:after_end]
        self.assertLess(arm_at, pre_log)
        self.assertLess(pre_log, enable_at)
        self.assertLess(enable_at, after)
        self.assertIn('580=', fn[after:after+180])
        self.assertIn('155c0=', fn[after:after+180])
        self.assertIn('0x580', after_block)
        for line in source.splitlines():
            if 'Mem.Write' in line:
                self.assertNotIn('0x155c0', line)
                self.assertNotIn('0x177A0', line)
                self.assertNotIn('0x580', line)
                self.assertNotIn('0x17800', line)
                self.assertNotIn('0x1780c', line)
        self.assertEqual(bare.count('Ps5AtomExecute(Pci,Rom,65536,76,Params,8)'), 2)
        self.assertNotIn('65536,12,', bare)
        self.assertNotIn('65536,4,', bare)

    @unittest.skipUnless((EDK / 'MdePkg/Include/Uefi.h').exists(), 'EDK headers required')
    def test_compiled_video_path_arms_timing_before_enable_without_table4_or_580(self):
        with tempfile.TemporaryDirectory() as work:
            p = pathlib.Path(work)
            (p / 'test.c').write_text(r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#undef NULL
#define PS5_NATIVE_DP_VIDEO 1
#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DebugLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
/* r5.1x helpers used by ps5-native-dp-video.h (frame-rate check, mode variable) */
UINT64 EFIAPI AsmReadTsc(VOID){static UINT64 T;return T+=15963000ULL;}   /* monotonic, 10 ms per read */
UINT64 EFIAPI MultU64x32(UINT64 M,UINT32 N){return M*N;}
UINT64 EFIAPI DivU64x64Remainder(UINT64 A,UINT64 B,UINT64 *R){if(R)*R=B?A%B:0;return B?A/B:0;}
static EFI_STATUS EFIAPI MockSetVariable(CHAR16 *N,EFI_GUID *G,UINT32 A,UINTN S,VOID *D){(void)N;(void)G;(void)A;(void)S;(void)D;return EFI_SUCCESS;}
static EFI_RUNTIME_SERVICES Rt={.SetVariable=MockSetVariable}; EFI_RUNTIME_SERVICES *gRT=&Rt;
UINT32 EFIAPI MmioRead32(UINTN A){(void)A;return 0;}
static void Ps5GpuSnapshot(void){}
#include "ps5-native-dp-source.h"
static EFI_BOOT_SERVICES Bs; EFI_BOOT_SERVICES *gBS=&Bs;
static EFI_STATUS EFIAPI MockGetBarAttributes(EFI_PCI_IO_PROTOCOL *P,UINT8 B,UINT64 *S,VOID **R){(void)P;(void)B;(void)S;(void)R;return EFI_UNSUPPORTED;}   /* r5.14 refresh measurement: optional */
EFI_GUID gEfiPciIoProtocolGuid={0};
static EFI_PCI_IO_PROTOCOL Pci;
static PS5_HDMI_VIDEO_PROTOCOL VideoProto;
enum {EV_ATOM=1, EV_W580=2, EV_W154=3, EV_W178=4, EV_W155=5, EV_W177=6, EV_W17C=7};
static struct {int kind; unsigned table, action; uint32_t value;} Ev[256];
static unsigned Nev, Nstages, Rn;
static UINTN Stages[16];
static UINT64 Roff[1024];
static UINT32 Rval[1024];
static void Rec(int k, unsigned t, unsigned a, uint32_t v){assert(Nev<256); Ev[Nev].kind=k; Ev[Nev].table=t; Ev[Nev].action=a; Ev[Nev].value=v; Nev++;}
static int Find(UINT64 O){for(unsigned i=0;i<Rn;i++) if(Roff[i]==O) return (int)i; return -1;}
static void Store(UINT64 O, UINT32 V){int i=Find(O); if(i<0){assert(Rn<1024); i=(int)Rn++; Roff[i]=O;} Rval[i]=V;}
static UINT32 Load(UINT64 O){int i=Find(O); if(i>=0) return Rval[i]; if(O==0x14004||O==0x14204) return 0x80000110; if(O==0x14f50) return 0x010ff012; if(O==0x6c9fc) return 6000; if(O==0x155c0) return 0x101; return 0;}
VOID *EFIAPI ZeroMem(VOID *P, UINTN N){for(UINTN I=0;I<N;I++)((UINT8*)P)[I]=0; return P;}
VOID *EFIAPI AllocateZeroPool(UINTN N){return calloc(1,N);}
VOID EFIAPI FreePool(VOID *P){free(P);}
static EFI_STATUS EFIAPI Crc(VOID *P, UINTN N, UINT32 *C){(void)P; *C=N==0xd800?0xd5357201:0; return EFI_SUCCESS;}   /* ROM check (0xd800 bytes) + r5.14 mode variable */
static EFI_STATUS EFIAPI Locate(EFI_LOCATE_SEARCH_TYPE T, EFI_GUID *G, VOID *K, UINTN *N, EFI_HANDLE **H){(void)T;(void)G;(void)K; *N=1; *H=malloc(sizeof(EFI_HANDLE)); (*H)[0]=(VOID*)1; return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Handle(EFI_HANDLE H, EFI_GUID *G, VOID **P){(void)H;(void)G; *P=&Pci; return EFI_SUCCESS;}
static EFI_STATUS EFIAPI LocateProtocol(EFI_GUID *G, VOID *R, VOID **I){(void)G;(void)R; *I=&VideoProto; return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Stall(UINTN Us){(void)Us; return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Stage(UINTN N){assert(Nstages<16); Stages[Nstages++]=N; return EFI_SUCCESS;}
static EFI_STATUS EFIAPI ConfigRead(EFI_PCI_IO_PROTOCOL *P, EFI_PCI_IO_PROTOCOL_WIDTH W, UINT32 O, UINTN N, VOID *V){
 (void)P;(void)W; assert(N==1); if(O==0)*(UINT32*)V=0x13fc1002; else {assert(O==4); *(UINT16*)V=2;} return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Read(EFI_PCI_IO_PROTOCOL *P, EFI_PCI_IO_PROTOCOL_WIDTH W, UINT8 B, UINT64 O, UINTN N, VOID *V){
 (void)P;(void)W; assert(B==5 && N==1); *(UINT32*)V=Load(O); return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Write(EFI_PCI_IO_PROTOCOL *P, EFI_PCI_IO_PROTOCOL_WIDTH W, UINT8 B, UINT64 O, UINTN N, VOID *V){
 (void)P;(void)W; assert(B==5 && N==1); UINT32 Value=*(UINT32*)V;
 if(O==0x580) Rec(EV_W580,0,0,Value);
 if(O==0x154a0) Rec(EV_W154,0,0,Value);
 if(O==0x17800) Rec(EV_W178,0,0,Value);
 if(O==0x1780c) Rec(EV_W17C,0,0,Value);
 if(O==0x155c0) Rec(EV_W155,0,0,Value);
 if(O==0x177A0) Rec(EV_W177,0,0,Value);
 if(O==0x13e3c && (Value&3)==3) Value|=4;
 if(O==0x14118 && (Value&3)==3) Value|=0x100;
 if(O==0x13530 && (Value&1)==1) Value|=2;
 if(O==0x15730){ if(Value&1) Value|=0x10000; else Value&=~0x10000U; }
 Store(O, Value); return EFI_SUCCESS;
}
EFI_STATUS Ps5AtomExecute(EFI_PCI_IO_PROTOCOL *P, const UINT8 *Rom, UINTN Bytes, UINTN Table, UINT32 *Params, UINTN N){
 (void)P;(void)Rom; assert(Bytes==65536);
 unsigned action=Table==76?((Params[0]>>8)&255):0;
 Rec(EV_ATOM, (unsigned)Table, action, Params?Params[1]:0);
 if(Table==12){assert(N==4 && Params[0]==1485000 && Params[1]==0x1e0b && Params[2]==0 && Params[3]==0); return EFI_SUCCESS;}
 assert(Table==76 && N==8);
 if(action==1){assert(Params[1]==81000 && ((Params[0]>>24)&255)==4); Store(0x155bc, 0);}
 return EFI_SUCCESS;
}
static int count_kind(int kind){int n=0; for(unsigned i=0;i<Nev;i++) if(Ev[i].kind==kind) n++; return n;}
static int FindEv(int kind, unsigned table, unsigned action){
 for(unsigned i=0;i<Nev;i++) if(Ev[i].kind==kind && (kind!=EV_ATOM || (Ev[i].table==table && Ev[i].action==action))) return (int)i;
 return -1;
}
int main(void){
 void *Rom=mmap((void*)0xc0000,65536,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0);
 assert(Rom==(void*)0xc0000);
 { UINT8 *R=(UINT8*)Rom; R[0]=0x55; R[1]=0xaa; R[2]=108; R[0x48]=0x00; R[0x49]=0x01; R[0x104]='A'; R[0x105]='T'; R[0x106]='O'; R[0x107]='M'; }
 Bs.CalculateCrc32=Crc; Bs.LocateHandleBuffer=Locate; Bs.HandleProtocol=Handle; Bs.LocateProtocol=LocateProtocol; Bs.Stall=Stall;
 VideoProto.Stage=Stage; VideoProto.Read=NULL; Pci.GetBarAttributes=MockGetBarAttributes;
 Pci.Pci.Read=ConfigRead; Pci.Mem.Read=Read; Pci.Mem.Write=Write;
 EFI_STATUS Status=Ps5NativeDpSource();
 int dis=FindEv(EV_ATOM,76,0), arm=FindEv(EV_ATOM,12,0), en=FindEv(EV_ATOM,76,1), init=FindEv(EV_ATOM,76,7), dig=-1, table4=0, w580=0;
 for(unsigned i=0;i<Nev;i++){
  if(Ev[i].kind==EV_ATOM && Ev[i].table==4) table4++;
  if(Ev[i].kind==EV_W580) w580++;
  if(dig<0 && Ev[i].kind==EV_W154 && (Ev[i].value&7)==0) dig=(int)i;
 }
 const UINTN expect[]={6,7,8,9,10,0,1,2,3,4,5,11};
 int ok=Status==EFI_SUCCESS && dis<0 && arm>=0 && en>=0 && init>=0 && dig>=0 && arm<init && dig<init && init<en && !table4 && !w580 && Nstages==14;
 if(!ok){
  fprintf(stderr,"status=%lx dis=%d arm=%d init=%d dig=%d en=%d table4=%d w580=%d stages=%u\n",(unsigned long)Status,dis,arm,init,dig,en,table4,w580,Nstages);
  for(unsigned i=0;i<Nev;i++) fprintf(stderr,"ev %u kind=%d table=%u action=%u value=%x\n",i,Ev[i].kind,Ev[i].table,Ev[i].action,Ev[i].value);
  for(unsigned i=0;i<Nstages;i++) fprintf(stderr,"stage %u\n",(unsigned)Stages[i]);
 }
 assert(ok);
 for(unsigned i=0;i<12;i++) assert(Stages[i]==expect[i]);
 assert((Load(0x154a0)&0x01000000)==0);
 assert((Load(0x15724)&0x07000007)==0x01000000);
 assert((Load(0x155bc)&0x100)==0x100);
 assert(count_kind(EV_W178)==0 && count_kind(EV_W17C)==0 && count_kind(EV_W155)==0 && count_kind(EV_W580)==0 && count_kind(EV_W177)==0);
 munmap(Rom,65536); return 0;
}
''')
            subprocess.run(['gcc','-Wall','-Wno-unused-parameter','-fshort-wchar','-DMDEPKG_NDEBUG','-I'+str(R),'-I'+str(R/'atom-host'),'-I'+str(EDK/'MdePkg/Include'),'-I'+str(EDK/'MdePkg/Include/X64'),str(p/'test.c'),str(R/'dcn201-link.c'),str(R/'dcn201-stream.c'),str(R/'dcn201-timing.c'),str(R/'dcn201-test-pattern.c'),str(R/'dcn201-aux.c'),str(R/'ps5-display-mode.c'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)
