import re,unittest
from pathlib import Path
R=Path(__file__).resolve().parent
class CrcProbeTests(unittest.TestCase):
    def setUp(self):
        s=(R/'ps5-native-dp-video.h').read_text()
        self.src=s;self.probe=s[s.index('STATIC VOID Ps5NativeDpCrcProbe('):s.index('#ifdef PS5_NATIVE_DP_SCANOUT')]
    def test_crc_enabled_with_full_frame_windows(self):
        p=self.probe
        # OTG_CRC_CNTL cleared first, windows (0x140a8..0x140b4) 1920x1080, then CONT_EN|EN
        self.assertLess(p.index('{0x140a0,0}'),p.index('{0x140a0,0x11}'))
        for reg,end in (('0x140a8','1920'),('0x140ac','1080'),('0x140b0','1920'),('0x140b4','1080')):
            self.assertIn('{%s,%sU<<16}'%(reg,end),p)
        self.assertIn('VideoWrite(Pci,0x154a4,Dig|1)',p)
    def test_only_dpg_colour_and_crc_registers_written(self):
        writes=set(re.findall(r'VideoWrite\(Pci,(0x[0-9a-f]+)',self.probe))|set(re.findall(r'\{(0x[0-9a-f]+),',self.probe.split('CONST UINT32 Colour')[0].split('Setup[][2]=')[1]))
        self.assertEqual(writes,{'0x140a0','0x140a8','0x140ac','0x140b0','0x140b4','0x154a4','0x1345c','0x13460','0x13464','0x14080','0x14008'})
    def test_rgb_sequence_ends_red_and_probe_runs_after_final_state(self):
        self.assertIn('{0xffc0ffc0,0,0}};',self.probe)
        body=self.src[self.src.index('STATIC EFI_STATUS Ps5NativeDpVideo('):]
        self.assertLess(body.index('final state after converter output start'),body.index('Ps5NativeDpCrcProbe(Pci)'))
class OtgBlankColourTests(unittest.TestCase):
    def test_v242_blank_green_then_restore(self):
        s=(R/'ps5-native-dp-video.h').read_text()
        p=s[s.index('STATIC VOID Ps5NativeDpCrcProbe('):s.index('#ifdef PS5_NATIVE_DP_SCANOUT')]
        self.assertLess(p.index('VideoWrite(Pci,0x14080,0x000ffc00)'),p.index('BlankBefore|0x100'))
        self.assertLess(p.index('BlankBefore|0x100'),p.index('BlankBefore&~0x100U'))
        self.assertIn('VideoWrite(Pci,0x14080,BlackBefore)',p)
class ScanoutTests(unittest.TestCase):
    def setUp(self):
        s=(R/'ps5-native-dp-video.h').read_text()
        self.fn=s[s.index('STATIC BOOLEAN Ps5NativeScanout('):s.index('STATIC VOID Sample60f9(')]
        self.rows=[l for l in (R/'dcn201-pipe-replay.h').read_text().splitlines() if l.startswith(' {0x')]
    def test_page_check_before_cpu_write_and_flush_before_replay(self):
        f=self.fn
        self.assertLess(f.index('Ps5NativeFramebufferPageCheck'),f.index('P[Y*Pitch+X]=C'))
        self.assertLess(f.index('AsmWbinvd()'),f.index('R->rows[I].offset'))   # mode replay table since display modes
    def test_unblank_after_replay_and_dpg_released_last(self):
        f=self.fn
        self.assertLess(f.index('R->rows[I].offset'),f.index('VideoWrite(Pci,R->hubp_cntl[I],V&~1U)'))   # unblank every HUBP of the mode after the replay
        self.assertLess(f.index('VideoWrite(Pci,R->hubp_cntl[I],V&~1U)'),f.index('VideoWrite(Pci,0x13450,V&~1U)'))   # DPG released last
    def test_replay_keeps_hubp_blanked_and_targets_surface(self):
        cntl=[l for l in self.rows if 'DCHUBP_CNTL ' in l]
        self.assertEqual(len(cntl),2)
        for l in cntl:self.assertTrue(int(l.split(',')[1].split('}')[0],16)&1)
        self.assertTrue(any('0x6c000000' in l and 'PRIMARY_SURFACE_ADDRESS */' in l for l in self.rows))
        names=[l.split('/*')[1] for l in self.rows]
        self.assertFalse(any(x in n for n in names for x in ('STATUS','READ_LINE','LB_V_COUNTER','SOFT_RESET')))
        self.assertLess(max(i for i,n in enumerate(names) if 'DCHUBBUB' in n),min(i for i,n in enumerate(names) if 'MPCC' in n))
if __name__=='__main__':unittest.main()
