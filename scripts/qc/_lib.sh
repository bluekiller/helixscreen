# shellcheck shell=bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Helpers every gate in scripts/qc/ may call. Sourced by scripts/quality-checks.sh
# before any gate, and by a bats test before it sources the one gate it drives.
# shellcheck disable=SC2034,SC2154

# Timing helper - prints elapsed time for a section (seconds)
section_time() {
  local start=$1
  local end
  end=$(date +%s)
  local elapsed=$((end - start))
  if [ $elapsed -gt 0 ]; then
    printf " (%ds)" "$elapsed"
  fi
}

# A verdict that carries a count is recorded in $QC_COUNTS as well as printed,
# so a cached pass can replay what the full run examined.
qc_note() { printf '%s\n' "$1" >> "$QC_COUNTS" 2>/dev/null || true; }
qc_count() { echo "$1"; qc_note "$1"; }

# Path triggers more than one gate shares (see QC_TRIGGER_<gate> in each file).
QC_TRIGGER_XML='\.xml$|^src/ui/|^tools/xml-linter/'
QC_TRIGGER_NATIVE_SRC='\.(cpp|c|h|mm)$'
QC_TRIGGER_ICONS='\.xml$|icon|font'
