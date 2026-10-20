#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# HelixPrint Moonraker Plugin Installer
#
# This script installs the helix_print Moonraker plugin.
#
# Usage:
#   ./install.sh              # Interactive install (manual config steps)
#   ./install.sh --auto       # Full auto-install (updates config, restarts Moonraker)
#   ./install.sh --uninstall  # Remove the plugin
#
# On a Moonraker host HelixScreen does not run on, remote-install.sh fetches
# a release's plugin files and runs this script from them.

set -e

# Get script directory (POSIX-compatible)
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PLUGIN_FILE="$SCRIPT_DIR/helix_print.py"
AUTO_MODE=false

# Exit codes for --uninstall and --uninstall-auto:
#   0 - uninstalled; nothing left in the config to clean up
#   2 - the plugin was removed, but the phase-tracking strip skipped or
#       failed a config file, which needs a human to look at it
#   1 (or any other nonzero) - the uninstall itself failed; the plugin may
#       still be present
EXIT_UNINSTALL_OK=0
EXIT_UNINSTALL_FAILED=1
EXIT_UNINSTALL_NEEDS_ATTENTION=2

# Exit codes for --relink (HelixPluginInstaller reads them):
#   0 - the link already names this tree's plugin; nothing done
#   3 - the link was re-pointed at this tree's plugin
#   4 - no plugin of ours to repair (no link, a regular file, or no
#       [helix_print] section); nothing done
#   1 - the link could not be repaired
EXIT_RELINK_CURRENT=0
EXIT_RELINK_DONE=3
EXIT_RELINK_NOT_OURS=4

# Colors for output (works with printf)
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

info() {
    printf "${GREEN}[INFO]${NC} %s\n" "$1"
}

warn() {
    printf "${YELLOW}[WARN]${NC} %s\n" "$1"
}

error() {
    printf "${RED}[ERROR]${NC} %s\n" "$1"
    exit 1
}

# helix_print_section_files and friends: which config files hold our sections.
[ -f "$SCRIPT_DIR/helix_print_conf.sh" ] || error "Missing $SCRIPT_DIR/helix_print_conf.sh"
# shellcheck source=moonraker-plugin/helix_print_conf.sh
. "$SCRIPT_DIR/helix_print_conf.sh"

# Moonraker source roots on known vendor layouts. Keep in step with
# MOONRAKER_SRC_PATHS in scripts/lib/installer/moonraker.sh; a bats test
# fails when an entry there is missing here.
MOONRAKER_SRC_PATHS="
/home/pi/moonraker
/home/biqu/moonraker
/home/mks/moonraker
/home/qidi/moonraker
/home/klipper/moonraker
/home/lava/moonraker
/root/moonraker
/root/printer_software/moonraker
/usr/data/moonraker
/usr/share/moonraker
/mnt/UDISK/moonraker
/userdata/moonraker
/opt/moonraker
"

# Config directories on known vendor layouts, the directories of
# MOONRAKER_CONF_PATHS in scripts/lib/installer/moonraker.sh.
MOONRAKER_CONFIG_DIRS="
/home/pi/printer_data/config
/home/pi/klipper_config
/home/biqu/printer_data/config
/home/mks/printer_data/config
/home/qidi/printer_data/config
/root/printer_data/config
/opt/config/printer_data/config
/opt/config
/usr/data/config/printer_data/config
/usr/data/printer_data/config
/etc/klipper/config
"

# Where running processes are listed; tests point it at a fixture tree.
PROC_ROOT="${HELIX_PROC_ROOT:-/proc}"

# Print Moonraker's components directory under a source root. Three layouts:
#   <root>/moonraker/components            git checkout (~/moonraker)
#   <root>/components                      the package directory itself
#   <root>/moonraker/moonraker/components  Creality K1/K2: the repo nested
#                                          inside an install directory
components_under() {
    for sub in "$1/moonraker/components" "$1/components" "$1/moonraker/moonraker/components"; do
        if [ -d "$sub" ]; then
            printf '%s' "$sub"
            return 0
        fi
    done
    return 1
}

# Print the running Moonraker's /proc directory.
running_moonraker_proc() {
    for f in "$PROC_ROOT"/[0-9]*/cmdline; do
        [ -r "$f" ] || continue
        args=$(tr '\000' '\n' < "$f" 2>/dev/null) || continue
        case "$args" in
            *moonraker.py*)
                printf '%s' "$(dirname "$f")"
                return 0
                ;;
        esac
    done
    return 1
}

# Print the running Moonraker's argv, one argument per line.
running_moonraker_args() {
    proc=$(running_moonraker_proc) || return 1
    tr '\000' '\n' < "$proc/cmdline" 2>/dev/null
}

# Print where the running Moonraker's "/" is from here: empty when it shares
# ours, the chroot directory when it runs in one (Forge-X runs Moonraker in
# /data/.mod/.forge-x). Every path in its argv is relative to that root.
running_moonraker_root() {
    proc=$(running_moonraker_proc) || return 0
    root=$(readlink "$proc/root" 2>/dev/null) || return 0
    [ "$root" = "/" ] && return 0
    printf '%s' "${root%/}"
}

# Translate a path on this side into the same file as the running Moonraker
# names it: strip its chroot prefix when the path lies inside the chroot. A
# path outside it is only visible to Moonraker through a bind mount at the
# same place (Forge-X binds /opt/config), which the caller checks.
path_for_moonraker() {
    mroot=$(running_moonraker_root)
    case "$1" in
        "$mroot"/*) [ -n "$mroot" ] && { printf '%s' "${1#"$mroot"}"; return 0; } ;;
    esac
    printf '%s' "$1"
}

# Print the argument following the first of the given flags in the running
# Moonraker's argv.
running_moonraker_arg() {
    running_moonraker_args | awk -v a="$1" -v b="$2" 'p == a || p == b { print; exit } { p = $0 }'
}

# Whether a link can be made in directory @1: a probe link is made there and
# removed. test -w alone passes for root on a read-only filesystem.
can_link_in() {
    probe="$1/.helix_print.probe.$$"
    ln -s "$PLUGIN_FILE" "$probe" 2>/dev/null || return 1
    rm -f "$probe"
}

# Find Moonraker's components directory. An explicit path wins, then the
# user's home, then the Moonraker that is actually running (whatever its
# layout), then the known vendor roots, then a pip-installed package.
find_moonraker_components() {
    for root in "$1" "$HOME/moonraker" "$HOME/klipper_config/moonraker"; do
        if [ -n "$root" ] && components_under "$root"; then
            return 0
        fi
    done

    script=$(running_moonraker_args | awk '/moonraker\.py$/ { print; exit }')
    if [ -n "$script" ]; then
        script="$(running_moonraker_root)$script"
        if [ -d "$(dirname "$script")/components" ]; then
            printf '%s' "$(dirname "$script")/components"
            return 0
        fi
    fi

    for root in $MOONRAKER_SRC_PATHS; do
        if components_under "$root"; then
            return 0
        fi
    done

    pip_loc=$(python3 -c "import moonraker; print(moonraker.__path__[0])" 2>/dev/null || true)
    if [ -n "$pip_loc" ] && [ -d "$pip_loc/components" ]; then
        printf '%s' "$pip_loc/components"
    fi
}

# Find the directory holding moonraker.conf: the user's home first, then the
# running Moonraker's -c/-d arguments, then the known vendor layouts.
find_config_dir() {
    mroot=$(running_moonraker_root)
    conf=$(running_moonraker_arg -c --configfile)
    data=$(running_moonraker_arg -d --datapath)
    conf="${conf:+$mroot$conf}"
    data="${data:+$mroot$data}"
    for loc in \
        "$HOME/printer_data/config" \
        "$HOME/klipper_config" \
        "${conf:+$(dirname "$conf")}" \
        "${data:+$data/config}" \
        $MOONRAKER_CONFIG_DIRS
    do
        if [ -n "$loc" ] && [ -d "$loc" ] && [ -f "$loc/moonraker.conf" ]; then
            printf '%s' "$loc"
            return 0
        fi
    done
    printf ''
}

# Print the file the [helix_print] section belongs in, given the directory
# holding moonraker.conf. Normally moonraker.conf itself. A firmware mod that
# owns moonraker.conf (a symlink into the mod's own tree, which its updates
# rewrite) includes a user.moonraker.conf for the user's sections: Forge-X's
# `[include mod_data/user.moonraker.conf]`. The section goes there instead.
helix_conf_file() {
    conf="$1/moonraker.conf"
    if [ -L "$conf" ]; then
        # Trailing blanks after "]" include the CR of a file saved on Windows;
        # neither changes what Moonraker includes.
        inc=$(sed -n 's/^\[include[[:space:]]\{1,\}\([^]*?]*user\.moonraker\.conf\)[[:space:]]*\][[:space:]]*$/\1/p' "$conf" 2>/dev/null |
            tail -n 1)
        if [ -n "$inc" ]; then
            case "$inc" in
                /*) printf '%s' "$(running_moonraker_root)$inc" ;;
                *) printf '%s' "$1/$inc" ;;
            esac
            return 0
        fi
    fi
    printf '%s' "$conf"
}

# Print the running Klipper's extras/virtual_sdcard.py, the code that decides
# which files SDCARD_PRINT_FILE can open. Empty when no Klipper runs here.
running_klipper_sdcard_source() {
    for f in "$PROC_ROOT"/[0-9]*/cmdline; do
        [ -r "$f" ] || continue
        klippy=$(tr '\000' '\n' < "$f" 2>/dev/null | awk '/klippy\/klippy\.py$/ { print; exit }')
        [ -n "$klippy" ] || continue
        root=$(readlink "$(dirname "$f")/root" 2>/dev/null) || root=""
        [ "$root" = "/" ] && root=""
        for sdcard in "${root%/}$(dirname "$klippy")/extras/virtual_sdcard.py" \
                      "$(dirname "$klippy")/extras/virtual_sdcard.py"; do
            if [ -f "$sdcard" ]; then
                printf '%s' "$sdcard"
                return 0
            fi
        done
        return 0
    done
}

# Generic probe: a virtual_sdcard that prunes dot-directories from its walk
# with str.startswith, the idiomatic spelling of the filter.
sdcard_prunes_dot_dirs_generic() {
    grep -Eq "dirs\[:\] *= *\[d for d in dirs if not d\.startswith\(['\"]\.['\"]\)" "$1"
}

# Vendor probe, Forge-X: its Klipper patch "Ignore hidden files and
# directories" (ff5m commit aab9d340) spells the filter as an index compare.
sdcard_prunes_dot_dirs_forgex() {
    grep -Eq "dirs\[:\] *= *\[d for d in dirs if not d\[0\] *== *['\"]\.['\"]" "$1"
}

# Capability: whether the running Klipper hides dot-directories from
# SDCARD_PRINT_FILE. There a print started through the plugin's symlink under
# a hidden directory fails with "Unable to open file", so the symlink directory
# has to be a visible one.
klipper_hides_dot_dirs() {
    sdcard=$(running_klipper_sdcard_source)
    [ -n "$sdcard" ] || return 1
    for probe in sdcard_prunes_dot_dirs_generic sdcard_prunes_dot_dirs_forgex; do
        "$probe" "$sdcard" && return 0
    done
    return 1
}

# Give [helix_print] in @1 a visible `symlink_dir: helix_print`, unless the
# section already sets symlink_dir (the user's value stands).
ensure_visible_symlink_dir() {
    file="$1"
    if awk '/^\[helix_print\]/ { s = 1; next } /^\[/ { s = 0 }
            s && /^[[:space:]]*symlink_dir[[:space:]]*[:=]/ { found = 1 }
            END { exit !found }' "$file"; then
        info "Keeping the symlink_dir already set in $(basename "$file")"
        return 0
    fi
    info "Klipper skips hidden directories here: setting symlink_dir: helix_print"
    helix_rewrite_conf "$file" \
        '{ print } !done && /^\[helix_print\]/ { print "symlink_dir: helix_print"; done = 1 }'
}

# Set `enabled: True` in place of any `enabled` option in @1's [helix_print]
# section that reads as false to Moonraker (false, no, off or 0, any case),
# with a backup. A file with no such line is left untouched. Exits through
# error() when the file needs the edit and cannot take it.
enable_helix_section() {
    file="$1"
    # shellcheck disable=SC2016  # awk's own $ fields
    _eh_disabled='/^\[/ { s = ($0 ~ /^\[helix_print\]/) }
        s && tolower($0) ~ /^[ \t]*enabled[ \t]*[:=][ \t]*(false|no|off|0)[ \t\r]*$/'
    awk "$_eh_disabled { found = 1 } END { exit !found }" "$file" || return 0
    backup_file=$(backup_config "$file") || error "Could not back up $file"
    info "Created backup: $backup_file"
    info "Turning the plugin back on: enabled: True in $(basename "$file")"
    helix_rewrite_conf "$file" "$_eh_disabled { print \"enabled: True\"; next } { print }" ||
        error "Could not write $file - see $backup_file"
}

# Copy config file @1 to @1.bak.helix.<timestamp>, keeping only that newest of
# the backups this script makes of it, and print the backup's path. Backups
# anyone else made are never touched. Returns 1 when the copy failed; older
# backups are then left alone.
backup_config() {
    _bk_new="$1.bak.helix.$(date +%Y%m%d_%H%M%S)"
    cp "$1" "$_bk_new" || return 1
    for _bk_old in "$1".bak.helix.[0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]_[0-9][0-9][0-9][0-9][0-9][0-9]; do
        if [ -f "$_bk_old" ] && [ "$_bk_old" != "$_bk_new" ]; then
            rm -f "$_bk_old"
        fi
    done
    printf '%s' "$_bk_new"
}

# Remove the sections whose header matches extended regex @2 from config file
# @1 with awk program @3 (helix_strip_awk), with a backup. Returns 1 when the
# file had such a section and could not be edited.
remove_helix_section() {
    file="$1"
    [ -f "$file" ] && grep -Eq "$2" "$file" || return 0
    if ! backup_file=$(backup_config "$file"); then
        warn "Could not back up $(basename "$file") - leaving its HelixPrint sections in place"
        return 1
    fi
    info "Created backup: $backup_file"
    info "Removing HelixPrint sections from $(basename "$file")"
    if ! helix_rewrite_conf "$file" "$3"; then
        warn "Could not remove HelixPrint sections from $(basename "$file") - see $backup_file"
        return 1
    fi
    return 0
}

# Main installation
main() {
    info "HelixPrint Moonraker Plugin Installer"
    printf '\n'

    # Check plugin file exists
    if [ ! -f "$PLUGIN_FILE" ]; then
        error "Plugin file not found: $PLUGIN_FILE"
    fi

    # Find Moonraker
    components_dir=$(find_moonraker_components "$1")

    if [ -z "$components_dir" ]; then
        error "Could not find Moonraker installation.
Please provide the path: ./install.sh /path/to/moonraker"
    fi

    info "Components directory: $components_dir"
    printf '\n'

    # Create symlink
    target="$components_dir/helix_print.py"

    if [ -f "$target" ] && [ ! -L "$target" ]; then
        error "A file (not symlink) exists at $target
Please remove it manually before installing."
    fi

    # Use ln -sf for atomic replacement (removes existing symlink first)
    ln -sf "$PLUGIN_FILE" "$target"
    info "Created symlink: $target -> $PLUGIN_FILE"
    printf '\n'

    # Remind about configuration
    info "Installation complete!"
    printf '\n'
    printf '%s\n' "Next steps:"
    printf '%s\n' "  1. Add the following to your moonraker.conf:"
    printf '\n'
    printf '%s\n' "     [helix_print]"
    printf '%s\n' "     # enabled: True"
    printf '%s\n' "     # temp_dir: .helix_temp"
    printf '%s\n' "     # symlink_dir: .helix_print"
    printf '%s\n' "     # cleanup_delay: 86400"
    printf '\n'
    printf '%s\n' "  2. Restart Moonraker:"
    printf '%s\n' "     sudo systemctl restart moonraker"
    printf '\n'
    printf '%s\n' "  3. Verify the plugin is loaded:"
    config_dir=$(find_config_dir)
    printf '%s\n' "     curl $(moonraker_local_url "$config_dir/moonraker.conf" "$(running_moonraker_root)")/server/helix/status"
    printf '\n'
}

# Uninstall function (interactive)
uninstall() {
    info "Uninstalling HelixPrint plugin..."
    exit_status=$EXIT_UNINSTALL_OK

    components_dir=$(find_moonraker_components "$1")

    if [ -z "$components_dir" ]; then
        error "Could not find Moonraker installation."
    fi

    target="$components_dir/helix_print.py"

    if [ -L "$target" ]; then
        rm "$target"
        info "Removed symlink: $target"
    elif [ -f "$target" ]; then
        warn "Found regular file (not symlink) at $target"
        warn "Please remove it manually if desired."
    else
        info "Plugin symlink not found (already uninstalled?)"
    fi

    config_dir=$(find_config_dir)
    if [ -n "$config_dir" ]; then
        # The plugin itself is already gone at this point, so any nonzero
        # here - a skipped file or a failed one - is "needs attention", not
        # a failed uninstall.
        strip_phase_tracking_instrumentation "$config_dir" || exit_status=$EXIT_UNINSTALL_NEEDS_ATTENTION
    fi

    printf '\n'
    printf '%s\n' "Don't forget to:"
    printf '%s\n' "  1. Remove [helix_print] section from moonraker.conf"
    printf '%s\n' "  2. Restart Moonraker: sudo systemctl restart moonraker"

    return "$exit_status"
}

# The same probe as _has_real_curl in scripts/lib/installer/release.sh.
# Detect whether `curl` is curl proper, with the flags we pass it. Creality K1
# firmware ships libhv's example client as /usr/bin/curl: it answers to the name,
# prints "curl version 1.0.0" and rejects -s, -S, -L, -o and -f. Real curl's first
# --version output starts "curl <digit>...". Shell builtins only, so it holds on
# a PATH with no head or grep. Cache the result so we only probe once.
_REAL_CURL=""
_has_real_curl() {
    if [ -z "$_REAL_CURL" ]; then
        _REAL_CURL=no
        if command -v curl >/dev/null 2>&1; then
            case "$(curl --version 2>/dev/null)" in
                "curl "[0-9]*) _REAL_CURL=yes ;;
            esac
        fi
    fi
    [ "$_REAL_CURL" = "yes" ]
}

# Whether a GET of the URL succeeds: real curl, else wget. BusyBox systems
# (Forge-X outside its chroot) carry wget but no curl, and a K1's /usr/bin/curl
# is an impostor that rejects -s and -f.
moonraker_answers() {
    if _has_real_curl && curl -sf --max-time 2 "$1" > /dev/null 2>&1; then
        return 0
    fi
    command -v wget > /dev/null 2>&1 && wget -q -T 2 -O /dev/null "$1" > /dev/null 2>&1
}

# Wait for Moonraker to become available after restart, at MOONRAKER_URL.
wait_for_moonraker() {
    max_attempts=30
    attempt=0
    moonraker_url="${MOONRAKER_URL:-http://127.0.0.1:7125}"

    info "Waiting for Moonraker to become available..."

    while [ "$attempt" -lt "$max_attempts" ]; do
        if moonraker_answers "$moonraker_url/server/info"; then
            info "Moonraker is ready!"
            return 0
        fi
        attempt=$((attempt + 1))
        sleep 1
    done

    warn "Moonraker did not respond within ${max_attempts} seconds"
    return 1
}

# Run a command as root: directly when we are root, else through sudo.
as_root() {
    if [ "$(id -u 2>/dev/null)" = 0 ]; then
        "$@"
    else
        sudo "$@"
    fi
}

# Restart Moonraker: through its service manager, else by asking Moonraker
# (at MOONRAKER_URL) to restart itself, which needs no root and works where
# sudo wants a password. Returns 1 when neither restarted it.
restart_moonraker() {
    info "Restarting Moonraker..."
    if command -v systemctl > /dev/null 2>&1 && systemctl list-units --type=service 2>/dev/null | grep -q moonraker; then
        as_root systemctl restart moonraker && return 0
        warn "Could not restart Moonraker via systemctl"
    elif command -v service > /dev/null 2>&1; then
        as_root service moonraker restart && return 0
        warn "Could not restart Moonraker via service"
    fi
    if request_moonraker_restart; then
        info "Asked Moonraker to restart itself"
        return 0
    fi
    return 1
}

# The restart an auto install or uninstall ends with, then the wait for
# Moonraker to answer again. A caller under systemd's NoNewPrivileges (the
# HelixScreen service) cannot sudo, so it passes --no-restart and restarts
# Moonraker itself. Returns 1 when Moonraker still needs a restart, which the
# caller reports instead of completion; a slow comeback is only a warning.
finish_with_restart() {
    if [ "$NO_RESTART" = "true" ]; then
        info "Leaving the Moonraker restart to the caller"
        return 0
    fi
    if ! restart_moonraker; then
        warn "Could not restart Moonraker, so the change has not taken effect"
        warn "Moonraker needs a restart: run 'sudo systemctl restart moonraker', or restart it from Mainsail or Fluidd"
        return 1
    fi
    wait_for_moonraker || warn "Moonraker did not come back up in time, but $1 itself completed"
    return 0
}

# POST /server/restart: Moonraker re-launches itself in place, the one restart
# that works where no service manager reaches it (Forge-X's chroot). Real curl
# first, then wget, which BusyBox carries where curl is absent or an impostor;
# -sf and wget's own status make an HTTP error (a 401) a failure.
# Kept identical in scripts/lib/installer/moonraker.sh: this script runs
# standalone (piped from GitHub, or from an install tree without scripts/lib)
# and cannot source it; a bats test holds the two copies equal.
# Returns 0 when Moonraker accepted the request.
request_moonraker_restart() {
    _restart_url="${MOONRAKER_URL:-http://127.0.0.1:7125}/server/restart"
    if _has_real_curl && curl -sf --max-time 5 -X POST "$_restart_url" >/dev/null 2>&1; then
        return 0
    fi
    command -v wget >/dev/null 2>&1 &&
        wget -q -T 5 -O /dev/null --post-data='' "$_restart_url" >/dev/null 2>&1
}

# Strip phase-tracking instrumentation from PRINT_START via the bundled
# strip_phase_tracking.py, which owns the marker format and the safety
# contract around editing a printer's config (matched blocks only, a
# verified backup, symlinks and modes preserved). python3 is what Moonraker
# itself runs on; its absence here means something else on this printer is
# already broken. Either way this never aborts the rest of uninstall - it
# reports the outcome and returns it, so the caller can fold it into its own
# exit status instead of claiming success it cannot back up.
#
# Returns strip_phase_tracking.py's own exit status unchanged (0 clean, 2
# needs attention, 1 failed) so a caller can wire it straight through; a
# missing python3 is reported as EXIT_UNINSTALL_NEEDS_ATTENTION since
# PRINT_START could not be checked at all.
strip_phase_tracking_instrumentation() {
    scan_dir="$1"

    if ! command -v python3 > /dev/null 2>&1; then
        warn "python3 not found - could not check PRINT_START for phase-tracking instrumentation"
        warn "If a previous HelixScreen instrumented PRINT_START, remove any block between"
        warn "'# <<< HELIX_TRACKING v2 >>>' and '# <<< /HELIX_TRACKING >>>' by hand, then restart Klipper"
        return "$EXIT_UNINSTALL_NEEDS_ATTENTION"
    fi

    strip_status=$EXIT_UNINSTALL_OK
    python3 "$SCRIPT_DIR/strip_phase_tracking.py" "$scan_dir" || strip_status=$?
    if [ "$strip_status" -ne 0 ]; then
        warn "Phase-tracking strip reported a failure - see the output above"
    fi
    return "$strip_status"
}

# Auto-uninstall function (non-interactive, for HelixScreen integration)
auto_uninstall() {
    info "HelixPrint Auto-Uninstall Mode"
    printf '\n'
    exit_status=$EXIT_UNINSTALL_OK

    # Find Moonraker (auto-detect only)
    components_dir=$(find_moonraker_components "")

    if [ -z "$components_dir" ]; then
        error "Could not auto-detect Moonraker installation."
    fi

    target="$components_dir/helix_print.py"

    # Find config directory
    config_dir=$(find_config_dir)

    # Remove symlink
    if [ -L "$target" ]; then
        rm "$target"
        info "Removed symlink: $target"
    elif [ -f "$target" ]; then
        error "Found regular file (not symlink) at $target - manual removal required"
    else
        info "Plugin symlink not found (already uninstalled?)"
    fi

    # helix_macros.cfg and its printer.cfg include are left in place: the
    # macros are shared HelixScreen helpers (HELIX_START_PRINT,
    # HELIX_CLEAN_NOZZLE and friends) that keep working without this plugin.
    # Any HELIX_PHASE_*/HELIX_READY calls this plugin's own instrumentation
    # left in PRINT_START are removed, with a backup, by the call below. The
    # plugin itself is already gone at this point, so any nonzero here - a
    # skipped file or a failed one - is "needs attention", not a failed
    # uninstall.
    if [ -n "$config_dir" ]; then
        strip_phase_tracking_instrumentation "$config_dir" || exit_status=$EXIT_UNINSTALL_NEEDS_ATTENTION
    fi

    # Remove the plugin's sections from every file Moonraker reads them from:
    # moonraker.conf and anything it includes, such as the user file a
    # mod-owned moonraker.conf includes. The plugin itself is already gone at
    # this point, so a failure backing up or editing a config here is "needs
    # attention", not a failed uninstall, and must not trip set -e and skip
    # the rest of this function.
    if [ -n "$config_dir" ] && [ -f "$config_dir/moonraker.conf" ]; then
        for file in $(helix_print_owned_files "$config_dir/moonraker.conf" "$(running_moonraker_root)"); do
            remove_helix_section "$file" "$HELIX_PRINT_OWNED_RE" "$HELIX_PRINT_STRIP_AWK" ||
                exit_status=$EXIT_UNINSTALL_NEEDS_ATTENTION
        done
    fi

    # The plugin is already gone, so a restart that never happened is
    # reported, not turned into a failed uninstall.
    MOONRAKER_URL=$(moonraker_local_url "$config_dir/moonraker.conf" "$(running_moonraker_root)")
    if ! finish_with_restart "the plugin removal"; then
        printf '\n'
        warn "Plugin removed, but Moonraker keeps loading it until it restarts"
        [ "$exit_status" -eq "$EXIT_UNINSTALL_OK" ] || warn "PRINT_START needs a look too - see the warnings above"
        return "$exit_status"
    fi

    printf '\n'
    if [ "$exit_status" -eq "$EXIT_UNINSTALL_OK" ]; then
        info "Auto-uninstall complete!"
    else
        warn "Auto-uninstall complete, but PRINT_START needs a look - see the warnings above"
    fi

    return "$exit_status"
}

# Re-point an installed plugin's link at this tree (--relink): what the app
# runs at startup, so a plugin installed from an install directory that has
# since moved, or been replaced, loads from the tree beside the running app.
# Touches the link only, never the config, and never creates a link that is
# not there: an install is the user's choice. Returns an EXIT_RELINK_* code.
relink() {
    components_dir=$(find_moonraker_components "")
    [ -n "$components_dir" ] || error "Could not auto-detect Moonraker installation."
    target="$components_dir/helix_print.py"

    if [ ! -L "$target" ]; then
        info "No plugin link at $target - nothing to repair"
        return "$EXIT_RELINK_NOT_OURS"
    fi
    current=$(readlink "$target")
    case "$current" in
        */helix_print.py) ;;
        *)
            info "$target links to $current, not a HelixPrint plugin - leaving it"
            return "$EXIT_RELINK_NOT_OURS"
            ;;
    esac
    config_dir=$(find_config_dir)
    if [ -z "$config_dir" ] ||
        [ -z "$(helix_print_section_files "$config_dir/moonraker.conf" "$(running_moonraker_root)")" ]; then
        info "No [helix_print] section configured - leaving $target alone"
        return "$EXIT_RELINK_NOT_OURS"
    fi

    # The link text is Moonraker's path, which differs from ours in a chroot.
    # Compared as files, not paths: through a bind mount (Forge-X binds
    # /opt/config) one file has two paths that resolve apart. -ef is in dash,
    # BusyBox ash and bash, every shell this runs under.
    mroot=$(running_moonraker_root)
    case "$current" in
        /*) seen="$mroot$current" ;;
        *) seen="$components_dir/$current" ;;
    esac
    # shellcheck disable=SC3013
    if [ -e "$seen" ] && [ "$seen" -ef "$PLUGIN_FILE" ]; then
        info "The plugin link already names $PLUGIN_FILE"
        return "$EXIT_RELINK_CURRENT"
    fi

    link_to=$(path_for_moonraker "$PLUGIN_FILE")
    [ -f "$mroot$link_to" ] || error "Moonraker cannot see $PLUGIN_FILE from its root ($mroot)"
    can_link_in "$components_dir" || error "Cannot write to components directory: $components_dir"
    ln -sf "$link_to" "$target" || error "Could not re-point $target"
    info "Re-pointed $target -> $link_to (was $current)"
    return "$EXIT_RELINK_DONE"
}

# Auto-install function (non-interactive, for HelixScreen integration)
auto_install() {
    info "HelixPrint Auto-Install Mode"
    printf '\n'

    AUTO_MODE=true

    # Check plugin file exists
    if [ ! -f "$PLUGIN_FILE" ]; then
        error "Plugin file not found: $PLUGIN_FILE"
    fi

    # Find Moonraker (auto-detect only)
    components_dir=$(find_moonraker_components "")

    if [ -z "$components_dir" ]; then
        error "Could not auto-detect Moonraker installation."
    fi

    # Find config directory
    config_dir=$(find_config_dir)

    info "Moonraker components: $components_dir"
    info "Config: ${config_dir:-not found}"
    printf '\n'

    # Pre-flight checks, all before the first write: the link can be made
    # and the config can be edited.
    if ! can_link_in "$components_dir"; then
        error "Cannot write to components directory: $components_dir"
    fi

    section_file=""
    if [ -n "$config_dir" ] && [ -f "$config_dir/moonraker.conf" ]; then
        section_file=$(helix_conf_file "$config_dir")
        if [ -e "$section_file" ] && [ ! -w "$section_file" ]; then
            error "Cannot write to $section_file"
        fi
    fi

    # Create symlink
    target="$components_dir/helix_print.py"

    if [ -f "$target" ] && [ ! -L "$target" ]; then
        error "A file (not symlink) exists at $target"
    fi

    # The link is read by Moonraker, so it names the plugin as Moonraker
    # sees it, which differs from here when Moonraker runs in a chroot.
    link_to=$(path_for_moonraker "$PLUGIN_FILE")
    if [ ! -f "$(running_moonraker_root)$link_to" ]; then
        error "Moonraker cannot see $PLUGIN_FILE from its root ($(running_moonraker_root))"
    fi

    # Auto-configure moonraker.conf if possible
    if [ -n "$section_file" ]; then
        # A section in any file Moonraker reads counts, so the install never
        # writes a second one beside it.
        section_holder=""
        for file in $(helix_print_section_files "$config_dir/moonraker.conf" "$(running_moonraker_root)"); do
            [ -n "$section_holder" ] || section_holder="$file"
            # An install ends with the plugin working, so a section that
            # switches it off is switched back on.
            enable_helix_section "$file"
        done
        if [ -n "$section_holder" ]; then
            info "Config section [helix_print] already exists in $section_holder"
        else
            # Create backup before modifying config
            if [ -f "$section_file" ]; then
                backup_file=$(backup_config "$section_file")
                info "Created backup: $backup_file"
            fi

            info "Adding [helix_print] section to $section_file"
            # End an unterminated last line first. The blank line after it is
            # the one HELIX_PRINT_STRIP_AWK takes back out on uninstall.
            if [ -s "$section_file" ] && [ -n "$(tail -c 1 "$section_file")" ]; then
                printf '\n' >> "$section_file"
            fi
            printf '\n' >> "$section_file"
            printf '%s\n' "[helix_print]" >> "$section_file"
            printf '%s\n' "# HelixScreen plugin - auto-configured" >> "$section_file"
            section_holder="$section_file"
        fi

        if klipper_hides_dot_dirs; then
            ensure_visible_symlink_dir "$section_holder"
        fi

        # An updater entry for a git checkout of the plugin follows a tree
        # Moonraker no longer loads once the link points here.
        for file in $(helix_conf_files_matching "$config_dir/moonraker.conf" "$(running_moonraker_root)" "$HELIX_PRINT_UPDATER_RE"); do
            remove_helix_section "$file" "$HELIX_PRINT_UPDATER_RE" "$HELIX_PRINT_UPDATER_STRIP_AWK" ||
                error "Could not remove [update_manager helix_print] from $file"
        done
    else
        warn "Could not find moonraker.conf - manual config required"
    fi

    # The link goes last: an install that stops on a config write before it
    # leaves the previous link naming the tree it named, which a caller that
    # put that tree back (remote-install.sh) relies on. ln -sf replaces an
    # existing symlink atomically.
    ln -sf "$link_to" "$target"
    info "Created symlink: $target -> $link_to"

    # The install itself is done by now, so a restart that never happened is
    # reported, never a failure that would roll it back.
    MOONRAKER_URL=$(moonraker_local_url "$config_dir/moonraker.conf" "$(running_moonraker_root)")
    if ! finish_with_restart "the plugin install"; then
        printf '\n'
        warn "Plugin installed, but it does not load until Moonraker restarts"
        return 0
    fi

    printf '\n'
    info "Auto-install complete!"
}

show_help() {
    printf '%s\n' "Usage: $0 [OPTIONS] [MOONRAKER_PATH]"
    printf '\n'
    printf '%s\n' "Options:"
    printf '%s\n' "  --auto, -a              Full auto-install (updates config, restarts Moonraker)"
    printf '%s\n' "  --no-restart            Before --auto/--uninstall-auto: skip the Moonraker restart;"
    printf '%s\n' "                          the caller restarts it (e.g. via Moonraker's server.restart)"
    printf '%s\n' "  --uninstall, -u         Remove the plugin symlink and strip PRINT_START"
    printf '%s\n' "                          instrumentation, if any (interactive)"
    printf '%s\n' "  --uninstall-auto        Full auto-uninstall: removes the symlink and config"
    printf '%s\n' "                          section, strips PRINT_START instrumentation, restarts"
    printf '%s\n' "                          Moonraker"
    printf '%s\n' "  --relink                Re-point an installed plugin's link at this copy;"
    printf '%s\n' "                          exit 0 already current, 3 re-pointed, 4 nothing of"
    printf '%s\n' "                          ours to repair, 1 failed. Never restarts Moonraker"
    printf '%s\n' "  --help, -h              Show this help message"
    printf '\n'
    printf '%s\n' "Exit codes for --uninstall and --uninstall-auto:"
    printf '%s\n' "  0   uninstalled; nothing left in the config to clean up"
    printf '%s\n' "  2   the plugin was removed, but a config file was skipped or failed during"
    printf '%s\n' "      the phase-tracking strip and needs a human to look at it"
    printf '%s\n' "  1   (or any other nonzero) the uninstall itself failed"
    printf '\n'
    printf '%s\n' "Arguments:"
    printf '%s\n' "  MOONRAKER_PATH     Path to Moonraker installation (auto-detected if not provided)"
    printf '\n'
    printf '%s\n' "Environment Variables:"
    printf '%s\n' "  MOONRAKER_URL      URL for Moonraker's restart request and health check"
    printf '%s\n' "                     Default: http://127.0.0.1:<[server] port in moonraker.conf, else 7125>"
    printf '%s\n' "                     Example: MOONRAKER_URL=http://192.168.1.100:7125 ./install.sh --auto"
    printf '\n'
    printf '%s\n' "Examples:"
    printf '%s\n' "  ./install.sh --auto      # Auto-install (updates moonraker.conf, restarts Moonraker)"
}

# Parse arguments
NO_RESTART=false
while [ $# -gt 0 ]; do
    case "$1" in
        --no-restart)
            NO_RESTART=true
            shift
            ;;
        --auto|-a)
            AUTO_MODE=true
            shift
            ;;
        --uninstall|-u)
            uninstall "$2"
            exit $?
            ;;
        --uninstall-auto)
            auto_uninstall
            exit $?
            ;;
        --relink)
            status=0
            relink || status=$?
            exit "$status"
            ;;
        --help|-h)
            show_help
            exit 0
            ;;
        *)
            # Assume it's a moonraker path for interactive mode
            if [ "$AUTO_MODE" = "true" ]; then
                auto_install
            else
                main "$1"
            fi
            exit 0
            ;;
    esac
done

# No arguments left - run in appropriate mode
if [ "$AUTO_MODE" = "true" ]; then
    auto_install
else
    main ""
fi
