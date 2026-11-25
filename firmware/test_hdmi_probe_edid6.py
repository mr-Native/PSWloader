"""edid6 host test (no hardware): real HdmiProbeDxe built with PS5_HDMI_PREPARE + PS5_HDMI_EDID_REQUEST
against mocked services/transport, WITH DEBUG() compiled in.

The DebugPrint mock packs arguments like the payload's PeiDxeDebugLibReportStatusCode: more than 12
arguments in one DEBUG() is an error (on the console: ASSERT -> CpuDeadLoop, the hang of edid2-4).
Every test fails if any executed DEBUG() line exceeds 12 arguments or an ASSERT fires.

edid5 contract additionally to edid4: each inbound message is announced and exported before its body is
read and logged with 8 data bytes after; HDMI events are switched off exactly once (after the EDID, at the
latest after 2 messages, else at the window end); everything terminates, also under an endless stream.
edid6 additionally: the EDID goes to the log (16 hex lines + parsed lines) and into the volatile UEFI variables
"Ps5HdmiEdid" (block 0 repaired like Linux fix_edid) and "Ps5HdmiEdidInfo" (64 bytes), attributes BS|RT only.
PS5_PROBE_SOURCE=<HdmiProbeDxe.c> runs the DEBUG argument test against another source (edid4 must FAIL).
"""
import ctypes as C
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
HERE = Path(__file__).parent.resolve()
EDK = Path(os.environ.get('PS5_EDK2_ROOT', os.environ.get('PSW_WORK', '/opt/pswloader-work') + '/edk2'))
SOURCE = Path(os.environ.get('PS5_PROBE_SOURCE', HERE / 'Ps5PlatformPkg/HdmiProbeDxe/HdmiProbeDxe.c'))
_src = SOURCE.read_text()
def _const(name):
    import re
    return int(re.search(r'#define ' + name + r'\s+(\d+)U', _src).group(1))
WINDOW_S = _const('PS5_EDID_WINDOW_SECONDS')        # r5.16.3: 30 s EDID window (the Dell needs ~20-25 s)
WINDOW_MSGS = _const('PS5_EDID_WINDOW_MESSAGES')
RX_LOGGED = _const('PS5_EDID_RX_LOGGED')
PROGRESS_MAX = 6 + WINDOW_S                          # timestamps on: 6 fast lines, then one per second
DELL = Path(os.environ.get('PS5_KERNEL_EVIDENCE', '/opt/ps5-linux-kernel/evidence')) / '20261018/dell-original.bin'

MOCK = r'''
static int Irq,Tpl,InstallCalls,PrepCalls,TimerArmed,TimerCancelled,EntryPollFail,Forbidden;
static int PollCalls,StageCalls,SubmitCalls,EspPresent,GroupEvents,Signals,Closes,EventOff,EventOther;
static int DebugCalls,DebugBad,DebugMax,Asserts,Endless,DropMode,HexLines,DtdLines;
static uint8_t TestEdid[256];
/* ---- runtime services: tiny volatile variable store ---- */
static struct {CHAR16 Name[32];EFI_GUID Guid;UINT32 Attr;UINTN Size;UINT8 Data[2048];} Vars[4];static int VarCount,SetCalls,BadAttr;
static int NameEq(const CHAR16 *A,const CHAR16 *B){while(*A&&*A==*B){A++;B++;}return *A==*B;}
static EFI_STATUS EFIAPI MockSetVar(CHAR16 *N,EFI_GUID *G,UINT32 A,UINTN S,VOID *D){
 SetCalls++;if(A!=(EFI_VARIABLE_BOOTSERVICE_ACCESS|EFI_VARIABLE_RUNTIME_ACCESS))BadAttr++;if(Tpl!=TPL_APPLICATION||!Irq)Forbidden+=10000;
 int I=0;for(;I<VarCount;I++)if(NameEq(Vars[I].Name,N))break;if(I==VarCount){if(VarCount==4)return EFI_OUT_OF_RESOURCES;VarCount++;}
 int K=0;while(N[K]&&K<31){Vars[I].Name[K]=N[K];K++;}Vars[I].Name[K]=0;Vars[I].Guid=*G;Vars[I].Attr=A;Vars[I].Size=S;memcpy(Vars[I].Data,D,S);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI MockGetVar(CHAR16 *N,EFI_GUID *G,UINT32 *A,UINTN *S,VOID *D){
 for(int I=0;I<VarCount;I++)if(NameEq(Vars[I].Name,N)&&CompareGuid(G,&Vars[I].Guid)){if(*S<Vars[I].Size){*S=Vars[I].Size;return EFI_BUFFER_TOO_SMALL;}
  if(A)*A=Vars[I].Attr;*S=Vars[I].Size;memcpy(D,Vars[I].Data,Vars[I].Size);return EFI_SUCCESS;}return EFI_NOT_FOUND;}
static EFI_STATUS EFIAPI MockGetTime(EFI_TIME *T,EFI_TIME_CAPABILITIES *C){memset(T,0,sizeof(*T));T->Year=2026;T->Month=11;T->Day=14;return EFI_SUCCESS;}
static EFI_RUNTIME_SERVICES Rt;
EFI_RUNTIME_SERVICES *gRT=&Rt;
VOID *EFIAPI ZeroMem(VOID *B,UINTN L){memset(B,0,L);return B;}
static int Queue[16],QHead,QTail;            /* 0 = HDMI event (no EDID), 1 = EDID notification */
static long long SubmitTicks;
static UINT64 OffSince,MaxOff;
static PS5_HDMI_VIDEO_PROTOCOL *Installed;
struct ps5_icc_wire_note ps5_icc_tolerated,ps5_icc_dropped;
EFI_GUID gEfiSimpleTextInProtocolGuid,gEfiSimpleTextInputExProtocolGuid,gEfiDevicePathProtocolGuid,gEfiSimpleFileSystemProtocolGuid,gEfiPartitionInfoProtocolGuid;
static UINT64 Clock,Step;
static EFI_BOOT_SERVICES Bs;
static EFI_DXE_SERVICES Ds;
EFI_BOOT_SERVICES *gBS=&Bs;
EFI_DXE_SERVICES *gDS=&Ds;
EFI_SYSTEM_TABLE *gST=NULL;
/* ---- DebugLib with the ReportStatusCode packing limit ---- */
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return TRUE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN L){return TRUE;}
BOOLEAN EFIAPI DebugAssertEnabled(VOID){return TRUE;}
BOOLEAN EFIAPI DebugCodeEnabled(VOID){return FALSE;}
BOOLEAN EFIAPI DebugClearMemoryEnabled(VOID){return FALSE;}
VOID *EFIAPI DebugClearMemory(VOID *B,UINTN L){return B;}
VOID EFIAPI DebugAssert(CONST CHAR8 *F,UINTN L,CONST CHAR8 *D){Asserts++;}
VOID EFIAPI DebugPrint(UINTN L,CONST CHAR8 *F,...){
 int N=0;for(const char *p=F;*p;p++){if(*p!='%')continue;p++;while(*p&&strchr(".-+ ,0123456789lL*",*p)){if(*p=='*')N++;p++;}
  if(!*p)break;if(strchr("pXxdusSagtcr",*p))N++;}
 DebugCalls++;if(N>DebugMax)DebugMax=N;if(N>12)DebugBad++;
 if(!strncmp(F,"PS5 EDID %03x:",14))HexLines++;if(!strncmp(F,"PS5 EDID DTD",12))DtdLines++;}
UINTN EFIAPI UnicodeSPrint(CHAR16 *B,UINTN S,CONST CHAR16 *F,...){return 0;}
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){unsigned char *d=D;const unsigned char *s=S;while(N--)*d++=*s++;return D;}
BOOLEAN EFIAPI CompareGuid(CONST GUID *A,CONST GUID *B){const unsigned char *a=(const void*)A,*b=(const void*)B;for(int i=0;i<16;i++)if(a[i]!=b[i])return FALSE;return TRUE;}
UINT64 EFIAPI DivU64x64Remainder(UINT64 A,UINT64 B,UINT64 *R){if(R)*R=A%B;return A/B;}
static EFI_STATUS EFIAPI Install(EFI_HANDLE *H,EFI_GUID *G,EFI_INTERFACE_TYPE T,VOID *P){Installed=P;InstallCalls++;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI MockCreateEvent(UINT32 T,EFI_TPL P,EFI_EVENT_NOTIFY N,VOID *C,EFI_EVENT *E){*E=(EFI_EVENT)1;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI MockCreateEventEx(UINT32 T,EFI_TPL P,EFI_EVENT_NOTIFY N,CONST VOID *C,CONST EFI_GUID *G,EFI_EVENT *E){
 static EFI_GUID Want={0x873f7798,0xe801,0x4ec4,{0x86,0x97,0x30,0xb7,0xb3,0xa1,0xfa,0x52}};
 if(CompareGuid(G,&Want))GroupEvents++;*E=(EFI_EVENT)2;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI MockSignal(EFI_EVENT E){if(Tpl!=TPL_APPLICATION||!Irq)Forbidden+=100;Signals++;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI MockClose(EFI_EVENT E){Closes++;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI MockSetTimer(EFI_EVENT E,EFI_TIMER_DELAY T,UINT64 D){if(T==TimerCancel)TimerCancelled++;else TimerArmed++;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI MockInstallMulti(EFI_HANDLE *H,...){return EFI_SUCCESS;}
static EFI_STATUS EFIAPI BadStall(UINTN U){Forbidden++;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI BadCheck(EFI_EVENT E){Forbidden++;return EFI_NOT_READY;}
static EFI_STATUS EFIAPI BadWait(UINTN N,EFI_EVENT *E,UINTN *I){Forbidden++;return EFI_UNSUPPORTED;}
static EFI_PARTITION_INFO_PROTOCOL Part;
static EFI_HANDLE FsHandles[1]={(EFI_HANDLE)7};
static EFI_STATUS EFIAPI MockLocate(EFI_LOCATE_SEARCH_TYPE S,EFI_GUID *G,VOID *K,UINTN *N,EFI_HANDLE **B){
 if(!EspPresent)return EFI_NOT_FOUND;*N=1;*B=FsHandles;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI MockHandle(EFI_HANDLE H,EFI_GUID *G,VOID **I){*I=&Part;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI MockFree(VOID *P){return EFI_SUCCESS;}
static EFI_TPL EFIAPI Raise(EFI_TPL New){int Old=Tpl;if(New<Old)Forbidden++;Tpl=New;return Old;}
static VOID EFIAPI Restore(EFI_TPL Old){if(Old>Tpl)Forbidden++;Tpl=Old;}
static EFI_STATUS EFIAPI Get(EFI_PHYSICAL_ADDRESS A,EFI_GCD_MEMORY_SPACE_DESCRIPTOR *D){
 D->BaseAddress=A;D->Length=4096;D->GcdMemoryType=EfiGcdMemoryTypeMemoryMappedIo;D->Attributes=EFI_MEMORY_UC;return EFI_SUCCESS;}
UINT32 EFIAPI PciExpressRead32(UINTN A){switch(A&0xfff){case 0:return 0x9107104d;case 0x18:return 0x8520000c;case 0x20:return 0x8540000c;default:return 0;}}
UINT16 EFIAPI PciExpressRead16(UINTN A){return 7;}
static int Pending(void){return Endless||QHead!=QTail;}
static int Kind(void){return Endless?0:Queue[QHead%16];}
UINT8 EFIAPI MmioRead8(UINTN A){if(A==MAILBOX+0x801&&Pending())return 0x10;return 0;}
UINT16 EFIAPI MmioRead16(UINTN A){if(!Pending())return 0;if(A==MAILBOX+0x808)return Kind()?12+4+256:16;if(A==MAILBOX+0x802)return 0x8000;return 0;}
UINT32 EFIAPI MmioRead32(UINTN A){if(A==DOORBELL+0x14)return Pending()?1:0;return 0;}
UINT8 EFIAPI MmioWrite8(UINTN A,UINT8 V){return V;}
UINT16 EFIAPI MmioWrite16(UINTN A,UINT16 V){return V;}
UINT32 EFIAPI MmioWrite32(UINTN A,UINT32 V){return V;}
UINT64 EFIAPI AsmReadTsc(VOID){Clock+=Step;return Clock;}
VOID EFIAPI CpuPause(VOID){}
BOOLEAN EFIAPI SaveAndDisableInterrupts(VOID){int Old=Irq;if(Old)OffSince=Clock;Irq=0;return Old;}
BOOLEAN EFIAPI SetInterruptState(BOOLEAN New){if(New&&!Irq&&Clock-OffSince>MaxOff)MaxOff=Clock-OffSince;Irq=New;return New;}
void ps5_edid_notification(void *C,uint8_t S,uint16_t T,const uint8_t *P,unsigned N){
 struct ps5_edid_state *St=C;if(St&&S==0x10&&N>=4+256&&P[1]==2){memcpy(St->raw,P+4,256);St->bytes=256;St->flags=9;St->updates++;}}
int ps5_icc_query_events(const struct ps5_icc_io*io,uint8_t s,uint16_t t,const uint8_t*p,unsigned b,uint8_t*o,unsigned c,unsigned*ob,ps5_icc_notification cb,void*ctx){return 0;}
int ps5_icc_query(const struct ps5_icc_io *io,uint8_t s,uint16_t t,const uint8_t *p,unsigned b,uint8_t *o,unsigned c,unsigned *ob){return 0;}
int ps5_hdmi_read_config_events(const struct ps5_icc_io *Io,struct ps5_hdmi_config *Out,ps5_icc_notification C,void *Context){
 *Out=(struct ps5_hdmi_config){0};Out->converter=2;return 0;}
int ps5_flava3_edid_stage(unsigned stage,struct ps5_hdmi_commands *out){StageCalls++;return stage<PS5_FLAVA3_EDID_STAGES?0:-1;}
int ps5_flava3_prepare(const struct ps5_icc_io *Io,const struct ps5_hdmi_config *Config,unsigned *Completed,ps5_icc_notification C,void *Context){PrepCalls++;*Completed=3;return 0;}
int ps5_icc_poll(const struct ps5_icc_io *Io,ps5_icc_notification C,void *Context){
 PollCalls++;
 if(EntryPollFail && PollCalls==1)return -4;
 if(!Pending())return 0;
 int K=Kind();if(!Endless)QHead++;
 if(DropMode){ps5_icc_dropped.count++;return 0;}
 uint8_t P[4+256]={0};P[1]=K?2:1;P[3]=K?1:0;if(K)memcpy(P+4,TestEdid,256);
 C(Context,0x10,0x8000,P,K?4+256:4);return 1;}
int ps5_hdmi_commands_init(struct ps5_hdmi_commands *P,unsigned I){return 0;}
int ps5_hdmi_commands_event(struct ps5_hdmi_commands *P,unsigned Minor,uint8_t Value){if(Minor==4&&Value==0)EventOff++;else EventOther++;return 0;}
int ps5_flava3_video_stage_mode(const struct ps5_hdmi_config*C,unsigned S,const struct ps5_flava3_mode*M,struct ps5_hdmi_commands*P){return 0;}
int ps5_flava3_video_stage_modex(const struct ps5_hdmi_config*C,unsigned S,const struct ps5_flava3_mode_x*M,struct ps5_hdmi_commands*P){return 0;}   /* r5.13 entry point */
int ps5_flava3_prepare_stage(const struct ps5_hdmi_config*C,unsigned S,struct ps5_hdmi_commands*P){return 0;}
int ps5_verde_edid_stage(struct ps5_hdmi_commands*P){StageCalls++;return 0;}
int ps5_verde_video_stage_mode(const struct ps5_hdmi_config*C,unsigned S,const struct ps5_flava3_mode*M,struct ps5_hdmi_commands*P){return (S==4||S==5||S==8||S==9||S==10||S==14)?1:0;}
VOID *EFIAPI SetMem(VOID *B,UINTN L,UINT8 V){unsigned char *p=B;while(L--)*p++=V;return B;}   /* r5.20 VideoDiagIcc */
int ps5_hdmi_commands_submit(const struct ps5_icc_io*Io,const struct ps5_hdmi_commands*P,ps5_icc_notification C,void*Context){
 if(Irq||Tpl!=TPL_HIGH_LEVEL)Forbidden+=1000;
 SubmitCalls++;Clock+=(UINT64)SubmitTicks;return 0;}
int ps5_hdmi_register_read(const struct ps5_icc_io*Io,uint16_t R,unsigned N,uint8_t*O,ps5_icc_notification C,void*Context){return 0;}
#include "button-keys.c"
static void ResetAll(void){
 Irq=1;Tpl=TPL_APPLICATION;InstallCalls=PrepCalls=TimerArmed=TimerCancelled=EntryPollFail=Forbidden=0;
 PollCalls=StageCalls=SubmitCalls=EspPresent=GroupEvents=Signals=Closes=EventOff=EventOther=0;
 DebugCalls=DebugBad=DebugMax=Asserts=Endless=DropMode=HexLines=DtdLines=0;QHead=QTail=0;
 VarCount=SetCalls=BadAttr=0;Rt.SetVariable=MockSetVar;Rt.GetVariable=MockGetVar;Rt.GetTime=MockGetTime;
 SubmitTicks=0;MaxOff=0;Installed=NULL;Clock=0;Step=TSC_HZ/1000;
 ps5_icc_tolerated.count=ps5_icc_dropped.count=0;
#ifdef PS5_EDID_RX_LOGGED
 mEdidRxNote.calls=0;
#endif
 mVideoStarted=mPollerStopped=FALSE;mVideoBusy=FALSE;mEdidFlushes=0;mEdid=(struct ps5_edid_state){0};
 mPolls=0;for(int i=0;i<4;i++)mPollKind[i]=0;mButtonEnabled=-1;mButtonTimer=NULL;
 Part.Type=PARTITION_TYPE_GPT;Part.System=1;
 Part.Info.Gpt.UniquePartitionGUID=(EFI_GUID){0xbd21c511,0x2781,0x4058,{0xb8,0x47,0xf6,0x25,0xa7,0xec,0xcb,0xb6}};
 Bs.RaiseTPL=Raise;Bs.RestoreTPL=Restore;Ds.GetMemorySpaceDescriptor=Get;Bs.InstallProtocolInterface=Install;
 Bs.CreateEvent=MockCreateEvent;Bs.CreateEventEx=MockCreateEventEx;Bs.SetTimer=MockSetTimer;Bs.InstallMultipleProtocolInterfaces=MockInstallMulti;
 Bs.LocateHandleBuffer=MockLocate;Bs.HandleProtocol=MockHandle;Bs.FreePool=MockFree;Bs.SignalEvent=MockSignal;Bs.CloseEvent=MockClose;
 Bs.Stall=BadStall;Bs.CheckEvent=BadCheck;Bs.WaitForEvent=BadWait;
}
int Entry(int FailFirstPoll){ResetAll();EntryPollFail=FailFirstPoll;return Ps5HdmiProbeEntry(NULL,NULL)==EFI_SUCCESS;}
void SetEsp(int V){EspPresent=V;}
void SetEdid(const uint8_t *E){memcpy(TestEdid,E,256);}
int VarSize(const char *Name){CHAR16 W[32];int K=0;while(Name[K]&&K<31){W[K]=(CHAR16)Name[K];K++;}W[K]=0;
 for(int I=0;I<VarCount;I++)if(NameEq(Vars[I].Name,W))return (int)Vars[I].Size;return -1;}
int VarByte(const char *Name,int Off){CHAR16 W[32];int K=0;while(Name[K]&&K<31){W[K]=(CHAR16)Name[K];K++;}W[K]=0;
 for(int I=0;I<VarCount;I++)if(NameEq(Vars[I].Name,W))return Vars[I].Data[Off];return -1;}
int VarAttrOk(void){return SetCalls==3&&BadAttr==0;}   /* r5.26.5: + Ps5HdmiBridge */
int Lines(int N){return N?DtdLines:HexLines;}
void Push(int K){Queue[QTail++%16]=K;}
void SetEndless(int V,int Drop){Endless=V;DropMode=Drop;}
void SetTolerated(int N){ps5_icc_tolerated.count=N;ps5_icc_dropped.count=N;}
void SetStep(long long V){Step=(UINT64)V;}
void SetSubmitTicks(long long V){SubmitTicks=V;}
void SetTpl(int V){Tpl=V;}
void StartCounters(void){SubmitCalls=StageCalls=Signals=PollCalls=0;MaxOff=0;}
int Stage(int N){return Installed&&Installed->Stage(N)==EFI_SUCCESS;}
void Poller(void){ButtonPoll(NULL,NULL);}
long long Now(void){return (long long)Clock;}
long long MaxOffTicks(void){return (long long)MaxOff;}
int Value(int N){switch(N){case 0:return InstallCalls;case 1:return StageCalls;case 2:return SubmitCalls;case 3:return PollCalls;
 case 4:return Irq;case 5:return (int)Tpl;case 6:return TimerArmed;case 7:return GroupEvents;case 8:return Signals;
 case 9:return TimerCancelled;case 10:return mPollerStopped;case 11:return PrepCalls;case 12:return Forbidden;case 13:return Closes;
 case 14:return EventOff;case 15:return EventOther;case 16:return DebugBad;case 17:return DebugMax;case 18:return Asserts;
 case 19:return DebugCalls;case 20:return (int)mEdid.bytes;case 21:return QHead;default:return -1;}}
'''


@unittest.skipUnless((EDK / 'MdePkg/Include/Base.h').exists(), 'EDK headers required')
class Edid5Tests(unittest.TestCase):
    TSC = 1596300000

    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory(); work = Path(cls.work.name)
        (work / 'mock.c').write_text('#include <string.h>\n#include "' + str(SOURCE) + '"\n' + MOCK)
        lib = work / 'probe.so'
        subprocess.run(['gcc', '-shared', '-fPIC', '-fshort-wchar', '-DPS5_EDK2','-DPS5_NO_WARM_MARKER', '-DPS5_HDMI_PREPARE',
                        '-DPS5_HDMI_EDID_REQUEST', '-I' + str(HERE),
                        '-I' + str(EDK / 'MdePkg/Include'), '-I' + str(EDK / 'MdePkg/Include/X64'),
                        str(work / 'mock.c'), str(HERE / 'ps5-edid-info.c'), '-o', str(lib)], check=True)
        cls.lib = C.CDLL(str(lib))
        for f in ('Now', 'MaxOffTicks'):
            getattr(cls.lib, f).restype = C.c_longlong
        cls.lib.SetStep.argtypes = [C.c_longlong]
        cls.lib.SetSubmitTicks.argtypes = [C.c_longlong]

    @classmethod
    def tearDownClass(cls): cls.work.cleanup()

    def V(self, n): return self.lib.Value(n)

    def edid(self):
        e = bytearray(DELL.read_bytes()[:256])
        e[0] = 0x01                                             # PS5 byte-0 quirk, block still sums to 0 like on the console
        e[127] = (-sum(e[:127])) & 0xff
        self.lib.SetEdid((C.c_uint8 * 256).from_buffer_copy(bytes(e)))
        return bytes(e)

    def entry(self, fail=0):
        self.assertEqual(self.lib.Entry(fail), 1)
        self.assertEqual((self.V(0), self.V(6), self.V(7), self.V(1), self.V(11)), (1, 1, 0, 0, 1))
        self.lib.StartCounters()

    def ok_context(self, tpl=4):
        self.assertEqual([self.V(4), self.V(5), self.V(12)], [1, tpl, 0])
        self.assertEqual((self.V(16), self.V(18)), (0, 0), 'DEBUG() with >12 arguments (max %d) or ASSERT' % self.V(17))
        self.assertGreater(self.V(19), 0)

    def run_phase(self):
        self.lib.SetEsp(1)
        self.assertEqual(self.lib.Stage(6), 1)

    def test_debug_lines_within_12_arguments_with_tolerated_messages(self):
        """edid2-4 hang: the 'ICC first tolerated' line had 20 arguments -> ASSERT -> CpuDeadLoop."""
        self.entry(); self.lib.Push(0); self.lib.Push(1); self.lib.SetTolerated(1)
        self.run_phase()
        self.ok_context()
        self.assertLessEqual(self.V(17), 12)

    def test_edid_published_as_volatile_variables_and_logged(self):
        raw = self.edid()
        self.entry(); self.lib.Push(0); self.lib.Push(1)
        self.run_phase()
        self.ok_context()
        self.assertEqual((self.lib.Lines(0), self.lib.VarAttrOk()), (16, 1))        # 16 hex lines, 3x SetVariable BS|RT (EDID, info, bridge)
        self.assertGreaterEqual(self.lib.Lines(1), 1)
        self.assertEqual(self.lib.VarSize(b'Ps5HdmiEdid'), 256)
        self.assertEqual(self.lib.VarSize(b'Ps5HdmiEdidInfo'), 64)
        stored = bytes(self.lib.VarByte(b'Ps5HdmiEdid', i) for i in range(256))
        fixed = bytearray(raw); fixed[0] = 0; fixed[127] = (-sum(fixed[:127])) & 0xff
        self.assertEqual(stored, bytes(fixed))
        self.assertEqual(sum(stored[:128]) & 0xff, 0)
        info = bytes(self.lib.VarByte(b'Ps5HdmiEdidInfo', i) for i in range(64))
        import struct, zlib
        sig, ver, hsz, n, flags, upd = struct.unpack_from('<IHHIII', info, 0)
        self.assertEqual((sig, ver, hsz, n, flags, upd), (0x49444550, 1, 64, 256, 9, 1))
        self.assertEqual((info[20], info[21], info[22], info[23]), (1, raw[127], fixed[127], 1))
        self.assertEqual(struct.unpack_from('<Q', info, 32)[0], 1596300000)
        self.assertEqual(struct.unpack_from('<I', info, 60)[0], zlib.crc32(stored))

    def test_edid_as_second_message_then_events_off_once(self):
        self.entry(); self.lib.Push(0); self.lib.Push(1)
        self.run_phase()
        self.assertEqual((self.V(20), self.V(14), self.V(15), self.V(21)), (256, 1, 0, 2))
        self.ok_context()

    def test_edid_first_events_off_right_after(self):
        self.entry(); self.lib.Push(1); self.lib.Push(0); self.lib.Push(0)
        self.run_phase()
        self.assertEqual((self.V(20), self.V(14), self.V(21)), (256, 1, 1))   # stops after the EDID
        self.ok_context()

    def test_no_message_events_off_at_window_end(self):
        self.entry()
        self.run_phase()
        self.assertEqual((self.V(20), self.V(14)), (0, 1))
        self.ok_context()

    def test_each_message_exported_before_and_after_its_body(self):
        self.entry(); self.lib.Push(0)
        self.run_phase()
        # start + 8 packets + rx1 pending + rx1 done + ts lines (rx, update) + (off: before + after)
        # + window progress (<= PROGRESS_MAX) + end
        self.assertTrue(14 <= self.V(8) <= 1 + 8 + 2 + 2 + 2 + PROGRESS_MAX + 1, self.V(8))
        self.assertEqual(self.V(13), self.V(8))
        self.ok_context()

    def test_endless_stream_terminates(self):
        for drop in (0, 1):
            self.entry(); self.lib.SetEndless(1, drop); self.lib.SetStep(1)
            self.run_phase()
            self.assertEqual(self.V(14), 1)                     # events off exactly once
            self.assertLessEqual(self.V(3), 64 + 16)            # bounded number of polls
            # start + 8 packets + 8 resent + one ts line per message (capped) + 2 per logged message + off + progress + end
            self.assertLessEqual(self.V(8), 1 + 8 + 8 + WINDOW_MSGS + 2 * RX_LOGGED + 2 + PROGRESS_MAX + 1)
            self.ok_context()

    def test_raised_tpl_skips_phase(self):
        for tpl in (8, 16):
            self.entry(); self.lib.SetEsp(1); self.lib.SetTpl(tpl); self.lib.Push(1)
            self.assertEqual(self.lib.Stage(6), 1)
            self.assertEqual((self.V(1), self.V(8), self.V(14)), (0, 0, 0))
            self.ok_context(tpl)

    def test_no_esp_phase_still_runs(self):          # r2: the EDID no longer depends on a mounted ESP
        self.entry(); self.lib.Push(0); self.lib.Push(1)
        self.assertEqual(self.lib.Stage(6), 1)
        self.assertEqual(self.V(14), 1)                # events off exactly once, as with the ESP
        self.assertEqual(self.lib.VarSize(b'Ps5HdmiEdid'), 256)
        self.ok_context()

    def test_window_time_and_request_budget(self):
        self.entry(); self.lib.SetEsp(1); self.lib.SetStep(self.TSC // 1000)
        t0 = self.lib.Now(); self.assertEqual(self.lib.Stage(6), 1)
        self.assertLess((self.lib.Now() - t0) / self.TSC, WINDOW_S + 1.0)   # no EDID: the whole window, then stop
        self.ok_context()
        self.entry(); self.lib.SetEsp(1); self.lib.SetSubmitTicks(self.TSC)
        self.assertEqual(self.lib.Stage(6), 1)
        # 1 s per submit: the 15 s request budget (b1440d, the Dell needs > 1 s for packet 7) lets all 8 packets out,
        # plus the one resend after 8 s of silence (r5.16.3) -> at most 2 x 8 stage calls, never an endless resend
        self.assertLessEqual(self.V(1), 2 * 8)
        self.assertLess(self.lib.MaxOffTicks(), self.TSC * 11 // 10)
        self.ok_context()

    def test_button_poller_limits(self):
        self.entry(); self.lib.SetEndless(1, 0)
        for _ in range(2048 // 4 + 10):
            self.lib.Poller()
        self.assertEqual((self.V(10), self.V(9)), (1, 1))


if __name__ == '__main__':
    unittest.main()
