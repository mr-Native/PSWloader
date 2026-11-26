#!/bin/sh
# build.sh - PSWSetup.exe with mingw-w64 (static, no runtime DLLs; v0.3: no PE timestamp -> reproducible)
set -e; cd "$(dirname "$0")"
x86_64-w64-mingw32-windres PSWSetup.rc -O coff -o PSWSetup.res
x86_64-w64-mingw32-gcc -municode -O2 -Wall -Wextra -static -s -Wl,--no-insert-timestamp PSWSetup.c PSWSetup.res -o PSWSetup.exe -luser32
sha256sum PSWSetup.exe
