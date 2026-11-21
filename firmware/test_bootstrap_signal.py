"""Execute milestone guards with simulated MMIO/clock and transport outcomes."""
import ctypes as C
from pathlib import Path
import subprocess
import tempfile
import unittest
HERE=Path(__file__).parent.resolve()

class SignalTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory();work=Path(cls.temp.name)
        (work/'mock.c').write_text(r'''
#include <stdint.h>
static int Mode,Queries,ConfigReads,RawWrites,AriSeen,Bad,Reboots;
static uint64_t Clock;
static uint64_t ticks(void *c){(void)c;Clock+=159630000;return Clock;}
static void relax(void *c){(void)c;}
static uint64_t pat(void){return (uint64_t)(Mode==1?6:0)<<24;}
static uint32_t raw_read(uint64_t a,unsigned w){
 (void)w;
 switch(a-0xf0000000ULL){
 case 0x9000:return Mode==2?0xffffffff:0x13df1022;
 case 0x9019:return Mode==3?0x20:0x40;
 case 0x9006:return 16;
 case 0x9034:return 0x40;
 case 0x9040:return Mode==4?1:0x10;
 case 0x9041:return 0x40;
 case 0x9042:return 0x42;
 case 0x9068:return Mode==5?0:32;
 default:Bad++;return 0;
 }
}
static void raw_write(uint64_t a,unsigned w,uint32_t v){(void)a;(void)w;(void)v;RawWrites++;}
#define PS5_SIGNAL_TEST
#include "''' + str(HERE/'bootstrap-signal.c') + r'''"
int ps5_pci_access(const struct ps5_pci_io *io,uint32_t a,unsigned w,int ar,int wr,uint32_t *v){
 (void)io;(void)w;ConfigReads++;AriSeen=ar;if(wr)Bad++;
 if(Mode==6)return -2;
 switch(a&0xfff){
 case 0:*v=Mode==7?0:0x9107104d;break;
 case 0x18:*v=Mode==8?0:0x8520000c;break;
 case 0x20:*v=0x8540000c;break;
 case 4:*v=Mode==9?0:2;break;
 default:*v=0;
 }
 return 0;
}
int ps5_icc_query(const struct ps5_icc_io *io,uint8_t service,uint16_t type,const uint8_t *p,unsigned n,uint8_t *out,unsigned cap,unsigned *bytes){
 Queries++;
 if(service!=9||type||n!=1||p[0]!=1||out||cap||bytes||io->timeout_ticks!=1596300000ULL)Bad++;
 return Mode==10 || (Mode==11 && Queries==7)?-3:0;
}
int ps5_icc_power(const struct ps5_icc_io *io,int reboot){(void)io;if(reboot!=1)Bad++;Reboots++;return Mode==10?-3:0;}
int RebootCount(void){return Reboots;}
void Reset(int mode){Reboots=0;Mode=mode;Queries=ConfigReads=RawWrites=AriSeen=Bad=0;Clock=0;state=0;}
int Value(int n){switch(n){case 0:return Queries;case 1:return ConfigReads;case 2:return RawWrites;case 3:return AriSeen;default:return Bad;}}
''')
        subprocess.run(['gcc','-shared','-fPIC','-Wall','-Wextra','-Werror',str(work/'mock.c'),'-o',str(work/'mock.so')],check=True)
        cls.lib=C.CDLL(str(work/'mock.so'))
    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()
    def test_successful_milestones(self):
        self.lib.Reset(0)
        for stage in (1,2,3):self.lib.ps5_boot_signal(stage)
        self.assertEqual(self.lib.Value(0),6)
        self.assertEqual(self.lib.Value(1),6) # Verify controller once before ownership.
        self.assertEqual(self.lib.Value(3),1)
        self.assertEqual(self.lib.Value(4),0)
    def test_guards_suppress_requests_and_disable_later_attempts(self):
        for mode in (1,2,3,4,6,7,8,9):
            self.lib.Reset(mode);self.lib.ps5_boot_signal(1)
            reads=self.lib.Value(1);self.lib.ps5_boot_signal(2)
            self.assertEqual(self.lib.Value(0),0)
            self.assertEqual(self.lib.Value(1),reads)
            self.assertEqual(self.lib.Value(2),0)
    def test_transport_failure_stops_group_and_future_signals(self):
        self.lib.Reset(10);self.lib.ps5_boot_signal(3);self.lib.ps5_boot_signal(2)
        self.assertEqual(self.lib.Value(0),1)
    def test_ari_disabled_and_invalid_stage(self):
        self.lib.Reset(5);self.lib.ps5_boot_signal(0);self.lib.ps5_boot_signal(4)
        self.assertEqual(self.lib.Value(1),0)
        self.lib.ps5_boot_signal(1)
        self.assertEqual(self.lib.Value(3),0)
        self.assertEqual(self.lib.Value(0),1)

    def test_regular_abort_encoding(self):
        for code,queries in ((0x100,6),(0x101,7),(0x208,15),(0x300,8),(0x505,15),(0x604,15)):
            self.lib.Reset(0);self.lib.ps5_boot_error(code)
            self.assertEqual(self.lib.Value(0),queries*3)
            self.assertEqual(self.lib.Value(4),0)
    def test_exception_and_unknown_errors_never_access_controller(self):
        for code in (0x400,0x40e,0x200,0x209,0x506,0x605,0xffffffff):
            self.lib.Reset(0);self.lib.ps5_boot_error(code)
            self.assertEqual(self.lib.Value(0),0);self.assertEqual(self.lib.Value(1),0)
    def test_abort_signal_transport_failure_suppresses_remaining_groups(self):
        self.lib.Reset(10);self.lib.ps5_boot_error(0x201)
        self.assertEqual(self.lib.Value(0),1)

    def test_failure_in_second_report_stops_all_remaining_reports(self):
        self.lib.Reset(11);self.lib.ps5_boot_error(0x100)
        self.assertEqual(self.lib.Value(0),7)

    def test_guarded_return_is_reboot_only_and_never_buzzer(self):
        self.lib.Reset(0);self.assertEqual(self.lib.ps5_boot_reboot(),0)
        self.assertEqual(self.lib.RebootCount(),1);self.assertEqual(self.lib.Value(0),0)
        for mode in (1,2,3,4,6,7,8,9):
            self.lib.Reset(mode);self.assertEqual(self.lib.ps5_boot_reboot(),-10)
            self.assertEqual(self.lib.RebootCount(),0)
