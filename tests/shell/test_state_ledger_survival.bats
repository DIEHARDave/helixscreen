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

@test "a standalone run touches only the printer_data it was pointed at" {
    mkdir -p "$PD"
    echo mine > "$INSTALL_DIR/config/.camera_migrated"
    cp "$RESTORE" "$INSTALL_DIR/config/restore-user-config.sh"
    find "$BATS_TEST_TMPDIR" | sort > "$BATS_TEST_TMPDIR/before.list"

    HELIX_PD_HELIX_DIR="$PD" run sh "$INSTALL_DIR/config/restore-user-config.sh"
    [ "$status" -eq 0 ]

    # It ran: the real ledger moved into the sandbox and was linked back.
    [ "$(cat "$PD/.camera_migrated")" = "mine" ]
    [ -L "$INSTALL_DIR/config/.camera_migrated" ]
    # Every new path is inside the install dir or the printer_data it was given.
    find "$BATS_TEST_TMPDIR" -not -name '*.list' | sort > "$BATS_TEST_TMPDIR/after.list"
    run comm -13 "$BATS_TEST_TMPDIR/before.list" "$BATS_TEST_TMPDIR/after.list"
    [ "$status" -eq 0 ]
    run sh -c 'printf "%s\n" "$1" | grep -v -e "^$2/" -e "^$3/"' _ "$output" "$INSTALL_DIR" "$PD"
    [ -z "$output" ]
}

@test "helixscreen.init restores before it reads the env file and before it sources the hooks" {
    local init="$WORKTREE_ROOT/config/helixscreen.init"
    local call src env
    call="$(grep -n 'restore-user-config.sh' "$init" | head -1 | cut -d: -f1)"
    src="$(grep -n '^    \. "\$PLATFORM_HOOKS"' "$init" | head -1 | cut -d: -f1)"
    env="$(grep -n -e 'LAUNCHER" --print-env HELIX_NO_SPLASH' "$init" | head -1 | cut -d: -f1)"
    [ -n "$call" ] && [ -n "$src" ] && [ -n "$env" ]
    [ "$call" -lt "$src" ]
    [ "$call" -lt "$env" ]
}

@test "refresh-service-units.sh still templates units and delegates the restore" {
    grep -q 'template_unit "\$SRC" "\$DEST"' "$WORKTREE_ROOT/config/refresh-service-units.sh"
    grep -q 'systemctl daemon-reload' "$WORKTREE_ROOT/config/refresh-service-units.sh"
    grep -q 'restore-user-config.sh' "$WORKTREE_ROOT/config/refresh-service-units.sh"
}

# --- where printer_data is ---

@test "a recorded printer_data wins over scanning" {
    mkdir -p "$BATS_TEST_TMPDIR/elsewhere"
    HELIX_PD_HELIX_DIR="$BATS_TEST_TMPDIR/elsewhere"
    unset -f discover_pd_helix
    HELIX_RESTORE_LIB_ONLY=1 . "$RESTORE"
    [ "$(discover_pd_helix)" = "$BATS_TEST_TMPDIR/elsewhere" ]
}

@test "the scan list covers the U1 and puts /mnt/UDISK before /root, without layouts the installer never makes" {
    grep -q '/home/lava/printer_data/config/helixscreen' "$RESTORE"
    ! grep -q '/usr/data/config/printer_data' "$RESTORE"
    ! grep -q '/opt/config/printer_data' "$RESTORE"
    local udisk root
    udisk="$(grep -n '^        /mnt/UDISK/printer_data' "$RESTORE" | cut -d: -f1)"
    root="$(grep -n '^        /root/printer_data' "$RESTORE" | cut -d: -f1)"
    [ "$udisk" -lt "$root" ]
}

@test "the installer writes the resolved printer_data into the init script" {
    . "$WORKTREE_ROOT/scripts/lib/installer/service.sh"
    local init="$BATS_TEST_TMPDIR/S99helixscreen"
    printf 'DAEMON_DIR="/opt/helixscreen"\nHELIX_PD_HELIX=""  # Updated by installer\n' > "$init"
    _set_init_script_pd_helix "$init"
    grep -qx "HELIX_PD_HELIX=\"$PD\"" "$init"
}

@test "the unit records printer_data and refresh-service-units.sh reads it back and re-templates it" {
    grep -q '^# HELIX_PD_HELIX=@@HELIX_PD_HELIX@@' "$WORKTREE_ROOT/config/helixscreen.service"
    grep -q "s/^# HELIX_PD_HELIX=//p" "$WORKTREE_ROOT/config/refresh-service-units.sh"
    grep -q '@@HELIX_PD_HELIX@@' "$WORKTREE_ROOT/config/refresh-service-units.sh"
    grep -q 'HELIX_PD_HELIX_DIR="$PD_HELIX_VAL"' "$WORKTREE_ROOT/config/refresh-service-units.sh"
}

# --- a real file that differs from its printer_data copy ---

# $1 name  $2 real content  $3 pd content  $4 real_newer | pd_newer
real_vs_pd() {
    mkdir -p "$PD"
    printf '%s' "$3" > "$PD/$1"
    printf '%s' "$2" > "$INSTALL_DIR/config/$1"
    if [ "$4" = real_newer ]; then
        touch -d '2026-01-01 00:00:00' "$PD/$1"
        touch -d '2026-06-01 00:00:00' "$INSTALL_DIR/config/$1"
    else
        touch -d '2026-06-01 00:00:00' "$PD/$1"
        touch -d '2026-01-01 00:00:00' "$INSTALL_DIR/config/$1"
    fi
    restore_config_symlinks
}

@test "an app-written settings.json newer than the printer_data copy wins, and the old copy is kept" {
    real_vs_pd settings.json '{"config_version":7,"a":2}' '{"config_version":6,"a":1}' real_newer
    [ -L "$INSTALL_DIR/config/settings.json" ]
    [ "$(cat "$PD/settings.json")" = '{"config_version":7,"a":2}' ]
    [ "$(cat "$PD/settings.json.pre-restore")" = '{"config_version":6,"a":1}' ]
}

@test "the release's default settings.json never replaces the user's, and is not kept" {
    real_vs_pd settings.json '{"default":true}' '{"config_version":6,"a":1}' real_newer
    [ -L "$INSTALL_DIR/config/settings.json" ]
    [ "$(cat "$PD/settings.json")" = '{"config_version":6,"a":1}' ]
    [ ! -e "$PD/settings.json.pre-restore" ]
}

@test "an app-written settings.json older than the printer_data copy loses but is kept" {
    real_vs_pd settings.json '{"config_version":5,"a":0}' '{"config_version":6,"a":1}' pd_newer
    [ "$(cat "$PD/settings.json")" = '{"config_version":6,"a":1}' ]
    [ "$(cat "$PD/settings.json.pre-restore")" = '{"config_version":5,"a":0}' ]
}

@test "a differing real file other than settings.json is kept beside the printer_data copy" {
    real_vs_pd helixscreen.env 'HELIX_X=default' 'HELIX_X=mine' real_newer
    [ -L "$INSTALL_DIR/config/helixscreen.env" ]
    [ "$(cat "$PD/helixscreen.env")" = 'HELIX_X=mine' ]
    [ "$(cat "$PD/helixscreen.env.pre-restore")" = 'HELIX_X=default' ]
}

@test "an identical real file leaves no pre-restore copy" {
    real_vs_pd helixscreen.env 'HELIX_X=1' 'HELIX_X=1' real_newer
    [ -L "$INSTALL_DIR/config/helixscreen.env" ]
    [ ! -e "$PD/helixscreen.env.pre-restore" ]
}

# --- uninstall finds the printer_data copy ---

@test "undo_klipper_includes reads and removes the printer_data ledger when the install dir has none" {
    . "$WORKTREE_ROOT/scripts/lib/installer/uninstall.sh"
    mkdir -p "$PD"
    echo snippet > "$BATS_TEST_TMPDIR/snippet.cfg"
    echo "cfg:$BATS_TEST_TMPDIR/snippet.cfg" > "$PD/.klipper_includes"
    [ ! -e "$INSTALL_DIR/config/.klipper_includes" ]
    undo_klipper_includes
    [ ! -e "$BATS_TEST_TMPDIR/snippet.cfg" ]
    [ ! -e "$PD/.klipper_includes" ]
}

@test "_ledger_path prefers the install dir and falls back to printer_data" {
    . "$WORKTREE_ROOT/scripts/lib/installer/uninstall.sh"
    mkdir -p "$PD"
    echo x > "$PD/.seeded_settings"
    [ "$(_ledger_path .seeded_settings)" = "$PD/.seeded_settings" ]
    echo y > "$INSTALL_DIR/config/.seeded_settings"
    [ "$(_ledger_path .seeded_settings)" = "$INSTALL_DIR/config/.seeded_settings" ]
    [ "$(_ledger_path .never_written)" = "$INSTALL_DIR/config/.never_written" ]
}

@test "uninstall removes the platform hook record from printer_data too" {
    grep -q 'remove_state_file "$(_ledger_path .platform_hook)"' "$WORKTREE_ROOT/scripts/lib/installer/uninstall.sh"
}
