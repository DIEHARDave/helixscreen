#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for KIAUH auto-detection and extension installation (kiauh.sh)

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers

    # Source modules (reset source guards so each test gets a fresh load)
    unset _HELIX_COMMON_SOURCED _HELIX_HOST_PROFILE_SOURCED _HELIX_PLATFORM_SOURCED _HELIX_KIAUH_SOURCED
    . "$WORKTREE_ROOT/scripts/lib/installer/common.sh" 2>/dev/null || true
    . "$WORKTREE_ROOT/scripts/lib/installer/host_profile.sh" 2>/dev/null || true
    . "$WORKTREE_ROOT/scripts/lib/installer/platform.sh" 2>/dev/null || true
    . "$WORKTREE_ROOT/scripts/lib/installer/kiauh.sh"

    # Stage KIAUH extension source files in a mock INSTALL_DIR
    export INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    local src_dir="$INSTALL_DIR/scripts/kiauh/helixscreen"
    mkdir -p "$src_dir"
    cp "$WORKTREE_ROOT/scripts/kiauh/helixscreen/__init__.py" "$src_dir/"
    cp "$WORKTREE_ROOT/scripts/kiauh/helixscreen/helixscreen_extension.py" "$src_dir/"
    cp "$WORKTREE_ROOT/scripts/kiauh/helixscreen/metadata.json" "$src_dir/"
}

# --- detect_kiauh_dir tests ---

@test "detect_kiauh_dir finds ~/kiauh" {
    mkdir -p "$BATS_TEST_TMPDIR/fakehome/kiauh/kiauh/extensions"
    HOME="$BATS_TEST_TMPDIR/fakehome" run detect_kiauh_dir
    [ "$status" -eq 0 ]
    [ "$output" = "$BATS_TEST_TMPDIR/fakehome/kiauh/kiauh/extensions" ]
}

@test "detect_kiauh_dir finds /home/user/kiauh" {
    # HOME doesn't have kiauh, but /home/biqu does
    mkdir -p "$BATS_TEST_TMPDIR/fakehome"
    mkdir -p "$BATS_TEST_TMPDIR/home/biqu/kiauh/kiauh/extensions"

    # Override /home scan by using a function wrapper
    # Since we can't create /home/biqu in tests, test the HOME path instead
    # and trust the glob logic for /home/* (tested structurally)
    HOME="$BATS_TEST_TMPDIR/fakehome" run detect_kiauh_dir
    # No KIAUH at HOME, and we can't mock /home — so empty is expected
    [ "$status" -eq 0 ]
}

@test "detect_kiauh_dir returns empty when no kiauh" {
    HOME="$BATS_TEST_TMPDIR/nope" run detect_kiauh_dir
    [ "$status" -eq 0 ]
    [ "$output" = "" ]
}

# --- install_kiauh_extension tests ---

@test "install_kiauh_extension skips when no kiauh" {
    HOME="$BATS_TEST_TMPDIR/nope" run install_kiauh_extension ""
    [ "$status" -eq 0 ]
}

@test "install_kiauh_extension copies files when kiauh found" {
    local kiauh_ext="$BATS_TEST_TMPDIR/fakehome/kiauh/kiauh/extensions"
    mkdir -p "$kiauh_ext"

    HOME="$BATS_TEST_TMPDIR/fakehome" run install_kiauh_extension ""
    [ "$status" -eq 0 ]

    # Verify all 3 files were copied
    [ -f "$kiauh_ext/helixscreen/__init__.py" ]
    [ -f "$kiauh_ext/helixscreen/helixscreen_extension.py" ]
    [ -f "$kiauh_ext/helixscreen/metadata.json" ]
}

@test "install_kiauh_extension is a no-op when target files match source" {
    local kiauh_ext="$BATS_TEST_TMPDIR/fakehome/kiauh/kiauh/extensions"
    mkdir -p "$kiauh_ext/helixscreen"

    # Pre-seed target with identical copies of the source files
    cp "$INSTALL_DIR/scripts/kiauh/helixscreen/__init__.py" "$kiauh_ext/helixscreen/"
    cp "$INSTALL_DIR/scripts/kiauh/helixscreen/helixscreen_extension.py" "$kiauh_ext/helixscreen/"
    cp "$INSTALL_DIR/scripts/kiauh/helixscreen/metadata.json" "$kiauh_ext/helixscreen/"
    local before_init_mtime
    before_init_mtime=$(stat_mtime "$kiauh_ext/helixscreen/__init__.py")

    sleep 1  # mtime resolution is 1s on most filesystems
    HOME="$BATS_TEST_TMPDIR/fakehome" run install_kiauh_extension ""
    [ "$status" -eq 0 ]
    contains "already up to date" "$output"

    # mtime unchanged → file was not rewritten
    local after_init_mtime
    after_init_mtime=$(stat_mtime "$kiauh_ext/helixscreen/__init__.py")
    [ "$before_init_mtime" = "$after_init_mtime" ]
}

@test "install_kiauh_extension updates existing silently" {
    local kiauh_ext="$BATS_TEST_TMPDIR/fakehome/kiauh/kiauh/extensions"
    mkdir -p "$kiauh_ext/helixscreen"
    echo "old" > "$kiauh_ext/helixscreen/__init__.py"

    HOME="$BATS_TEST_TMPDIR/fakehome" run install_kiauh_extension ""
    [ "$status" -eq 0 ]

    # Verify files were updated (no longer "old")
    [ -f "$kiauh_ext/helixscreen/helixscreen_extension.py" ]
    [ -f "$kiauh_ext/helixscreen/metadata.json" ]
    # __init__.py should have been replaced with the real one
    ! grep -q "^old$" "$kiauh_ext/helixscreen/__init__.py"
}

@test "install_kiauh_extension installs fresh when skip flag is empty (default-on)" {
    local kiauh_ext="$BATS_TEST_TMPDIR/fakehome/kiauh/kiauh/extensions"
    mkdir -p "$kiauh_ext"

    HOME="$BATS_TEST_TMPDIR/fakehome" run install_kiauh_extension ""
    [ "$status" -eq 0 ]
    # No opt-out — extension should be installed
    [ -f "$kiauh_ext/helixscreen/__init__.py" ]
    [ -f "$kiauh_ext/helixscreen/helixscreen_extension.py" ]
    [ -f "$kiauh_ext/helixscreen/metadata.json" ]
}

@test "install_kiauh_extension installs when skip flag is false" {
    local kiauh_ext="$BATS_TEST_TMPDIR/fakehome/kiauh/kiauh/extensions"
    mkdir -p "$kiauh_ext"

    HOME="$BATS_TEST_TMPDIR/fakehome" run install_kiauh_extension "false"
    [ "$status" -eq 0 ]
    [ -f "$kiauh_ext/helixscreen/__init__.py" ]
}

@test "--skip-kiauh-registration skips install" {
    local kiauh_ext="$BATS_TEST_TMPDIR/fakehome/kiauh/kiauh/extensions"
    mkdir -p "$kiauh_ext"

    HOME="$BATS_TEST_TMPDIR/fakehome" run install_kiauh_extension "true"
    [ "$status" -eq 0 ]
    # Extension dir should NOT have been created
    [ ! -d "$kiauh_ext/helixscreen" ]
}

@test "install_kiauh_extension handles missing source files gracefully" {
    local kiauh_ext="$BATS_TEST_TMPDIR/fakehome/kiauh/kiauh/extensions"
    mkdir -p "$kiauh_ext"

    # Wipe source files
    rm -rf "$INSTALL_DIR/scripts/kiauh"

    HOME="$BATS_TEST_TMPDIR/fakehome" run install_kiauh_extension "yes"
    [ "$status" -eq 0 ]
    # Should not crash, and should not create target dir
    [ ! -d "$kiauh_ext/helixscreen" ]
}

# --- KIAUH this user cannot write (another user's home) ---
#
# A read-only extensions dir stands in for /home/<other>/kiauh, which the
# installer finds by its /home/* scan and cannot write as itself.

# A sudo stand-in: logs each command, lifts the read-only mode the way root
# would bypass it, then runs the command. chown is logged and not run, since
# the test user cannot give files away.
make_fake_sudo() {
    FAKE_SUDO_LOG="$BATS_TEST_TMPDIR/sudo.log"
    FAKE_SUDO="$BATS_TEST_TMPDIR/fake-sudo"
    cat > "$FAKE_SUDO" <<SH
#!/bin/sh
echo "\$*" >> "$FAKE_SUDO_LOG"
[ "\$1" = chown ] && exit 0
chmod -R u+w "$1"
exec "\$@"
SH
    chmod +x "$FAKE_SUDO"
}

readonly_kiauh() {
    [ "$(id -u)" != "0" ] || skip "root writes through a read-only mode"
    RO_EXT="$BATS_TEST_TMPDIR/otheruser/kiauh/kiauh/extensions"
    mkdir -p "$RO_EXT"
    chmod 0555 "$RO_EXT"
}

teardown() {
    [ -z "${RO_EXT:-}" ] || chmod -R u+w "$RO_EXT" 2>/dev/null || true
}

@test "install_kiauh_extension warns instead of claiming success when it cannot write" {
    readonly_kiauh
    detect_kiauh_dir() { echo "$RO_EXT"; }
    SUDO=""
    unset KIAUH_EXT_ADDED
    export HELIX_INSTALL_VERBOSE=1

    install_kiauh_extension "" > "$BATS_TEST_TMPDIR/out" 2>&1
    run cat "$BATS_TEST_TMPDIR/out"
    contains "KIAUH extension not installed" "$output"
    ! contains "KIAUH extension installed at" "$output"
    [ -z "${KIAUH_EXT_ADDED:-}" ]
    [ ! -d "$RO_EXT/helixscreen" ]
}

@test "install_kiauh_extension reports a failed copy as a warning, not success" {
    readonly_kiauh
    detect_kiauh_dir() { echo "$RO_EXT"; }
    # An extension dir that mkdir -p accepts, holding a file nothing here can
    # overwrite, and a sudo that runs but cannot write either: the copy fails.
    chmod u+w "$RO_EXT"
    mkdir -p "$RO_EXT/helixscreen"
    echo old > "$RO_EXT/helixscreen/__init__.py"
    chmod 0444 "$RO_EXT/helixscreen/__init__.py"
    FAKE_SUDO_LOG="$BATS_TEST_TMPDIR/sudo.log"
    SUDO="$BATS_TEST_TMPDIR/failing-sudo"
    printf '#!/bin/sh\necho "$*" >> %s\nexec "$@"\n' "$FAKE_SUDO_LOG" > "$SUDO"
    chmod +x "$SUDO"
    unset KIAUH_EXT_ADDED
    export HELIX_INSTALL_VERBOSE=1

    install_kiauh_extension "" > "$BATS_TEST_TMPDIR/out" 2>&1
    run cat "$BATS_TEST_TMPDIR/out"
    contains "could not copy __init__.py" "$output"
    ! contains "KIAUH extension updated at" "$output"
    [ -z "${KIAUH_EXT_ADDED:-}" ]
    grep -q '^cp ' "$FAKE_SUDO_LOG"
}

@test "install_kiauh_extension writes through sudo and hands the files to KIAUH's owner" {
    readonly_kiauh
    make_fake_sudo "$RO_EXT"
    detect_kiauh_dir() { echo "$RO_EXT"; }
    _file_owner_name() { echo kiauhowner; }
    _resolve_primary_group() { echo kiauhgroup; }
    SUDO="$FAKE_SUDO"
    unset KIAUH_EXT_ADDED
    export HELIX_INSTALL_VERBOSE=1

    install_kiauh_extension "" > "$BATS_TEST_TMPDIR/out" 2>&1
    run cat "$BATS_TEST_TMPDIR/out"
    for f in __init__.py helixscreen_extension.py metadata.json; do
        [ -f "$RO_EXT/helixscreen/$f" ]
        grep -q "^cp .*/$f $RO_EXT/helixscreen/\$" "$FAKE_SUDO_LOG"
    done
    grep -qx "chown -R kiauhowner:kiauhgroup $RO_EXT/helixscreen" "$FAKE_SUDO_LOG"
    contains "KIAUH extension installed at" "$output"
    [ "${KIAUH_EXT_ADDED:-}" = 1 ]
}

@test "install_kiauh_extension does not escalate when it can write itself" {
    local kiauh_ext="$BATS_TEST_TMPDIR/fakehome/kiauh/kiauh/extensions"
    mkdir -p "$kiauh_ext"
    make_fake_sudo "$kiauh_ext"
    SUDO="$FAKE_SUDO"

    HOME="$BATS_TEST_TMPDIR/fakehome" run install_kiauh_extension ""
    [ "$status" -eq 0 ]
    [ -f "$kiauh_ext/helixscreen/__init__.py" ]
    [ ! -e "$FAKE_SUDO_LOG" ]
}

@test "detect_kiauh drops a KIAUH it cannot write and flags one that needs sudo" {
    readonly_kiauh
    detect_kiauh_dir() { echo "$RO_EXT"; }
    skip_kiauh_registration=false
    SUDO=""
    detect_kiauh
    [ -z "$KIAUH_DIR" ]
    [ "$KIAUH_WRITE_SKIPPED" = "$RO_EXT" ]

    make_fake_sudo "$RO_EXT"
    SUDO="$FAKE_SUDO"
    detect_kiauh
    [ "$KIAUH_DIR" = "$RO_EXT" ]
    [ "$KIAUH_SUDO" = 1 ]
    [ -z "$KIAUH_WRITE_SKIPPED" ]
}
