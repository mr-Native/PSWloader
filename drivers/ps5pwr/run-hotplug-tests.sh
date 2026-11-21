#!/bin/sh
# v1.4 host test of the ps5pwr hotplug core with a mocked ICC mailbox (same sources and the same
# -DPS5_ICC_LINUX_NOTIFY transport as ps5pwr.sys), plus a clang -fsanitize run. Usage: run-hotplug-tests.sh [-v]
set -eu
D=$(cd "$(dirname "$0")" && pwd); R=$D/../../firmware; O=${TMPDIR:-/tmp}/ps5pwr-hotplug-test.$$
mkdir -p "$O"; trap 'rm -rf "$O"' EXIT
SRC="$D/test_ps5pwr_hotplug.c $D/ps5pwr-hotplug.c $R/icc-power.c $R/icc-hdmi.c $R/hdmi-commands.c $R/flava3-prepare.c $R/flava3-video.c $R/ps5-display-mode.c"
gcc -std=c11 -O2 -Wall -Wextra -Werror -DPS5_ICC_LINUX_NOTIFY=1 -DPS5_TESTDATA="\"$R/testdata\"" -I"$R" -o "$O/t-gcc" $SRC
"$O/t-gcc" "$@"
clang -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
  -DPS5_ICC_LINUX_NOTIFY=1 -DPS5_TESTDATA="\"$R/testdata\"" -I"$R" -o "$O/t-asan" $SRC
"$O/t-asan" > "$O/asan.txt" && tail -1 "$O/asan.txt" | sed 's/^/asan+ubsan: /'
python3 "$D/test_ps5pwr.py" 2>&1 | tail -1
