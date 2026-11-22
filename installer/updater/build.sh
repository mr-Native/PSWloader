#!/bin/sh
# build.sh - Build static PSWUpdater[-vX.Y.Z].exe with mingw-w64 (reproducible, no runtime DLLs)
set -e
cd "$(dirname "$0")"
VER="${1:-}"
OUT="PSWUpdater.exe"
if [ -n "$VER" ]; then
  OUT="PSWUpdater-${VER}.exe"
fi
x86_64-w64-mingw32-windres PSWUpdater.rc -O coff -o PSWUpdater.res
x86_64-w64-mingw32-gcc -municode -O2 -Wall -Wextra -static -s -Wl,--no-insert-timestamp \
  PSWUpdater.c PSWUpdater.res -o "$OUT" -luser32
rm -f PSWUpdater.res
sha256sum "$OUT"
