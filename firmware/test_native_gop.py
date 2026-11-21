import os
import unittest
from pathlib import Path
R=Path(__file__).resolve().parent
# Removed 17.11.2026 (G17): four tests that pinned offsets and blobs of retired kernel-side diagnostics
# (compiled out since v386b); see git history.

class GopTests(unittest.TestCase):
    def setUp(self):
        self.g=(R/'ps5-native-gop.h').read_text()
        self.v=(R/'ps5-native-dp-video.h').read_text()
        self.t=(R/'dcn201-pipe-replay.h').read_text()
    def test_v330_quiet_boot_default_and_paints_gated(self):
        g = (HERE / 'ps5-native-gop.h').read_text() if 'HERE' in globals() else Path(__file__).with_name('ps5-native-gop.h').read_text()
        v = Path(__file__).with_name('ps5-native-dp-video.h').read_text()
        for src in (g, v):
            self.assertIn('#ifdef PS5_DIAG_GRAPHICS\n#define PS5_PAINT 1', src)
            self.assertIn('#define PS5_PAINT 0', src)
        # every direct framebuffer pixel loop in the GOP header is gated (Blt/clear are the GOP API itself)
        for line in g.splitlines():
            if '*Ps5GopPixel(' in line and 'for(' in line and 'Ps5RtMark' not in line:
                self.assertIn('if(PS5_PAINT)', line, line.strip()[:80])
        self.assertIn('if(PS5_PAINT)Ps5NativeDpCrcProbe(Pci);', v)                     # 17 s colour probe
        self.assertIn('if(!PS5_PAINT)C=0xff000000;', v)                                # black start surface


    def test_mode_matches_proven_scanout(self):
        g=self.g
        # display modes: size comes from the mode replay, the default stays the proven 1920x1080 surface
        for f in ('mPs5GopW=1920U,mPs5GopH=1080U','PixelBlueGreenRedReserved8BitPerColor','PixelsPerScanLine=PS5_GOP_PITCH','MaxMode=1'):
            self.assertIn(f,g)
    def test_blt_bounds_and_flush(self):
        g=self.g
        self.assertIn('W>PS5_GOP_W-Sx',g);self.assertIn('H>PS5_GOP_H-Dy',g)
        self.assertIn('Op>=EfiGraphicsOutputBltOperationMax',g)
        self.assertIn('if(Op!=EfiBltVideoToBltBuffer)AsmWbinvd();',g)
        self.assertIn('if(ModeNumber!=0)return EFI_UNSUPPORTED;',g)
    def test_gop_only_after_successful_scanout_and_connected(self):
        v=self.v; i=v.index('if(Ps5NativeScanout(Pci)) {')
        self.assertLess(i,v.index('Ps5NativeGopInstallMode(PS5_DCN201_REPLAY_SURFACE,R->width,R->height,R->pitch_pixels);'))
        self.assertIn('ConnectController(mPs5GopHandle,NULL,NULL,TRUE)',self.g)
    def test_replay_writes_address_high_before_low(self):
        for n in '03':
            hi=self.t.index(f'/* HUBPREQ{n}_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH */')
            lo=self.t.index(f'/* HUBPREQ{n}_DCSURF_PRIMARY_SURFACE_ADDRESS */')
            self.assertLess(hi,lo)
class GopConsoleTests(unittest.TestCase):
    def test_v250_conout_registration_and_tag(self):
        g=(R/'ps5-native-gop.h').read_text()
        self.assertIn('EfiBootManagerUpdateConsoleVariable(ConOut',g)
        self.assertIn('0xD3B36F2C,0xD551,0x11D4',g)
        self.assertLess(g.index('InstallProtocolInterface(&mPs5GopHandle,&ConsoleOutDevice'),g.rindex('ConnectController(mPs5GopHandle'))
class EbsMarkerTests(unittest.TestCase):
    def test_v259_ebs_marker_uses_cpu_stores_only(self):
        g=(R/'ps5-native-gop.h').read_text()
        f=g[g.index('STATIC VOID EFIAPI Ps5GopEbsMarker('):g.index('/* v2.60:')]
        self.assertNotIn('gBS',f);self.assertIn('AsmWbinvd()',f)
        self.assertIn('EVT_SIGNAL_EXIT_BOOT_SERVICES',g)
class VaMarkerTests(unittest.TestCase):
    def test_v260_va_marker_uses_cpu_stores_only(self):
        g=(R/'ps5-native-gop.h').read_text()
        f=g[g.index('STATIC VOID EFIAPI Ps5GopVaMarker('):g.index('/* v2.63: runtime-service trace.')]
        self.assertNotIn('gBS',f);self.assertNotIn('gRT',f);self.assertIn('AsmWbinvd()',f)
        self.assertNotIn('CreateEvent(EVT_SIGNAL_VIRTUAL_ADDRESS_CHANGE',g)  # v2.70 removed (faulted under winload CR3)
class LateEbsMarkerTests(unittest.TestCase):
    def test_v262_late_ebs_marker_at_callback(self):
        g=(R/'ps5-native-gop.h').read_text()
        f=g[g.index('STATIC VOID EFIAPI Ps5GopEbsLateMarker('):g.index('STATIC EFI_EVENT mPs5GopVaEvent;')]
        self.assertNotIn('gBS',f);self.assertIn('AsmWbinvd()',f)
        self.assertIn('EVT_SIGNAL_EXIT_BOOT_SERVICES,TPL_CALLBACK,Ps5GopEbsLateMarker',g)
class RtTraceTests(unittest.TestCase):
    def test_v263_rt_trace_restores_before_svam_and_draws_only_in_window(self):
        g=(R/'ps5-native-gop.h').read_text()
        f=g[g.index('STATIC EFI_STATUS EFIAPI Ps5RtSetVirtualAddressMap('):g.index('STATIC VOID Ps5RtTraceInstall(')]
        self.assertLess(f.index('CopyMem('),f.index('mPs5RtOrig.SetVirtualAddressMap('))
        self.assertIn('CalculateCrc32(Rt,Rt->Hdr.HeaderSize)',f)
        m=g[g.index('STATIC VOID Ps5RtMark('):g.index('#define PS5_RT_IN')]
        self.assertIn('if(!mPs5RtTrace)return;',m)
        self.assertIn('mPs5RtTrace=TRUE',g);self.assertIn('mPs5RtTrace=FALSE',g)
class StiPatchTests(unittest.TestCase):
    def test_v268_sti_helper_and_app_ctx(self):
        g=(R/'ps5-native-gop.h').read_text()
        # v3.31+: the load-bearing sti patch is signature-based (Ps5PatchStiBySig over mPs5StiSig),
        # not the old fixed RVA PS5_WL_STI; its fb;c3 still sits at 0x2a9220 in the 9444 winload.
        self.assertIn('Ps5PatchStiBySig',g);self.assertIn('mPs5StiSig',g)
        wl=Path(os.environ.get('PSW_WORK', '/opt/pswloader-work') + '/windows-reference/winload.efi')
        if wl.exists():
            d=wl.read_bytes();self.assertEqual(d[0x400+0x2a9220-0x1000:0x400+0x2a9222-0x1000],bytes([0xfb,0xc3]))
        late=g[g.index('Ps5GopEbsLateMarker(EFI_EVENT'):]
        self.assertIn('Ps5WinloadProbeV268();',late)   # the sti patch runs from the late EBS callback
class NoVaTimeInstrumentTests(unittest.TestCase):
    def test_v270_no_boot_services_code_or_physical_fb_at_svam(self):
        g=(R/'ps5-native-gop.h').read_text()
        inst=g[g.index('STATIC EFI_STATUS Ps5NativeGopInstall('):]
        self.assertNotIn('EVT_SIGNAL_VIRTUAL_ADDRESS_CHANGE',inst)
        self.assertNotIn('Ps5RtTraceInstall();',inst)
        r=(R/'Ps5PlatformPkg/Library/Ps5ResetSystemLib/Ps5ResetSystemLib.c').read_text()
        self.assertNotIn('PS5_FB_BASE',r)
if __name__=='__main__':unittest.main()
