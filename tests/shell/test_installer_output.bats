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
