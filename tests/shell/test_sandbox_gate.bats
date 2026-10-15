#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Proves the host sandbox in setup_suite.bash is active and catches an escape
# across each boundary a test can cross.
#
# Every escape here is deliberate, so each one redirects the ledger to a private
# file. A blocked call recorded in the suite ledger fails the run, and these
# tests must not do that.

SANDBOX_COMMANDS_DOC="killall pkill pidof reboot shutdown halt poweroff telinit launchctl crontab mount umount diskutil mkfs addr2line systemctl jobpool"

setup() {
    load helpers
    PRIVATE_LEDGER="$BATS_TEST_TMPDIR/ledger.tsv"
    : > "$PRIVATE_LEDGER"
}

# Run a deliberate escape without touching the suite ledger.
escape() {
    HELIX_TEST_SANDBOX_LEDGER="$PRIVATE_LEDGER" "$@"
}

@test "sandbox: the suite installed a shim directory and a ledger" {
    [ -n "$HELIX_TEST_SANDBOX_BIN" ]
    [ -d "$HELIX_TEST_SANDBOX_BIN" ]
    [ -n "$HELIX_TEST_SANDBOX_LEDGER" ]
    case ":$PATH:" in
        *":$HELIX_TEST_SANDBOX_BIN:"*) ;;
        *) fail "sandbox bin is not on PATH: $PATH" ;;
    esac
}

@test "sandbox: makes the suite runs bypass the host's build pool" {
    [ "${JOBPOOL:-}" = 0 ]
}

@test "sandbox: every documented command has a shim on PATH" {
    local cmd
    for cmd in $SANDBOX_COMMANDS_DOC; do
        [ -x "$HELIX_TEST_SANDBOX_BIN/$cmd" ] || fail "no shim for $cmd"
    done
}

@test "sandbox: the shim list in setup_suite matches this test's copy" {
    local declared
    declared=$(grep '^HELIX_SANDBOX_COMMANDS=' "$BATS_TEST_DIRNAME/setup_suite.bash" \
        | cut -d'"' -f2)
    [ "$declared" = "$SANDBOX_COMMANDS_DOC" ] \
        || fail "setup_suite lists [$declared], this test lists [$SANDBOX_COMMANDS_DOC]"
}

@test "sandbox: a suite-run make is GNU make 4.2+ whenever gmake exists" {
    # macOS ships GNU make 3.81 as /usr/bin/make (Homebrew's 4.x installs as
    # gmake), and the repo Makefile hard-errors below 4.2, so install tests
    # that run make would fail on the environment alone. setup_suite forwards
    # make to gmake on such a host; a Linux make (or the jobpool shim, which
    # execs a modern make) parses as 4.2+ and takes the passthrough.
    command -v gmake >/dev/null 2>&1 || skip "no gmake on this host"
    local ver major minor
    ver="$(make --version 2>/dev/null | head -1)"
    major="$(printf '%s\n' "$ver" | sed -n 's/^GNU Make \([0-9]*\)\..*$/\1/p')"
    minor="$(printf '%s\n' "$ver" | sed -n 's/^GNU Make [0-9]*\.\([0-9]*\).*$/\1/p')"
    [ -n "$major" ] || fail "make --version unreadable: $ver"
    if [ "$major" -eq 4 ]; then
        [ "${minor:-0}" -ge 2 ] || fail "suite make is $ver; the gmake forwarder did not engage"
    elif [ "$major" -lt 4 ]; then
        fail "suite make is $ver; the gmake forwarder did not engage"
    fi
}

@test "sandbox: a test's own make mock wins over the gmake forwarder" {
    # The forwarder also installs a bash function, and a function outranks
    # PATH; several tests script make through a mock, so the forwarder must
    # hand control back to it.
    command -v gmake >/dev/null 2>&1 || skip "no gmake on this host"
    mock_command_script make 'echo MOCK-MAKE "$@"'
    run make install PLATFORM_TARGET=pi
    [ "$output" = "MOCK-MAKE install PLATFORM_TARGET=pi" ]
}

# --- make stays out of the tree under test ----------------------------------

@test "sandbox: the suite sees none of an enclosing make's state" {
    local v
    for v in MAKEFLAGS MFLAGS GNUMAKEFLAGS MAKELEVEL MAKEOVERRIDES BATS_REQUIRED BATS_BIN \
        ABI_STAMPED_FOR HELIX_PATCHES_FROM_CLEAN HELIX_FROM_CLEAN_SENTINEL HELIX_TESTS_NO_STALE_CHECK; do
        [ -z "${!v+set}" ] || fail "$v reached the suite: ${!v}"
    done
}

@test "sandbox: a make in the tree under test is blocked and recorded" {
    run escape bash -c 'cd "$HELIX_TEST_REPO_ROOT" && make -n help'
    [ "$status" -ne 0 ]
    contains "blocked make in the tree under test" "$output"
    run escape make -C "$HELIX_TEST_REPO_ROOT" -n help
    [ "$status" -ne 0 ]
    run escape make --directory="$HELIX_TEST_REPO_ROOT/mk" -n help
    [ "$status" -ne 0 ]
    run escape sh -c 'cd /tmp && make -C"$HELIX_TEST_REPO_ROOT" -n help'
    [ "$status" -ne 0 ]
    [ "$(grep -c 'make .*-n help' "$PRIVATE_LEDGER")" -eq 4 ]
}

@test "sandbox: a make in the tree survives a callee that REPLACES PATH" {
    run escape env PATH="/usr/bin:/bin" bash -c 'cd "$HELIX_TEST_REPO_ROOT" && make -n help'
    [ "$status" -ne 0 ]
    grep -q 'make -n help' "$PRIVATE_LEDGER"
}

@test "sandbox: a make in scratch_tree, or asking make about itself, runs" {
    run escape make -C "$(scratch_tree)" --no-print-directory print-var-LVGL_DIR
    [ "$status" -eq 0 ]
    [ "$output" = lib/lvgl ]
    run escape bash -c 'cd "$HELIX_TEST_REPO_ROOT" && make --version'
    [ "$status" -eq 0 ]
    contains "GNU Make" "$output"
    [ ! -s "$PRIVATE_LEDGER" ]
}

@test "sandbox: scratch_tree has a build/ of its own and reads the rest from the tree" {
    local tree
    tree="$(scratch_tree)"
    [ -d "$tree/build" ] && [ ! -L "$tree/build" ]
    [ ! -e "$tree/.git" ]
    [ "$(readlink "$tree/Makefile")" = "$HELIX_TEST_REPO_ROOT/Makefile" ]
}

# The guard is a PATH shim plus a bash function, so `env PATH=<no sandbox>
# make` run from a directory named by a variable holding the tree escapes both.
# This catches that shape in the source. A reviewed exception carries
# `# REAL_TREE_MAKE_OK: <reason>` on the line.
@test "sandbox: no test runs make in the tree through a repo-root variable" {
    local offenders
    offenders=$(awk '
        /^[[:space:]]*#/ || /REAL_TREE_MAKE_OK:/ { next }
        /(^|[^A-Za-z_-])make[[:space:]]/ {
            if ($0 ~ /cd "?\$\{?(REPO|REPO_ROOT|ROOT|WORKTREE_ROOT)\}?"?[^|;]*&&.*(^|[^A-Za-z_-])make[[:space:]]/ ||
                $0 ~ /(^|[^A-Za-z_-])make[[:space:]].*(-C|--directory=?)[[:space:]]*"?\$\{?(REPO|REPO_ROOT|ROOT|WORKTREE_ROOT|BATS_TEST_DIRNAME)/)
                printf "%s:%d:%s\n", FILENAME, FNR, $0
        }
    ' "$BATS_TEST_DIRNAME"/*.bats "$BATS_TEST_DIRNAME/helpers.bash")
    [ -z "$offenders" ] || fail "make run in the tree under test; use scratch_tree (helpers.bash):
$offenders"
}

# --- the deliberate escapes -------------------------------------------------

@test "sandbox: a bare killall is blocked and recorded" {
    run escape killall helix-screen
    [ "$status" -ne 0 ]
    grep -q 'killall helix-screen' "$PRIVATE_LEDGER"
}

@test "sandbox: an escape from a non-bash callee is blocked by the PATH shim" {
    # sh does not import bash's exported functions, so only PATH covers this.
    cat > "$BATS_TEST_TMPDIR/escape.sh" <<'ESC'
killall helix-screen
ESC
    run escape sh "$BATS_TEST_TMPDIR/escape.sh"
    [ "$status" -ne 0 ]
    grep -q 'killall helix-screen' "$PRIVATE_LEDGER"
}

@test "sandbox: an escape survives a callee that REPLACES PATH" {
    # The exported function layer is what holds here: the shim directory is gone
    # from PATH, which is what a script hardening PATH with the stock system
    # directories first also does.
    run escape env PATH="/usr/bin:/bin" bash -c 'killall helix-screen'
    [ "$status" -ne 0 ]
    grep -q 'killall helix-screen' "$PRIVATE_LEDGER"
}

@test "sandbox: pidof is blocked, so name-based process lookup finds nothing" {
    run escape pidof helix-screen
    [ "$status" -ne 0 ]
    grep -q 'pidof helix-screen' "$PRIVATE_LEDGER"
}

# --- the sandbox must not get in a legitimate test's way --------------------

@test "sandbox: a test's own mock wins and is never recorded" {
    mock_command_script "killall" 'exit 0'
    run escape killall helix-screen
    [ "$status" -eq 0 ]
    [ ! -s "$PRIVATE_LEDGER" ]
}

@test "sandbox: kill on a pid the test owns is not intercepted" {
    sleep 30 &
    local pid=$!
    run kill "$pid"
    [ "$status" -eq 0 ]
    wait "$pid" 2>/dev/null || true
    [ ! -s "$PRIVATE_LEDGER" ]
}

# --- the residual gap, pinned shut ------------------------------------------

@test "sandbox: nothing lands in the gap between the two layers" {
    # The PATH layer is lost at a callee that REPLACES PATH instead of
    # prepending to it. The function layer is lost where bash functions do not
    # travel: a non-bash callee, or a scrubbed environment. A line that does
    # both at once is the one shape neither layer covers.
    #
    # A reviewed exception writes the marker below inside the @test that needs
    # it, which exempts that test and nothing else.
    local offenders
    offenders=$(awk '
        FNR == 1 || /^@test/ { reviewed = 0 }
        /sandbox-gap-reviewed/ { reviewed = 1 }
        /(^|[^A-Za-z_])PATH=/ {
            if ($0 ~ /^[[:space:]]*#/) next
            if ($0 ~ /\$\{?PATH|HELIX_TEST_SANDBOX_BIN/) next
            if ($0 !~ /env -i|(^|[^-A-Za-z_\/])(sh|dash|busybox)[[:space:]]/) next
            if (reviewed) next
            printf "%s:%d:%s\n", FILENAME, FNR, $0
        }
    ' "$BATS_TEST_DIRNAME"/*.bats "$BATS_TEST_DIRNAME/helpers.bash")
    [ -z "$offenders" ] || fail "PATH replaced for a callee that also loses the function layer:
$offenders"
}

@test "sandbox: systemctl daemon-reload is blocked without helpers, so polkit is never asked" {
    # Both PATHs leave out helpers.bash's per-test systemctl shim, which is
    # what a test that never loads helpers runs with.
    run escape env PATH="/usr/bin:/bin" bash -c 'systemctl daemon-reload'
    [ "$status" -ne 0 ]
    grep -q 'systemctl daemon-reload' "$PRIVATE_LEDGER"
    run escape env PATH="$HELIX_TEST_SANDBOX_BIN:/usr/bin:/bin" sh -c 'systemctl daemon-reload'
    [ "$status" -ne 0 ]
    [ "$(grep -c 'systemctl daemon-reload' "$PRIVATE_LEDGER")" -eq 2 ]
}
