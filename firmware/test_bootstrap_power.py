"""Decode the C emitter's captured slots; no physical power-channel claim."""
import ctypes as C
from pathlib import Path
import subprocess
import tempfile
import unittest
from power_trace import METER_MAC,detect,decode_packet

class PowerEmitterTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work=tempfile.TemporaryDirectory();work=Path(cls.work.name)
        (work/'mock.c').write_text('''
unsigned Count,Levels[128],Durations[128];
void ps5_power_test_slot(unsigned level,unsigned duration) {
 if(Count<128){Levels[Count]=level;Durations[Count]=duration;} Count++;
}
void Reset(void){Count=0;}
''')
        subprocess.run(['gcc','-shared','-fPIC','-Wall','-Wextra','-Werror','-DPS5_POWER_TEST',str(Path(__file__).with_name('bootstrap-power.c')),str(work/'mock.c'),'-o',str(work/'power.so')],check=True)
        cls.lib=C.CDLL(str(work/'power.so'));cls.lib.ps5_power_report.argtypes=[C.c_uint]
    @classmethod
    def tearDownClass(cls):cls.work.cleanup()
    def test_real_c_packet_decodes_on_host_with_baseline_drift(self):
        for code in (3,0x100,0x203,0x208,0x300,0x505,0x604,0x701,0x703,0x704,0x705,0x706,0x707,0x708,0x709,0x70a,0x70b,0x70c,0x70d,0x70e,*range(0x711,0x718),0x403,0x40e,0x41f,0x4ff,0x800,0x80e,0x820,0x8ff):
            self.lib.Reset();self.lib.ps5_power_calibrate();self.lib.ps5_power_report(code)
            count=C.c_uint.in_dll(self.lib,'Count').value
            self.assertEqual(count,104)
            levels=(C.c_uint*128).in_dll(self.lib,'Levels');durations=(C.c_uint*128).in_dll(self.lib,'Durations')
            rows=[]
            for i in range(count):
                for _ in range(durations[i]):
                    second=len(rows)
                    rows.append({'sampled_at':1000+second,'watts':80+4*levels[i]+.001*second,'mac':METER_MAC,'up':True,'query_seconds':.05})
            self.assertEqual(len(rows),448)
            decoded=decode_packet(rows,detect(rows,start=1000))
            self.assertEqual(decoded['code'],code)
            if code>>8==8:self.assertEqual((decoded['kind'],decoded['vector']),('dxe_unhandled_vector',code&255))
            if code==0x703:self.assertEqual(decoded['kind'],'uefi_dxe_ready')
            if code==0x704:self.assertEqual(decoded['kind'],'dxe_core_entry')
            if code==0x705:self.assertEqual(decoded['kind'],'dxe_exception_setup_passed')
            if code==0x706:self.assertEqual(decoded['kind'],'dxe_memory_services_passed')
            if code==0x707:self.assertEqual(decoded['kind'],'dxe_before_first_dispatch')
            if code==0x709:self.assertEqual(decoded['kind'],'cpu_driver_entry')
            if code==0x708:
                self.assertEqual((decoded['kind'],decoded['missing_protocol_mask']),('log_export_entry_protocols_present',0))
            if 0x711<=code<=0x717:
                self.assertEqual(decoded['kind'],'log_export_entry_protocols_missing')
                self.assertEqual(decoded['missing_protocol_mask'],code&7)
                self.assertEqual(decoded['missing_protocols'],[name for bit,name in ((1,'cpu'),(2,'rtc'),(4,'timer')) if code&bit])
    def test_fault_and_unknown_codes_emit_nothing(self):
        for code in (0,0x420,0x4fe,0x209,0x605,0x710,0x718,0x900,0xffff):
            self.lib.Reset();self.lib.ps5_power_report(code)
            self.assertEqual(C.c_uint.in_dll(self.lib,'Count').value,0)

    def test_c_encoder_roundtrip_for_pci_register_value(self):
        self.lib.ps5_power_report_pci_byte.argtypes=[C.c_uint,C.c_uint,C.c_uint8]
        for bridge in (0,1):
            self.lib.Reset();self.lib.ps5_power_calibrate();self.lib.ps5_power_report_pci_byte(bridge,4,6)
            count=C.c_uint.in_dll(self.lib,'Count').value
            levels=(C.c_uint*128).in_dll(self.lib,'Levels');durations=(C.c_uint*128).in_dll(self.lib,'Durations');rows=[]
            for i in range(count):
                for _ in range(durations[i]):rows.append({'sampled_at':1000+len(rows),'watts':80+4*levels[i],'mac':METER_MAC,'up':True})
            result=decode_packet(rows,detect(rows,start=1000))
            self.assertEqual((result['register_offset'],result['actual_byte']),(4,6))
            self.assertEqual(result['bridge'],'amd' if bridge else 'sony')
        for bridge,offset in ((2,4),(0,0x3f),(1,7)):
            self.lib.Reset();self.lib.ps5_power_report_pci_byte(bridge,offset,6)
            self.assertEqual(C.c_uint.in_dll(self.lib,'Count').value,0)
