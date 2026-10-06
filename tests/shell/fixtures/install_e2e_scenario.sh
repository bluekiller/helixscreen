#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs the bundled install.sh end to end against a throwaway root.
#
# Invoked by test_install_e2e.bats as:
#   unshare --user --map-root-user --mount --pid --net --fork bash <this> <work> <step>...
#
# The user + mount + pid + net namespaces are the sandbox. Inside them this script
# mounts empty tmpfs trees over every directory the installer writes (/etc,
# /opt, /home, /root, /var, /run, /tmp), bind-mounts stubs over the service
# manager, and gets a /proc that shows only its own processes. The host sees
# none of it: every mount dies with the namespace, and a name-based process
# lookup inside it (pidof, killall) finds nothing it did not start.
#
# <work> holds install.sh, the fake release archives, the stubs and the seed
# files; it is reached through /mnt once /tmp is covered. Each <step> runs the
# installer once and then copies the resulting tree to <work>/out/<n>-<step>/,
# which is what the bats file asserts on. Steps share one root, so an update
# step lands on the tree the previous install step left behind.
#
# Steps:
#   install        fresh install of release 1
#   seed-user      edit settings.json and helixscreen.env the way a user would
#   customize-unit add a local line to the installed systemd unit
#   clean-install  --clean --yes install of release 2
#   update         --update to release 2
#   update-beta-local    --update to release 3, a prerelease, from --local
#   update-beta-version  --update --version of release 3 from the stub CDN
#   self-update    --update to release 2 under HELIX_SELF_UPDATE=1
#   uninstall      --uninstall
#   net-probe      print the network interfaces the scenario can see

set -uo pipefail

work="${1:?work dir required}"
shift

# The suite sandbox blocks `mount` by name, because a test that mounts over the
# host's /etc wrecks the machine. This script runs in the private mount
# namespace named in the header, so it steps out of the sandbox for the mounts
# below, and only for those.
unset -f mount umount 2>/dev/null || true
if [ -n "${HELIX_TEST_SANDBOX_BIN:-}" ]; then
    PATH="${PATH//$HELIX_TEST_SANDBOX_BIN:/}"
    export PATH
fi

if ! mount --bind "$work" /mnt; then
    echo "SANDBOX_MOUNT_FAIL"
    exit 0
fi
for d in /etc /opt /home /root /var /run /tmp; do
    mount -t tmpfs tmpfs "$d" || { echo "SANDBOX_MOUNT_FAIL"; exit 0; }
done
mount -t proc proc /proc || { echo "SANDBOX_MOUNT_FAIL"; exit 0; }

# The network namespace has only a loopback, and it starts down. Bring it up so
# anything dialling 127.0.0.1 gets a refusal, as on a printer with no Moonraker,
# rather than an unreachable network.
ip link set lo up 2>/dev/null || true

# The bundle puts the stock system directories first on PATH, so each stub is
# bound over the first copy of the command in that order rather than put ahead
# of it.
for stub in /mnt/stubs/*; do
    name=$(basename "$stub")
    for dir in /usr/sbin /usr/bin /sbin /bin; do
        if [ -x "$dir/$name" ]; then
            mount --bind "$stub" "$dir/$name" || { echo "SANDBOX_MOUNT_FAIL"; exit 0; }
            break
        fi
    done
done

# The installer targets BusyBox ash; plain sh stands in where it is absent.
if busybox ash -c : 2>/dev/null; then
    run_installer() { busybox ash /mnt/install.sh "$@"; }
else
    run_installer() { sh /mnt/install.sh "$@"; }
fi

# A Debian host running Klipper as root. E2E_HOST=printer_data (the default)
# gives it Moonraker's config where the installer looks for it; E2E_HOST=bare
# leaves the Klipper ecosystem off the box.
cp /mnt/seed/os-release /mnt/seed/passwd /mnt/seed/group /etc/
mkdir -p /etc/systemd/system /etc/init.d /run/systemd/system /var/log
if [ "${E2E_HOST:-printer_data}" = printer_data ]; then
    mkdir -p /root/printer_data/config
    cp /mnt/seed/moonraker.conf /root/printer_data/config/moonraker.conf
fi

export HOME=/root
cd /tmp || exit 1

n=0
for step in "$@"; do
    n=$((n + 1))
    echo "=== STEP $n: $step"
    rc=0
    case "$step" in
        install)
            run_installer --local /mnt/release-1/helixscreen-x86-v1.0.0.tar.gz || rc=$?
            ;;
        seed-user)
            inst=$(sed -n 's/^WorkingDirectory=//p' /etc/systemd/system/helixscreen.service)
            printf '\nHELIX_E2E_USER_KEY=kept\n' >> "$inst/config/helixscreen.env"
            printf '{"config_version": 9, "e2e_user_value": "kept"}\n' \
                > "$inst/config/settings.json"
            ;;
        customize-unit)
            echo "# e2e local customization" >> /etc/systemd/system/helixscreen.service
            ;;
        update)
            run_installer --update --local /mnt/release-2/helixscreen-x86-v1.0.1.tar.gz || rc=$?
            ;;
        update-beta-local)
            run_installer --update --local /mnt/release-3/helixscreen-x86-v1.1.0-beta.1.tar.gz || rc=$?
            ;;
        update-beta-version)
            (export R2_BASE_URL=https://e2e.invalid HTTP_BASE_URL=http://e2e.invalid
             run_installer --update --version v1.1.0-beta.1) || rc=$?
            ;;
        self-update)
            (export HELIX_SELF_UPDATE=1
             run_installer --update --local /mnt/release-2/helixscreen-x86-v1.0.1.tar.gz) || rc=$?
            ;;
        clean-install)
            run_installer --clean --yes --local /mnt/release-2/helixscreen-x86-v1.0.1.tar.gz || rc=$?
            ;;
        uninstall)
            run_installer --uninstall || rc=$?
            ;;
        net-probe)
            # /proc is this namespace's own mount, so /proc/net/dev lists the
            # interfaces of the network namespace the scenario runs in.
            echo "NET_IFACES: $(sed -n 's/^ *\([^:]*\):.*/\1/p' /proc/net/dev | sort | paste -sd' ' -)"
            ;;
        *)
            echo "unknown step: $step"
            rc=2
            ;;
    esac
    echo "=== STEP $n: $step exit=$rc"
    snap="/mnt/out/$n-$step"
    mkdir -p "$snap"
    cp -a /etc /opt /root /var "$snap/" 2>/dev/null
    [ "$rc" -eq 0 ] || exit "$rc"
done
echo "SCENARIO_DONE"
