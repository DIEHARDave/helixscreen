#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Put the user's config back after Moonraker's type:web update replaced the
# install dir, on every platform.
#
# Moonraker rmtree()s the install dir and extracts a fresh zip. That removes the
# symlinks from config/ into printer_data/config/helixscreen/ and platform/hooks.sh.
# Two callers run this afterwards:
#   - refresh-service-units.sh (systemd), from the update watcher and service start
#   - helixscreen.init start() (SysV, and the K2 procd shim that delegates to it)
# It is idempotent, cheap when nothing changed, and never fails its caller.
#
# Reads USER_VAL (the service user, optional) from the environment. The install
# dir is the parent of this script's directory.
# HELIX_RESTORE_LIB_ONLY=1 defines the functions without running them.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
IDIR="${IDIR:-$(dirname "$SCRIPT_DIR")}"
USER_VAL="${USER_VAL:-}"

# --- Restore config symlinks after Moonraker update ---
#
# Moonraker type:web does shutil.rmtree(INSTALL_DIR) then extracts a fresh ZIP.
# This destroys the symlinks from INSTALL_DIR/config/ → printer_data/config/helixscreen/
# and replaces them with default config files from the release ZIP.  The user's real
# config survives in printer_data (outside the managed path) but is orphaned.
#
# Re-create symlinks so the app reads the user's config instead of defaults.
# Runs as root (inherited from the calling service unit), so no sudo needed.

# Keep in sync with scripts/lib/installer/platform.sh.
HELIX_USER_CONFIG_FILES="settings.json helixscreen.env .disabled_services tool_spools.json crash_history.json user_filaments.json .webcams_backup.json .camera_migrated .klipper_includes .k2cam_webcam_disabled .seeded_settings .platform_hook"
HELIX_USER_CONFIG_DIRS="custom_images themes printer_database.d"

# Discover printer_data/config/helixscreen/ from the service user's home.
# Try the service user's home first, then scan common locations.
# Echoes the path, or nothing when none is found.
discover_pd_helix() {
    local user_home=""

    # An explicit location wins (also how the tests point it at a sandbox).
    if [ -n "${HELIX_PD_HELIX_DIR:-}" ] && [ -d "$HELIX_PD_HELIX_DIR" ]; then
        printf '%s' "$HELIX_PD_HELIX_DIR"
        return 0
    fi

    if [ -n "$USER_VAL" ] && [ "$USER_VAL" != "root" ]; then
        user_home=$(eval echo "~${USER_VAL}" 2>/dev/null || echo "")
    fi

    # Check user home first, then common Klipper user homes, then /root
    for candidate in \
        "${user_home}/printer_data/config/helixscreen" \
        /home/pi/printer_data/config/helixscreen \
        /home/biqu/printer_data/config/helixscreen \
        /home/mks/printer_data/config/helixscreen \
        /root/printer_data/config/helixscreen \
        /usr/data/printer_data/config/helixscreen \
        /usr/data/config/printer_data/config/helixscreen \
        /opt/config/printer_data/config/helixscreen \
        /mnt/UDISK/printer_data/config/helixscreen \
        /etc/klipper/config/helixscreen; do
        [ -z "$candidate" ] && continue
        if [ -d "$candidate" ]; then
            printf '%s' "$candidate"
            return 0
        fi
    done
    return 0
}

# Copy every entry under SRC missing from DST, never overwriting. Sorted so a
# parent directory is created before its children.
# Args: SRC DST — returns 0 when every entry now exists at DST.
merge_config_dir() {
    local src="$1" dst="$2" rel

    [ -d "$src" ] || return 0

    while IFS= read -r rel; do
        [ -n "$rel" ] && [ "$rel" != "." ] || continue
        rel="${rel#./}"
        if [ -d "${src}/${rel}" ] && [ ! -L "${src}/${rel}" ]; then
            [ -d "${dst}/${rel}" ] || mkdir -p "${dst}/${rel}" 2>/dev/null || return 1
        elif [ ! -e "${dst}/${rel}" ] && [ ! -L "${dst}/${rel}" ]; then
            cp -pR "${src}/${rel}" "${dst}/${rel}" 2>/dev/null || return 1
        fi
    done <<EOF
$(cd "$src" 2>/dev/null && find . 2>/dev/null | LC_ALL=C sort)
EOF
    return 0
}

# True when DIR holds at least one entry (including dotfiles). Cross-checks the
# find(1) walk: a walk reporting nothing about a demonstrably non-empty
# directory means the walk failed, and a failed walk must never be read as
# "everything copied".
dir_has_entries() {
    local d="$1" e
    [ -d "$d" ] || return 1
    for e in "$d"/* "$d"/.[!.]* "$d"/..?*; do
        [ -e "$e" ] || [ -L "$e" ] || continue
        return 0
    done
    return 1
}

# True when every entry under SRC has a counterpart under DST. The precondition
# for deleting SRC, checked independently of merge_config_dir's return code.
config_dir_fully_merged() {
    local src="$1" dst="$2" rel seen=""

    [ -d "$dst" ] || return 1

    while IFS= read -r rel; do
        [ -n "$rel" ] && [ "$rel" != "." ] || continue
        seen=1
        rel="${rel#./}"
        [ -e "${dst}/${rel}" ] || [ -L "${dst}/${rel}" ] || return 1
    done <<EOF
$(cd "$src" 2>/dev/null && find . 2>/dev/null)
EOF

    if [ -z "$seen" ] && dir_has_entries "$src"; then
        return 1
    fi
    return 0
}

# Last line of defence on the only rm -rf this script performs. IDIR is derived
# from the script's own location, so a script copied somewhere unexpected must
# not be able to point the delete at arbitrary user data.
# Args: INSTALL_CONFIG
install_config_path_ok() {
    local c="${1%/}"
    case "$c" in
        /*) ;;
        *) return 1 ;;
    esac
    case "$c" in
        *..*) return 1 ;;
    esac
    [ "${c##*/}" = "config" ] || return 1
    # Parent must be a HelixScreen install dir — same rule as validate_install_dir.
    case "${c%/config}" in
        *helixscreen*) return 0 ;;
    esac
    return 1
}

restore_config_symlinks() {
    local install_config="${IDIR}/config"
    [ -d "$install_config" ] || return 0

    local pd_helix
    pd_helix=$(discover_pd_helix)
    [ -n "$pd_helix" ] || return 0

    for file in $HELIX_USER_CONFIG_FILES; do
        local pd_file="${pd_helix}/${file}"
        local install_file="${install_config}/${file}"

        # Already a correct symlink — nothing to do
        if [ -L "$install_file" ]; then
            local target
            target=$(readlink "$install_file" 2>/dev/null || echo "")
            [ "$target" = "$pd_file" ] && continue
            rm -f "$install_file"
        fi

        # A dotfile is runtime state no release zip ships. A real one here with
        # nothing in printer_data is a ledger written before it was relocated:
        # move it across so the next rmtree cannot take it.
        case "$file" in
            .*)
                if [ ! -e "$pd_file" ] && [ -f "$install_file" ] && [ ! -L "$install_file" ]; then
                    cp -p "$install_file" "$pd_file" 2>/dev/null || continue
                fi
                ;;
        esac

        # User's config exists in printer_data — replace the default with a symlink
        if [ -f "$pd_file" ]; then
            # Remove the default file shipped in the ZIP
            rm -f "$install_file" 2>/dev/null
            ln -s "$pd_file" "$install_file" 2>/dev/null || true
        fi
    done

    # --- Directories ---
    #
    # The ZIP always extracts these as real directories holding whatever the
    # release ships (themes/, printer_database.d/README.md). Fold anything the
    # user doesn't already have into printer_data — so a newly shipped theme
    # still reaches them — then replace the real directory with a symlink so the
    # NEXT rmtree unlinks a symlink instead of deleting their data.
    #
    # Copy, verify, then delete. Any failure leaves the real directory in place.
    for dir in $HELIX_USER_CONFIG_DIRS; do
        local pd_dir="${pd_helix}/${dir}"
        local install_dir_path="${install_config}/${dir}"

        install_config_path_ok "$install_config" || continue

        if [ -L "$install_dir_path" ]; then
            local dir_target
            dir_target=$(readlink "$install_dir_path" 2>/dev/null || echo "")
            if [ "$dir_target" = "$pd_dir" ]; then
                [ -d "$pd_dir" ] || mkdir -p "$pd_dir" 2>/dev/null || true
                continue
            fi
            rm -f "$install_dir_path" 2>/dev/null
        fi

        [ -d "$pd_dir" ] || mkdir -p "$pd_dir" 2>/dev/null || continue

        if [ -d "$install_dir_path" ] && [ ! -L "$install_dir_path" ]; then
            merge_config_dir "$install_dir_path" "$pd_dir" || continue
            config_dir_fully_merged "$install_dir_path" "$pd_dir" || continue
            rm -rf "$install_dir_path" 2>/dev/null || continue
        elif [ -e "$install_dir_path" ]; then
            continue   # a plain file where we expect a directory — not ours to delete
        fi

        ln -s "$pd_dir" "$install_dir_path" 2>/dev/null || true
    done
}

# Moonraker's rmtree also deletes platform/hooks.sh, which only the installer
# writes. Without it every platform_* function is the init script's no-op stub
# (the stock UI is no longer stopped, the cache dir falls back to tmpfs). The
# installer records which hook key it deployed in config/.platform_hook, a user
# config file restored above; the matching template ships in the release.
restore_platform_hooks() {
    local hooks="${IDIR}/platform/hooks.sh" keyfile="${IDIR}/config/.platform_hook"
    local key src owner

    [ -s "$hooks" ] && return 0
    [ -f "$keyfile" ] || return 0
    key=$(head -n1 "$keyfile" 2>/dev/null | tr -d ' \r\n')
    case "$key" in
        ""|*[!A-Za-z0-9_-]*) return 0 ;;
    esac
    src="${IDIR}/assets/config/platform/hooks-${key}.sh"
    [ -f "$src" ] || return 0

    mkdir -p "${IDIR}/platform" 2>/dev/null || return 0
    cp "$src" "$hooks" 2>/dev/null || return 0
    chmod +x "$hooks" 2>/dev/null || true
    owner=$(stat -c %u:%g "$IDIR" 2>/dev/null || true)
    if [ -n "$owner" ]; then
        chown "$owner" "${IDIR}/platform" "$hooks" 2>/dev/null || true
    fi
}

if [ -z "${HELIX_RESTORE_LIB_ONLY:-}" ]; then
    restore_config_symlinks || true
    restore_platform_hooks || true
    exit 0
fi
