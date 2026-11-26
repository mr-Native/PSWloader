#!/usr/bin/env python3
"""psw-hide-watermark.py v1.0 (PSWloader v1.0, 18 November 2026) - removes the text in the lower right corner of Windows
("Test Mode", "Build 26100...", evaluation / secure boot / system requirement notes) by blanking its string-table entries in
shell32.dll.mui. HW 18.11.: in Windows Setup the text comes only from shell32.dll.mui and is read when the screen starts,
so the file has to be changed inside the setup stick's sources\\boot.wim, not at run time.

  psw-hide-watermark.py FILE.mui [OUT]          patch one shell32.dll.mui (OUT default: FILE in place)
  psw-hide-watermark.py --wim boot.wim          patch every shell32.dll.mui of every image in boot.wim (needs wimlib-imagex)

Blanked (characters replaced by spaces, same length, so nothing else in the file moves):
  - the test-mode entry (Testmodus / Test Mode / Mode test / Modo de prueba / Modalita test);
  - every entry that holds "(Build %ws" (the "Microsoft (R) Windows (R) (Build ...)" lines) and the entry right after the last
    of them ("device under test" text);
  - the entry "%ws Build %ws" and the entries after it up to the end of its string block (evaluation copy, licence, secure
    boot and system requirement notes of the same corner).
The safe-mode entry stays. Language independent: found by the format strings, not by translated words."""
import os, subprocess, sys, tempfile

NEEDLES = ['Testmodus', 'Test Mode', 'Mode test', 'Modo de prueba', 'Modalità test']


def entry_at(b, k):
    n = b[k] | (b[k + 1] << 8)
    return n, b[k + 2:k + 2 + 2 * n].decode('utf-16le', 'replace')


def blank(b, k):
    n = b[k] | (b[k + 1] << 8)
    b[k + 2:k + 2 + 2 * n] = (' ' * n).encode('utf-16le')


def starts(b, text):
    """offsets k of string-table entries whose text is exactly text (k = length word)"""
    pat = text.encode('utf-16le'); out = []; i = b.find(pat)
    while i >= 2:
        if (b[i - 2] | (b[i - 1] << 8)) == len(text) and i % 2 == 0: out.append(i - 2)
        i = b.find(pat, i + 2)
    return out


def patch(data):
    b = bytearray(data); hits = []
    for n in NEEDLES:
        for k in starts(b, n): blank(b, k); hits.append(n)
    # "(Build %ws" lines: walk the block that holds the test-mode entry
    for k in starts(bytes(data), NEEDLES[0]) + [s for n in NEEDLES[1:] for s in starts(bytes(data), n)]:
        last = None; p = k
        for _ in range(16):
            n, t = entry_at(data, p)
            if '(Build %ws' in t: blank(b, p); hits.append(t[:40]); last = p
            p += 2 + 2 * n
            if p + 2 > len(data): break
        if last is not None:
            n, _ = entry_at(data, last); q = last + 2 + 2 * n
            n2, t2 = entry_at(data, q)
            if n2: blank(b, q); hits.append(t2[:40])
    for k in starts(bytes(data), '%ws Build %ws'):
        p = k
        for _ in range(16):
            n, t = entry_at(data, p)
            if n == 0: break
            blank(b, p); hits.append(t[:40]); p += 2 + 2 * n
            if p + 2 > len(data): break
    return bytes(b), hits


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == '--wim':
        wim = sys.argv[2]
        info = subprocess.run(['wimlib-imagex', 'info', wim], capture_output=True, text=True, check=True).stdout
        count = int([l for l in info.splitlines() if l.startswith('Image Count:')][0].split(':')[1])
        for idx in range(1, count + 1):
            listing = subprocess.run(['wimlib-imagex', 'dir', wim, str(idx), '--path=/Windows/System32'], capture_output=True, text=True).stdout
            langs = [l.rsplit('/', 1)[1] for l in listing.splitlines() if l.count('/') == 3 and len(l.rsplit('/', 1)[1]) == 5 and l.rsplit('/', 1)[1][2] == '-']
            with tempfile.TemporaryDirectory() as td:
                cmds = []
                for lang in langs:
                    src = f'/Windows/System32/{lang}/shell32.dll.mui'
                    r = subprocess.run(['wimlib-imagex', 'extract', wim, str(idx), src, f'--dest-dir={td}/{lang}', '--no-acls'], capture_output=True)
                    f = os.path.join(td, lang, 'shell32.dll.mui')
                    if r.returncode or not os.path.exists(f): continue
                    new, hits = patch(open(f, 'rb').read())
                    if not hits: continue
                    open(f + '.new', 'wb').write(new); cmds.append(f'add {f}.new {src}')
                    print(f'image {idx} {lang}: {len(hits)} entries blanked')
                if cmds:
                    subprocess.run(['wimlib-imagex', 'update', wim, str(idx)], input='\n'.join(cmds) + '\n', text=True, check=True, capture_output=True)
        return
    src = sys.argv[1]; out = sys.argv[2] if len(sys.argv) > 2 else src
    new, hits = patch(open(src, 'rb').read())
    open(out, 'wb').write(new)
    for h in hits: print('blanked:', h)


if __name__ == '__main__':
    main()
