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
# Remote install (from GitHub):
#   curl -sSL https://raw.githubusercontent.com/prestonbrown/helixscreen/main/moonraker-plugin/remote-install.sh | sh

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
        inc=$(sed -n 's/^\[include[[:space:]]\{1,\}\([^]*?]*user\.moonraker\.conf\)[[:space:]]*\]$/\1/p' "$conf" 2>/dev/null | tail -n 1)
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

# Remove the [helix_print] section from one config file, with a backup.
# Returns 1 when the file had the section and could not be edited.
remove_helix_section() {
    file="$1"
    [ -f "$file" ] && grep -q '^\[helix_print\]' "$file" || return 0
    backup_file="${file}.bak.$(date +%Y%m%d_%H%M%S)"
    if ! cp "$file" "$backup_file"; then
        warn "Could not back up $(basename "$file") - leaving the [helix_print] section in place"
        return 1
    fi
    info "Created backup: $backup_file"
    info "Removing [helix_print] section from $(basename "$file")"
    # Use awk for cross-platform config section removal
    # This correctly handles helix_print as the last section in the file
    if ! (awk '
        /^\[helix_print\]/ { skip = 1; next }
        /^\[/ { skip = 0 }
        !skip { print }
    ' "$file" > "$file.tmp" && cat "$file.tmp" > "$file" && rm -f "$file.tmp"); then
        warn "Could not remove [helix_print] from $(basename "$file") - see $backup_file"
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
    printf '%s\n' "     curl http://localhost:7125/server/helix/status"
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

# Whether a GET of the URL succeeds. BusyBox systems (Forge-X outside its
# chroot) carry wget but no curl.
moonraker_answers() {
    if command -v curl > /dev/null 2>&1; then
        curl -s --max-time 2 "$1" > /dev/null 2>&1
    elif command -v wget > /dev/null 2>&1; then
        wget -q -T 2 -O /dev/null "$1" > /dev/null 2>&1
    else
        return 1
    fi
}

# Wait for Moonraker to become available after restart
wait_for_moonraker() {
    [ "$NO_RESTART" = "true" ] && return 0
    max_attempts=30
    attempt=0
    moonraker_url="${MOONRAKER_URL:-http://localhost:7125}"

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

# Restart Moonraker service
restart_moonraker() {
    # A caller under systemd's NoNewPrivileges (the HelixScreen service) cannot
    # sudo, so it passes --no-restart and asks Moonraker to restart itself.
    if [ "$NO_RESTART" = "true" ]; then
        info "Leaving the Moonraker restart to the caller"
        return 0
    fi
    info "Restarting Moonraker..."
    if command -v systemctl > /dev/null 2>&1 && systemctl list-units --type=service 2>/dev/null | grep -q moonraker; then
        sudo systemctl restart moonraker || warn "Failed to restart Moonraker via systemctl"
    elif command -v service > /dev/null 2>&1; then
        sudo service moonraker restart || warn "Failed to restart Moonraker via service"
    elif request_moonraker_restart; then
        info "Asked Moonraker to restart itself"
    else
        warn "No way to restart Moonraker found - please restart Moonraker manually"
    fi
}

# Ask Moonraker to restart in place (POST /server/restart): the one restart
# that works where it runs under no service manager this script can reach,
# such as Forge-X's chroot.
request_moonraker_restart() {
    url="${MOONRAKER_URL:-http://localhost:7125}/server/restart"
    if command -v curl > /dev/null 2>&1; then
        curl -s --max-time 5 -X POST "$url" > /dev/null 2>&1
    elif command -v wget > /dev/null 2>&1; then
        wget -q -T 5 -O /dev/null --post-data='' "$url" > /dev/null 2>&1
    else
        return 1
    fi
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

    # Remove config section if possible, from moonraker.conf and from the
    # user file a mod-owned moonraker.conf includes. The plugin itself is
    # already gone at this point, so a failure backing up or editing a config
    # here is "needs attention", not a failed uninstall, and must not trip
    # set -e and skip the rest of this function.
    if [ -n "$config_dir" ] && [ -f "$config_dir/moonraker.conf" ]; then
        section_file=$(helix_conf_file "$config_dir")
        for file in "$config_dir/moonraker.conf" "$section_file"; do
            remove_helix_section "$file" || exit_status=$EXIT_UNINSTALL_NEEDS_ATTENTION
            [ "$section_file" = "$config_dir/moonraker.conf" ] && break
        done
    fi

    # Restart Moonraker
    restart_moonraker

    # Wait for Moonraker to come back up. A slow or unreachable restart here
    # must not turn a completed uninstall into a reported failure: the
    # plugin is already gone, and this timeout is about restart timing, not
    # the uninstall's own outcome.
    wait_for_moonraker || warn "Moonraker did not come back up in time, but the plugin removal itself completed"

    printf '\n'
    if [ "$exit_status" -eq "$EXIT_UNINSTALL_OK" ]; then
        info "Auto-uninstall complete!"
    else
        warn "Auto-uninstall complete, but PRINT_START needs a look - see the warnings above"
    fi

    return "$exit_status"
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

    # Pre-flight permission checks
    if [ ! -w "$components_dir" ]; then
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

    # Use ln -sf for atomic replacement (removes existing symlink first)
    ln -sf "$link_to" "$target"
    info "Created symlink: $target -> $link_to"

    # Auto-configure moonraker.conf if possible
    if [ -n "$section_file" ]; then
        # Check if [helix_print] section already exists
        if grep -q '^\[helix_print\]' "$config_dir/moonraker.conf" "$section_file" 2>/dev/null; then
            info "Config section [helix_print] already exists"
        else
            # Create backup before modifying config
            if [ -f "$section_file" ]; then
                backup_file="${section_file}.bak.$(date +%Y%m%d_%H%M%S)"
                cp "$section_file" "$backup_file"
                info "Created backup: $backup_file"
            fi

            info "Adding [helix_print] section to $section_file"
            printf '\n' >> "$section_file"
            printf '%s\n' "[helix_print]" >> "$section_file"
            printf '%s\n' "# HelixScreen plugin - auto-configured" >> "$section_file"
        fi
    else
        warn "Could not find moonraker.conf - manual config required"
    fi

    # Restart Moonraker
    restart_moonraker

    # Wait for Moonraker to come back up
    wait_for_moonraker

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
    printf '%s\n' "  MOONRAKER_URL      URL for Moonraker API health check after restart"
    printf '%s\n' "                     Default: http://localhost:7125"
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
