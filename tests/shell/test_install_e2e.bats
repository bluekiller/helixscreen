#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# End-to-end runs of the bundled scripts/install.sh (prestonbrown/helixscreen#1600).
#
# Every other installer test calls one function; these run the whole script, so
# the handoff between steps is what they pin. The bundle is rebuilt from
# scripts/lib/installer/ exactly as the release workflow builds it, then run
# under BusyBox ash inside a user + mount + pid namespace
# (fixtures/install_e2e_scenario.sh): tmpfs over every directory it writes, a
# stateful systemctl stub, and a fake x86 release - /bin/true for the binaries,
# the real config/ tree and launcher, and enough padding to pass the size check.
#
# The host is a Debian box running Klipper as root in one of two shapes. With
# Moonraker's config at /root/printer_data/config (the default), the install
# lands at /root/helixscreen with its config linked into printer_data. Bare
# (E2E_HOST=bare), it lands at /opt/helixscreen with its config inside the
# payload, which is the shape an update's backup-and-restore has to carry.
# Each test gets a fresh root.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
SCENARIO="$WORKTREE_ROOT/tests/shell/fixtures/install_e2e_scenario.sh"
INST=/root/helixscreen
USER_CFG=/root/printer_data/config/helixscreen
UNIT=/etc/systemd/system/helixscreen.service

setup_file() {
    local art="$BATS_FILE_TMPDIR/e2e"
    mkdir -p "$art/stubs" "$art/seed"
    bash "$WORKTREE_ROOT/scripts/bundle-installer.sh" -o "$art/install.sh" >/dev/null

    _make_release "$art" 1 v1.0.0
    _make_release "$art" 2 v1.0.1

    cp /etc/os-release "$art/seed/os-release"
    printf 'root:x:0:0:root:/root:/bin/sh\n' > "$art/seed/passwd"
    printf 'root:x:0:\n' > "$art/seed/group"
    printf '[server]\nhost: 0.0.0.0\n\n[authorization]\n' > "$art/seed/moonraker.conf"

    # Records every call and keeps enough state for is-active/is-enabled to
    # answer consistently with the starts, stops and enables before them.
    cat > "$art/stubs/systemctl" <<'STUB'
#!/bin/sh
echo "systemctl $*" >> /var/log/e2e-systemctl.log
unit=""
for a in "$@"; do case "$a" in -*) ;; *) unit="$a" ;; esac; done
case "$1" in
    start|restart) touch "/run/e2e-active-$unit" ;;
    stop) rm -f "/run/e2e-active-$unit" ;;
    is-active) [ -e "/run/e2e-active-$unit" ] || exit 3 ;;
    is-enabled) [ -e "/run/e2e-enabled-$unit" ] || exit 1 ;;
    enable) touch "/run/e2e-enabled-$unit" ;;
    disable) rm -f "/run/e2e-enabled-$unit" ;;
esac
exit 0
STUB
    chmod +x "$art/stubs/systemctl"
}

# A release archive in the shipped layout, marked with its version.
_make_release() {
    local art=$1 n=$2 version=$3
    local pkg="$art/pkg$n/helixscreen"
    mkdir -p "$pkg/bin" "$pkg/assets" "$pkg/ui_xml" "$art/release-$n"
    local b
    for b in helix-screen helix-splash helix-watchdog; do
        cp /bin/true "$pkg/bin/$b"
    done
    cp "$WORKTREE_ROOT/scripts/helix-launcher.sh" "$pkg/bin/"
    cp -r "$WORKTREE_ROOT/config" "$pkg/config"
    echo "$version" > "$pkg/ui_xml/e2e-release.txt"
    # Incompressible, so the archive clears the installer's 1MB floor.
    head -c 1600000 /dev/urandom > "$pkg/assets/e2e-pad.bin"
    tar -czf "$art/release-$n/helixscreen-x86-$version.tar.gz" -C "$art/pkg$n" helixscreen
}

setup() {
    load helpers
    command -v unshare >/dev/null 2>&1 || skip "unshare not available"
    unshare --user --map-root-user --mount true 2>/dev/null \
        || skip "user+mount namespaces not permitted"
    [ -x /usr/bin/systemctl ] || [ -x /bin/systemctl ] || [ -x /usr/sbin/systemctl ] \
        || skip "no systemctl to bind the stub over"
    [ -d /mnt ] || skip "no /mnt to reach the work dir through"

    WORK="$BATS_TEST_TMPDIR/work"
    cp -a "$BATS_FILE_TMPDIR/e2e" "$WORK"
    mkdir -p "$WORK/out"
}

# Run the scenario steps in one fresh root; fails the test unless all complete.
run_scenario() {
    run unshare --user --map-root-user --mount --pid --fork bash "$SCENARIO" "$WORK" "$@"
    [[ "$output" == *"SANDBOX_MOUNT_FAIL"* ]] && skip "namespace mounts not permitted here"
    [ "$status" -eq 0 ] && [[ "$output" == *"SCENARIO_DONE"* ]] \
        || fail "scenario ($*) failed, status $status:
$output"
}

# The snapshot taken after step <n>-<name>.
snap() {
    echo "$WORK/out/$1"
}

# Follow symlinks inside a snapshot: an absolute link target names a path in
# the sandbox root, not on the host.
snap_resolve() {
    local root=$1 path=$2 i target
    for i in 1 2 3 4 5 6 7 8; do
        [ -L "$root$path" ] || break
        target=$(readlink "$root$path")
        case "$target" in
            /*) path=$target ;;
            *) path="$(dirname "$path")/$target" ;;
        esac
    done
    echo "$root$path"
}

@test "install.sh e2e: a fresh install lays down the payload, the unit and the updater entry" {
    run_scenario install
    local s
    s=$(snap 1-install)

    contains "Installation Complete!" "$output"
    [ -x "$s$INST/bin/helix-screen" ] || fail "no payload binary at $INST"
    [ -x "$s$INST/bin/helix-launcher.sh" ] || fail "no launcher at $INST"
    [ "$(cat "$s$INST/ui_xml/e2e-release.txt")" = "v1.0.0" ]

    # The unit runs the payload it was installed with.
    [ -f "$s$UNIT" ] || fail "no systemd unit"
    grep -qx "WorkingDirectory=$INST" "$s$UNIT"
    grep -qx "ExecStart=$INST/bin/helix-launcher.sh" "$s$UNIT"
    grep -qx "systemctl enable helixscreen" "$s/var/log/e2e-systemctl.log"
    grep -qx "systemctl start helixscreen" "$s/var/log/e2e-systemctl.log"

    # Moonraker's updater points at the same root.
    grep -qx "\[update_manager helixscreen\]" "$s/root/printer_data/config/moonraker.conf"
    grep -qx "path: $INST" "$s/root/printer_data/config/moonraker.conf"

    # Config lives in printer_data, and the payload's config links to it.
    [ -f "$s$USER_CFG/helixscreen.env" ] || fail "no helixscreen.env in printer_data"
    [ "$(readlink "$s$INST/config/settings.json")" = "$USER_CFG/settings.json" ]
}

@test "install.sh e2e: an update replaces the payload and carries config and env across" {
    run_scenario install seed-user update
    local s
    s=$(snap 3-update)

    [ "$(cat "$s$INST/ui_xml/e2e-release.txt")" = "v1.0.1" ]
    grep -q '"e2e_user_value": "kept"' "$(snap_resolve "$s" "$INST/config/settings.json")" \
        || fail "settings.json edit lost across the update"
    grep -qx "HELIX_E2E_USER_KEY=kept" "$(snap_resolve "$s" "$INST/config/helixscreen.env")" \
        || fail "helixscreen.env edit lost across the update"

    # The service is stopped for the swap and started on the new payload.
    local calls
    calls=$(sed -n "$(($(wc -l < "$(snap 1-install)/var/log/e2e-systemctl.log") + 1)),\$p" \
        "$s/var/log/e2e-systemctl.log")
    contains "systemctl stop helixscreen" "$calls"
    contains "systemctl start helixscreen" "$calls"
    grep -qx "ExecStart=$INST/bin/helix-launcher.sh" "$s$UNIT"
    grep -qx "path: $INST" "$s/root/printer_data/config/moonraker.conf"
}

@test "install.sh e2e: a self-update keeps the running service and the local unit" {
    run_scenario install customize-unit self-update
    local s before
    s=$(snap 3-self-update)
    before=$(snap 2-customize-unit)

    [ "$(cat "$s$INST/ui_xml/e2e-release.txt")" = "v1.0.1" ]
    cmp -s "$before$UNIT" "$s$UNIT" || fail "self-update rewrote the systemd unit"
    local calls
    calls=$(sed -n "$(($(wc -l < "$before/var/log/e2e-systemctl.log") + 1)),\$p" \
        "$s/var/log/e2e-systemctl.log")
    [[ "$calls" != *"systemctl stop helixscreen"* ]] \
        || fail "self-update stopped the service it runs under"
}

@test "install.sh e2e: uninstall removes the payload, the units and the updater entry" {
    run_scenario install uninstall
    local s
    s=$(snap 2-uninstall)

    [ ! -e "$s$INST" ] || fail "payload left behind at $INST"
    [ ! -e "$s$UNIT" ] || fail "systemd unit left behind"
    [ -z "$(ls "$s/etc/systemd/system" | grep -i helixscreen)" ] \
        || fail "helixscreen units left in /etc/systemd/system"
    ! grep -q "update_manager helixscreen" "$s/root/printer_data/config/moonraker.conf" \
        || fail "updater entry left in moonraker.conf"
    # The user's config is kept on purpose, where the uninstall says it is.
    contains "User config preserved at: $USER_CFG" "$output"
}

@test "install.sh e2e (bare host): an update carries the payload's own config and env across" {
    export E2E_HOST=bare
    run_scenario install seed-user update
    local s
    s=$(snap 3-update)

    [ "$(cat "$s/opt/helixscreen/ui_xml/e2e-release.txt")" = "v1.0.1" ]
    [ ! -L "$s/opt/helixscreen/config/settings.json" ] \
        || fail "bare host config should live in the payload, not a link"
    grep -q '"e2e_user_value": "kept"' "$s/opt/helixscreen/config/settings.json" \
        || fail "settings.json edit lost across the update"
    grep -qx "HELIX_E2E_USER_KEY=kept" "$s/opt/helixscreen/config/helixscreen.env" \
        || fail "helixscreen.env edit lost across the update"
}

@test "install.sh e2e (bare host): uninstall leaves nothing behind" {
    export E2E_HOST=bare
    run_scenario install uninstall
    local s
    s=$(snap 2-uninstall)

    [ -z "$(ls -A "$s/opt")" ] || fail "left in /opt: $(ls -A "$s/opt")"
    [ -z "$(ls -A "$s/root")" ] || fail "left in /root: $(ls -A "$s/root")"
    [ ! -e "$s/var/lib/helixscreen" ] || fail "state dir left in /var/lib"
    [ -z "$(ls "$s/etc/systemd/system" | grep -i helixscreen)" ] \
        || fail "helixscreen units left in /etc/systemd/system"
}
