#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# ensure-polkit-rule.sh - Install the polkit rules a non-root HelixScreen
# service user needs
#
# Called by ExecStartPre=+ in helixscreen.service (runs as root) on every
# start, so an install self-heals on its next restart, and by the installer's
# install_permission_rules().
#
# Usage: ensure-polkit-rule.sh <username>
#
# Two independent rules:
#   network  NetworkManager access, only where nmcli exists. Left alone when
#            any HelixScreen network rule is already present.
#   power    logind power-off and reboot for this host. A service has no seat
#            session, so logind's default answer for it is "authentication
#            required". Rewritten whenever its content differs, which repairs
#            a changed service user.
#
# Prefers .rules (polkit >= 0.106, Debian 12+), falls back to .pkla (polkit
# 0.105, Debian 11). polkitd watches both directories and reloads on change.
# Always exits 0: a rule it cannot write must not stop HelixScreen starting.
#
# HELIX_POLKIT_DIR overrides /etc/polkit-1 for tests.

HELIX_USER="${1:-root}"
POLKIT_DIR="${HELIX_POLKIT_DIR:-/etc/polkit-1}"
RULES_DIR="$POLKIT_DIR/rules.d"
PKLA_DIR="$POLKIT_DIR/localauthority/50-local.d"

# Root doesn't need polkit rules
[ "$HELIX_USER" = "root" ] && exit 0

# write_if_changed <dest> <content>: rewrite <dest> only when it differs.
write_if_changed() {
    if [ -f "$1" ] && [ "$(cat "$1")" = "$2" ]; then
        return 0
    fi
    if ! printf '%s\n' "$2" 2>/dev/null > "$1"; then
        echo "ensure-polkit-rule: cannot write $1" >&2
        return 0
    fi
    chmod 644 "$1" 2>/dev/null || true
    echo "ensure-polkit-rule: installed $1"
}

ensure_network_rule() {
    command -v nmcli >/dev/null 2>&1 || return 0

    [ -f "$RULES_DIR/49-helixscreen-network.rules" ] && return 0
    [ -f "$RULES_DIR/50-helixscreen-network.rules" ] && return 0
    [ -f "$PKLA_DIR/helixscreen-network.pkla" ] && return 0

    if [ -d "$RULES_DIR" ]; then
        write_if_changed "$RULES_DIR/49-helixscreen-network.rules" "\
// Installed by HelixScreen - allow service user to manage NetworkManager
polkit.addRule(function(action, subject) {
    if (action.id.indexOf(\"org.freedesktop.NetworkManager.\") === 0 &&
        subject.user === \"${HELIX_USER}\") {
        return polkit.Result.YES;
    }
});"
    elif [ -d "$PKLA_DIR" ]; then
        write_if_changed "$PKLA_DIR/helixscreen-network.pkla" "\
# Installed by HelixScreen - allow service user to manage NetworkManager
# connections (Wi-Fi connect/disconnect/scan)
[HelixScreen NetworkManager access]
Identity=unix-user:${HELIX_USER}
Action=org.freedesktop.NetworkManager.*
ResultAny=yes
ResultInactive=yes
ResultActive=yes"
    fi
}

# The six actions logind checks for PowerOff/Reboot: the plain one, the one
# for when other users are logged in, and the one for overriding an inhibitor.
POWER_ACTIONS="org.freedesktop.login1.power-off
org.freedesktop.login1.power-off-multiple-sessions
org.freedesktop.login1.power-off-ignore-inhibit
org.freedesktop.login1.reboot
org.freedesktop.login1.reboot-multiple-sessions
org.freedesktop.login1.reboot-ignore-inhibit"

ensure_power_rule() {
    if [ -d "$RULES_DIR" ]; then
        ids=$(printf '%s\n' "$POWER_ACTIONS" | sed 's/.*/        "&",/; $ s/,$//')
        write_if_changed "$RULES_DIR/49-helixscreen-power.rules" "\
// Installed by HelixScreen - allow service user to power off and reboot this host
polkit.addRule(function(action, subject) {
    var actions = [
${ids}
    ];
    if (actions.indexOf(action.id) >= 0 && subject.user === \"${HELIX_USER}\") {
        return polkit.Result.YES;
    }
});"
        rm -f "$PKLA_DIR/helixscreen-power.pkla"
    elif [ -d "$PKLA_DIR" ]; then
        write_if_changed "$PKLA_DIR/helixscreen-power.pkla" "\
# Installed by HelixScreen - allow service user to power off and reboot this host
[HelixScreen power off and reboot]
Identity=unix-user:${HELIX_USER}
Action=$(printf '%s' "$POWER_ACTIONS" | tr '\n' ';')
ResultAny=yes
ResultInactive=yes
ResultActive=yes"
    fi
}

ensure_network_rule
ensure_power_rule
exit 0
