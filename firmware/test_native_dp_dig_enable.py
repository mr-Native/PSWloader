"""v2.19: no 0x155c0 write; FE domain must stick after transmitter ENABLE."""
import os, pathlib, subprocess, tempfile, unittest
R = pathlib.Path(__file__).parent.resolve()
EDK = pathlib.Path(os.environ.get('PS5_EDK2_ROOT', os.environ.get('PSW_WORK', '/opt/pswloader-work') + '/edk2'))

class DigEnableTests(unittest.TestCase):
    def test_video_does_not_write_dig_enable_and_aborts_before_unblank_if_fe_dead(self):
        vid = (R / 'ps5-native-dp-video.h').read_text()
        vid = vid[vid.index('STATIC EFI_STATUS Ps5NativeDpVideo('):]
        self.assertIn('FE domain dead after transmitter ENABLE', vid)
        self.assertNotIn('FE domain dead after DIG_ENABLE-then-setup', vid)
        self.assertNotIn('DIG_ENABLE cleared FE domain', vid)
        self.assertNotIn('ENABLE_DIG', vid)
        self.assertNotIn('0x580', vid)
        self.assertNotIn('VideoWrite(Pci,0x177A0', vid)
        self.assertNotIn('VideoWrite(Pci,0x155c0', vid)
        self.assertNotIn('Ps5AtomExecute', vid)
        self.assertNotIn('65536,4,', vid)
        stages = vid.index('const UINTN Before[]={9,10}')
        setup = vid.index('ps5_dcn201_stream_setup')
        fe = vid.index('(FeDomain&0x07000007U)!=0x01000000U')
        dead = vid.index('FE domain dead after transmitter ENABLE')
        stage = vid.index('for(UINTN I=0;I<=5;I++)')
        unblank = vid.index('ps5_dcn201_stream_unblank')
        waited = vid.index('if(!Result && !(Stream&0x10000))Result=-3;')
        stage11 = vid.index('NativeVideoStage(Video,11)')
        self.assertEqual(vid.count('NativeVideoStage(Video,11)'), 1)
        self.assertLess(stages, setup)
        self.assertLess(setup, fe)
        self.assertLess(fe, dead)
        self.assertLess(dead, unblank)
        self.assertLess(unblank, waited)
        self.assertLess(waited, stage)
        err = vid.index('if(EFI_ERROR(Status))return Status;', stage)
        self.assertLess(stage, err)
        self.assertLess(err, stage11)
        self.assertIn('for(UINTN I=0;I<1000;I++)', vid)
        self.assertIn('VideoRead(Pci,0x177A0,&Sym)', vid)
        self.assertIn('VideoRead(Pci,0x155c0,&Backend)', vid)

    def test_v225_reads_60f9_after_stages_without_writing_it(self):
        vid = (R / 'ps5-native-dp-video.h').read_text()
        self.assertEqual(vid.count('PS5 DP video: 60f9 after stage=%u value=%x status=%r'), 1)
        self.assertNotIn('VideoWrite(Pci,0x60f9', vid)
        body = vid[vid.index('STATIC EFI_STATUS Ps5NativeDpVideo('):]
        stages_loop = body.index('for(UINTN I=0;I<=5;I++)')
        sample_i = body.index('Sample60f9(Video,I)', stages_loop)
        stage11 = body.index('NativeVideoStage(Video,11)')
        sample11 = body.index('Sample60f9(Video,11)', stage11)
        self.assertLess(stages_loop, sample_i)
        self.assertLess(sample_i, stage11)
        self.assertLess(stage11, sample11)
        self.assertLess(sample11, body.index('if(EFI_ERROR(Status))return Status;', sample11))

    @unittest.skipUnless((EDK / 'MdePkg/Include/Uefi.h').exists(), 'EDK headers required')
    def test_compiled_video_requires_fe_domain_and_never_writes_155c0(self):
        with tempfile.TemporaryDirectory() as work:
            p = pathlib.Path(work)
            (p / 'test.c').write_text(r"""

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#undef NULL
#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DebugLib.h>
#include <Protocol/PciIo.h>
#include "dcn201-link.h"
#include <Library/BaseLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
/* r5.1x helpers used by ps5-native-dp-video.h (frame-rate check, mode variable, resource list) */
UINT64 EFIAPI AsmReadTsc(VOID){static UINT64 T;return T+=15963000ULL;}   /* monotonic, 10 ms per read */
UINT64 EFIAPI MultU64x32(UINT64 M,UINT32 N){return M*N;}
UINT64 EFIAPI DivU64x64Remainder(UINT64 A,UINT64 B,UINT64 *R){if(R)*R=B?A%B:0;return B?A/B:0;}
VOID EFIAPI FreePool(VOID *P){(void)P;}
UINT32 EFIAPI MmioRead32(UINTN A){(void)A;return 0;}
static EFI_STATUS EFIAPI MockSetVariable(CHAR16 *N,EFI_GUID *G,UINT32 A,UINTN S,VOID *D){(void)N;(void)G;(void)A;(void)S;(void)D;return EFI_SUCCESS;}
static EFI_RUNTIME_SERVICES Rt={.SetVariable=MockSetVariable}; EFI_RUNTIME_SERVICES *gRT=&Rt;
EFI_STATUS Ps5AtomExecute(EFI_PCI_IO_PROTOCOL *Pci,const UINT8 *Rom,UINTN RomBytes,UINTN Table,UINT32 *Params,UINTN ParamDwords);
static void Ps5GpuSnapshot(void){}
#include "ps5-native-dp-video.h"
static EFI_BOOT_SERVICES Bs; EFI_BOOT_SERVICES *gBS=&Bs;
static EFI_STATUS EFIAPI MockCalculateCrc(VOID *D,UINTN S,UINT32 *C){UINT32 X=0;for(UINTN I=0;I<S;I++)X=X*31u+((UINT8*)D)[I];*C=X;return EFI_SUCCESS;}   /* r5.14 mode variable */
static EFI_STATUS EFIAPI MockHandleProtocol(EFI_HANDLE H,EFI_GUID *G,VOID **I){(void)H;(void)G;(void)I;return EFI_UNSUPPORTED;}
static EFI_STATUS EFIAPI MockLocateHandleBuffer(EFI_LOCATE_SEARCH_TYPE T,EFI_GUID *G,VOID *K,UINTN *N,EFI_HANDLE **B){(void)T;(void)G;(void)K;*N=0;*B=NULL;return EFI_NOT_FOUND;}
static EFI_STATUS EFIAPI MockGetBarAttributes(EFI_PCI_IO_PROTOCOL *P,UINT8 B,UINT64 *S,VOID **R){(void)P;(void)B;(void)S;(void)R;return EFI_UNSUPPORTED;}   /* r5.14 refresh measurement: optional */
static EFI_PCI_IO_PROTOCOL Pci;
static PS5_HDMI_VIDEO_PROTOCOL VideoProto;
enum {MODE_GOOD=0, MODE_DEAD=1, EV_R=1, EV_W=2};
static int mode;
static struct {int kind; uint32_t off, value;} Ev[4096];
static unsigned Nev, Rn;
static UINT64 Roff[256];
static UINT32 Rval[256];
static UINTN Nstages, Stages[16];
static unsigned AtomCalls;
static int Find(uint32_t O){for(unsigned i=0;i<Rn;i++) if(Roff[i]==O) return (int)i; return -1;}
static void Store(uint32_t O, uint32_t V){int i=Find(O); if(i<0){assert(Rn<256); i=(int)Rn++; Roff[i]=O;} Rval[i]=V;}
static uint32_t Stored(uint32_t O){int i=Find(O); return i<0 ? 0xffffffffu : Rval[i];}
static uint32_t Load(uint32_t O){
 if(mode==MODE_DEAD && O==0x15724) return 0;
 int i=Find(O); if(i>=0) return Rval[i];
 if(O==0x155c0) return 0x100;
 if(O==0x14004) return 1;
 return 0;
}
static void Rec(int k, uint32_t o, uint32_t v){assert(Nev<4096); Ev[Nev].kind=k; Ev[Nev].off=o; Ev[Nev].value=v; Nev++;}
static EFI_STATUS EFIAPI Stall(UINTN Us){(void)Us; return EFI_SUCCESS;}
static EFI_STATUS EFIAPI LocateProtocol(EFI_GUID *G, VOID *R, VOID **I){(void)G;(void)R; *I=&VideoProto; return EFI_SUCCESS;}
static int StageAt[16];
static EFI_STATUS StageStatus;
static EFI_STATUS EFIAPI Stage(UINTN N){assert(Nstages<16); StageAt[Nstages]=(int)Nev; Stages[Nstages++]=N; return N==1?StageStatus:EFI_SUCCESS;}
static EFI_STATUS EFIAPI Read(EFI_PCI_IO_PROTOCOL *P, EFI_PCI_IO_PROTOCOL_WIDTH W, UINT8 B, UINT64 O, UINTN N, VOID *V){
 (void)P;(void)W; assert(B==5 && N==1); uint32_t Value=Load((uint32_t)O); Rec(EV_R,(uint32_t)O,Value); *(UINT32*)V=Value; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Write(EFI_PCI_IO_PROTOCOL *P, EFI_PCI_IO_PROTOCOL_WIDTH W, UINT8 B, UINT64 O, UINTN N, VOID *V){
 (void)P;(void)W; assert(B==5 && N==1); uint32_t Value=*(UINT32*)V; Rec(EV_W,(uint32_t)O,Value);
 if(O==0x15730){ if(Value&1) Value|=0x10000; else Value&=~0x10000U; }
 Store((uint32_t)O, Value);
 return EFI_SUCCESS;
}
EFI_STATUS Ps5AtomExecute(EFI_PCI_IO_PROTOCOL *P, const UINT8 *Rom, UINTN Bytes, UINTN Table, UINT32 *Params, UINTN Count){
 (void)P;(void)Rom;(void)Bytes;(void)Table;(void)Params;(void)Count; AtomCalls++; return EFI_DEVICE_ERROR;
}
static int wrote(uint32_t off){for(unsigned i=0;i<Nev;i++) if(Ev[i].kind==EV_W && Ev[i].off==off) return 1; return 0;}
static int first_write(uint32_t off){
 for(unsigned i=0;i<Nev;i++) if(Ev[i].kind==EV_W && Ev[i].off==off) return (int)i;
 return -1;
}
static int fe_read_after(int after, uint32_t masked){
 for(unsigned i=(unsigned)after+1;i<Nev;i++) if(Ev[i].kind==EV_R && Ev[i].off==0x15724 && (Ev[i].value&0x07000007)==masked) return (int)i;
 return -1;
}
static int unblank_after(int after){
 for(unsigned i=(unsigned)after+1;i<Nev;i++) if(Ev[i].kind==EV_W && (Ev[i].off==0x14008 || Ev[i].off==0x15748)) return (int)i;
 return -1;
}
static int unblanked(void){return wrote(0x14008) || wrote(0x15748);}
static void reset(void){Nev=Rn=Nstages=AtomCalls=0; StageStatus=EFI_SUCCESS;}
static int expect_prefix(void){return Nstages>=2 && Stages[0]==9 && Stages[1]==10;}
static int banned(void){return wrote(0x580) || wrote(0x177A0) || wrote(0x155c0) || AtomCalls;}
int main(void){
 Bs.LocateProtocol=LocateProtocol; Bs.Stall=Stall; VideoProto.Stage=Stage; VideoProto.Read=NULL; Bs.CalculateCrc32=MockCalculateCrc; Pci.GetBarAttributes=MockGetBarAttributes; Bs.HandleProtocol=MockHandleProtocol; Bs.LocateHandleBuffer=MockLocateHandleBuffer;
 Pci.Mem.Read=Read; Pci.Mem.Write=Write;
 int rc=0;
 mode=MODE_DEAD; reset();
 EFI_STATUS dead=Ps5NativeDpVideo(&Pci,NULL,0);
 int setup=first_write(0x15724), fe=setup<0?-1:fe_read_after(setup,0);
 if(!(dead==EFI_DEVICE_ERROR && !banned() && !unblanked() && expect_prefix() && Nstages==2 && setup>=0 && fe>setup && (Stored(0x15724)&0x07000007)==0x01000000)){
  fprintf(stderr,"dead status=%lx banned=%d unblank=%d stored=%x stages=%u idx=%d/%d\n",(unsigned long)dead,banned(),unblanked(),Stored(0x15724),(unsigned)Nstages,setup,fe); rc=1;
 }
 mode=MODE_GOOD; reset();
 EFI_STATUS good=Ps5NativeDpVideo(&Pci,NULL,0);
 setup=first_write(0x15724); fe=setup<0?-1:fe_read_after(setup,0x01000000); int blank=fe<0?-1:unblank_after(fe);
 int stage1=-1, reads16=-1;
 for(unsigned i=0;i<Nstages;i++) if(Stages[i]==1) stage1=StageAt[i];
 for(unsigned i=(unsigned)(blank+1);i<Nev && reads16<0;i++) if(Ev[i].kind==EV_R && Ev[i].off==0x15730 && (Ev[i].value&0x10000)) reads16=(int)i;
 if(!(good==EFI_SUCCESS && !banned() && setup>=0 && fe>setup && blank>fe && reads16>blank && stage1>reads16 && (Load(0x15724)&0x07000007)==0x01000000 && Nstages==11 && Stages[3]==1 && Stages[8]==11 && Stages[9]==12 && Stages[10]==13)){
  fprintf(stderr,"good status=%lx banned=%d fe=%x stages=%u idx=%d/%d/%d/%d/%d\n",(unsigned long)good,banned(),Load(0x15724),(unsigned)Nstages,setup,fe,blank,reads16,stage1); rc=2;
 }
 mode=MODE_GOOD; reset(); StageStatus=EFI_DEVICE_ERROR;
 EFI_STATUS bad=Ps5NativeDpVideo(&Pci,NULL,0);
 if(!(bad==EFI_DEVICE_ERROR && !banned() && unblanked() && Nstages==4 && Stages[3]==1)){
  fprintf(stderr,"stage1 status=%lx banned=%d unblank=%d stages=%u\n",(unsigned long)bad,banned(),unblanked(),(unsigned)Nstages); rc=3;
 }
 return rc;
}
""")
            subprocess.run(['gcc','-Wall','-Wno-unused-parameter','-fshort-wchar','-DMDEPKG_NDEBUG','-I'+str(R),'-I'+str(EDK/'MdePkg/Include'),'-I'+str(EDK/'MdePkg/Include/X64'),str(p/'test.c'),str(R/'dcn201-link.c'),str(R/'dcn201-stream.c'),str(R/'dcn201-timing.c'),str(R/'dcn201-test-pattern.c'),str(R/'dcn201-aux.c'),str(R/'ps5-display-mode.c'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)
