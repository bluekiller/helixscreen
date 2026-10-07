#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds the K-Touch firmware with heap poisoning and task tracking
# (firmware/helixscreen-esp32/sdkconfig.debug-heap) in its own build directory, so the
# release build and its sdkconfig are untouched, then keeps the ELF that matches the
# image: a crash is only symbolized exactly against the ELF that was flashed.
#
# usage: scripts/esp32_build_debug_heap.sh [build-dir]
#   build-dir   relative to firmware/helixscreen-esp32 (default: build-debug-heap)
#
# Needs docker and the staged asset image the normal firmware build needs
# (storage_frogfs.bin). Flash from the build directory with its flash_args, as for a
# normal build.
set -euo pipefail

REPO=$(git rev-parse --show-toplevel)
FW="$REPO/firmware/helixscreen-esp32"
BUILD="${1:-build-debug-heap}"
IDF_IMAGE="espressif/idf:v5.5.5"
ARCHIVE="${HELIX_CRASH_ELF_DIR:-$HOME/.helixscreen-crash-elves}"

docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp \
    -e HELIX_SDKCONFIG_OVERLAY=sdkconfig.debug-heap \
    -v "$REPO:$REPO" -w "$FW" "$IDF_IMAGE" \
    bash -c ". /opt/esp/idf/export.sh >/dev/null && idf.py -B '$BUILD' -DSDKCONFIG='$BUILD/sdkconfig' build"

ELF="$FW/$BUILD/helixscreen_esp32.elf"
grep -q '^CONFIG_HEAP_POISONING_COMPREHENSIVE=y' "$FW/$BUILD/sdkconfig" ||
    { echo "error: $BUILD/sdkconfig is missing the debug-heap overlay" >&2; exit 1; }

SHA=$(cut -d' ' -f1 "$ELF.sha256")
NAME="${SHA:0:9}-$(git -C "$REPO" rev-parse --short HEAD)-debug-heap"
mkdir -p "$ARCHIVE"
cp "$ELF" "$ARCHIVE/$NAME.elf"
cp "$ELF.sha256" "$ARCHIVE/$NAME.elf.sha256"

echo "built:   $FW/$BUILD"
echo "ELF:     $ARCHIVE/$NAME.elf"
echo "match:   a crash's 'ELF file SHA256: ${SHA:0:9}' is this ELF"
