#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The install-dir ledgers the uninstaller reads (camera migration, Klipper
# includes, settings seeds, K2 camera marker) must live in printer_data and be
# symlinked from the install dir, so Moonraker's type:web rmtree of the install
# dir cannot delete them. A ledger that was never written must stay absent: an
# empty seeded marker would read as "migrated".

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
RESTORE="$WORKTREE_ROOT/config/restore-user-config.sh"
LEDGERS=".webcams_backup.json .camera_migrated .klipper_includes .k2cam_webcam_disabled .seeded_settings"


setup() {
    unset _HELIX_PLATFORM_SOURCED _HELIX_COMMON_SOURCED _HELIX_HOST_PROFILE_SOURCED
    . "$WORKTREE_ROOT/scripts/lib/installer/common.sh"
    . "$WORKTREE_ROOT/scripts/lib/installer/host_profile.sh"
    . "$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
    load helpers

    SUDO=""
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/pi"
    INSTALL_DIR="$BATS_TEST_TMPDIR/helixscreen"
    IDIR="$INSTALL_DIR"
    PD="$KLIPPER_HOME/printer_data/config/helixscreen"
    mkdir -p "$KLIPPER_HOME/printer_data/config" "$INSTALL_DIR/config"

    # The real restore code, defined without running it.
    HELIX_RESTORE_LIB_ONLY=1 . "$RESTORE"
    IDIR="$INSTALL_DIR"
    # Skip the user-home probing: point it at the sandbox.
    discover_pd_helix() { printf '%s' "$PD"; }
}

write_ledgers() {
    local f
    for f in $LEDGERS; do echo "content-of-$f" > "$INSTALL_DIR/config/$f"; done
}

# What Moonraker leaves behind: a fresh tree holding only what the zip ships.
simulate_web_update() {
    rm -rf "$INSTALL_DIR"
    mkdir -p "$INSTALL_DIR/config"
    echo '{"config_version":0}' > "$INSTALL_DIR/config/settings.json"
}

@test "ledgers move to printer_data and are symlinked from the install dir" {
    write_ledgers
    setup_config_symlink
    for f in $LEDGERS; do
        [ -L "$INSTALL_DIR/config/$f" ]
        [ "$(cat "$PD/$f")" = "content-of-$f" ]
    done
}

@test "ledgers survive a Moonraker web update and are relinked" {
    write_ledgers
    setup_config_symlink
    simulate_web_update
    restore_config_symlinks
    for f in $LEDGERS; do
        [ -L "$INSTALL_DIR/config/$f" ]
        [ "$(cat "$INSTALL_DIR/config/$f")" = "content-of-$f" ]
    done
}

@test "service-start restore moves a pre-existing real ledger into printer_data" {
    mkdir -p "$PD"
    write_ledgers
    restore_config_symlinks
    for f in $LEDGERS; do
        [ -L "$INSTALL_DIR/config/$f" ]
        [ "$(cat "$PD/$f")" = "content-of-$f" ]
    done
    simulate_web_update
    restore_config_symlinks
    [ "$(cat "$INSTALL_DIR/config/.camera_migrated")" = "content-of-.camera_migrated" ]
}

@test "a ledger that was never written stays absent" {
    echo '{}' > "$INSTALL_DIR/config/settings.json"
    setup_config_symlink
    simulate_web_update
    restore_config_symlinks
    for f in $LEDGERS; do
        [ ! -e "$INSTALL_DIR/config/$f" ]
        [ ! -L "$INSTALL_DIR/config/$f" ]
        [ ! -e "$PD/$f" ]
    done
}

@test "remove_state_file deletes the printer_data target as well as the link" {
    write_ledgers
    setup_config_symlink
    remove_state_file "$INSTALL_DIR/config/.camera_migrated"
    [ ! -e "$INSTALL_DIR/config/.camera_migrated" ]
    [ ! -e "$PD/.camera_migrated" ]
    [ -f "$PD/.klipper_includes" ]
}

@test "the installer and the restore script list the same user config files and dirs" {
    local a b
    a="$(grep '^HELIX_USER_CONFIG_FILES=' "$WORKTREE_ROOT/scripts/lib/installer/platform.sh")"
    b="$(grep '^HELIX_USER_CONFIG_FILES=' "$RESTORE")"
    [ -n "$a" ]
    [ "$a" = "$b" ]
    a="$(grep '^HELIX_USER_CONFIG_DIRS=' "$WORKTREE_ROOT/scripts/lib/installer/platform.sh")"
    b="$(grep '^HELIX_USER_CONFIG_DIRS=' "$RESTORE")"
    [ -n "$a" ]
    [ "$a" = "$b" ]
}

@test "main.sh relocates ledgers after the steps that write them" {
    local seed_line reloc_line
    seed_line="$(grep -n 'seed_update_channel$' "$WORKTREE_ROOT/scripts/lib/installer/main.sh" | tail -1 | cut -d: -f1)"
    reloc_line="$(grep -n 'setup_config_symlink quiet' "$WORKTREE_ROOT/scripts/lib/installer/main.sh" | cut -d: -f1)"
    [ -n "$seed_line" ] && [ -n "$reloc_line" ]
    [ "$reloc_line" -gt "$seed_line" ]
}

# --- platform/hooks.sh ---

hooks_fixture() {
    mkdir -p "$INSTALL_DIR/assets/config/platform" "$INSTALL_DIR/platform"
    echo '# k1 hooks' > "$INSTALL_DIR/assets/config/platform/hooks-k1.sh"
    echo '# k1 hooks' > "$INSTALL_DIR/platform/hooks.sh"
}

@test "deploy_platform_hooks records the hook key as a relocatable ledger" {
    . "$WORKTREE_ROOT/scripts/lib/installer/service.sh"
    hooks_fixture
    rm -f "$INSTALL_DIR/platform/hooks.sh"
    deploy_platform_hooks "$INSTALL_DIR" k1
    [ -s "$INSTALL_DIR/platform/hooks.sh" ]
    [ "$(cat "$INSTALL_DIR/config/.platform_hook")" = "k1" ]
    setup_config_symlink
    [ -L "$INSTALL_DIR/config/.platform_hook" ]
    [ "$(cat "$PD/.platform_hook")" = "k1" ]
}

@test "restore re-deploys platform/hooks.sh after a web update" {
    . "$WORKTREE_ROOT/scripts/lib/installer/service.sh"
    hooks_fixture
    echo k1 > "$INSTALL_DIR/config/.platform_hook"
    setup_config_symlink
    simulate_web_update
    mkdir -p "$INSTALL_DIR/assets/config/platform"
    echo '# k1 hooks' > "$INSTALL_DIR/assets/config/platform/hooks-k1.sh"
    [ ! -e "$INSTALL_DIR/platform/hooks.sh" ]
    restore_config_symlinks
    restore_platform_hooks
    [ -x "$INSTALL_DIR/platform/hooks.sh" ]
    [ "$(cat "$INSTALL_DIR/platform/hooks.sh")" = "# k1 hooks" ]
}

@test "restore leaves an existing hooks file alone and ignores a hostile key" {
    hooks_fixture
    echo '# custom' > "$INSTALL_DIR/platform/hooks.sh"
    echo k1 > "$INSTALL_DIR/config/.platform_hook"
    restore_platform_hooks
    [ "$(cat "$INSTALL_DIR/platform/hooks.sh")" = "# custom" ]
    rm "$INSTALL_DIR/platform/hooks.sh"
    echo '../../etc/x' > "$INSTALL_DIR/config/.platform_hook"
    restore_platform_hooks
    [ ! -e "$INSTALL_DIR/platform/hooks.sh" ]
}

@test "restore without a recorded key deploys nothing" {
    hooks_fixture
    rm "$INSTALL_DIR/platform/hooks.sh"
    restore_platform_hooks
    [ ! -e "$INSTALL_DIR/platform/hooks.sh" ]
}

# --- SysV and procd: no systemd unit, restore runs from the init script ---

sysv_start_restore() {
    # What helixscreen.init does at start: run the sibling script by path.
    sh "$INSTALL_DIR/config/restore-user-config.sh"
}

@test "a SysV web update (no systemd unit) gets env, themes, ledgers and hooks back" {
    . "$WORKTREE_ROOT/scripts/lib/installer/service.sh"
    mkdir -p "$INSTALL_DIR/assets/config/platform" "$PD"
    echo '# k1 hooks' > "$INSTALL_DIR/assets/config/platform/hooks-k1.sh"
    echo 'HELIX_X=1' > "$INSTALL_DIR/config/helixscreen.env"
    mkdir -p "$INSTALL_DIR/config/themes"; echo mine > "$INSTALL_DIR/config/themes/mine.json"
    write_ledgers
    deploy_platform_hooks "$INSTALL_DIR" k1
    setup_config_symlink
    [ -L "$INSTALL_DIR/config/themes" ]

    # Moonraker: rmtree, then a zip holding defaults, the scripts and the template.
    simulate_web_update
    echo 'HELIX_X=default' > "$INSTALL_DIR/config/helixscreen.env"
    mkdir -p "$INSTALL_DIR/config/themes" "$INSTALL_DIR/assets/config/platform"
    echo shipped > "$INSTALL_DIR/config/themes/shipped.json"
    echo '# k1 hooks' > "$INSTALL_DIR/assets/config/platform/hooks-k1.sh"
    cp "$RESTORE" "$INSTALL_DIR/config/restore-user-config.sh"
    # No /etc/systemd/system unit is consulted: the restore never reads one.
    HELIX_RESTORE_LIB_ONLY=1 . "$INSTALL_DIR/config/restore-user-config.sh"
    discover_pd_helix() { printf '%s' "$PD"; }
    restore_config_symlinks
    restore_platform_hooks

    [ -L "$INSTALL_DIR/config/helixscreen.env" ]
    [ "$(cat "$INSTALL_DIR/config/helixscreen.env")" = "HELIX_X=1" ]
    [ -L "$INSTALL_DIR/config/themes" ]
    [ -f "$INSTALL_DIR/config/themes/mine.json" ]
    [ -f "$INSTALL_DIR/config/themes/shipped.json" ]
    [ -L "$INSTALL_DIR/config/.camera_migrated" ]
    [ -x "$INSTALL_DIR/platform/hooks.sh" ]
}

@test "running restore-user-config.sh as the init script does relinks config and hooks" {
    . "$WORKTREE_ROOT/scripts/lib/installer/service.sh"
    mkdir -p "$INSTALL_DIR/assets/config/platform"
    echo '# k1 hooks' > "$INSTALL_DIR/assets/config/platform/hooks-k1.sh"
    echo 'HELIX_X=1' > "$INSTALL_DIR/config/helixscreen.env"
    write_ledgers
    deploy_platform_hooks "$INSTALL_DIR" k1
    setup_config_symlink
    simulate_web_update
    echo 'HELIX_X=default' > "$INSTALL_DIR/config/helixscreen.env"
    mkdir -p "$INSTALL_DIR/assets/config/platform"
    echo '# k1 hooks' > "$INSTALL_DIR/assets/config/platform/hooks-k1.sh"
    cp "$RESTORE" "$INSTALL_DIR/config/restore-user-config.sh"

    HELIX_PD_HELIX_DIR="$PD" run sh "$INSTALL_DIR/config/restore-user-config.sh"
    [ "$status" -eq 0 ]
    [ "$(cat "$INSTALL_DIR/config/helixscreen.env")" = "HELIX_X=1" ]
    [ -L "$INSTALL_DIR/config/.camera_migrated" ]
    [ -x "$INSTALL_DIR/platform/hooks.sh" ]
}

@test "restore-user-config.sh runs standalone, exits 0 and finds nothing to do" {
    cp "$RESTORE" "$INSTALL_DIR/config/restore-user-config.sh"
    run sysv_start_restore
    [ "$status" -eq 0 ]
}

@test "helixscreen.init runs the restore before it sources the platform hooks" {
    local init="$WORKTREE_ROOT/config/helixscreen.init"
    local call src
    call="$(grep -n 'restore-user-config.sh' "$init" | head -1 | cut -d: -f1)"
    src="$(grep -n '^    \. "\$PLATFORM_HOOKS"' "$init" | head -1 | cut -d: -f1)"
    [ -n "$call" ] && [ -n "$src" ]
    [ "$call" -lt "$src" ]
}

@test "refresh-service-units.sh still templates units and delegates the restore" {
    grep -q 'template_unit "\$SRC" "\$DEST"' "$WORKTREE_ROOT/config/refresh-service-units.sh"
    grep -q 'systemctl daemon-reload' "$WORKTREE_ROOT/config/refresh-service-units.sh"
    grep -q 'restore-user-config.sh' "$WORKTREE_ROOT/config/refresh-service-units.sh"
}
