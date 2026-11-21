"""labconf.py v1.0 - site-specific values for the host tools (disk serial, power meter, monitoring paths).

Read from the file named by $PSW_LAB_CONF, else from windows-native-lab/lab.conf next to the repository folder (KEY=VALUE lines,
'#' comments). An environment variable with the same name wins. Without a configured value the placeholder is returned;
tools that guard hardware with these values (disk serial, meter MAC) then refuse to run."""
import os
from pathlib import Path

_HERE = Path(__file__).resolve().parent
_CACHE = None


def _load():
    global _CACHE
    if _CACHE is None:
        _CACHE = {}
        for path in (os.environ.get('PSW_LAB_CONF', ''), str(_HERE.parent.parent / 'windows-native-lab' / 'lab.conf')):
            if path and Path(path).is_file():
                for line in Path(path).read_text().splitlines():
                    line = line.strip()
                    if line and not line.startswith('#') and '=' in line:
                        k, v = line.split('=', 1)
                        _CACHE.setdefault(k.strip(), v.strip())
                break
    return _CACHE


def get(key, placeholder):
    return os.environ.get(key) or _load().get(key) or placeholder
