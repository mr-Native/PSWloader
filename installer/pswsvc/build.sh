#!/bin/sh
# build.sh - pswsvc.exe with mingw-w64 (static, no runtime DLLs, no PE timestamp -> reproducible)
set -e; cd "$(dirname "$0")"
x86_64-w64-mingw32-gcc -municode -O2 -Wall -Wextra -static -s -Wl,--no-insert-timestamp pswsvc.c -o pswsvc.exe -ladvapi32 -lsetupapi -lcfgmgr32 -luser32 -lcrypt32
sha256sum pswsvc.exe
