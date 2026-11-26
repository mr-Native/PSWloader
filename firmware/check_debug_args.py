#!/usr/bin/env python3
"""check_debug_args.py v1.0 - count the variable arguments of every DEBUG((...)) call.

The payload's DXE DebugLib (PeiDxeDebugLibReportStatusCode) packs at most 12 arguments; a 13th trips
ASSERT((CHAR8 *)BaseListMarkerPointer <= FormatString) and, with PcdDebugPropertyMask 0x2F, CpuDeadLoop().
Usage: check_debug_args.py <file.c|.h> ...   (exit 1 if any call has more than 12 arguments)
"""
import re, sys

def calls(text):
    for m in re.finditer(r'DEBUG\s*\(\s*\(', text):
        i, depth, start = m.end(), 2, m.end()
        in_str = False
        while i < len(text) and depth:
            c = text[i]
            if in_str:
                if c == '\\': i += 1
                elif c == '"': in_str = False
            elif c == '"': in_str = True
            elif c == '(': depth += 1
            elif c == ')': depth -= 1
            i += 1
        body = text[start:i - 2]
        # split top-level commas
        parts, depth, cur, in_str, j = [], 0, '', False, 0
        while j < len(body):
            c = body[j]
            if in_str:
                cur += c
                if c == '\\': cur += body[j + 1]; j += 1
                elif c == '"': in_str = False
            elif c == '"': in_str = True; cur += c
            elif c in '([{': depth += 1; cur += c
            elif c in ')]}': depth -= 1; cur += c
            elif c == ',' and depth == 0: parts.append(cur); cur = ''
            else: cur += c
            j += 1
        parts.append(cur)
        line = text.count('\n', 0, m.start()) + 1
        yield line, len(parts) - 2, ' '.join(parts[1].split())[:70]

bad = 0
for path in sys.argv[1:]:
    for line, n, fmt in calls(open(path, errors='replace').read()):
        if n > 12:
            bad += 1
            print(f'{path}:{line}: {n} arguments > 12: {fmt}')
print('DEBUG argument check:', 'FAILED' if bad else 'OK', f'({bad} calls > 12)')
sys.exit(1 if bad else 0)
