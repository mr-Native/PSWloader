#!/bin/sh
set -e; cd "$(dirname "$0")"
x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -static -s ps5-powerbutton.c -o ps5-powerbutton.exe -lpowrprof -ladvapi32
sha256sum ps5-powerbutton.exe
