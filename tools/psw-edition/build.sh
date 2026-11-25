#!/bin/sh
# build.sh - PSWEdition.exe (static, mingw-w64, reproducible, no runtime DLLs)
set -e
cd "$(dirname "$0")"
x86_64-w64-mingw32-gcc -municode -O2 -Wall -Wextra -Werror -static -s -Wl,--no-insert-timestamp psw-edition.c -o PSWEdition.exe -lpsapi
sha256sum PSWEdition.exe
