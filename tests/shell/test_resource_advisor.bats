#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# scripts/resource-advisor.sh runs as a PreToolUse hook on every Bash call. It
# never blocks: it answers with a suggestion, or with nothing. The share and the
# memory figure come from `helix-claim jobs -v`, stubbed here through
# HELIX_ADVISOR_JOBS_CMD so each case controls how busy the box looks.

load helpers

ADVISOR="scripts/resource-advisor.sh"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    TEST_DIR="$(mktemp -d)"
    export HELIX_ADVISOR_JOBS_CMD="$TEST_DIR/jobs"
    export JOBS_CALLED="$TEST_DIR/jobs.called"
    HELIX_ADVISOR_HOSTS=$(hostname -s)
    export HELIX_ADVISOR_HOSTS
    roomy
}

teardown() {
    rm -rf "$TEST_DIR"
}

# A `helix-claim jobs -v` stand-in: the -v line on stderr, the share on stdout.
stub_jobs() {
    local share="$1" avail="$2" extra="${3:-}"
    cat > "$HELIX_ADVISOR_JOBS_CMD" <<EOF
#!/usr/bin/env bash
touch "$JOBS_CALLED"
echo "ncpu=32 peers=1(claimed+inferred) cc1plus=0 availGB=$avail -> -j$share$extra" >&2
echo "$share"
EOF
    chmod +x "$HELIX_ADVISOR_JOBS_CMD"
}
roomy() { stub_jobs 16 60; }
tight_share() { stub_jobs 6 60; }
tight_memory() { stub_jobs 16 12; }

advise() {
    local json
    json=$(jq -cn --arg c "$1" '{tool_name: "Bash", tool_input: {command: $c}}')
    run bash -c "printf '%s' \"\$1\" | $ADVISOR" _ "$json"
}

context() {
    printf '%s' "$output" | jq -r '.hookSpecificOutput.additionalContext'
}

# ---------------------------------------------------------------------------
# Silence and speed on ordinary commands
# ---------------------------------------------------------------------------

@test "an ordinary command gets no output and never reads the box" {
    advise "git status --short"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
    [ ! -e "$JOBS_CALLED" ]
}

@test "a single-tag test run is not heavy" {
    tight_share
    advise "make t F='[ams]'"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "a command already on zeus is left alone" {
    tight_share
    advise "scripts/zeus-run.sh mutate --tests '[ams]'"
    [ -z "$output" ]
    advise "ssh zeus.local 'sudo -n docker exec helix-tsan make full-test-run'"
    [ -z "$output" ]
}

# ---------------------------------------------------------------------------
# Always on zeus, however quiet thelio is
# ---------------------------------------------------------------------------

@test "addr2line against helix-tests is sent to zeus on a roomy box" {
    advise "addr2line -f -C -e build/bin/helix-tests 0x69125f5"
    [ "$status" -eq 0 ]
    contains "zeus" "$(context)"
    contains "addr2line" "$(context)"
}

@test "gdb against a helix binary points at coredumpctl" {
    advise "gdb -batch -ex bt build/bin/helix-screen core.123"
    contains "coredumpctl info" "$(context)"
}

# A zeus-run.sh stand-in offering exactly the modes named.
zeus_modes() {
    { printf '#!/usr/bin/env bash\ncase "$1" in\n'
      for m in "$@"; do printf '    %s)\n        ;;\n' "$m"; done
      printf 'esac\n'; } > "$TEST_DIR/zeus-run.sh"
    export HELIX_ADVISOR_ZEUS_RUN="$TEST_DIR/zeus-run.sh"
}

@test "unit-sweep goes to zeus's sweep and says nothing about bats" {
    zeus_modes sweep
    advise "make unit-sweep"
    contains "zeus-run.sh sweep" "$(context)"
    lacks "make test-shell" "$(context)"
}

@test "full-test-run sends the C++ half to zeus and keeps bats on thelio" {
    zeus_modes sweep
    advise "make -j full-test-run"
    contains "zeus-run.sh sweep" "$(context)"
    contains "make test-shell" "$(context)"
    lacks "zeus-run.sh full" "$(context)"
}

@test "a sweep suggestion points at helix-claim resources" {
    zeus_modes sweep
    advise "make unit-sweep"
    contains "scripts/helix-claim resources" "$(context)"
}

@test "a full gate is sent to zeus's sweep mode on a roomy box" {
    zeus_modes sweep
    advise "make -j full-test-run"
    contains "zeus-run.sh sweep" "$(context)"
    contains "push the branch" "$(context)"
}

@test "a full gate is silent while zeus-run has no sweep mode" {
    zeus_modes test
    advise "make unit-sweep"
    [ -z "$output" ]
}

@test "a mutation run is sent to zeus" {
    advise "make mutate-diff"
    contains "zeus-run.sh mutate" "$(context)"
}

# ---------------------------------------------------------------------------
# Only when thelio is tight
# ---------------------------------------------------------------------------

@test "an explicit -j above the fair share is flagged" {
    tight_share
    advise "make -j24"
    contains '-j$(scripts/helix-claim jobs)' "$(context)"
    contains "-j6" "$(context)"
}

@test "an explicit -j at or under the fair share is not flagged" {
    tight_share
    advise "make -j4 test"
    [ -z "$output" ]
}

@test "a docker cross build is sent to zeus only when thelio is tight" {
    advise "make snapmaker-u1-docker"
    [ -z "$output" ]
    tight_memory
    advise "make snapmaker-u1-docker"
    contains "zeus" "$(context)"
    contains "scripts/helix-claim resources" "$(context)"
}

@test "an idf build in docker is heavy when thelio is tight" {
    tight_share
    advise "docker run --rm -v \$PWD:/src espressif/idf idf.py build"
    contains "zeus" "$(context)"
}

@test "a test binary in a loop is flagged only when thelio is tight" {
    local loop='for i in $(seq 1 200); do ./build/bin/helix-tests "[x]" || f=$((f+1)); done'
    advise "$loop"
    [ -z "$output" ]
    tight_memory
    advise "$loop"
    contains "loop" "$(context)"
}

@test "memory under 16GB counts as tight even with a wide share" {
    tight_memory
    advise "make -j24"
    contains "12GB" "$(context)"
}

# ---------------------------------------------------------------------------
# Contract with Claude Code and with helix-claim
# ---------------------------------------------------------------------------

@test "a suggestion is a PreToolUse context block that does not decide permission" {
    advise "make mutate-diff"
    [ "$status" -eq 0 ]
    [ "$(printf '%s' "$output" | jq -r '.hookSpecificOutput.hookEventName')" = "PreToolUse" ]
    [ "$(printf '%s' "$output" | jq -r '.hookSpecificOutput.permissionDecision // "none"')" = "none" ]
}

@test "extra trailing fields on the jobs line are ignored" {
    stub_jobs 6 60 " shards=3 nice=10"
    advise "make -j24"
    contains "-j6" "$(context)"
}

@test "a missing helix-claim still exits 0 and still sends symbolizers to zeus" {
    export HELIX_ADVISOR_JOBS_CMD="$TEST_DIR/does-not-exist"
    advise "make -j24"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
    advise "addr2line -e build/bin/helix-tests 0x1"
    [ "$status" -eq 0 ]
    contains "zeus" "$(context)"
}

@test "garbage on stdin exits 0 with no output" {
    run bash -c "printf 'not json' | $ADVISOR"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
    # Past the fast path, so jq itself sees the broken input.
    run bash -c "printf 'mutate-diff {' | $ADVISOR"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "without jq it exits 0 with no output" {
    mkdir -p "$TEST_DIR/bin"
    local t
    for t in bash cat sed grep hostname dirname nproc timeout; do
        ln -s "$(command -v "$t")" "$TEST_DIR/bin/$t"
    done
    run env PATH="$TEST_DIR/bin" bash "$ADVISOR" <<< '{"tool_input":{"command":"make mutate-diff"}}'
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "on a host the advice is not about it is silent" {
    export HELIX_ADVISOR_HOSTS="some-other-box"
    advise "make mutate-diff"
    [ -z "$output" ]
}

@test "a hung helix-claim is cut off and fails open" {
    printf '#!/usr/bin/env bash\nsleep 30\n' > "$HELIX_ADVISOR_JOBS_CMD"
    local start=$SECONDS
    advise "make -j40 x"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
    [ $((SECONDS - start)) -lt 6 ] || fail "took $((SECONDS - start))s"
}

@test "the jobs line is read the same from a path with spaces" {
    mkdir -p "$TEST_DIR/a b"
    export HELIX_ADVISOR_JOBS_CMD="$TEST_DIR/a b/jobs"
    stub_jobs 6 60
    advise "make -j40 x"
    contains "fair share, -j6" "$(context)"
}

# ---------------------------------------------------------------------------
# Boundaries
# ---------------------------------------------------------------------------

@test "-j equal to the share is fine and one above is flagged" {
    tight_share
    advise "make -j6 x"
    [ -z "$output" ]
    advise "make -j7 x"
    contains "fair share, -j6" "$(context)"
}

@test "a share of 8 is tight and 9 is not" {
    local loop='for i in 1 2; do ./build/bin/helix-tests x; done'
    stub_jobs 8 60
    advise "$loop"
    contains "loop" "$(context)"
    stub_jobs 9 60
    advise "$loop"
    [ -z "$output" ]
}

@test "15GB available is tight and 16GB is not" {
    local loop='for i in 1 2; do ./build/bin/helix-tests x; done'
    stub_jobs 16 15
    advise "$loop"
    contains "15GB" "$(context)"
    stub_jobs 16 16
    advise "$loop"
    [ -z "$output" ]
}

@test "a plain test-binary run does not read the box" {
    advise "./build/bin/helix-tests '[ams]'"
    [ -z "$output" ]
    [ ! -e "$JOBS_CALLED" ]
}

# ---------------------------------------------------------------------------
# Mentions are not invocations
# ---------------------------------------------------------------------------

@test "a commit message naming heavy commands is silent" {
    tight_memory
    advise 'git commit -m x -m "mutation: make mutate-diff reverted it; make full-test-run green; gdb on helix-tests"'
    [ -z "$output" ]
}

@test "a grep for a heavy command is silent" {
    tight_memory
    advise "grep -rn addr2line scripts/helix-claim && grep -n 'make full-test-run' CLAUDE.md"
    [ -z "$output" ]
}

@test "a loop that only greps bats files is silent" {
    tight_memory
    advise 'for f in tests/shell/*.bats; do grep -c @test $f; done'
    [ -z "$output" ]
}

@test "a heavy command after a harmless one in the same line is caught" {
    advise "cd build && make -j mutate-diff"
    contains "zeus-run.sh mutate" "$(context)"
}

# ---------------------------------------------------------------------------
# Spellings of the real offenders
# ---------------------------------------------------------------------------

@test "a symbolizer by full path or eu- prefix is sent to zeus" {
    advise "/usr/bin/addr2line -e build/bin/helix-tests 0x1"
    contains "zeus" "$(context)"
    advise "eu-addr2line -e build/bin/helix-tests 0x1"
    contains "zeus" "$(context)"
}

@test "the native ASAN forms are sent to zeus" {
    advise "make SANITIZE=address test"
    contains "zeus-run.sh asan" "$(context)"
    advise "make test-asan"
    contains "zeus-run.sh asan" "$(context)"
}

@test "an ASAN build for another board is not an ASAN run here" {
    tight_memory
    advise "make deploy-pi-asan"
    [ -z "$output" ]
}

@test "every spelling of an oversized -j is flagged" {
    tight_share
    # The advisor expands $(nproc) on the host, so pin it: a 4-core runner's
    # -j$(nproc) sits under the share of 6 and would not be oversized there.
    mkdir -p "$TEST_DIR/nproc-bin"
    printf '#!/bin/sh\necho 32\n' > "$TEST_DIR/nproc-bin/nproc"
    chmod +x "$TEST_DIR/nproc-bin/nproc"
    PATH="$TEST_DIR/nproc-bin:$PATH"
    for c in 'make -j$(nproc)' 'make -j 32 test' 'make --jobs=32' 'make --jobs 32'; do
        advise "$c"
        contains "fair share, -j6" "$(context)" || fail "missed: $c"
    done
}

@test "a computed or bare -j is left to its source" {
    tight_share
    for c in 'make -j"$(scripts/helix-claim jobs)"' 'make -j$(scripts/helix-claim jobs) test' 'make -j'; do
        advise "$c"
        [ -z "$output" ] || fail "flagged: $c"
    done
}

@test "an explicit -j above the share is flagged even on a roomy box" {
    advise "make -j24 test"
    contains "fair share, -j16" "$(context)"
}

@test "xargs over the test binary is a loop" {
    tight_share
    advise "seq 200 | xargs -I{} ./build/bin/helix-tests '[x]'"
    contains "loop" "$(context)"
}

@test "a docker toolchain target is a container build" {
    tight_share
    advise "make docker-toolchain-k1"
    contains "zeus" "$(context)"
}

@test "a huge command returns quickly" {
    local big
    big=$(printf 'make x %.0s' $(seq 1 15000))   # ~105KB, under the 128KB single-argument limit
    local start=$SECONDS
    advise "$big"
    [ "$status" -eq 0 ]
    [ $((SECONDS - start)) -lt 5 ] || fail "took $((SECONDS - start))s"
}
