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
    for c in systemctl sudo apt-get apt apt-cache dpkg-query pkexec udevadm pidof config-manager curl wget; do
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
    for m in common logo host_profile platform requirements competing_uis moonraker kiauh release plan uninstall main; do
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

@test "detect_competing_uis lists the K1 stock display processes the stop kills" {
    K1_FIRMWARE=stock_klipper INIT_SYSTEM=sysv
    stub pidof 'case "$1" in Monitor) echo 123 ;; *) exit 1 ;; esac'
    detect_competing_uis
    [ "$COMPETING_UIS_FOUND" = Creality-UI ]
    K1_FIRMWARE=simple_af
    detect_competing_uis
    [ -z "$COMPETING_UIS_FOUND" ]
}

@test "detect_competing_uis lists a Klipper Mod KlipperScreen python process" {
    AD5M_FIRMWARE=klipper_mod INIT_SYSTEM=sysv
    stub ps 'echo "root 321 1 0 00:00 ? 00:00:01 python3 /root/KlipperScreen/screen.py"; exit 0'
    detect_competing_uis
    [ "$COMPETING_UIS_FOUND" = KlipperScreen ]
    if grep -qs 'kill' "$BATS_TEST_TMPDIR/calls.log"; then fail "detection killed something"; fi
}

@test "detect_missing_runtime_deps prints no warning above the plan" {
    HELIX_INSTALL_VERBOSE=0
    run detect_missing_runtime_deps pi
    lacks "WARN" "$output"
}

@test "install_runtime_deps detects first when detection has not run" {
    unset MISSING_RUNTIME_DEPS
    _has_no_new_privs() { return 1; }
    stub apt-get 'exit 0'
    run install_runtime_deps pi
    grep -q 'apt-get install' "$BATS_TEST_TMPDIR/calls.log" \
        || fail "nothing was installed: $output"
}

@test "payload_legacy_prompt_adopt takes its answer from the terminal device" {
    printf 'y\n' > "$BATS_TEST_TMPDIR/tty"
    HELIX_TTY_DEVICE="$BATS_TEST_TMPDIR/tty"
    ASSUME_YES=false
    run payload_legacy_prompt_adopt /opt/helixscreen < /dev/null
    [ "$status" -eq 0 ]
}

@test "payload_legacy_prompt_adopt declines when nothing can ask, --yes included" {
    HELIX_TTY_DEVICE=/nonexistent
    ASSUME_YES=true
    run payload_legacy_prompt_adopt /opt/helixscreen < /dev/null
    [ "$status" -eq 1 ]
    payload_legacy_prompt_adopt /opt/helixscreen < /dev/null || true
    [ "$ASSUME_YES" = true ]
}

@test "detect_tmp_dir probes writability without sudo" {
    SUDO=sudo
    stub sudo 'exit 0'
    mkdir -p "$BATS_TEST_TMPDIR/ro" "$BATS_TEST_TMPDIR/home"
    chmod a-w "$BATS_TEST_TMPDIR/ro"
    INSTALL_DIR="$BATS_TEST_TMPDIR/ro/helixscreen"
    HOME="$BATS_TEST_TMPDIR/home"
    TMP_DIR=""
    detect_tmp_dir
    chmod u+w "$BATS_TEST_TMPDIR/ro"
    [ "$TMP_DIR" = "$HOME/.helixscreen-install" ]
    if grep -qs '^sudo' "$BATS_TEST_TMPDIR/calls.log"; then fail "detection ran sudo"; fi
}

@test "set_install_paths leaves the AD5M gcodes root alone" {
    AD5M_GCODES_ROOT="$BATS_TEST_TMPDIR/data"
    mkdir -p "$AD5M_GCODES_ROOT"
    : > "$AD5M_GCODES_ROOT/helixscreen-ad5m-v1.0.0.zip"
    TMP_DIR="$BATS_TEST_TMPDIR/helixscreen-install"
    set_install_paths ad5m forge_x
    [ -f "$AD5M_GCODES_ROOT/helixscreen-ad5m-v1.0.0.zip" ]
}

@test "check_disk_space defers a write probe sudo would have to ask for" {
    SUDO=sudo
    stub sudo 'exit 1'
    INSTALL_DIR="/nonexistent-helix-root/helixscreen"
    HELIX_DATA_MOUNT_CANDIDATES="/nonexistent-helix-data"
    DISK_CHECK_DEFERRED=""
    check_disk_space ad5x
    [ "$DISK_CHECK_DEFERRED" = 1 ]
    if grep -qs 'sudo dd' "$BATS_TEST_TMPDIR/calls.log"; then fail "probe ran sudo without -n"; fi
}

@test "probe_release HEADs the archive and downloads nothing" {
    TMP_DIR="$BATS_TEST_TMPDIR/scratch"
    R2_BASE_URL=https://r2.test HTTP_BASE_URL=http://mirror.test R2_CHANNEL=stable
    local_tarball=""
    stub curl 'case "$*" in
  --version*) echo "curl 8.0.0" ;;
  *manifest.json*) printf "{\"version\":\"1.2.3\",\"assets\":{\"pi\":{\"zip_sha256\":\"ab\"}}}" ;;
  *r2.test/releases/v1.2.3/helixscreen-pi.zip*) exit 0 ;;
  *) exit 22 ;;
esac'
    probe_release v1.2.3 pi
    [ "$PROBE_SIZE_TEXT" = "SHA256 available" ]
    [ ! -e "$TMP_DIR" ]
    grep -q 'r2.test/releases/v1.2.3/helixscreen-pi.zip' "$BATS_TEST_TMPDIR/calls.log"
}

@test "probe_release fails the way a download would when no archive exists" {
    TMP_DIR="$BATS_TEST_TMPDIR/scratch"
    R2_BASE_URL=https://r2.test HTTP_BASE_URL=http://mirror.test R2_CHANNEL=stable
    local_tarball=""
    stub curl 'case "$*" in --version*) echo "curl 8.0.0" ;; *) exit 22 ;; esac'
    run probe_release v9.9.9 pi
    [ "$status" -eq 1 ]
    contains "No HelixScreen v9.9.9 release for pi" "$output"
}

@test "probe_release sizes a --local archive without the network" {
    local_tarball="$BATS_TEST_TMPDIR/helixscreen-pi.zip"
    head -c 4096 /dev/zero > "$local_tarball"
    probe_release local pi
    contains "local file" "$PROBE_SIZE_TEXT"
    if grep -qs '^curl' "$BATS_TEST_TMPDIR/calls.log"; then fail "probed the network"; fi
}

_cp_setup() {
    TMP_DIR="$BATS_TEST_TMPDIR/scratch/helixscreen-install"
    INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    update_mode=false DRY_RUN=false ASSUME_YES=false SUDO=""
    INIT_SYSTEM=systemd KLIPPER_USER=pi UI_UTF8=0
    MISSING_RUNTIME_DEPS="" MISSING_UNZIP_PKG="" COMPETING_UIS_FOUND=""
    MOONRAKER_ADDS="" KIAUH_DIR="" PROBE_SIZE_TEXT="" DISK_CHECK_DEFERRED=""
}

@test "confirm_point on a dry run shows the plan and exits 0 with nothing written" {
    _cp_setup
    DRY_RUN=true
    COMPETING_UIS_FOUND=KlipperScreen
    run confirm_point pi v1.2.3
    [ "$status" -eq 0 ]
    contains "Install    v1.2.3 (stable)" "$output"
    contains "Disable    KlipperScreen" "$output"
    contains "Dry run, nothing changed." "$output"
    lacks "Checked system" "$output"
    [ ! -e "$TMP_DIR" ]
}

@test "confirm_point with no terminal continues, closes Checked system and opens the log" {
    _cp_setup
    UI_TTY=0
    run confirm_point pi v1.2.3
    [ "$status" -eq 0 ]
    contains "Checked system ... ok (pi, systemd, user pi)" "$output"
    grep -q 'PLAN Install: v1.2.3' "$TMP_DIR/install.log"
}

@test "confirm_point asks on a terminal, and n changes nothing" {
    _cp_setup
    UI_TTY=1
    printf 'n\n' > "$BATS_TEST_TMPDIR/tty"
    HELIX_TTY_DEVICE="$BATS_TEST_TMPDIR/tty"
    run confirm_point pi v1.2.3
    [ "$status" -eq 0 ]
    contains "Nothing changed." "$output"
    [ ! -e "$TMP_DIR" ]
}

@test "confirm_point never asks on --update and shows the version change" {
    _cp_setup
    UI_TTY=1
    update_mode=true
    mkdir -p "$INSTALL_DIR"
    printf '{"project_name":"helixscreen","version":"v1.2.2"}' > "$INSTALL_DIR/release_info.json"
    printf 'n\n' > "$BATS_TEST_TMPDIR/tty"
    HELIX_TTY_DEVICE="$BATS_TEST_TMPDIR/tty"
    run confirm_point pi v1.2.3
    [ "$status" -eq 0 ]
    contains "v1.2.2 -> v1.2.3" "$output"
    [ -f "$TMP_DIR/install.log" ]
}

@test "confirm_point asks sudo for its password once, before any step" {
    _cp_setup
    UI_TTY=0
    SUDO=sudo
    stub sudo 'case "$1" in -n) exit 1 ;; -v) exit 0 ;; esac; exit 1'
    run confirm_point pi v1.2.3
    [ "$status" -eq 0 ]
    [ "$(grep -c '^sudo -v' "$BATS_TEST_TMPDIR/calls.log")" -eq 1 ]
}

@test "confirm_point stops with nothing changed when sudo is refused" {
    _cp_setup
    UI_TTY=0
    SUDO=sudo
    stub sudo 'exit 1'
    run confirm_point pi v1.2.3
    [ "$status" -eq 1 ]
    contains "nothing changed" "$output"
    [ ! -e "$TMP_DIR" ]
}

@test "the payload root is recorded after the confirm point, not by mod_payload_mode_block" {
    HOST_MOD_ROOT="$BATS_TEST_TMPDIR/usr/data/config/mod"
    HELIX_MOD_PAYLOAD=1 HOST_SERVICE_MECHANISM=mod-managed uninstall_mode=false
    MOD_PAYLOAD_ROOT="" MOD_PAYLOAD_FLAG_GIVEN="" HOST_LEGACY_INSTALL_ROOT=""
    INSTALL_DIR="$BATS_TEST_TMPDIR/usr/data/config/mod_data/helixscreen"
    mkdir -p "$(host_mod_data)"
    mod_payload_mode_block
    [ ! -e "$(host_payload_root_record)" ]
    record_payload_root_if_payload
    [ "$(cat "$(host_payload_root_record)")" = "$INSTALL_DIR" ]
}
