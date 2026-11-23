#!/usr/bin/env python3
"""v1.0: UEFI ext-APIC IER opener (ps5-extapic.h) + its BDS hook (ps5_boot_manager.py)."""
import os
import subprocess
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
H = (HERE / 'ps5-extapic.h').read_text()
EDK2 = Path(os.environ.get('PSW_WORK', '/opt/pswloader-work') + '/edk2')


class ExtApic(unittest.TestCase):
    def test_ier_written_while_ierEn_set_then_tpr_eoi(self):
        body = H[H.index('Ps5ExtApicOpenIer ('):]
        guard = body.index('(mPs5ExtApic.Ectrl & BIT0) == 0')
        loop = body.index('PS5_XAPIC (Base, 0x480 + 0x10 * Index) = 0xFFFFFFFFU', body.index('} else', body.index('0x480 + 0x10 * 1) |= 0x1U')))   # r5.25.11: '} else' and '{' split by the PS5_WARM_HW_TIMER guard
        self.assertLess(guard, loop)
        # r5.25.5: a stale-ISR drain (EOIs + TPR 0) may precede the loop; after the loop TPR then EOI again
        self.assertLess(loop, body.index('PS5_XAPIC (Base, 0x80) = 0', loop))
        self.assertLess(body.index('PS5_XAPIC (Base, 0x80) = 0', loop), body.index('PS5_XAPIC (Base, 0xB0) = 0', loop))
        self.assertNotIn('PS5_XAPIC (Base, 0x410) =', body)          # never clear IerEn (IER would freeze)
        self.assertIn('(Msr & BIT10) != 0', body)                     # x2APIC: skip

    def test_hooked_first_in_before_console(self):
        src = (HERE / 'ps5_boot_manager.py').read_text()
        self.assertIn("Ps5ExtApicOpenIer ();", src)
        self.assertIn("'ps5-extapic.h'", src)
        self.assertIn('#include "ps5-extapic.h"', (HERE / 'Ps5BootWindows.c').read_text())
        if not (EDK2 / '.git').exists():
            return
        pbm = subprocess.check_output(['git', 'show', 'HEAD:UefiPayloadPkg/Library/PlatformBootManagerLib/PlatformBootManager.c'],
                                      cwd=EDK2, text=True)
        anchor = '  EFI_BOOT_MANAGER_LOAD_OPTION  BootOption;\n\n  //\n  // Register ENTER as CONTINUE key'
        self.assertEqual(pbm.count(anchor), 1)
        fn = pbm[pbm.index('PlatformBootManagerBeforeConsole ('):]
        self.assertLess(fn.index(anchor), fn.index('PlatformConsoleInit'))   # before consoles connect


if __name__ == '__main__':
    unittest.main()
