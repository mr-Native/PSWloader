#!/usr/bin/env python3
"""v1.12.1 host test for the path CLI -> IOCTL buffer -> driver op table (the v1.12.0 HW test failed with error 87 on
-BoostEnterHold). The real ps5pwr.ps1 runs under pwsh with a mock of its [Ps5Pwr] class that records every IOCTL (code,
input bytes, output length) and answers like a v1.12.1 driver or like an old v1.10.3 driver (no VERSION IOCTL, GFXCLK ops
above 4 -> error 87). The recorded GFXCLK input buffers are then decoded by a C program with the driver's own
ps5_smu_gfxclk_decode (smu-mbox.c) and the driver's IOCTL code macros."""
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
CLI = (HERE / 'ps5pwr.ps1').read_text()
DRV = (HERE / 'ps5pwr.c').read_text()

MOCK = r'''Add-Type -TypeDefinition @"
using System;
public static class Ps5Pwr {
  public const uint STATUS = 0x80026400; public const uint POWER = 0x8002a404; public const uint HOTPLUG = 0x8002640c;
  public const uint FAN_GET = 0x80026410; public const uint BUTTON = 0x80026424; public const uint FAN_STATUS = 0x80026420;
  public const uint FAN_SET = 0x8002a414; public const uint TCTL = 0x80026418; public const uint GFXCLK = 0x8002a41c;
  public const uint BAPM = 0x8002a430; public const uint VERSION = 0x80026428;
  public const uint HP_PICK = 0x80026434; public const uint HP_PROGRAM = 0x8002a438;
  public static bool Old = Environment.GetEnvironmentVariable("PS5PWR_MOCK_OLD") == "1";
  public static byte[] Call(uint code, byte[] input, int outLen) {
    Console.Error.WriteLine("IOCTL " + code.ToString("x8") + " " + (input == null ? "-" : BitConverter.ToString(input).Replace("-", "")) + " " + outLen);
    byte[] o = new byte[Math.Max(outLen, 4)];
    if (code == VERSION) {
      if (Old) throw new Exception("ioctl failed 1");
      BitConverter.GetBytes(20u).CopyTo(o, 0); BitConverter.GetBytes(1u).CopyTo(o, 4); BitConverter.GetBytes(13u).CopyTo(o, 8);
      BitConverter.GetBytes(0u).CopyTo(o, 12); BitConverter.GetBytes(0x1F8u).CopyTo(o, 16); return o; }
    if (code == HP_PICK) { BitConverter.GetBytes(2u).CopyTo(o, 0); BitConverter.GetBytes(5u).CopyTo(o, 4); BitConverter.GetBytes(1u).CopyTo(o, 12);
      BitConverter.GetBytes((ushort)3440).CopyTo(o, 24); BitConverter.GetBytes((ushort)1440).CopyTo(o, 32); BitConverter.GetBytes(4u).CopyTo(o, 44); o[48] = 0; o[49] = 0xff; return o; }
    if (code == HP_PROGRAM) { BitConverter.GetBytes(15u).CopyTo(o, 4); BitConverter.GetBytes(BitConverter.ToUInt32(input, 0)).CopyTo(o, 12); BitConverter.GetBytes(6u).CopyTo(o, 16); return o; }
    if (code == GFXCLK && Old && BitConverter.ToUInt32(input, 0) > 4) throw new Exception("ioctl failed 87");
    if (code == GFXCLK) BitConverter.GetBytes(1u).CopyTo(o, 4);
    return o; }
}
"@'''

DECODE = r'''
#include <stdio.h>
#include <stdint.h>
#include "smu-mbox.h"
#define CTL_CODE(d, f, m, a) (((uint32_t)(d) << 16) | ((uint32_t)(a) << 14) | ((uint32_t)(f) << 2) | (m))
#define METHOD_BUFFERED 0
#define FILE_READ_ACCESS 1
#define FILE_WRITE_ACCESS 2
int main(int c, char **v) {
  (void)c;
  printf("%08x %08x\n", (unsigned)(%GFX%), (unsigned)(%VER%));
  for (int i = 1; v[i]; i++) {               /* hex input buffers of GFXCLK calls: op (LE u32) + mhz (LE u32) */
    unsigned a = 0, b = 0; uint32_t op, mhz, arg = 0;
    if (sscanf(v[i], "%8x%8x", &a, &b) != 2) return 2;
    op = __builtin_bswap32(a); mhz = __builtin_bswap32(b);
    uint32_t msg = ps5_smu_gfxclk_decode(op, mhz, &arg);
    printf("%u %u %u %u\n", op, mhz, msg, arg);
  }
  return 0;
}
'''


def macro(name):
    return re.search(r'#define ' + name + r' (CTL_CODE\([^)]*\))', DRV).group(1)


@unittest.skipUnless(shutil.which('pwsh') and shutil.which('cc'), 'pwsh / cc missing')
class CliIoctlPath(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp())
        i = CLI.index('Add-Type -TypeDefinition @"'); j = CLI.index('"@', i) + 2
        (cls.tmp / 'cli.ps1').write_text(CLI[:i] + MOCK + CLI[j:])
        src = DECODE.replace('%GFX%', macro('IOCTL_PS5PWR_GFXCLK')).replace('%VER%', macro('IOCTL_PS5PWR_VERSION'))
        (cls.tmp / 'd.c').write_text(src)
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(HERE), '-o', str(cls.tmp / 'd'),
                        str(cls.tmp / 'd.c'), str(HERE / 'smu-mbox.c')], check=True)
        out = subprocess.run([str(cls.tmp / 'd')], capture_output=True, text=True, check=True).stdout.split()
        cls.gfx, cls.ver = out[0], out[1]

    def cli(self, *args, old=False):
        r = subprocess.run([shutil.which('pwsh'), '-NoProfile', '-File', str(self.tmp / 'cli.ps1'), *args], capture_output=True, text=True,
                           env={'PATH': os.environ.get('PATH', '/usr/bin:/bin'), 'HOME': str(self.tmp), 'PS5PWR_MOCK_OLD': '1' if old else '0'})
        calls = [l.split() for l in r.stderr.splitlines() if l.startswith('IOCTL ')]
        return r, [(c[1], c[2].lower(), int(c[3])) for c in calls]

    def decode(self, bufs):
        out = subprocess.run([str(self.tmp / 'd'), *bufs], capture_output=True, text=True, check=True).stdout.splitlines()[1:]
        return [tuple(int(x) for x in l.split()) for l in out]

    def test_ioctl_codes_match_cli_constants(self):
        self.assertEqual(self.gfx, '8002a41c')
        self.assertEqual(self.ver, '80026428')
        self.assertIn('GFXCLK = 0x8002a41c', CLI)
        self.assertIn('VERSION = 0x80026428', CLI)

    def test_boost_enter_hold_reaches_op5(self):
        r, calls = self.cli('-BoostEnterHold')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        gfx = [c for c in calls if c[0] == self.gfx]
        self.assertEqual(calls[0][0], self.ver)                         # loaded version checked first
        self.assertEqual(gfx[0][1], '0500000000000000')                 # op 5, mhz 0, 8 bytes = PS5PWR_GFXCLK_IN
        self.assertEqual(gfx[0][2], 24)
        self.assertEqual(self.decode([gfx[0][1]])[0], (5, 0, PS5_SMU_MSG_UNIVERSAL_ENTRY, 0))
        self.assertIn('boost_enter_hold', r.stdout)

    def test_above_stock_reaches_op6_with_readback_buffer(self):
        r, calls = self.cli('-SetGfxClk', '2300', '-AllowAboveStock')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        gfx = [c for c in calls if c[0] == self.gfx]
        self.assertEqual(gfx[0][1], '06000000fc080000')                 # op 6, 2300 MHz
        self.assertEqual(gfx[0][2], 32)                                 # PS5PWR_GFXCLK_OUT2
        self.assertEqual(self.decode([gfx[0][1]])[0], (6, 2300, PS5_SMU_MSG_REQUEST_GFXCLK, 2300))

    def test_stock_request_unchanged(self):
        r, calls = self.cli('-SetGfxClk', '2230')
        self.assertEqual(self.decode([[c for c in calls if c[0] == self.gfx][0][1]])[0], (1, 2230, PS5_SMU_MSG_REQUEST_GFXCLK, 2230))
        r, calls = self.cli('-SetGfxClk', '2300')                        # without -AllowAboveStock: refused, nothing sent
        self.assertNotEqual(r.returncode, 0)
        self.assertEqual([c for c in calls if c[0] == self.gfx], [])

    def test_old_driver_gives_clear_message_and_sends_nothing(self):
        r, calls = self.cli('-BoostEnterHold', old=True)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('no boost HOLD', r.stderr + r.stdout)
        self.assertIn('older than 1.12.1', r.stderr + r.stdout)
        self.assertEqual([c for c in calls if c[0] == self.gfx], [])     # op 5 never reaches an old driver
        r, calls = self.cli('-Version', old=True)
        self.assertEqual(r.returncode, 0)
        self.assertIn('older than 1.12.1', r.stdout)

    def test_version(self):
        r, _ = self.cli('-Version')
        self.assertIn('"loaded": "1.13.0"', r.stdout)
        self.assertIn('"hold_and_above_stock": true', r.stdout)

    def test_v113_hotplug_pick_and_program(self):
        self.assertEqual(macro('IOCTL_PS5PWR_HP_PICK'), 'CTL_CODE(0x8002, 0x90D, METHOD_BUFFERED, FILE_READ_ACCESS)')
        self.assertIn('HP_PICK = 0x80026434', CLI)
        r, calls = self.cli('-HotplugPick')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual((calls[0][0], calls[0][2]), ('80026434', 560))                # sizeof(struct ps5_hp_pick)
        for k in ('"seq": 2', '"mode_id": 5', '"mode": "3440p100"', '"width": 3440', '"height": 1440', '"edid_bytes": 4', '"edid_hex": "00ff0000"'):
            self.assertIn(k, r.stdout)
        r, calls = self.cli('-HotplugProgram', '5')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(calls[0], ('8002a438', '05000000', 144))                       # mode id 5, PS5PWR_HP_PROGRAM_OUT
        self.assertIn('"count": 15', r.stdout)

    def test_decode_table(self):
        got = self.decode(['0000000000000000', '01000000b6080000', '0200000000000000', '0300000000000000',
                           '0400000000000000', '0500000000000000', '0600000060090000', '0700000000000000'])
        self.assertEqual([g[2] for g in got], [0x0f, 0x0e, 0x22, 0x23, 0x37, 0x22, 0x0e, 0])
        self.assertEqual([g[3] for g in got], [0, 2230, 0, 0, 0, 0, 2400, 0])

    def test_driver_uses_shared_table_and_version_constants(self):
        self.assertIn('ps5_smu_oc_decode(in->op, in->mhz, &arg, &queue)', DRV)
        self.assertIn('#define PS5PWR_VER_MINOR 18u', DRV)
        self.assertIn('#define PS5PWR_VER_PATCH 4u', DRV)
        self.assertIn('DriverVer   = 11/26/2026,1.19.2.0', (HERE / 'ps5pwr.inf').read_text())
        # v1.17.0: PS5 Linux fan curve in the driver (caps 0x2000), switched by Parameters\FanCurve
        self.assertIn('0x2000u | 0x4000u)', DRV)   # v1.18.0: 0x4000 hotplug FLAVA3+VERDE
        self.assertIn('reg_dword(L"FanCurve", 1)', DRV)
        # v1.17.1: fc_service runs on the ticks where it reads FanCurve itself (1.17.0 never read it), and no
        # registry write happens while icc_lock (a FAST_MUTEX, APC_LEVEL) is held
        self.assertIn('if (tick & 1) fc_service(e);', DRV)
        body = DRV[DRV.index('static NTSTATUS fc_start('):DRV.index('static void fc_stop_locked(')]
        self.assertNotIn('reg_set(', body)
        # v1.17.2: the firmware's GameOS table (PswFanOriginal) is read at PASSIVE outside icc_lock and only used when the
        # snapshot already holds the quiet table
        self.assertNotIn('fw_var(', body)
        self.assertIn('fw_var(L"PswFanOriginal"', DRV)
        self.assertIn('ps5_fc_quiet_left(e->fc_original) && e->fc_fw_original_ok', body)
        # v1.16.0: a restart is left to the firmware's ResetSystem (warm re-entry) when it publishes PswWarmReentry
        self.assertIn('fw_var(L"PswWarmReentry"', DRV)
        self.assertIn('return PoCallDriver(e->lower, irp);\n        }\n        NTSTATUS st = forward_sync', DRV)


PS5_SMU_MSG_UNIVERSAL_ENTRY, PS5_SMU_MSG_REQUEST_GFXCLK = 0x22, 0x0e

if __name__ == '__main__':
    unittest.main()
