"""r5.25.18 (HW 23.11. diag14): after a warm re-entry the ICC button poller timer stays off; the button is polled
only when a console reads keys. With the periodic timer a warm restart from WinPE / Windows Setup hung in DXE."""
from pathlib import Path
import unittest

SOURCE = (Path(__file__).parent / 'Ps5PlatformPkg/HdmiProbeDxe/HdmiProbeDxe.c').read_text()
START = SOURCE[SOURCE.index('STATIC VOID Ps5ButtonInputStart(VOID) {'):]


class WarmButtonPollerTests(unittest.TestCase):
    def test_periodic_timer_only_without_warm_reentry(self):
        self.assertIn('  if(!mPs5Warm)\n    gBS->SetTimer(mButtonTimer,TimerPeriodic,500000);', START)
        self.assertLess(START.index('mPs5Warm = ('), START.index('gBS->SetTimer(mButtonTimer,TimerPeriodic,500000);'))
        self.assertNotIn('PS5_WARM_NO_BUTTON_POLL', SOURCE)

    def test_reader_still_polls(self):
        read = SOURCE[SOURCE.index('STATIC EFI_STATUS EFIAPI ButtonRead('):]
        self.assertIn('if(!ps5_button_pending(&mKeys))ButtonPoll(NULL,NULL);', read[:600])


if __name__ == '__main__':
    unittest.main()
