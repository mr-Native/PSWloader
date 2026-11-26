"""r5.28.7: "Boot once" into a firmware disk entry hands the entry's name hash to the restarted firmware, which numbers its
Boot#### entries anew (HW 26.11.: Boot0002 was another disk there, the RTL9210B was never taken although it was present)."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

HERE = Path(__file__).parent
FW = (HERE / 'Ps5BootWindows.c').read_text()
RESET = (HERE / 'Ps5PlatformPkg/Library/Ps5ResetSystemLib/Ps5ResetSystemLib.c').read_text()


def fnv(text):
    h = 0x811C9DC5
    for ch in text:
        for byte in (ord(ch) & 0xFF, ord(ch) >> 8):
            h = ((h ^ byte) * 16777619) & 0xFFFFFFFF
    return h


class NameHashTests(unittest.TestCase):
    def test_firmware_hash_is_fnv1a_over_utf16(self):
        line = next(l for l in FW.splitlines() if l.startswith('STATIC UINT32 Ps5NameHash('))
        c = ('#include <stdio.h>\n#include <stdint.h>\ntypedef uint32_t UINT32; typedef uint8_t UINT8; typedef uint16_t CHAR16;\n'
             '#define STATIC static\n#define CONST const\n#define NULL ((void*)0)\n' + line +
             '\nint main(void){ static const CHAR16 s[]={' + ','.join(str(ord(c)) for c in 'USB: Realtek RTL9210B-CG 512 GB') +
             ',0}; printf("%08x", Ps5NameHash(s)); return 0; }\n')
        with tempfile.TemporaryDirectory() as d:
            Path(d, 't.c').write_text(c)
            subprocess.run(['gcc', '-o', f'{d}/t', f'{d}/t.c'], check=True)
            out = subprocess.run([f'{d}/t'], capture_output=True, text=True, check=True).stdout
        self.assertEqual(int(out, 16), fnv('USB: Realtek RTL9210B-CG 512 GB'))

    def test_reset_lib_uses_the_same_hash_and_marker(self):
        body = RESET[RESET.index('STATIC VOID WarmOptionName ('):RESET.index('STATIC VOID WarmReadOsRequest (')]
        self.assertIn('0x811C9DC5u', body)
        self.assertIn('H ^= (UINT8)Desc[D]; H *= 16777619u; H ^= (UINT8)(Desc[D] >> 8); H *= 16777619u;', body)
        self.assertIn('Out[6] = 0xA5;', body)
        self.assertIn('WarmOptionName (Next, P->NextGuid);', RESET)

    def test_restarted_firmware_trusts_the_name_not_the_number(self):
        h = FW[FW.index('  else if(Flags&8) {'):FW.index('  else if(L[0]==0x315453414C575350ULL){CopyMem(&mPs5WarmGuid')]
        self.assertIn('mPs5WarmNextHashOk=(Gb[6]==0xA5);', h)
        self.assertIn('Ps5NameHash(O.Description)==mPs5WarmNextHash', h)
        self.assertIn('EfiBootManagerGetLoadOptions(&Cnt,LoadOptionTypeBoot)', h)
        self.assertIn('mPs5WarmDirect=TRUE; mPs5WarmNextPending=TRUE;', h)
        self.assertIn('Ps5NameHash(Nm)==mPs5WarmNextHash', FW)
        self.assertIn('if (mPs5WarmNextPending && Lb[0]==0x315453414C575350ULL && !mPs5WarmFallbackAt) {', FW)




class UsbEntriesAcrossStartsTests(unittest.TestCase):   # r5.28.9: the r5.28.8 ESP list hung every internal start
    def test_no_usb_list_on_the_esp(self):
        self.assertNotIn('Ps5UsbListSync(', FW)
        self.assertNotIn('L"\\\\PSWloader\\\\usbdisks.bin"', FW)

if __name__ == '__main__':
    unittest.main()
