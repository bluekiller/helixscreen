#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The plan the read-only pass builds, shown before anything changes.

# Source guard
[ -n "${_HELIX_PLAN_SOURCED:-}" ] && return 0
_HELIX_PLAN_SOURCED=1

_PLAN=""

plan_set() { # key value
    _PLAN="${_PLAN}$(printf '  %-10s %s' "$1" "$2")
"
    _log_write "PLAN $1: $2"
}

print_plan() {
    printf '%s' "$_PLAN" >&2
}

# Steps the run will show, so a no-terminal run can number them [n/N].
plan_count_steps() {
    STEP_TOTAL=6
    [ -n "${MISSING_RUNTIME_DEPS:-}${MISSING_UNZIP_PKG:-}" ] && STEP_TOTAL=$((STEP_TOTAL + 1))
    [ -n "${COMPETING_UIS_FOUND:-}" ] && STEP_TOTAL=$((STEP_TOTAL + 1))
    return 0
}

_plan_sep() { if [ "$UI_UTF8" = 1 ]; then printf ' · '; else printf ', '; fi; }

# The Printer line: board, firmware flavor, init system, Klipper user.
plan_printer_line() { # platform
    _ppl="${HARDWARE_LABEL:-$1}"
    _ppl_fw="${MOD_FLAVOR:-${K1_FIRMWARE:-}}"
    [ -n "$_ppl_fw" ] && _ppl="$_ppl$(_plan_sep)$_ppl_fw"
    [ -n "${INIT_SYSTEM:-}" ] && _ppl="$_ppl$(_plan_sep)$INIT_SYSTEM"
    [ -n "${KLIPPER_USER:-}" ] && _ppl="$_ppl$(_plan_sep)user $KLIPPER_USER"
    printf '%s' "$_ppl"
}

# The Add line: what the install registers with Moonraker and KIAUH.
plan_adds_line() {
    _pal=""
    for _pa in ${MOONRAKER_ADDS:-}; do
        case "$_pa" in
            update-manager) _pa_text="Moonraker update manager" ;;
            allowlist) _pa_text="service allowlist" ;;
            *) continue ;;
        esac
        _pal="${_pal:+$_pal$(_plan_sep)}$_pa_text"
    done
    [ -n "${KIAUH_DIR:-}" ] && _pal="${_pal:+$_pal$(_plan_sep)}KIAUH extension"
    printf '%s' "$_pal"
}

# The confirm point: everything main() runs before it only reads, everything
# after it changes the machine. Shows the plan, exits on --dry-run or a "no",
# asks sudo for its password once, then opens the log. Returns only when the
# install goes ahead.
confirm_point() { # platform version
    _cp_install="$2 (${R2_CHANNEL:-stable})"
    if [ "${update_mode:-false}" = true ] && [ -f "${INSTALL_DIR:-}/release_info.json" ]; then
        _cp_from=$(parse_json_string_field version < "$INSTALL_DIR/release_info.json")
        if [ -n "$_cp_from" ]; then
            if [ "$UI_UTF8" = 1 ]; then _cp_arrow="→"; else _cp_arrow="->"; fi
            _cp_install="$_cp_from $_cp_arrow $_cp_install"
        fi
    fi
    # Package names never contain spaces, so word splitting joins the lists.
    # shellcheck disable=SC2086
    _cp_libs=$(echo ${MISSING_UNZIP_PKG:-} ${MISSING_RUNTIME_DEPS:-})
    _cp_add="$(plan_adds_line)"

    plan_set Printer "$(plan_printer_line "$1")"
    [ -d "${INSTALL_DIR:-}" ] && plan_set Found "HelixScreen at $INSTALL_DIR"
    plan_set Install "$_cp_install${PROBE_SIZE_TEXT:+$(_plan_sep)$PROBE_SIZE_TEXT}"
    [ -n "$_cp_libs" ] && plan_set Libraries "$_cp_libs (apt)"
    [ -n "${COMPETING_UIS_FOUND:-}" ] && plan_set Disable "$COMPETING_UIS_FOUND"
    [ -n "$_cp_add" ] && plan_set Add "$_cp_add"
    [ -n "${DISK_CHECK_DEFERRED:-}" ] && plan_set Disk "would check after sudo"
    [ -n "${SUDO:-}" ] && plan_set sudo "needed for: service, libraries, udev and polkit rules"

    print_banner "$2" "${R2_CHANNEL:-stable}"
    print_plan
    printf '\n' >&2

    if [ "${DRY_RUN:-false}" = true ]; then
        printf '%s\n' "Dry run, nothing changed." >&2
        exit 0
    fi

    # --update is the decision, so it never asks.
    if [ "${update_mode:-false}" != true ] && [ "$UI_TTY" = 1 ]; then
        tty_confirm "Continue?" y || { printf '%s\n' "Nothing changed." >&2; exit 0; }
    fi

    if [ -n "${SUDO:-}" ] && ! $SUDO -n true 2>/dev/null; then
        printf '%s\n' "sudo is needed to install the service and system files." >&2
        $SUDO -v || { log_error "sudo was not granted; nothing changed."; exit 1; }
    fi
    # shellcheck disable=SC2034  # consumed by requirements.sh (_fs_probe_write_kb)
    HELIX_CONFIRMED=1

    plan_count_steps
    step "Checked system"
    step_done "$(plan_printer_line "$1")"
    mkdir -p "$TMP_DIR" 2>/dev/null || $SUDO mkdir -p "$TMP_DIR"
    log_open "$TMP_DIR/install.log" || true
}
