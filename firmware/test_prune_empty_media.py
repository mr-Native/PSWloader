#!/usr/bin/env python3
"""r5.28.12/13: boot options of empty card-reader slots ("USB: Generic- SD/MMC 0 GB") are pruned on every refresh path,
without the full prune (which would drop USB disks the boot path did not look for)."""
import pathlib, re, unittest
SRC = (pathlib.Path(__file__).with_name('Ps5BootWindows.c')).read_text()

class PruneEmptyMedia(unittest.TestCase):
    def test_function_only_touches_present_devices(self):
        body = SRC[SRC.index('STATIC VOID Ps5PruneEmptyMedia(VOID)'):]
        body = body[:body.index('\n}\n')]
        self.assertIn('IsDevicePathEnd(Dp)', body)            # whole-disk BlockIo found for the complete path
        self.assertIn('LOAD_OPTION_HIDDEN', body)
        self.assertIn('MediaPresent && B->Media->LastBlock>0', body)
        self.assertIn('Ps5IsCardReader(H)', body)             # r5.28.13: a stuck USB bridge (0 bytes) keeps its option
    def test_called_after_every_refresh(self):
        for m in re.finditer(r'EfiBootManagerRefreshAllBootOption\(\);', SRC):
            after = SRC[m.end():m.end() + 260]
            self.assertIn('Ps5PruneEmptyMedia();', after, SRC[m.start() - 80:m.end() + 120])
    def test_restore_skips_zero_gb(self):
        body = SRC[SRC.index('STATIC VOID Ps5RestoreUsbBootOptions(VOID)'):]
        self.assertIn('Ps5DescNoMedium(E->Description)', body[:body.index('\n}\n')])

if __name__ == '__main__':
    unittest.main()

