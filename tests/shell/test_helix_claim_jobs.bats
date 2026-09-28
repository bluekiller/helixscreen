#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# `helix-claim jobs` splits the cores between the trees building now. The
# Makefile asks it from $(shell) while its own make is running, so a make that
# is an ancestor of the caller must not count as a peer, or every build halves
# its own share. A make that is NOT an ancestor is a peer whatever its tree.
#
# pgrep is stubbed so the set of "running makes" is exactly what each test says.

load helpers

CLAIM="$(cd "${BATS_TEST_DIRNAME}/../.." && pwd)/scripts/helix-claim"

setup() {
    export HELIX_CLAIM_DIR="$BATS_TEST_TMPDIR/claims"
    mkdir -p "$HELIX_CLAIM_DIR"
    export MOCK_MAKE_PIDS="$BATS_TEST_TMPDIR/make.pids"
    : > "$MOCK_MAKE_PIDS"
    mock_command_script pgrep '[ "$2" = make ] && cat "$MOCK_MAKE_PIDS"; exit 0'
}

teardown() {
    [ -n "${PEER:-}" ] && kill "$PEER" 2>/dev/null
    return 0
}

@test "the make that called jobs is not counted as a peer" {
    echo "$$" > "$MOCK_MAKE_PIDS"
    run "$CLAIM" jobs -v
    [ "$status" -eq 0 ]
    contains "peers=0" "$output"
}

@test "a make that is not an ancestor is a peer" {
    (cd "$BATS_TEST_TMPDIR" && exec sleep 60) &
    PEER=$!
    echo "$PEER" > "$MOCK_MAKE_PIDS"
    run "$CLAIM" jobs -v
    [ "$status" -eq 0 ]
    contains "peers=1" "$output"
}
