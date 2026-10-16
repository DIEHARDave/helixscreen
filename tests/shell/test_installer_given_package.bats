#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# A package handed to the installer is the package it installs: an archive
# named on the command line (with or without --local), or the extracted
# release tree the installer itself was unpacked with. Neither consults the
# channel or the network. --version and --update still resolve online, and a
# version with no build for this platform names the release that has one.
#
# The whole-install cases run main() as an x86 desktop box: that flavour
# installs into the user's home, so every path it writes is the test's own.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
LIB="$WORKTREE_ROOT/scripts/lib/installer"

setup() {
    load helpers
    STUBBIN="$BATS_TEST_TMPDIR/stubbin"
    mkdir -p "$STUBBIN"
    for c in sudo apt-get apt apt-cache dpkg-query pkexec udevadm pidof curl wget; do
        printf '#!/bin/sh\necho "%s $*" >> "%s/calls.log"\nexit 1\n' "$c" "$BATS_TEST_TMPDIR" > "$STUBBIN/$c"
        chmod +x "$STUBBIN/$c"
    done
    PATH="$STUBBIN:$PATH"
    mock_command_script systemctl "echo \"systemctl \$*\" >> '$BATS_TEST_TMPDIR/calls.log'; exit 1"
    mock_command_script id "[ \"\$1\" = -u ] && [ \$# -eq 1 ] && { echo 1000; exit 0; }
exec /usr/bin/id \"\$@\""

    export HOME="$BATS_TEST_TMPDIR/home/alice"
    mkdir -p "$HOME"
    unset XDG_DATA_HOME XDG_STATE_HOME KLIPPER_HOME DISPLAY WAYLAND_DISPLAY XDG_SESSION_TYPE INSTALL_DIR TMP_DIR
    unset HELIX_SELF_UPDATE R2_CHANNEL
    export HELIX_SYSTEMD_UNIT_DIR="$BATS_TEST_TMPDIR/etc/systemd/system"
    export HELIX_INIT_D_DIR="$BATS_TEST_TMPDIR/etc/init.d"
    export HELIX_HOME_ROOT="$BATS_TEST_TMPDIR/home"
    export HELIX_PROC_ROOT="$BATS_TEST_TMPDIR/no-proc"
    export _HELIX_KNOWN_INSTALL_DIRS="$BATS_TEST_TMPDIR/opt/helixscreen"
    export HELIX_TTY_DEVICE=/nonexistent
    export HELIX_INSTALL_TTY=0
    export SUDO=""
    mkdir -p "$HELIX_SYSTEMD_UNIT_DIR" "$HELIX_INIT_D_DIR"

    local bats_traps
    bats_traps="$(trap -p EXIT INT TERM HUP)"
    for m in common logo host_profile platform permissions requirements competing_uis desktop release service moonraker kiauh plan uninstall main; do
        . "$LIB/$m.sh"
    done
    trap - ERR EXIT INT TERM HUP
    eval "$bats_traps"

    DESK="$HOME/.local/share/helixscreen"
    DL="$HOME/Downloads"
    mkdir -p "$DL"

    # Every way the installer reaches the network records itself and fails,
    # so a test can prove a run never asked.
    get_latest_version() { echo "NETWORK get_latest_version $*" >> "$BATS_TEST_TMPDIR/calls.log"; echo v9.9.9; }
    _url_probe() { echo "NETWORK probe $*" >> "$BATS_TEST_TMPDIR/calls.log"; echo unknown; }
    _fetch_channel_manifest() { echo "NETWORK manifest $*" >> "$BATS_TEST_TMPDIR/calls.log"; return 1; }
    download_release() { echo "NETWORK download $*" >> "$BATS_TEST_TMPDIR/calls.log"; exit 1; }
}

called() { grep -qs "^$1" "$BATS_TEST_TMPDIR/calls.log"; }
# A request leaves the machine; `curl --version` only asks what curl this is.
reached_out() { grep -v '^curl --version$' "$BATS_TEST_TMPDIR/calls.log" 2>/dev/null | grep -qE '^(NETWORK|curl|wget) '; }
calls() { cat "$BATS_TEST_TMPDIR/calls.log" 2>/dev/null; }

# A release payload as the packaging lays it out: binary, data, the bundled
# installer and the release_info.json naming version and asset.
make_payload() { # dir asset [version]
    mkdir -p "$1/bin" "$1/assets/images" "$1/ui_xml" "$1/config"
    create_fake_elf "$1/bin/helix-screen" 02 3e
    chmod +x "$1/bin/helix-screen"
    : > "$1/assets/images/helix-icon-256.png"
    printf '#!/bin/sh\n' > "$1/install.sh"
    printf '{"project_name":"helixscreen","project_owner":"prestonbrown","version":"%s","asset_name":"%s"}\n' \
        "${3:-1.2.3-beta.4}" "$2" > "$1/release_info.json"
}

make_archive() { # asset name
    local pkg="$BATS_TEST_TMPDIR/pkg-$1/helixscreen"
    make_payload "$pkg" "$1"
    head -c 1100000 /dev/urandom > "$pkg/assets/pad.bin"
    tar -czf "$DL/$2" -C "$BATS_TEST_TMPDIR/pkg-$1" helixscreen
}

# Run main() as an x86 box whose installer sits in DIR.
run_main_from() { # dir args...
    local self="$1"
    shift
    installer_self_dir() { printf '%s\n' "$self"; }
    detect_platform() { echo x86; }
    detect_init_system() { INIT_SYSTEM=systemd; }
    run main "$@"
}

# ---------------------------------------------------------------------------
# An archive on the command line
# ---------------------------------------------------------------------------

@test "a positional .tar.gz or .zip that exists is --local" {
    : > "$DL/helixscreen-pi-v1.2.3.tar.gz"
    : > "$DL/helixscreen-pi.zip"
    parse_installer_args "$DL/helixscreen-pi-v1.2.3.tar.gz"
    [ "$local_tarball" = "$DL/helixscreen-pi-v1.2.3.tar.gz" ]
    parse_installer_args --desktop "$DL/helixscreen-pi.zip"
    [ "$local_tarball" = "$DL/helixscreen-pi.zip" ]
}

@test "a positional that is not an existing archive is still an unknown option" {
    run parse_installer_args "$DL/missing-v1.2.3.tar.gz"
    [ "$status" -eq 1 ]
    contains "Unknown option: $DL/missing-v1.2.3.tar.gz" "$output"
    : > "$DL/notes.txt"
    run parse_installer_args "$DL/notes.txt"
    [ "$status" -eq 1 ]
    contains "Unknown option" "$output"
    run parse_installer_args bogus
    [ "$status" -eq 1 ]
    contains "Unknown option: bogus" "$output"
}

@test "two different archives are refused" {
    : > "$DL/a-v1.tar.gz"
    : > "$DL/b-v2.tar.gz"
    run parse_installer_args --local "$DL/a-v1.tar.gz" "$DL/b-v2.tar.gz"
    [ "$status" -eq 1 ]
    contains "one archive" "$output"
}

@test "a positional archive installs with no channel or network lookup" {
    make_archive helixscreen-x86-desktop.zip helixscreen-x86-desktop-v1.2.3.tar.gz
    run_main_from "$BATS_TEST_TMPDIR/elsewhere" --desktop "$DL/helixscreen-x86-desktop-v1.2.3.tar.gz"
    [ "$status" -eq 0 ] || fail "main failed ($status): $output"
    [ -x "$DESK/bin/helix-screen" ]
    contains "Installing from local file" "$output"
    if reached_out; then fail "the --local run reached out: $(calls)"; fi
}

# ---------------------------------------------------------------------------
# The package the installer was unpacked with
# ---------------------------------------------------------------------------

@test "the installer installs the package it sits in, without the network" {
    make_payload "$DL/helixscreen" helixscreen-x86-desktop.zip
    echo pkg > "$DL/helixscreen/ui_xml/marker.txt"
    run_main_from "$DL/helixscreen" --desktop
    [ "$status" -eq 0 ] || fail "main failed ($status): $output"
    contains "Installing the package next to this installer: v1.2.3-beta.4 (x86-desktop)" "$output"
    [ "$(cat "$DESK/ui_xml/marker.txt")" = pkg ]
    desktop_root_is_ours "$DESK"
    # Copied, not moved: the user's extracted package is left as it was.
    [ "$(cat "$DL/helixscreen/ui_xml/marker.txt")" = pkg ]
    [ -x "$DL/helixscreen/bin/helix-screen" ]
    if reached_out; then fail "the package run reached out: $(calls)"; fi
}

@test "the package's own flavour decides desktop vs kiosk when nothing else does" {
    make_payload "$DL/helixscreen" helixscreen-x86-desktop.zip
    run_main_from "$DL/helixscreen" --dry-run
    [ "$status" -eq 0 ] || fail "dry run failed ($status): $output"
    contains "window on this desktop (the package)" "$output"
    contains "v1.2.3-beta.4" "$output"
    [ ! -e "$DESK" ]
    if reached_out; then fail "the dry run reached out: $(calls)"; fi
}

@test "the copy of the installer inside an installed tree resolves online" {
    make_payload "$DESK" helixscreen-x86-desktop.zip
    run_main_from "$DESK" --dry-run
    [ "$status" -eq 0 ] || fail "dry run failed ($status): $output"
    lacks "next to this installer" "$output"
    called "NETWORK get_latest_version" || fail "an installed tree's installer did not resolve online: $(calls)"
}

@test "a package at a known install root is an install, not a package" {
    make_payload "$BATS_TEST_TMPDIR/opt/helixscreen" helixscreen-x86.zip
    run_main_from "$BATS_TEST_TMPDIR/opt/helixscreen" --desktop --dry-run
    [ "$status" -eq 0 ] || fail "dry run failed ($status): $output"
    lacks "next to this installer" "$output"
    called "NETWORK get_latest_version" || fail "did not resolve online: $(calls)"
}

@test "a package built for another platform is refused, naming both" {
    make_payload "$DL/helixscreen" helixscreen-x86.zip
    run_main_from "$DL/helixscreen" --desktop
    [ "$status" -eq 1 ]
    contains "This package is for x86, but this computer needs x86-desktop" "$output"
    [ ! -e "$DESK" ]
    if reached_out; then fail "the refused run reached out: $(calls)"; fi
}

@test "a package for another board is refused on any platform" {
    make_payload "$DL/helixscreen" helixscreen-k2.zip
    installer_self_dir() { printf '%s\n' "$DL/helixscreen"; }
    platform=pi
    INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    detect_package_beside_installer
    [ "$package_dir" = "$DL/helixscreen" ]
    run settle_package_beside_installer
    [ "$status" -eq 1 ]
    contains "This package is for k2, but this computer needs pi" "$output"
}

@test "a directory without release_info.json is no package" {
    make_payload "$DL/helixscreen" helixscreen-x86-desktop.zip
    rm "$DL/helixscreen/release_info.json"
    installer_self_dir() { printf '%s\n' "$DL/helixscreen"; }
    detect_package_beside_installer
    [ -z "$package_dir" ]
}

@test "--version and --update beside a package still go online" {
    make_payload "$DL/helixscreen" helixscreen-x86-desktop.zip
    run_main_from "$DL/helixscreen" --desktop --dry-run --version v2.0.0
    [ "$status" -eq 0 ] || fail "dry run failed ($status): $output"
    lacks "next to this installer" "$output"
    called "NETWORK probe" || fail "--version did not probe online: $(calls)"

    : > "$BATS_TEST_TMPDIR/calls.log"
    run_main_from "$DL/helixscreen" --desktop --dry-run --update
    [ "$status" -eq 0 ] || fail "dry run failed ($status): $output"
    lacks "next to this installer" "$output"
    called "NETWORK get_latest_version" || fail "--update did not resolve online: $(calls)"
}

@test "an in-app update never takes the tree it runs from as a package" {
    make_payload "$DL/helixscreen" helixscreen-x86-desktop.zip
    installer_self_dir() { printf '%s\n' "$DL/helixscreen"; }
    HELIX_SELF_UPDATE=1
    detect_package_beside_installer
    [ -z "$package_dir" ]
}

# ---------------------------------------------------------------------------
# A version with no build for this platform
# ---------------------------------------------------------------------------

manifest() { # version assets...
    local v=$1 a
    shift
    printf '{\n  "version": "%s",\n  "assets": {\n' "$v"
    for a in "$@"; do
        printf '    "%s": {\n      "url": "https://example.invalid/%s.tar.gz"\n    },\n' "$a" "$a"
    done
    printf '    "zz": {}\n  }\n}\n'
}

stub_channels() {
    _fetch_channel_manifest() {
        case "$1" in
            stable) _R2_MANIFEST=$(manifest 1.0.5 pi x86) ;;
            beta) _R2_MANIFEST=$(manifest 1.1.0-beta.9 pi x86 x86-desktop) ;;
            *) return 1 ;;
        esac
    }
    _url_probe() { echo missing; }
}

@test "a missing build names the release that has this platform" {
    stub_channels
    platform=x86-desktop
    run probe_release v1.0.5 x86-desktop
    [ "$status" -eq 1 ]
    contains "No HelixScreen v1.0.5 release for x86-desktop." "$output"
    contains "--version v1.1.0-beta.9" "$output"
    contains "sh -s -- --desktop --version v1.1.0-beta.9" "$output"
}

@test "a missing build off the desktop flavour gives the command without --desktop" {
    stub_channels
    _fetch_channel_manifest() {
        case "$1" in
            stable) _R2_MANIFEST=$(manifest 1.0.5 pi) ;;
            beta) _R2_MANIFEST=$(manifest 1.1.0-beta.9 pi k2) ;;
            *) return 1 ;;
        esac
    }
    platform=k2
    run probe_release v1.0.5 k2
    [ "$status" -eq 1 ]
    contains "--version v1.1.0-beta.9" "$output"
    contains "sh -s -- --version v1.1.0-beta.9" "$output"
    lacks "--desktop" "$output"
}

@test "a missing build with no release found gives the generic --version hint" {
    _url_probe() { echo missing; }
    platform=x86-desktop
    run probe_release v1.0.5 x86-desktop
    [ "$status" -eq 1 ]
    contains "No HelixScreen v1.0.5 release for x86-desktop." "$output"
    contains "--version" "$output"
    lacks "v1.1.0-beta.9" "$output"
}

@test "a missing build never offers the version that just failed" {
    # The manifest lists the platform, yet every URL for it answered 404.
    _fetch_channel_manifest() { _R2_MANIFEST=$(manifest 1.0.5 pi x86-desktop); }
    _url_probe() { echo missing; }
    platform=x86-desktop
    run probe_release v1.0.5 x86-desktop
    [ "$status" -eq 1 ]
    lacks "--version v1.0.5" "$output"
    contains "--version vX.Y.Z" "$output"
}

@test "a package that is this run's INSTALL_DIR is the install, not a package" {
    make_payload "$HOME/helixscreen" helixscreen-pi.zip
    installer_self_dir() { printf '%s\n' "$HOME/helixscreen"; }
    platform=pi
    detect_package_beside_installer
    [ "$package_dir" = "$HOME/helixscreen" ]
    INSTALL_DIR="$HOME/helixscreen"
    settle_package_beside_installer
    [ -z "$package_dir" ]
}
