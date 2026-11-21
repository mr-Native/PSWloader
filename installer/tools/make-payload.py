#!/usr/bin/env python3
"""make-payload.py EXPECTED.tsv [OUTDIR] (v1.1: OUTDIR, default ../payload) - copy our driver/script builds into ../payload and write payload/manifest.json.
EXPECTED.tsv columns: component <TAB> payload-relative path <TAB> host source path <TAB> sha256 (full). Refuses (exit 1) if a
source is missing or its hash differs - the installer must only ever ship the exact builds listed in EXPECTED.tsv."""
import hashlib, json, shutil, sys
from pathlib import Path
here = Path(__file__).resolve().parent; pay = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else here.parent / 'payload'
rows = [l.rstrip('\n').split('\t') for l in open(sys.argv[1]) if l.strip() and not l.startswith('#')]
man, bad = [], 0
for comp, rel, src, sha in rows:
    p = Path(src)
    if not p.is_file(): print('MISSING', src); bad = 1; continue
    h = hashlib.sha256(p.read_bytes()).hexdigest()
    if h != sha.lower(): print('HASH', src, h, '!=', sha); bad = 1; continue
    dst = pay / rel; dst.parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(p, dst)
    man.append({'component': comp, 'path': rel, 'sha256': h, 'source': src})
if bad: sys.exit(1)
(pay / 'manifest.json').write_text(json.dumps({'files': [{k: v for k, v in m.items() if k != 'source'} for m in man]}, indent=1)); print('manifest', len(man), 'files')
