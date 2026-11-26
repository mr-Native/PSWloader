#!/usr/bin/env python3
"""static checks for ps5pwr v1.0-v1.4 against the PS5 Linux reference (spcie.c / ps5.c)"""
import os
import re
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
C = (HERE / 'ps5pwr.c').read_text()
INF = (HERE / 'ps5pwr.inf').read_text()
HP = (HERE / 'ps5pwr-hotplug.c').read_text()
BUILD = (HERE / 'build.sh').read_text()
LINUX = Path(os.environ.get('PS5_LINUX_SRC', '/opt/ps5-linux')) / 'drivers/ps5/spcie.c'   # PS5 Linux kernel source (optional)


class Ps5Pwr(unittest.TestCase):
    def test_led_bytes_match_linux_white_dim(self):
        ours = [int(x, 16) for x in re.search(r'k_white_dim\[25\] = \{(.*?)\};', C, re.S).group(1).replace('\n', ' ').split(',')]
        if LINUX.exists():
            ref = re.search(r'indicator_white_dim\[\] = \{(.*?)\};', LINUX.read_text(), re.S).group(1)
            self.assertEqual(ours, [int(x, 16) for x in ref.replace('\n', ' ').replace('\t', ' ').split(',') if x.strip()])
        self.assertIn('icc_query_robust(e, 0x09, LED_TYPE', C)

    def test_shutdown_only_on_s5_and_reboot_on_reset(self):
        self.assertIn('PowerSystemShutdown', C)
        self.assertIn('ShutdownType == PowerActionShutdownReset', C)
        self.assertIn('ps5_icc_power(&io, reboot)', C); self.assertIn('icc_power_robust(e, reboot)', C)
        self.assertIn('InterlockedExchange(&e->power_sent, 1)', C)   # never twice

    def test_doorbell_and_binding(self):
        self.assertIn('#define DOORBELL_OFF 0x108000u', C)
        self.assertIn(r'PCI\VEN_104D&DEV_9107&SUBSYS_9103104D', INF)
        self.assertIn('MSISupported', INF)

    # v1.4 hotplug, never touches ICC outside icc_lock, stops on S5; v1.18.0: ON by default (missing value = 4), 0 switches off
    def test_hotplug_default_on_and_dynamic(self):
        self.assertIn('reg_dword(L"HotplugMode", PS5_HP_PICK)', C)
        self.assertIn('if (m > PS5_HP_PICK) m = 0;', C)   # v1.13: 0..4
        self.assertNotIn('HotplugMode, 0x00010001, 1', INF)

    def test_hotplug_ioctl_and_transport(self):
        self.assertIn('CTL_CODE(0x8002, 0x903, METHOD_BUFFERED, FILE_READ_ACCESS)', C)
        self.assertIn('-DPS5_ICC_LINUX_NOTIFY=1', BUILD)
        self.assertIn('ps5pwr-hotplug.c', BUILD)

    def test_hotplug_stops_before_unmap_and_on_s5(self):
        self.assertRegex(C, r'case IRP_MN_SURPRISE_REMOVAL:\n(\s+oc_safe_reset\("stop"\);[^\n]*\n)?\s+hp_halt\(e\);[^\n]*\n(\s+btn_off\(e, "stop"\);[^\n]*\n)?\s+unmap\(e\);')
        self.assertRegex(C, r'case IRP_MN_REMOVE_DEVICE:\n(\s+oc_safe_reset\("remove"\);[^\n]*\n)?\s+hp_halt\(e\);')
        self.assertRegex(C, r'e->shutdown_type = .*\n\s+InterlockedExchange\(&e->hp_stop, 1\);')

    def test_button_v110_conditions(self):
        # cf (1): off + drain on S5/restart/Sx, IOCTL power, thread exit (stop/remove/unload), enable failure, bugcheck
        self.assertIn('{ 0, (uint8_t)(on != 0) }', C)                      # svc 0x08 type 1 {0,on} as HdmiProbeDxe
        self.assertIn('PS5_BUTTON_SERVICE, 1, req', C)
        self.assertRegex(C, r'btn_off\(e, s->Parameters\.Power\.State\.SystemState == PowerSystemShutdown \? "S5" : "Sx"\)')
        self.assertRegex(C, r'btn_off_locked\(e, "IOCTL power"\);[^\n]*\n\s+e->power_result = icc_power_robust')
        self.assertRegex(C, r'\n    \}\n    btn_off\(e, "thread stops"\);')
        self.assertIn('if (e->btn_enable_result) btn_off_locked(e, "enable failed");', C)
        self.assertIn('KeRegisterBugCheckCallback(&e->btn_bug, btn_bugcheck', C)
        self.assertIn('KeDeregisterBugCheckCallback(&e->btn_bug)', C)
        bug = re.search(r'static VOID btn_bugcheck\(.*?\n\}', C, re.S).group(0)
        self.assertNotIn('icc_acquire', bug); self.assertNotIn('ps5_hp_log', bug); self.assertIn('e->icc_owner', bug)
        # drain loop after the off message
        off = re.search(r'static void btn_off_locked\(.*?\n\}', C, re.S).group(0)
        self.assertLess(off.index('btn_send(e, 0)'), off.index('ps5_icc_poll'))
        # the S5 off runs before the S5 IRP is forwarded / the ICC power message is sent
        self.assertLess(C.index('? "S5" : "Sx"'), C.index('NTSTATUS st = forward_sync(e, irp);        /* let the bus finish first */'))
        # every icc_lock user goes through icc_acquire/icc_release (owner flag for the bugcheck callback)
        self.assertNotIn('ExAcquireFastMutex(&e->icc_lock);', C.replace('static void icc_acquire(PEXT e) { ExAcquireFastMutex(&e->icc_lock);', ''))
        # cf (2): kill switch, default on
        self.assertIn('reg_dword(L"EnablePowerButton", 1)', C)
        # cf (3): named event for the user-mode listener, admins/SYSTEM only
        self.assertIn('\\\\BaseNamedObjects\\\\Ps5PowerButton', C)
        self.assertIn('SeAliasAdminsSid', C); self.assertNotIn('SeWorldSid', C)
        self.assertIn('hp->extra_note(hp->extra_ctx', HP)
        self.assertIn('button-keys.c', BUILD)

    def test_hotplug_firmware_stage_order(self):
        # ps5-native-dp-video.h: 6,7,8 (DpReset), 9,10, DCN, 0..5, 14 if geometry, 11, 12, 13
        # v1.18: shared by FLAVA3 and VERDE (stages_for); VERDE drops its no-op stages 8..10, 4, 5, 14 (host test checks the lists)
        self.assertIn('if (full) { out[n++] = 6; out[n++] = 7; if (!v) { out[n++] = 8; out[n++] = 9; out[n++] = 10; } }', HP)
        self.assertIn('if (geometry && !v) out[n++] = 14;', HP)
        self.assertIn('!hp->bridge_null && hp->bridge.uw_geometry', HP)
        self.assertIn('out[n++] = 11; out[n++] = 12; out[n++] = 13;', HP)


if __name__ == '__main__':
    unittest.main()
