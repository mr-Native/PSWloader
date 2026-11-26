"""Exercise actual file-export guards with in-memory UEFI file protocols."""
import ctypes as C
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
HERE=Path(__file__).parent.resolve()
EDK=Path(os.environ.get('PS5_EDK2_ROOT',os.environ.get('PSW_WORK', '/opt/pswloader-work') + '/edk2'))

@unittest.skipUnless((EDK/'MdePkg/Include/Base.h').exists(),'EDK headers required')
class ExportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work=tempfile.TemporaryDirectory();work=Path(cls.work.name)
        source=HERE/'Ps5PlatformPkg/LogExportDxe/LogExportDxe.c'
        (work/'mock.c').write_text('static unsigned long long Ps5TestLogCell;\n#define PS5_WARM_LOGCELL ((unsigned long long)(unsigned long)&Ps5TestLogCell)\n#include "'+str(source)+'"\n'+r'''
#include <stdlib.h>
#include <string.h>
static int Mode,Writes,Flushes,Closes,Opens,BadOpen,Snapshots;
static int EventCalls,EventFailure,ClosedEvents,Registers,Present,Reenter;
static EFI_BOOT_SERVICES Bs;
EFI_BOOT_SERVICES *gBS=&Bs;
EFI_GUID gEfiPartitionInfoProtocolGuid={1},gEfiSimpleFileSystemProtocolGuid={2},gEfiFileInfoGuid={3},gEfiEventReadyToBootGuid={4};
static EFI_PARTITION_INFO_PROTOCOL Partition;
static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL Fs;
static EFI_FILE_PROTOCOL Root,File;
VOID *EFIAPI AllocatePool(UINTN Size){return malloc(Size);}
VOID EFIAPI FreePool(VOID *P){free(P);}
BOOLEAN EFIAPI CompareGuid(CONST GUID *A,CONST GUID *B){return !memcmp(A,B,16);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){
 if((UINTN)S==0x111001000ULL){Snapshots++;memset(D,0,N);((UINT64*)D)[0]=Mode==7?0:0x31474F4C355350ULL;return D;}
 return memcpy(D,S,N);
}
BOOLEAN EFIAPI SaveAndDisableInterrupts(VOID){return TRUE;}
BOOLEAN EFIAPI SetInterruptState(BOOLEAN S){return S;}
static EFI_STATUS EFIAPI Handle(EFI_HANDLE H,EFI_GUID *G,VOID **P){
 if(G==&gEfiPartitionInfoProtocolGuid)*P=&Partition;else *P=&Fs;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Close(EFI_FILE_PROTOCOL *F){Closes++;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Open(EFI_FILE_PROTOCOL *R,EFI_FILE_PROTOCOL **F,CHAR16 *N,UINT64 M,UINT64 A){
 Opens++;if(M!=(EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE))BadOpen++;
 if(Mode==6)return EFI_NOT_FOUND;*F=&File;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Volume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *F,EFI_FILE_PROTOCOL **R){*R=&Root;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Info(EFI_FILE_PROTOCOL *F,EFI_GUID *G,UINTN *N,VOID *P){
 if(!P){*N=sizeof(EFI_FILE_INFO)+32;return EFI_BUFFER_TOO_SMALL;}
 EFI_FILE_INFO *I=P;memset(P,0,*N);I->FileSize=Mode==3?128:65568;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Read(EFI_FILE_PROTOCOL *F,UINTN *N,VOID *P){memcpy(P,mMarker,16);if(Mode==2)((UINT8*)P)[0]='X';return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Position(EFI_FILE_PROTOCOL *F,UINT64 P){return P==16?EFI_SUCCESS:EFI_INVALID_PARAMETER;}
static EFI_STATUS EFIAPI Write(EFI_FILE_PROTOCOL *F,UINTN *N,VOID *P){Writes++;if(Reenter){Available(NULL,NULL);Ready(NULL,NULL);BootMilestone(NULL,NULL);}if(*N!=RING_BYTES)BadOpen++;if(Mode==4)--*N;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Flush(EFI_FILE_PROTOCOL *F){Flushes++;return Mode==5?EFI_DEVICE_ERROR:EFI_SUCCESS;}
static EFI_GUID mEsp={0xbd21c511,0x2781,0x4058,{0xb8,0x47,0xf6,0x25,0xa7,0xec,0xcb,0xb6}};   /* r5.25.4: no longer in the module */
int Run(int M){
 Mode=M;Reenter=0;Writes=Flushes=Closes=Opens=BadOpen=Snapshots=0;
 Partition.Type=PARTITION_TYPE_GPT;Partition.System=1;Partition.Info.Gpt.UniquePartitionGUID=mEsp;
 if(M==1)Partition.Info.Gpt.UniquePartitionGUID.Data1=0;
 Bs.HandleProtocol=Handle;Fs.OpenVolume=Volume;Root.Open=Open;Root.Close=Close;
 File.Close=Close;File.GetInfo=Info;File.Read=Read;File.SetPosition=Position;File.Write=Write;File.Flush=Flush;
 return Export(NULL,L"\\PS5UEFI.LOG")==EFI_SUCCESS;
}
int Value(int N){switch(N){case 0:return Writes;case 1:return Flushes;case 2:return Closes;case 3:return Opens;case 4:return Snapshots;default:return BadOpen;}}
static EFI_STATUS EFIAPI Locate(EFI_LOCATE_SEARCH_TYPE T,EFI_GUID *G,VOID *K,UINTN *N,EFI_HANDLE **H){
 if(!Present)return EFI_NOT_FOUND;*N=1;*H=malloc(sizeof(EFI_HANDLE));(*H)[0]=NULL;return EFI_SUCCESS;
}
static EFI_STATUS EventResult(EFI_EVENT *E){EventCalls++;if(EventCalls==EventFailure)return EFI_OUT_OF_RESOURCES;*E=(EFI_EVENT)(UINTN)EventCalls;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Create(UINT32 T,EFI_TPL P,EFI_EVENT_NOTIFY N,VOID *C,EFI_EVENT *E){return EventResult(E);}
static EFI_STATUS EFIAPI CreateEx(UINT32 T,EFI_TPL P,EFI_EVENT_NOTIFY N,CONST VOID *C,CONST EFI_GUID *G,EFI_EVENT *E){return EventResult(E);}
static EFI_STATUS EFIAPI Register(EFI_GUID *G,EFI_EVENT E,VOID **R){Registers++;if(EventFailure==3+Registers)return EFI_OUT_OF_RESOURCES;*R=(VOID*)(UINTN)Registers;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI CloseEvent(EFI_EVENT E){ClosedEvents++;return EFI_SUCCESS;}
int Start(int Fail,int Exists){
 Run(0);Writes=Flushes=Closes=Opens=BadOpen=Snapshots=0;
 mDone=mEarlyDone=mBusy=FALSE;mReadySnapshots=mBootSnapshots=0;EventCalls=ClosedEvents=Registers=0;EventFailure=Fail;Present=Exists;Reenter=0;
 Bs.LocateHandleBuffer=Locate;Bs.CreateEvent=Create;Bs.CreateEventEx=CreateEx;Bs.RegisterProtocolNotify=Register;Bs.CloseEvent=CloseEvent;
 return Ps5LogExportEntry(NULL,NULL)==EFI_SUCCESS;
}
void Milestone(int Nested){Mode=0;Present=1;Reenter=Nested;BootMilestone(NULL,NULL);}
void Trigger(int Early,int M,int Exists,int Nested){Mode=M;Present=Exists;Reenter=Nested;Partition.Info.Gpt.UniquePartitionGUID=mEsp;if(M==1)Partition.Info.Gpt.UniquePartitionGUID.Data1=0;if(Early)Available(NULL,NULL);else Ready(NULL,NULL);}
int Closed(void){return ClosedEvents;}

''')
        output=work/'export.so'
        subprocess.run(['gcc','-shared','-fPIC','-fshort-wchar','-DMDEPKG_NDEBUG','-I'+str(EDK/'MdePkg/Include'),'-I'+str(EDK/'MdePkg/Include/X64'),str(work/'mock.c'),'-o',str(output)],check=True)
        cls.lib=C.CDLL(str(output))
    @classmethod
    def tearDownClass(cls):cls.work.cleanup()
    def test_valid_slot_written_once_and_flushed(self):
        self.assertEqual(self.lib.Run(0),1)
        self.assertEqual([self.lib.Value(i) for i in range(6)],[1,1,2,1,1,0])
    def test_any_partition_with_valid_slot_is_written(self):   # r5.25.4: the prepared slot file decides, not the partition GUID
        self.assertEqual(self.lib.Run(1),1)
        self.assertEqual([self.lib.Value(i) for i in range(6)],[1,1,2,1,1,0])
    def test_wrong_marker_size_missing_file_or_no_snapshot_never_written(self):
        for mode in (2,3,6,7):
            self.assertEqual(self.lib.Run(mode),0);self.assertEqual(self.lib.Value(0),0)
    def test_short_write_or_flush_failure_not_success(self):
        for mode in (4,5):
            self.assertEqual(self.lib.Run(mode),0)
            self.assertEqual(self.lib.Value(0),1);self.assertEqual(self.lib.Value(2),2)

    def test_early_then_two_boot_attempts_are_bounded(self):
        self.assertEqual(self.lib.Start(0, 1), 1)
        self.assertEqual(self.lib.Value(0), 1)
        self.lib.Trigger(1, 0, 1, 0)
        self.assertEqual(self.lib.Value(0), 1)
        self.lib.Trigger(0, 0, 1, 0)
        self.assertEqual(self.lib.Value(0), 2)
        self.lib.Trigger(0, 0, 1, 0)
        self.lib.Trigger(1, 0, 1, 0)
        self.assertEqual(self.lib.Value(0), 3)
        self.lib.Trigger(0, 0, 1, 0)
        self.assertEqual(self.lib.Value(0), 3)

    def test_late_protocols_and_failed_export_allow_retry(self):
        self.assertEqual(self.lib.Start(0, 0), 1)
        self.lib.Trigger(1, 2, 1, 0)  # wrong marker (r5.25.4: any partition may hold the slot)
        self.assertEqual(self.lib.Value(0), 0)
        self.lib.Trigger(1, 5, 1, 0)  # flush failed
        self.assertEqual(self.lib.Value(0), 1)
        self.lib.Trigger(1, 0, 1, 0)
        self.assertEqual(self.lib.Value(0), 2)
        self.lib.Trigger(1, 0, 1, 0)
        self.assertEqual(self.lib.Value(0), 2)

    def test_reentrant_notification_cannot_overwrite_shared_snapshot(self):
        self.assertEqual(self.lib.Start(0, 0), 1)
        self.lib.Trigger(1, 0, 1, 1)
        self.assertEqual(self.lib.Value(0), 1)
        self.lib.Trigger(0, 0, 1, 1)
        self.assertEqual(self.lib.Value(0), 2)

    def test_failed_registration_closes_all_created_events(self):
        for failure, closed in ((1, 0), (2, 1), (3, 2), (4, 3), (5, 3)):
            self.assertEqual(self.lib.Start(failure, 0), 0)
            self.assertEqual(self.lib.Closed(), closed)
            self.assertEqual(self.lib.Value(0), 0)

    def test_boot_milestones_persist_after_ready_and_are_bounded_and_nonreentrant(self):
        self.assertEqual(self.lib.Start(0,1),1)
        self.lib.Trigger(0,0,1,0)
        self.assertEqual(self.lib.Value(0),2)
        for _ in range(320):self.lib.Milestone(1)
        self.assertEqual(self.lib.Value(0),302)
        self.assertEqual(self.lib.Value(1),302)
