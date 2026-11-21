"""Real DXE entry with mocked services/transport, never hardware MMIO."""
import ctypes as C
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
HERE=Path(__file__).parent.resolve()
EDK=Path(os.environ.get('PS5_EDK2_ROOT',os.environ.get('PSW_WORK', '/opt/pswloader-work') + '/edk2'))

@unittest.skipUnless((EDK/'MdePkg/Include/Base.h').exists(),'EDK headers required')
class ProbeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work=tempfile.TemporaryDirectory();work=Path(cls.work.name)
        source=HERE/'Ps5PlatformPkg/HdmiProbeDxe/HdmiProbeDxe.c'
        (work/'mock.c').write_text('#include "'+str(source)+'"\n'+r'''
static int Valid,MapKind,Reply,Queries,Polls,Irq,Tpl,WrongContext,ExpectedIrq;
static int PrepReply,PrepCalls;
#ifdef PS5_HDMI_PREPARE
static PS5_HDMI_VIDEO_PROTOCOL *Installed;
static int SubmitCalls,SubmitReply,InstallCalls,ReentryBlocked,StopCalls,StopReply;
EFI_GUID gEfiSimpleTextInProtocolGuid,gEfiSimpleTextInputExProtocolGuid,gEfiDevicePathProtocolGuid;
static EFI_STATUS EFIAPI Install(EFI_HANDLE *H,EFI_GUID *G,EFI_INTERFACE_TYPE T,VOID *P){
 if(Irq!=ExpectedIrq||Tpl!=TPL_APPLICATION)WrongContext++;
 Installed=P;InstallCalls++;return EFI_SUCCESS;
}
int ps5_flava3_video_stage_mode(const struct ps5_hdmi_config*C,unsigned S,const struct ps5_flava3_mode*M,struct ps5_hdmi_commands*P){return C->converter!=2||(S>7&&S!=11)||M?-1:0;}
int ps5_flava3_video_stage_modex(const struct ps5_hdmi_config*C,unsigned S,const struct ps5_flava3_mode_x*M,struct ps5_hdmi_commands*P){return C->converter!=2||(S>7&&S!=11)||M?-1:0;}
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){unsigned char *d=D;const unsigned char *s=S;while(N--)*d++=*s++;return D;}
VOID *EFIAPI ZeroMem(VOID *D,UINTN N){unsigned char *d=D;while(N--)*d++=0;return D;}   /* r5.1 ButtonReadEx */
int ps5_flava3_prepare_stage(const struct ps5_hdmi_config*C,unsigned S,struct ps5_hdmi_commands*P){return C->converter!=2||S>2?-1:0;}
int ps5_hdmi_commands_submit(const struct ps5_icc_io*Io,const struct ps5_hdmi_commands*P,ps5_icc_notification C,void*Context){
 SubmitCalls++;if(Irq||Tpl!=TPL_HIGH_LEVEL||C!=ps5_edid_notification||Context!=&mEdid||Io->timeout_ticks!=TSC_HZ*12)WrongContext++;
 ReentryBlocked=Installed->Stage(0)==EFI_ALREADY_STARTED;return SubmitReply;
}
int ps5_hdmi_register_read(const struct ps5_icc_io*Io,uint16_t R,unsigned N,uint8_t*O,ps5_icc_notification C,void*Context){if(Irq||Tpl!=TPL_HIGH_LEVEL)WrongContext++;O[0]=0x42;return 0;}
#include "button-keys.c"
int ReadStage(void){uint8_t V=0;return Installed&&Installed->Read(0x60f8,1,&V)==EFI_SUCCESS&&V==0x42;}
int Stage(int N,int Result){SubmitReply=Result;return Installed&&Installed->Stage(N)==EFI_SUCCESS;}
int StageCount(void){return SubmitCalls;}
int Reentry(void){return ReentryBlocked;}
int ps5_icc_query(const struct ps5_icc_io *io,uint8_t service,uint16_t type,const uint8_t *payload,unsigned payload_bytes,uint8_t *output,unsigned capacity,unsigned *output_bytes){
 StopCalls++;
 if(!io||Irq||Tpl!=TPL_HIGH_LEVEL||service!=2||type!=0x1f||payload||payload_bytes||output||capacity||output_bytes||io->timeout_ticks!=TSC_HZ*12)WrongContext++;
 return StopReply;
}
int StopCount(void){return StopCalls;}
void SetStop(int V){StopReply=V;}
#endif
static UINT64 Clock;
static EFI_BOOT_SERVICES Bs;
static EFI_DXE_SERVICES Ds;
EFI_BOOT_SERVICES *gBS=&Bs;
EFI_SYSTEM_TABLE *gST=NULL;
UINTN EFIAPI UnicodeSPrint(CHAR16 *B,UINTN S,CONST CHAR16 *F,...){return 0;}
static EFI_STATUS EFIAPI MockCreateEvent(UINT32 T,EFI_TPL P,EFI_EVENT_NOTIFY N,VOID *C,EFI_EVENT *E){*E=(EFI_EVENT)1;return EFI_SUCCESS;}
VOID *EFIAPI SetMem(VOID *B,UINTN L,UINT8 V){unsigned char *p=B;while(L--)*p++=V;return B;}   /* r5.20 VideoDiagIcc */
static EFI_STATUS EFIAPI MockCreateEventEx(UINT32 T,EFI_TPL P,EFI_EVENT_NOTIFY N,CONST VOID *C,CONST EFI_GUID *G,EFI_EVENT *E){*E=(EFI_EVENT)1;return EFI_SUCCESS;}   /* button input (r5.17) */
static EFI_STATUS EFIAPI MockSetTimer(EFI_EVENT E,EFI_TIMER_DELAY T,UINT64 D){return EFI_SUCCESS;}
static EFI_STATUS EFIAPI MockInstallMulti(EFI_HANDLE *H,...){return EFI_SUCCESS;}
EFI_DXE_SERVICES *gDS=&Ds;
static EFI_TPL EFIAPI Raise(EFI_TPL New){int Old=Tpl;Tpl=New;return Old;}
static VOID EFIAPI Restore(EFI_TPL Old){Tpl=Old;}
static EFI_STATUS EFIAPI Get(EFI_PHYSICAL_ADDRESS A,EFI_GCD_MEMORY_SPACE_DESCRIPTOR *D){
 D->BaseAddress=A;D->Length=MapKind==2?128:4096;D->GcdMemoryType=EfiGcdMemoryTypeMemoryMappedIo;
 D->Attributes=MapKind==1?EFI_MEMORY_WB:EFI_MEMORY_UC;return MapKind==3?EFI_NOT_FOUND:EFI_SUCCESS;
}
UINT32 EFIAPI PciExpressRead32(UINTN A){if(!Valid)return 0xffffffff;switch(A&0xfff){case 0:return 0x9107104d;case 0x18:return 0x8520000c;case 0x20:return 0x8540000c;default:return 0;}}
UINT16 EFIAPI PciExpressRead16(UINTN A){return 7;}
UINT8 EFIAPI MmioRead8(UINTN A){return 0;}
UINT16 EFIAPI MmioRead16(UINTN A){return 0;}
UINT32 EFIAPI MmioRead32(UINTN A){return 0;}
UINT8 EFIAPI MmioWrite8(UINTN A,UINT8 V){return V;}
UINT16 EFIAPI MmioWrite16(UINTN A,UINT16 V){return V;}
UINT32 EFIAPI MmioWrite32(UINTN A,UINT32 V){return V;}
UINT64 EFIAPI AsmReadTsc(VOID){Clock+=TSC_HZ/100;return Clock;}
VOID EFIAPI CpuPause(VOID){}
BOOLEAN EFIAPI SaveAndDisableInterrupts(VOID){int Old=Irq;Irq=0;return Old;}
BOOLEAN EFIAPI SetInterruptState(BOOLEAN New){Irq=New;return New;}
void ps5_edid_notification(void *C,uint8_t S,uint16_t T,const uint8_t *P,unsigned N){}
static int UsbcCalls,UsbcBad;
int ps5_icc_query_events(const struct ps5_icc_io*io,uint8_t service,uint16_t type,const uint8_t*payload,unsigned bytes,uint8_t*out,unsigned cap,unsigned*ob,ps5_icc_notification cb,void*ctx){
 if(service==0x12){UsbcCalls++;if(type!=0x10||bytes!=2||payload[0]!=0||payload[1]!=5||Irq||Tpl!=TPL_HIGH_LEVEL)UsbcBad++;return 0;}
 return -1;}
int UsbcValue(int N){return N?UsbcBad:UsbcCalls;}
int ps5_hdmi_read_config_events(const struct ps5_icc_io *Io,struct ps5_hdmi_config *Out,ps5_icc_notification C,void *Context){
 Queries++;if(Irq||Tpl!=TPL_HIGH_LEVEL||C!=ps5_edid_notification||Context!=&mEdid)WrongContext++;
 *Out=(struct ps5_hdmi_config){0};Out->converter=2;return Reply;
}
int ps5_icc_poll(const struct ps5_icc_io *Io,ps5_icc_notification C,void *Context){Polls++;return 0;}
int ps5_flava3_prepare(const struct ps5_icc_io *Io,const struct ps5_hdmi_config *Config,unsigned *Completed,ps5_icc_notification C,void *Context) {
 PrepCalls++;if(Irq||Tpl!=TPL_HIGH_LEVEL||C!=ps5_edid_notification||Context!=&mEdid||Config->converter!=2)WrongContext++;
 *Completed=PrepReply?1:3;return PrepReply;
}
int Run(int Device,int Mapping,int Result,int InitialIrq){
 Valid=Device;MapKind=Mapping;Reply=Result;Irq=ExpectedIrq=InitialIrq;Tpl=TPL_APPLICATION;
 Queries=Polls=WrongContext=0;Clock=0;Bs.RaiseTPL=Raise;Bs.RestoreTPL=Restore;Ds.GetMemorySpaceDescriptor=Get;
#ifdef PS5_HDMI_PREPARE
 Installed=NULL;SubmitCalls=InstallCalls=ReentryBlocked=StopCalls=StopReply=0;Bs.InstallProtocolInterface=Install;Bs.CreateEvent=MockCreateEvent;Bs.CreateEventEx=MockCreateEventEx;Bs.SetTimer=MockSetTimer;Bs.InstallMultipleProtocolInterfaces=MockInstallMulti;
#endif
 return Ps5HdmiProbeEntry(NULL,NULL)==EFI_SUCCESS;
}
int RunPrepared(int Device,int Mapping,int QueryResult,int PreparationResult,int InitialIrq) {
 PrepReply=PreparationResult;PrepCalls=0;return Run(Device,Mapping,QueryResult,InitialIrq);
}
int PrepCount(void){return PrepCalls;}
int Value(int N){switch(N){case 0:return Queries;case 1:return Polls;case 2:return Irq;case 3:return Tpl;default:return WrongContext;}}
''')
        output=work/'probe.so'
        subprocess.run(['gcc','-shared','-fPIC','-fshort-wchar','-DPS5_EDK2','-DPS5_NO_WARM_MARKER','-DMDEPKG_NDEBUG','-I'+str(HERE),'-I'+str(EDK/'MdePkg/Include'),'-I'+str(EDK/'MdePkg/Include/X64'),str(work/'mock.c'),'-o',str(output)],check=True)
        cls.lib=C.CDLL(str(output))
        prepared=work/'prepared.so'
        subprocess.run(['gcc','-shared','-fPIC','-fshort-wchar','-DPS5_EDK2','-DPS5_NO_WARM_MARKER','-DPS5_HDMI_PREPARE','-DMDEPKG_NDEBUG','-I'+str(HERE),'-I'+str(EDK/'MdePkg/Include'),'-I'+str(EDK/'MdePkg/Include/X64'),str(work/'mock.c'),'-o',str(prepared)],check=True)
        cls.prepared=C.CDLL(str(prepared))
    @classmethod
    def tearDownClass(cls):cls.work.cleanup()
    def test_guards_do_not_query_or_poll(self):
        for valid,mapping in ((0,0),(1,1),(1,2),(1,3)):
            self.assertEqual(self.lib.Run(valid,mapping,0,1),0)
            self.assertEqual([self.lib.Value(i) for i in range(4)],[0,0,1,4])
    def test_query_failure_restores_interrupts_and_tpl(self):
        for irq in (0,1):
            self.assertEqual(self.lib.Run(1,0,-3,irq),0)
            self.assertEqual([self.lib.Value(i) for i in range(5)],[1,0,irq,4,0])
    def test_success_uses_bounded_poll_and_restores_context(self):
        self.assertEqual(self.lib.Run(1,0,0,1),1)
        self.assertEqual(self.lib.Value(0),1)
        self.assertGreater(self.lib.Value(1),0);self.assertLess(self.lib.Value(1),12)
        self.assertEqual([self.lib.Value(i) for i in (2,3,4)],[1,4,0])

    def test_usbc_enabled_once_after_successful_query(self):
        """v3.23: like PS5 Linux spcie_icc_init: ICC 0x12/0x10 {0,5} inside the ICC critical section."""
        before = self.lib.UsbcValue(0)
        self.assertTrue(self.lib.Run(1, 0, 0, 1))
        self.assertEqual(self.lib.UsbcValue(0), before + 1)
        self.assertEqual(self.lib.UsbcValue(1), 0)
        self.assertFalse(self.lib.Run(1, 0, -3, 1))      # failed HDMI config query -> no USB-C command
        self.assertEqual(self.lib.UsbcValue(0), before + 1)

    def test_preparation_explicit_only_and_context_restored(self):
        for irq in (0,1):
            for result in (0,-3,-6):
                self.assertEqual(self.prepared.RunPrepared(1,0,0,result,irq),int(result==0))
                self.assertEqual(self.prepared.PrepCount(),1)
                self.assertEqual([self.prepared.Value(i) for i in (2,3,4)],[irq,4,0])
                self.assertEqual(self.prepared.Value(1)>0,result==0)
        self.assertEqual(self.lib.RunPrepared(1,0,0,0,1),1)
        self.assertEqual(self.lib.PrepCount(),0)
    def test_failed_query_or_guard_never_prepares(self):
        for device,mapping,query in ((0,0,0),(1,1,0),(1,0,-3)):
            self.assertEqual(self.prepared.RunPrepared(device,mapping,query,0,1),0)
            self.assertEqual(self.prepared.PrepCount(),0)
            self.assertEqual([self.prepared.Value(i) for i in (2,3,4)],[1,4,0])

    def test_fixed_video_service_restores_context_and_blocks_reentry(self):
        self.assertEqual(self.prepared.RunPrepared(1,0,0,0,1),1)
        for stage in range(11):
            for result in (0,-3):
                self.assertEqual(self.prepared.Stage(stage,result),int(result==0))
                self.assertEqual([self.prepared.Value(i) for i in (2,3,4)],[1,4,0])
                self.assertEqual(self.prepared.Reentry(),1)
        self.assertEqual(self.prepared.ReadStage(),1)
        self.assertEqual([self.prepared.Value(i) for i in (2,3,4)],[1,4,0])
        count=self.prepared.StageCount()
        self.assertEqual(self.prepared.Stage(11,0),1)
        self.assertEqual(self.prepared.StageCount(),count+1)
        self.assertEqual([self.prepared.Value(i) for i in (2,3,4)],[1,4,0])
        count=self.prepared.StageCount()
        self.assertEqual(self.prepared.Stage(12,0),0)
        self.assertEqual(self.prepared.StageCount(),count)

    def test_stage6_stopHdcpHw_once_before_submit_and_abort_skips_packet(self):
        source=(HERE/'Ps5PlatformPkg/HdmiProbeDxe/HdmiProbeDxe.c').read_text()
        self.assertEqual(source.count('ps5_icc_query(&Io,2,0x1f,NULL,0,NULL,0,NULL)'),1)
        self.assertEqual(source.count("PS5 HDMI: stopHdcpHw=%d\\n"),1)
        query=source.index('ps5_icc_query(&Io,2,0x1f,NULL,0,NULL,0,NULL)')
        submit=source.index('ps5_hdmi_commands_submit(&Io,&Packet,ps5_edid_notification,&mEdid)')
        self.assertLess(query,submit)
        self.assertEqual(self.prepared.RunPrepared(1,0,0,0,1),1)
        before=self.prepared.StageCount()
        self.prepared.SetStop(-3)
        self.assertEqual(self.prepared.Stage(6,0),0)
        self.assertEqual(self.prepared.StopCount(),1)
        self.assertEqual(self.prepared.StageCount(),before)
        self.assertEqual([self.prepared.Value(i) for i in (2,3,4)],[1,4,0])
        self.prepared.SetStop(0)
        self.assertEqual(self.prepared.Stage(7,0),1)
        self.assertEqual(self.prepared.Stage(11,0),1)
        self.assertEqual(self.prepared.StopCount(),1)
        self.assertEqual(self.prepared.StageCount(),before+2)
        self.assertEqual(self.prepared.Stage(6,0),1)
        self.assertEqual(self.prepared.StopCount(),2)
        self.assertEqual(self.prepared.StageCount(),before+3)
