#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Pins the order of the key calls in teardown_printer_scope(), the one ordered teardown
# behind both a printer switch and process exit.
#
# The order is the contract: observers detach before subjects deinit, the client outlives
# everything that unregisters from it, panels die before the subjects they observe. No
# unit test can run it (it ends in a full rebuild of global state), so this gate compares
# the call sequence to a golden list. Moving the body to another file must leave the
# list unchanged; a deliberate reorder edits the list in the same commit.

load helpers

TEARDOWN_SRC="src/application/application.cpp"
GOLDEN="tests/shell/fixtures/teardown_printer_scope_order.txt"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
}

# Body of teardown_printer_scope() in $1, comments stripped.
teardown_body() {
    awk '
        /^void [A-Za-z]+::teardown_printer_scope\(/ { inside = 1 }
        inside { print }
        inside && /^\}/ { exit }
    ' "$1" | sed -e 's@//.*$@@'
}

# The key calls in $1's teardown body, in source order, one per line.
teardown_order() {
    local body keys='' k
    body=$(teardown_body "$1")
    [ -n "$body" ] || { echo "could not locate teardown_printer_scope() in $1"; return 1; }
    while IFS= read -r k; do
        k=$(printf '%s' "$k" | sed -e 's/[][\.*^$()+?{}|\/]/\\&/g')
        keys="${keys:+$keys|}$k"
    done < "$GOLDEN"
    printf '%s\n' "$body" | grep -oE "$keys"
}

@test "teardown_printer_scope runs its key calls in the pinned order" {
    run teardown_order "$TEARDOWN_SRC"
    [ "$status" -eq 0 ]
    diff <(printf '%s\n' "$output") "$GOLDEN"
}

@test "the teardown order gate fails when two calls swap" {
    local mutated="${BATS_TEST_TMPDIR}/swapped.cpp"
    # Move the panel reset after the subject reset.
    awk '
        /^    m_panels\.reset\(\);$/ { held = $0; next }
        held != "" && /^    m_subjects\.reset\(\);$/ { print; print held; held = ""; next }
        { print }
    ' "$TEARDOWN_SRC" > "$mutated"
    ! cmp -s "$mutated" "$TEARDOWN_SRC"

    run teardown_order "$mutated"
    [ "$status" -eq 0 ]
    ! diff <(printf '%s\n' "$output") "$GOLDEN" > /dev/null
}

@test "the teardown order gate fails when the function cannot be located" {
    local mutated="${BATS_TEST_TMPDIR}/renamed.cpp"
    sed -e 's@::teardown_printer_scope(@::teardown_renamed(@' "$TEARDOWN_SRC" > "$mutated"

    run teardown_order "$mutated"
    [ "$status" -eq 1 ]
    [[ "$output" == *"could not locate"* ]]
}
