#!/usr/bin/env python3
"""sync-psw-ui.py v1.0 - copies installer/engine/psw-ui.ps1 (the look of the PSWloader windows) between the markers
'# >>> psw-ui' and '# <<< psw-ui' into every script that shows windows. --check only reports scripts that differ."""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / 'installer/engine/psw-ui.ps1'
TARGETS = ['tools/psw-display.ps1', 'tools/psw-uefi-bootmanager.ps1', 'installer/updater/PSWUpdateCheck.ps1', 'installer/dashboard/PSWDashboard.ps1',
           'installer/engine/psw-welcome.ps1']


def main():
    block = SRC.read_text().strip('\n')
    check = '--check' in sys.argv
    bad = 0
    for t in TARGETS:
        p = ROOT / t
        s = p.read_text()
        a, b = s.find('# >>> psw-ui'), s.find('# <<< psw-ui')
        if a < 0 or b < a:
            print(f'{t}: markers missing'); bad += 1; continue
        b = s.index('\n', b) if '\n' in s[b:] else len(s)
        new = s[:a] + block + s[b:]
        if new != s:
            bad += 1
            print(f'{t}: {"differs" if check else "updated"}')
            if not check:
                p.write_text(new)
    return 1 if (check and bad) else 0


if __name__ == '__main__':
    sys.exit(main())
