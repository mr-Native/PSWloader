"""v380: a medium found within the old 100 passes boots as before; without one the display starts anyway and
the search continues with a USB port power cycle every 25 s."""
from pathlib import Path
import re
import unittest

SOURCE = (Path(__file__).parent / 'Ps5BootWindows.c').read_text()
BODY = SOURCE[SOURCE.index('EFI_STATUS Ps5BootPinnedWindows (VOID)'):]
FOUND = BODY[BODY.index('if (Selected!=NULL) {'):BODY.index('gBS->Stall(100000);')]
WAIT = BODY[BODY.index('gBS->Stall(100000);'):]


class BootMediumWaitTests(unittest.TestCase):
    def test_found_medium_keeps_the_proven_order(self):
        # VBIOS copy to the ESP, then the display, then the boot menu: the order of v367p
        order = [FOUND.index(x) for x in ('Ps5ExportVbios(Selected)', 'if (!DisplayStarted) { Ps5StartDisplay(); DisplayStarted=TRUE; }',
                                           'EfiBootManagerRefreshAllBootOption();', 'Ps5NativeSetupWindow(NULL)')]
        self.assertEqual(order, sorted(order))
        start = SOURCE[SOURCE.index('STATIC VOID Ps5StartDisplay (VOID)'):SOURCE.index('EFI_STATUS Ps5BootPinnedWindows (VOID)')]
        self.assertLess(start.index('Ps5NativeGpuProbe();'), start.index('Ps5NativeDpSource();'))
        for call in ('Ps5NativeGpuProbe();', 'Ps5NativeDpSource();'):
            self.assertEqual(SOURCE.count(call), 1)

    def test_first_100_passes_stay_quiet_then_the_display_starts_without_a_medium(self):
        self.assertEqual(re.findall(r'#define PS5_BOOT_MEDIUM_QUIET_ATTEMPTS (\d+)U', SOURCE), ['30'])   # r2: 3 s
        self.assertEqual(re.findall(r'#define PS5_BOOT_MEDIUM_ATTEMPTS\s+(\d+)U', SOURCE), ['1300'])
        quiet = WAIT.index('if (Attempt+1<PS5_BOOT_MEDIUM_QUIET_ATTEMPTS) continue;')
        display = WAIT.index('Ps5StartDisplay(); DisplayStarted=TRUE;')
        status = WAIT.index('Print(L"[boot medium: none')
        self.assertLess(quiet, display)
        self.assertLess(display, status)
        self.assertNotIn('Attempt<100', SOURCE)

    def test_port_power_cycle_at_once_and_every_25_s_status_every_5_s(self):
        self.assertIn('if (Late%50!=0) continue;', WAIT)
        self.assertIn('Ps5UsbRecover(!Patient && Cyc%250==0,NULL);', WAIT)   # r4: patient 60 s first (r5.25.2: no kept port)
        self.assertEqual(WAIT.count('EfiBootManagerConnectAll();'), 1)
        self.assertLess(WAIT.index('Ps5UsbRecover(!Patient && Cyc%250==0,NULL);'), WAIT.index('EfiBootManagerConnectAll();'))
        self.assertLess(WAIT.index('EfiBootManagerConnectAll();'), WAIT.index('return EFI_NOT_FOUND;'))
        # the console cannot scroll: a fresh page with every power cycle after the first
        self.assertIn('if (!Patient && Cyc%250==0 && Cyc!=0 && gST->ConOut!=NULL) gST->ConOut->ClearScreen(gST->ConOut);', WAIT)   # r4
        recover = SOURCE[SOURCE.index('STATIC VOID Ps5UsbRecover (BOOLEAN PowerCycle, EFI_HANDLE Keep)'):SOURCE.index('STATIC VOID Ps5StartDisplay (VOID)')]
        self.assertIn('Class[2]!=0x0c || Class[1]!=0x03 || Class[0]!=0x30', recover)          # xHCI only
        self.assertIn('if (!PowerCycle) continue;', recover)                                  # status calls change nothing
        # power off while the driver still runs, wait two seconds (r5.25.11: one pause for all controllers), then stop and start
        order = [recover.index(x) for x in ('V=(V&PS5_XHCI_PORT_NEUTRAL)&~PS5_XHCI_PORT_POWER;', 'if (Cy) gBS->Stall(2000000);',
                                             'gBS->DisconnectController(CyH[K],NULL,NULL);', 'gBS->ConnectController(CyH[K],NULL,NULL,TRUE);')]
        self.assertEqual(order, sorted(order))
        # PORTSC is written without its write-1-to-clear bits (bit 1 PED, bits 17..23), without PR and without WPR
        self.assertEqual(0x4e00ffe9 & ((1 << 1) | (1 << 4) | (0x7f << 17) | (1 << 31)), 0)
        self.assertEqual(0x4e00ffe9 & 0x200, 0x200)

    def test_usbpowercycle_keeps_the_boot_port_and_waits_quietly(self):   # r5.25.2
        recover = SOURCE[SOURCE.index('STATIC VOID Ps5UsbRecover (BOOLEAN PowerCycle, EFI_HANDLE Keep)'):SOURCE.index('STATIC VOID Ps5StartDisplay (VOID)')]
        self.assertIn('|| P+1==KeepPort ||', recover)   # r5.25.3: plus working USB disks kept
        self.assertIn('return (UINT32)((USB_DEVICE_PATH *)Nx)->ParentPortNumber+1;', SOURCE)   # r5.25.3: Ps5RootPortOf
        self.assertIn('KeepPort=Ps5RootPortOf(KeepDp,HcDp,HcLen);', recover)
        # r5.25.3: ports with a working USB device stay powered; no controller restart when nothing needs a cycle
        self.assertIn('if ((V&3U)==3U && !(ZeroMask&(1U<<P))', recover)   # r5.25.11: + warm-run condition
        self.assertLess(recover.index('if (Off==0)'), recover.index('if (Cy) gBS->Stall(2000000);'))
        self.assertIn('Ps5UsbRecover(TRUE,Selected);', BODY)
        # the hold is decided before the boot list, and the no-medium path never power-cycles during it
        self.assertLess(BODY.index('BOOLEAN CycleHold=FALSE;'), BODY.index('BOOLEAN ListDecided=FALSE;'))
        self.assertLess(WAIT.index('if (CycleHold) continue;'), WAIT.index('Ps5UsbRecover(!Patient'))
        self.assertIn('else if (!CycleHold) DEBUG((DEBUG_ERROR,"PS5 boot list: entry %u present - booting it', BODY)

    def test_warm_usb_target_waits_from_the_cycle(self):
        # r5.25.12: a USB boot device is marked in the last-boot record; a warm run waits 60 s for it, counted from the
        # usbpowercycle (controller restart re-enumerates the chosen USB disk), and cycles a 0-byte bridge after 5 s
        self.assertIn('L[3]=Ps5HandleBehindUsb(Selected)?PS5_WARM_LASTUSB:0;', SOURCE)
        self.assertLess(SOURCE.index('L[3]=Ps5HandleBehindUsb(Selected)'), SOURCE.index('      L[0]=0x315453414C575350ULL;'))
        self.assertIn('mPs5WarmUsb=(L[0]==0x315453414C575350ULL && L[3]==PS5_WARM_LASTUSB);', SOURCE)
        warm = BODY[BODY.index('if (mPs5WarmDirect) {'):BODY.index('} else if (BootListN) {')]
        self.assertIn('UINTN Limit=mPs5WarmUsb?PS5_WARM_USB_WAIT_ATTEMPTS:PS5_BOOTLIST_WAIT_ATTEMPTS;', warm)
        self.assertIn('if (CycleDone && CycleAt+PS5_WARM_USB_WAIT_ATTEMPTS>Limit) Limit=CycleAt+PS5_WARM_USB_WAIT_ATTEMPTS;', warm)
        self.assertLess(warm.index('Limit=CycleAt+'), warm.index('if (Attempt<Limit) {'))
        self.assertLess(warm.index('if (Attempt<Limit) {'), warm.index('Ps5WarmColdRestart("target not found");'))
        self.assertIn('mPs5CycleOnlyZero=TRUE; mRecoverSilent=TRUE; Ps5UsbRecover(TRUE,NULL);', warm)

    def test_restart_shows_start_screen_unless_fast_restart(self):
        # r5.25.12 (user): a restart from Windows shows the start screen like a cold start (fastboot: 3 or 15 s);
        # only fastrestart=1 skips it (and the monitor lead); a cold start never looks at fastrestart
        self.assertIn('BOOLEAN SkipScreen=mPs5WarmDirectHit && mPs5Set.fastrestart;', FOUND)
        self.assertIn('if(mPs5FrameTsc0 && !SkipScreen) {', FOUND)
        self.assertIn('EFI_STATUS SetupStatus=SkipScreen?EFI_SUCCESS:Ps5NativeSetupWindow(NULL);', FOUND)
        self.assertNotIn('mPs5WarmDirectHit?EFI_SUCCESS:Ps5NativeSetupWindow', SOURCE)
        settings = (Path(__file__).parent / 'ps5-settings.h').read_text()
        self.assertIn('s->fastrestart = 0;', settings)
        self.assertIn('if (s->fastrestart == 1) n = ps5_set_put(out, n, cap, "fastrestart=1\\r\\n");', settings)
        vfr = (Path(__file__).parent / 'Ps5PlatformPkg/Ps5InfoDxe/Ps5BootVfr.vfr').read_text()
        self.assertLess(vfr.index('varid = Ps5FastBoot.Value'), vfr.index('varid = Ps5FastRestart.Value'))

if __name__ == '__main__':
    unittest.main()
