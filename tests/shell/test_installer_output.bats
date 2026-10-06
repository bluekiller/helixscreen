#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The installer's output layer in common.sh: what reaches the screen, what
# reaches the log file, and how terminal, color and UTF-8 are decided.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers
    unset HELIX_INSTALL_VERBOSE COLORTERM NO_COLOR
    export HELIX_INSTALL_TTY=0
    . "$WORKTREE_ROOT/scripts/lib/installer/common.sh"
}

@test "log_info is silent on screen by default" {
    run log_info "detail line"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "log_info prints with HELIX_INSTALL_VERBOSE=1" {
    HELIX_INSTALL_VERBOSE=1
    run log_info "detail line"
    [[ "$output" == *"[INFO] detail line"* ]]
}

@test "log_warn and log_error always print" {
    run log_warn "careful"
    contains "[WARN] careful" "$output"
    run log_error "broken"
    contains "[ERROR] broken" "$output"
}

@test "log_note prints a plain indented line" {
    run log_note "edit config from Mainsail"
    [ "$output" = "    edit config from Mainsail" ]
}

@test "lines before log_open are buffered, then flushed with timestamps" {
    log_info "early one"
    log_warn "early two"
    log_open "$BATS_TEST_TMPDIR/install.log"
    log_info "after open"
    run cat "$BATS_TEST_TMPDIR/install.log"
    [[ "${lines[0]}" =~ ^\[[0-9]{2}:[0-9]{2}:[0-9]{2}\]\ INFO\ early\ one$ ]] || fail "line 0: ${lines[0]}"
    [[ "${lines[1]}" =~ ^\[[0-9]{2}:[0-9]{2}:[0-9]{2}\]\ WARN\ early\ two$ ]] || fail "line 1: ${lines[1]}"
    [[ "${lines[2]}" =~ INFO\ after\ open$ ]]
}

@test "the log file never contains color escapes" {
    HELIX_INSTALL_TTY=1 ui_detect
    log_open "$BATS_TEST_TMPDIR/install.log"
    log_warn "${BOLD}colored${NC} on screen"
    run grep -c "$(printf '\033')" "$BATS_TEST_TMPDIR/install.log"
    [ "$output" = "0" ]
}

@test "no terminal means no color" {
    HELIX_INSTALL_TTY=0 ui_detect
    [ "$UI_TTY" = 0 ]
    [ "$UI_COLOR" = 0 ]
    [ -z "$CYAN" ]
}

@test "a forced terminal with TERM=dumb gets no color" {
    TERM=dumb HELIX_INSTALL_TTY=1 ui_detect
    [ "$UI_COLOR" = 0 ]
}

@test "NO_COLOR disables color on a terminal" {
    NO_COLOR=1 TERM=xterm-256color HELIX_INSTALL_TTY=1 ui_detect
    [ "$UI_COLOR" = 0 ]
}

@test "256-color terminals are detected from TERM" {
    TERM=xterm-256color HELIX_INSTALL_TTY=1 ui_detect
    [ "$UI_COLOR" = 256 ]
    TERM=vt100 HELIX_INSTALL_TTY=1 ui_detect
    [ "$UI_COLOR" = 16 ]
}

@test "UTF-8 is read from LC_ALL, then LC_CTYPE, then LANG" {
    LC_ALL= LC_CTYPE= LANG=en_US.UTF-8 ui_detect;  [ "$UI_UTF8" = 1 ]
    LC_ALL=C LC_CTYPE= LANG=en_US.UTF-8 ui_detect; [ "$UI_UTF8" = 0 ]
    LC_ALL= LC_CTYPE=C.utf8 LANG= ui_detect;       [ "$UI_UTF8" = 1 ]
    LC_ALL= LC_CTYPE= LANG= ui_detect;             [ "$UI_UTF8" = 0 ]
}

@test "the output layer runs under busybox ash" {
    command -v busybox >/dev/null || skip "no busybox"
    run busybox ash -c '. "$1"; HELIX_INSTALL_TTY=0 ui_detect; log_note hi; log_open "$2"; log_warn w; cat "$2"' \
        _ "$WORKTREE_ROOT/scripts/lib/installer/common.sh" "$BATS_TEST_TMPDIR/a.log"
    [ "$status" -eq 0 ]
    [[ "$output" == *"WARN w"* ]]
}

@test "no terminal: a step prints one numbered line when done" {
    STEP_TOTAL=8
    step "Downloaded"
    run step_done "100 MB, SHA256 verified"
    [ "$output" = "[1/8] Downloaded ... ok (100 MB, SHA256 verified)" ]
}

@test "no terminal: nothing prints until the step resolves" {
    STEP_TOTAL=8
    run step "Downloaded"
    [ -z "$output" ]
}

@test "no terminal: steps without a total are not numbered" {
    STEP_TOTAL=0
    step "Checked system"
    run step_done
    [ "$output" = "Checked system ... ok" ]
}

@test "no terminal: a failed step says FAILED" {
    STEP_TOTAL=3
    step "Installing libraries"
    run step_fail
    [ "$output" = "[1/3] Installing libraries ... FAILED" ]
}

@test "step_skip prints nothing and does not consume a number" {
    STEP_TOTAL=2
    step "Installing libraries"; step_skip
    step "Downloaded"
    run step_done
    [ "$output" = "[1/2] Downloaded ... ok" ]
}

@test "warnings inside a step are indented under it" {
    STEP_TOTAL=2
    step "Downloaded"
    run log_warn "plain-HTTP mirror"
    [ "$output" = "      [WARN] plain-HTTP mirror" ]
}

@test "terminal + UTF-8: done line uses a check mark and erases the spinner" {
    NO_COLOR=1 HELIX_INSTALL_TTY=1 LANG=en_US.UTF-8 TERM=vt100 ui_detect
    step "Downloaded"
    run step_done "100 MB"
    contains $'\r' "$output"
    contains "✓ Downloaded" "$output"
    contains "100 MB" "$output"
}

@test "terminal without UTF-8 falls back to ASCII marks" {
    NO_COLOR=1 HELIX_INSTALL_TTY=1 LC_ALL=C LANG=C TERM=vt100 ui_detect
    step "Downloaded"
    run step_done
    contains "ok Downloaded" "$output"
    case "$output" in *"✓"*) fail "unicode mark on a non-UTF-8 terminal" ;; esac
}

@test "steps are recorded in the log" {
    log_open "$BATS_TEST_TMPDIR/install.log"
    step "Downloaded"; step_done "100 MB" >/dev/null 2>&1
    run grep -E 'STEP Downloaded|DONE Downloaded \(100 MB\)' "$BATS_TEST_TMPDIR/install.log"
    [ "${#lines[@]}" -eq 2 ]
}

_two_redraws() { step "Downloaded"; log_warn a; log_warn b; }

@test "terminal + UTF-8: the spinner advances on each redraw" {
    NO_COLOR=1 HELIX_INSTALL_TTY=1 LANG=en_US.UTF-8 TERM=vt100 ui_detect
    run _two_redraws
    contains "⠋" "$output"
    contains "⠙" "$output"
}

@test "terminal without UTF-8: the ASCII spinner advances on each redraw" {
    NO_COLOR=1 HELIX_INSTALL_TTY=1 LC_ALL=C LANG=C TERM=vt100 ui_detect
    run _two_redraws
    contains "|" "$output"
    contains "/" "$output"
}

@test "run_logged is silent on success and returns 0" {
    run run_logged sh -c 'echo hello; echo world >&2'
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "run_logged sends all output to the log" {
    log_open "$BATS_TEST_TMPDIR/install.log"
    run_logged sh -c 'echo to-stdout; echo to-stderr >&2'
    run cat "$BATS_TEST_TMPDIR/install.log"
    contains "to-stdout" "$output"
    contains "to-stderr" "$output"
    contains "RUN sh -c" "$output"
}

@test "run_logged keeps the command's exit code" {
    run run_logged sh -c 'exit 100'
    [ "$status" -eq 100 ]
}

@test "run_logged prints the command, exit code and the output tail on failure" {
    RUN_LOGGED_TAIL=2
    run run_logged sh -c 'for i in 1 2 3; do echo "line-$i"; done; exit 7'
    [ "$status" -eq 7 ]
    contains "failed (exit 7)" "$output"
    contains "sh -c" "$output"
    contains "line-2" "$output"
    contains "line-3" "$output"
    case "$output" in *line-1*) fail "tail leaked an earlier line" ;; esac
}

@test "run_logged echoes output live when verbose" {
    HELIX_INSTALL_VERBOSE=1
    run run_logged sh -c 'echo visible'
    contains "visible" "$output"
}

@test "print_failure appends the hint" {
    printf 'oops\n' > "$BATS_TEST_TMPDIR/out"
    run print_failure "thing" 5 "$BATS_TEST_TMPDIR/out" "try again"
    [ "$status" -eq 0 ]
    contains "thing failed (exit 5)" "$output"
    contains "oops" "$output"
    contains "try again" "$output"
}

_set_e_script() {
    printf '%s' "set -e; . '$WORKTREE_ROOT/scripts/lib/installer/common.sh'; HELIX_INSTALL_TTY=0 ui_detect; run_logged sh -c 'echo boom; exit 3'; echo after"
}

@test "run_logged under set -e prints the failure block before the shell exits" {
    run sh -c "$(_set_e_script)"
    contains "failed (exit 3)" "$output"
    contains "boom" "$output"
    case "$output" in *after*) fail "set -e did not stop the caller" ;; esac
}

@test "run_logged under set -e works in busybox ash" {
    command -v busybox >/dev/null || skip "no busybox"
    run busybox ash -c "$(_set_e_script)"
    contains "failed (exit 3)" "$output"
    contains "boom" "$output"
}

@test "run_logged exit code survives under busybox ash" {
    command -v busybox >/dev/null || skip "no busybox"
    run busybox ash -c '. "$1"; HELIX_INSTALL_TTY=0 ui_detect; run_logged sh -c "exit 42"; echo "rc=$?"' \
        _ "$WORKTREE_ROOT/scripts/lib/installer/common.sh"
    contains "rc=42" "$output"
}

@test "run_logged failure tail shows a last line with no trailing newline" {
    run run_logged sh -c 'printf "a\nlast-%s" nonl; exit 4'
    [ "$status" -eq 4 ]
    contains "last-nonl" "$output"
}

@test "run_logged verbose echo shows a last line with no trailing newline" {
    HELIX_INSTALL_VERBOSE=1
    run run_logged sh -c 'printf "a\nlast-nonl"'
    contains "last-nonl" "$output"
}

@test "run_logged keeps the next log entry on its own line after unterminated output" {
    log_open "$BATS_TEST_TMPDIR/install.log"
    run_logged sh -c 'printf "partial-nonl"'
    log_warn "after"
    run grep -c '^partial-nonl$' "$BATS_TEST_TMPDIR/install.log"
    [ "$output" = 1 ]
}

@test "run_logged unterminated output is handled under busybox ash" {
    command -v busybox >/dev/null || skip "no busybox"
    run busybox ash -c '. "$1"; HELIX_INSTALL_TTY=0 ui_detect; log_open "$2"; run_logged sh -c "printf \"a\nlast-nonl\"; exit 4"; log_warn after' \
        _ "$WORKTREE_ROOT/scripts/lib/installer/common.sh" "$BATS_TEST_TMPDIR/ash.log"
    contains "last-nonl" "$output"
    run grep -c '^last-nonl$' "$BATS_TEST_TMPDIR/ash.log"
    [ "$output" = 1 ]
}

@test "run_logged logs RUN and the failure block when mktemp is unavailable" {
    log_open "$BATS_TEST_TMPDIR/install.log"
    TMPDIR=/nonexistent run sh -c "set -e; . '$WORKTREE_ROOT/scripts/lib/installer/common.sh'; INSTALL_LOG='$BATS_TEST_TMPDIR/install.log'; run_logged sh -c 'exit 3'; echo after"
    case "$output" in *after*) fail "set -e did not stop the caller" ;; esac
    run grep -c 'RUN sh -c' "$BATS_TEST_TMPDIR/install.log"
    [ "$output" = 1 ]
}
