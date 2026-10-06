#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The installer's read-only pass: the plan it builds, how it asks, and the
# --dry-run and --verbose flags.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
LIB="$WORKTREE_ROOT/scripts/lib/installer"

# Inert stand-ins for every command that could reach the host's package
# manager, init system or privilege prompt. Each exits 1 and logs its call;
# a test that needs other behaviour overwrites its stub.
stub() { # name body
    printf '#!/bin/sh\necho "%s $*" >> "%s/calls.log"\n%s\n' \
        "$1" "$BATS_TEST_TMPDIR" "${2:-exit 1}" > "$STUBBIN/$1"
    chmod +x "$STUBBIN/$1"
}

setup() {
    STUBBIN="$BATS_TEST_TMPDIR/stubbin"
    mkdir -p "$STUBBIN"
    for c in systemctl sudo apt-get apt apt-cache dpkg-query pkexec udevadm pidof config-manager; do
        stub "$c"
    done
    PATH="$STUBBIN:$PATH"

    load helpers
    # From here helpers.bash owns systemctl: its inert shim, scripted with
    # mock_command_script. The rest of the stubs go back in front of
    # everything, the test sandbox included.
    rm "$STUBBIN/systemctl"
    PATH="$STUBBIN:$PATH"
    export HELIX_INSTALL_TTY=0
    export SUDO=""
    # main.sh installs its own EXIT/signal/ERR traps at source time; put
    # bats' back so a failing test still reports.
    local bats_traps
    bats_traps="$(trap -p EXIT INT TERM HUP)"
    for m in common logo host_profile platform requirements competing_uis moonraker kiauh plan uninstall main; do
        . "$LIB/$m.sh"
    done
    trap - ERR EXIT INT TERM HUP
    eval "$bats_traps"
}

@test "--dry-run and --verbose are parsed" {
    parse_installer_args --dry-run --verbose
    [ "$DRY_RUN" = true ]
    [ "$HELIX_INSTALL_VERBOSE" = 1 ]
    parse_installer_args -v
    [ "$HELIX_INSTALL_VERBOSE" = 1 ]
}

@test "DRY_RUN defaults to false" {
    parse_installer_args
    [ "$DRY_RUN" = false ]
}

@test "tty_confirm: ASSUME_YES answers yes without reading" {
    ASSUME_YES=true
    run tty_confirm "Continue?" n < /dev/null
    [ "$status" -eq 0 ]
}

@test "tty_confirm: no terminal and no /dev/tty returns the default" {
    HELIX_TTY_DEVICE=/nonexistent
    run tty_confirm "Continue?" y < /dev/null
    [ "$status" -eq 0 ]
    run tty_confirm "Continue?" n < /dev/null
    [ "$status" -eq 1 ]
}

@test "tty_confirm reads its answer from the tty device, not stdin" {
    printf 'n\n' > "$BATS_TEST_TMPDIR/tty"
    HELIX_TTY_DEVICE="$BATS_TEST_TMPDIR/tty"
    run tty_confirm "Continue?" y <<EOF
y
EOF
    [ "$status" -eq 1 ]
}

@test "confirm_clean_install takes a yes from the tty device" {
    printf 'y\n' > "$BATS_TEST_TMPDIR/tty"
    HELIX_TTY_DEVICE="$BATS_TEST_TMPDIR/tty"
    ASSUME_YES=false
    run confirm_clean_install < /dev/null
    [ "$status" -eq 0 ]
}

@test "confirm_clean_install refuses --clean with no terminal to ask" {
    HELIX_TTY_DEVICE=/nonexistent
    ASSUME_YES=false
    run confirm_clean_install < /dev/null
    [ "$status" -eq 1 ]
    contains "Refusing to run --clean without confirmation" "$output"
}

@test "plan prints one aligned line per set key, in order" {
    plan_set Printer "Raspberry Pi 4 · Kalico · systemd"
    plan_set Install "v1.1.0-beta.4 (beta)"
    run print_plan
    [ "${lines[0]}" = "  Printer    Raspberry Pi 4 · Kalico · systemd" ]
    [ "${lines[1]}" = "  Install    v1.1.0-beta.4 (beta)" ]
}

@test "plan_count_steps adds a step for packages and one for competing UIs" {
    MISSING_RUNTIME_DEPS="" MISSING_UNZIP_PKG="" COMPETING_UIS_FOUND=""
    plan_count_steps
    [ "$STEP_TOTAL" -eq 6 ]
    MISSING_UNZIP_PKG=unzip COMPETING_UIS_FOUND=KlipperScreen
    plan_count_steps
    [ "$STEP_TOTAL" -eq 8 ]
}

@test "detect_missing_runtime_deps installs nothing" {
    stub apt-get 'echo "apt-get $*" >> "'"$BATS_TEST_TMPDIR"'/apt.log"'
    detect_missing_runtime_deps pi
    [ -n "$MISSING_RUNTIME_DEPS" ]
    [ ! -f "$BATS_TEST_TMPDIR/apt.log" ]
}

@test "detect_missing_runtime_deps finds nothing to do off the Pi" {
    detect_missing_runtime_deps ad5m
    [ -z "$MISSING_RUNTIME_DEPS" ]
}

@test "detect_missing_unzip names unzip when apt can install it, and installs nothing" {
    _has_no_new_privs() { return 1; }
    # Only the stubs on PATH: no unzip, and an apt-get that logs any call.
    PATH="$STUBBIN" detect_missing_unzip
    [ "$MISSING_UNZIP_PKG" = unzip ]
    if grep -qs 'apt-get' "$BATS_TEST_TMPDIR/calls.log"; then fail "detection ran apt-get"; fi
    rm "$STUBBIN/apt-get"
    PATH="$STUBBIN" detect_missing_unzip
    [ -z "$MISSING_UNZIP_PKG" ]
}

@test "install_missing_unzip installs the package detection named" {
    MISSING_UNZIP_PKG=unzip
    stub apt-get 'exit 0'
    run install_missing_unzip
    grep -q 'apt-get install -y --no-install-recommends unzip' "$BATS_TEST_TMPDIR/calls.log" \
        || fail "apt-get install was not run"
}

@test "detect_competing_uis lists an active KlipperScreen unit without stopping it" {
    mock_command_script systemctl 'echo "systemctl $*" >> "'"$BATS_TEST_TMPDIR"'/calls.log"
case "$*" in *is-active*KlipperScreen*) exit 0 ;; esac
exit 1'
    INIT_SYSTEM=systemd
    detect_competing_uis
    [ "${COMPETING_UIS_FOUND#*KlipperScreen}" != "$COMPETING_UIS_FOUND" ]
    ! grep -E 'systemctl (stop|disable|mask|daemon-reload)' "$BATS_TEST_TMPDIR/calls.log"
}

@test "detect_moonraker_integration reports what would be added" {
    KLIPPER_CONFIG_DIR="$BATS_TEST_TMPDIR/printer_data/config"
    conf="$KLIPPER_CONFIG_DIR/moonraker.conf"
    mkdir -p "$KLIPPER_CONFIG_DIR"
    printf '[server]\n' > "$conf"
    : > "$BATS_TEST_TMPDIR/printer_data/moonraker.asvc"
    detect_moonraker_integration pi
    [ "${MOONRAKER_ADDS#*update-manager}" != "$MOONRAKER_ADDS" ]
    [ "${MOONRAKER_ADDS#*allowlist}" != "$MOONRAKER_ADDS" ]
    [ "$(cat "$conf")" = "[server]" ]
    [ ! -s "$BATS_TEST_TMPDIR/printer_data/moonraker.asvc" ]
}

@test "detect_moonraker_integration adds nothing that is already there" {
    KLIPPER_CONFIG_DIR="$BATS_TEST_TMPDIR/printer_data/config"
    mkdir -p "$KLIPPER_CONFIG_DIR"
    printf '[update_manager helixscreen]\n' > "$KLIPPER_CONFIG_DIR/moonraker.conf"
    printf 'helixscreen\n' > "$BATS_TEST_TMPDIR/printer_data/moonraker.asvc"
    detect_moonraker_integration pi
    [ -z "$MOONRAKER_ADDS" ]
}

@test "detect_kiauh finds the extensions dir unless registration is skipped" {
    HOME="$BATS_TEST_TMPDIR/home"
    mkdir -p "$HOME/kiauh/kiauh/extensions"
    skip_kiauh_registration=false
    detect_kiauh
    [ "$KIAUH_DIR" = "$HOME/kiauh/kiauh/extensions" ]
    skip_kiauh_registration=true
    detect_kiauh
    [ -z "$KIAUH_DIR" ]
}
