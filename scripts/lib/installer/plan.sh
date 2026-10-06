#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The plan the read-only pass builds, shown before anything changes.

# Source guard
[ -n "${_HELIX_PLAN_SOURCED:-}" ] && return 0
_HELIX_PLAN_SOURCED=1

_PLAN=""

# UNCALLED_OK: called from main() in Task 8
plan_set() { # key value
    _PLAN="${_PLAN}$(printf '  %-10s %s' "$1" "$2")
"
    _log_write "PLAN $1: $2"
}

# UNCALLED_OK: called from main() in Task 8
print_plan() {
    printf '%s' "$_PLAN" >&2
}

# Steps the run will show, so a no-terminal run can number them [n/N].
# UNCALLED_OK: called from main() in Task 8
plan_count_steps() {
    STEP_TOTAL=6
    [ -n "${MISSING_RUNTIME_DEPS:-}${MISSING_UNZIP_PKG:-}" ] && STEP_TOTAL=$((STEP_TOTAL + 1))
    [ -n "${COMPETING_UIS_FOUND:-}" ] && STEP_TOTAL=$((STEP_TOTAL + 1))
    return 0
}
