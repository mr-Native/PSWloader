#!/usr/bin/env bash
# build-vN.sh v1.0 - reproducible build+pack pipeline for ONE PS5 native Windows UEFI payload.
#
# ONE command builds, idempotently, a complete
#   uefi-dir-<VERSION>/ with UniversalPayload.elf (full EDK2 build) + ps5-upl.bin (PS5UPL01).
#
# Usage:
#   build-vN.sh <VERSION>            # full build+pack (deletes the target dir first)
#   build-vN.sh <VERSION> --dry-run  # only check prerequisites + show the planned commands, do NOT build
#
# VERSION is the bare name WITHOUT a path, e.g. v365  ->  target <work>/uefi-dir-v365
#
# Reproducibility / gotchas that are hard-wired here:
#   * build-uefi.py creates --output itself (mkdir exist_ok=False) -> rm -rf the target dir BEFORE, never mkdir.
#   * pack-payload.py opens the .bin with 'xb' (exclusive) -> a fresh dir is required (rm -rf covers that).
#   * --native-atom-library needs --native-setup; --native-dp-video needs --native-dp-source;
#     the exception-only stage needs --exception-power-telemetry + --entry-power-telemetry.
#   * build-uefi.py checks the EDK2 HEAD revision (must be ec82c96...) itself; we check it here first
#     to fail fast with a clear message.
#   * NO hardware: this script only builds on the host. It deploys/boots NOTHING.
#
# Rebuild discipline: only build after a source change was approved (ONE variable per test).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # repo folder with build-uefi.py / pack-payload.py / *.h
EDK2="${EDK2:-${PSW_WORK:-/opt/pswloader-work}/edk2}"
WORK="${WORK:-${PSW_WORK:-/opt/pswloader-work}}"
PINNED_EDK2_REV="ec82c96152507d64055a8965fe261fb9aaca3279"

# The pinned v363/v364 flags. Exactly ONE place for the truth.
FLAGS=(
  --native-setup
  --lvgl-ui
  --native-dp-video
  --native-dp-source
  --native-atom-library
  --hdmi-probe
  --hdmi-prepare
  --hdmi-edid-request
  --display-policy 2
  --dispclk-raise 1
  --display-replay 3440p60
  --display-replay 3440p100
  --display-replay ugreen1440p60
  --display-replay 2560x1440p120
  --display-replay 720p144
  --setup-seconds 3
  --menu-idle-seconds 0
  --m2-overlay
  --nvme-preinit probe
  --usb-mass-quiet
  --log-export
  --exception-power-telemetry
  --entry-power-telemetry
  --entry-power-stage exception-only
)

# Extra flags per variant (e.g. EXTRA_FLAGS=--m2-overlay for N4); empty = exactly the pinned flags.
read -r -a _extra <<< "${EXTRA_FLAGS:-}"
FLAGS+=("${_extra[@]}")

die() { echo "ERROR: $*" >&2; exit 1; }

# ---- Argumente ----
VERSION=""
DRY_RUN=0
for arg in "$@"; do
  case "$arg" in
    --dry-run) DRY_RUN=1 ;;
    -h|--help) grep -E '^#' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    --*) die "unknown flag: $arg" ;;
    *) [ -z "$VERSION" ] && VERSION="$arg" || die "too many positional arguments (VERSION already '$VERSION')" ;;
  esac
done
[ -n "$VERSION" ] || die "VERSION missing. Usage: build-vN.sh <VERSION> [--dry-run]"
[[ "$VERSION" =~ ^[A-Za-z0-9._-]+$ ]] || die "VERSION '$VERSION' contains invalid characters (only A-Za-z0-9._-)"

OUT="$WORK/uefi-dir-$VERSION"
BUILD="$HERE/build-uefi.py"
PACK="$HERE/pack-payload.py"

# ---- preconditions ----
[ -f "$BUILD" ]   || die "build-uefi.py not found: $BUILD"
[ -f "$PACK" ]    || die "pack-payload.py not found: $PACK"
[ -f "$HERE/ps5-native-gop.h" ] || die "source interface ps5-native-gop.h missing in $HERE"
[ -d "$EDK2/.git" ] || die "EDK2 checkout not found: $EDK2 (set EDK2=...)"
REV="$(git -C "$EDK2" rev-parse HEAD 2>/dev/null || true)"
[ "$REV" = "$PINNED_EDK2_REV" ] || die "EDK2 is at $REV, expected $PINNED_EDK2_REV (git -C $EDK2 checkout $PINNED_EDK2_REV)"

GOP_SHA="$(sha256sum "$HERE/ps5-native-gop.h" | cut -d' ' -f1)"

echo "== build-vN.sh =="
echo "  VERSION       : $VERSION"
echo "  target dir    : $OUT"
echo "  EDK2          : $EDK2  (HEAD $REV OK)"
echo "  ps5-native-gop.h sha256 : $GOP_SHA"
echo "  Flags         : ${FLAGS[*]}"

if [ "$DRY_RUN" -eq 1 ]; then
  echo
  echo "-- DRY-RUN: nothing is deleted and nothing is built. Planned steps:"
  echo "   1) rm -rf $OUT"
  echo "   2) python3 $BUILD --edk2 $EDK2 --output $OUT ${FLAGS[*]}"
  echo "   3) python3 $PACK $OUT/UniversalPayload.elf $OUT/ps5-upl.bin"
  echo "   4) sha256sum $OUT/UniversalPayload.elf $OUT/ps5-upl.bin"
  echo "DRY_RUN_OK $VERSION"
  exit 0
fi

# ---- 1) idempotent start: fresh target dir ----
rm -rf "$OUT"

# ---- 2) full EDK2 build -> UniversalPayload.elf + build.json ----
echo "-- [2/4] build-uefi.py (full EDK2 build, takes several minutes) ..."
python3 "$BUILD" --edk2 "$EDK2" --output "$OUT" "${FLAGS[@]}" >/dev/null
[ -f "$OUT/UniversalPayload.elf" ] || die "build produced no UniversalPayload.elf"
[ -f "$OUT/build.json" ]           || die "build produced no build.json"

# ---- 3) Pack -> ps5-upl.bin (PS5UPL01) + ps5-upl.bin.json ----
echo "-- [3/4] pack-payload.py (PS5UPL01) ..."
python3 "$PACK" "$OUT/UniversalPayload.elf" "$OUT/ps5-upl.bin" >/dev/null
[ -f "$OUT/ps5-upl.bin" ] || die "pack produced no ps5-upl.bin"

# ---- 4) print shas + check self-consistency ----
echo "-- [4/4] SHA-256:"
ELF_SHA="$(sha256sum "$OUT/UniversalPayload.elf" | cut -d' ' -f1)"
BIN_SHA="$(sha256sum "$OUT/ps5-upl.bin" | cut -d' ' -f1)"
# build.json.sha256 is the ELF sha; ps5-upl.bin.json.container_sha256 is the .bin sha.
JSON_ELF_SHA="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["sha256"])' "$OUT/build.json")"
JSON_BIN_SHA="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["container_sha256"])' "$OUT/ps5-upl.bin.json")"
[ "$ELF_SHA" = "$JSON_ELF_SHA" ] || die "ELF SHA ($ELF_SHA) != build.json.sha256 ($JSON_ELF_SHA)"
[ "$BIN_SHA" = "$JSON_BIN_SHA" ] || die ".bin SHA ($BIN_SHA) != ps5-upl.bin.json.container_sha256 ($JSON_BIN_SHA)"
echo "   UniversalPayload.elf : $ELF_SHA"
echo "   ps5-upl.bin          : $BIN_SHA"
echo
echo "BUILD_PACK_OK $VERSION $BIN_SHA"
