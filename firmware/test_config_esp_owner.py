"""r5.16.1 (config-esp-owner-retry): bootorder follows the config ESP that owns the settings.
Re-read when the owner handle changes between BDS passes, default internal (0) when the owner disappears."""
from pathlib import Path
import unittest

SOURCE = (Path(__file__).parent / 'Ps5BootWindows.c').read_text()
BODY = SOURCE[SOURCE.index('EFI_STATUS Ps5BootPinnedWindows (VOID)'):]
PASS = BODY[BODY.index('EFI_HANDLE ConfigEsp=Internal;'):BODY.index('EFI_HANDLE Selected = Internal')]


class ConfigEspOwnerTests(unittest.TestCase):
    def test_no_first_value_latch(self):
        self.assertNotIn('BootOrderRead', SOURCE)
        self.assertIn('EFI_HANDLE BootOrderEsp=NULL;', BODY)

    def test_owner_change_rereads_and_loss_defaults_internal(self):
        order = [PASS.index(x) for x in ('mPs5ConfigEsp=ConfigEsp;', 'if (ConfigEsp!=BootOrderEsp) {',
                                          'BootOrderEsp=ConfigEsp;',
                                          'Ps5PeekSettings(ConfigEsp,&Pk); BootOrder=ConfigEsp?Pk.bootorder:0;')]   # r5.21: + bootlist
        self.assertEqual(order, sorted(order))
        self.assertEqual(SOURCE.count('Ps5PeekSettings(ConfigEsp,&Pk)'), 1)
        self.assertIn('BootListN=ConfigEsp?Pk.bootlist_n:0;', PASS)


if __name__ == '__main__':
    unittest.main()
