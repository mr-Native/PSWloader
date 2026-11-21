#!/bin/sh
set -eu
D=$(cd "$(dirname "$0")" && pwd)
O=${TMPDIR:-/tmp}/ps5pwr-sensors-test.$$
trap 'rm -rf "$O"' EXIT
mkdir -p "$O"
cc -std=c11 -O2 -Wall -Wextra -Werror -I"$D" -o "$O/test" "$D/test_ps5pwr_sensors.c" "$D/ps5pwr-sensors.c"
"$O/test"
