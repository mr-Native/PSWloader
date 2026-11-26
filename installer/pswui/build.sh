#!/bin/sh
# build.sh - pswui.exe with mingw-w64 (static, no runtime DLLs, no PE timestamp -> reproducible)
set -e; cd "$(dirname "$0")"
x86_64-w64-mingw32-gcc -municode -mwindows -O2 -Wall -Wextra -static -s -Wl,--no-insert-timestamp pswui.c -o pswui.exe -lgdi32 -luser32 -lshell32
sha256sum pswui.exe
