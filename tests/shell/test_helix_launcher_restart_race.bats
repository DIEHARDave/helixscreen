#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# A UI restart that overlaps an exiting launcher. Z-Mod restarts HelixScreen on
# every Klipper start by killing helix-screen by name and running its own
# S80helixscreen while the old launcher is still in its exit trap. Three rules
# keep the new instance alive:
#   - a launcher's exit sweep stops only the processes it started;
#   - a launcher waits, bounded, for an older launcher to exit;
#   - the Z-Mod platform hooks wait for an exiting launcher, but not for a
#     healthy one.
#
# Every process here is real (fake binaries are sh scripts) so the launcher's
# /proc reads run for real. Each test hands the launcher its own proc root:
# real /proc where only the marker can match, or a directory of symlinks to the
# PIDs the test owns, so launchers from parallel tests never show up.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
LAUNCHER="$WORKTREE_ROOT/scripts/helix-launcher.sh"
HOOKS_DIR="$WORKTREE_ROOT/assets/config/platform"

setup() {
    load helpers

    # Records any by-name sweep instead of performing it: a real killall here
    # would reach other sessions' helix-screen processes.
    mock_command_script "killall" "echo \"\$*\" >> \"$BATS_TEST_TMPDIR/killall.log\""
    mock_command_script "systemctl" 'exit 0'
    mock_command_script "setterm" 'exit 0'

    MI="$BATS_TEST_TMPDIR/helixscreen"
    mkdir -p "$MI/bin" "$MI/config"
    cp "$LAUNCHER" "$MI/bin/helix-launcher.sh"
    FAKE_PROC="$BATS_TEST_TMPDIR/fakeproc"
    mkdir -p "$FAKE_PROC"
    OWNED_PIDS=""
}

teardown() {
    for p in $OWNED_PIDS; do
        kill "$p" 2>/dev/null || true
    done
}

# A fake binary that records "<HELIX_LAUNCHER_PID> <own pid>" in $2 and then
# stays up until signalled. Short sleeps, so a TERM leaves no long orphan.
install_resident() {
    cat > "$1" <<EOF
#!/bin/sh
echo "\${HELIX_LAUNCHER_PID:-none} \$\$" >> "$2"
trap 'exit 0' TERM
while :; do sleep 0.2; done
EOF
    chmod +x "$1"
}

# A process whose argv names helix-launcher.sh (an exiting launcher, as the
# scan sees it) that lives for $1 tenths of a second. Prints its PID.
start_fake_launcher() {
    mkdir -p "$BATS_TEST_TMPDIR/old/bin"
    cat > "$BATS_TEST_TMPDIR/old/bin/helix-launcher.sh" <<'EOF'
#!/bin/sh
i=0
while [ "$i" -lt "$1" ]; do sleep 0.1; i=$((i + 1)); done
EOF
    sh "$BATS_TEST_TMPDIR/old/bin/helix-launcher.sh" "$1" >/dev/null 2>&1 3>&- &
    echo $!
}

# Expose one real PID in the fake proc root.
expose() {
    ln -s "/proc/$1" "$FAKE_PROC/$1"
}

# Wait until file $1 has at least $2 lines (5s cap).
wait_lines() {
    local i=0
    while [ "$i" -lt 50 ]; do
        [ -f "$1" ] && [ "$(wc -l < "$1")" -ge "$2" ] && return 0
        sleep 0.1
        i=$((i + 1))
    done
    fail "timed out waiting for $2 lines in $1"
}

wait_gone() {
    local i=0
    while [ "$i" -lt 30 ]; do
        kill -0 "$1" 2>/dev/null || return 0
        sleep 0.1
        i=$((i + 1))
    done
    return 1
}

@test "a successor started while the old launcher exits survives its cleanup" {
    install_resident "$MI/bin/helix-screen" "$BATS_TEST_TMPDIR/screens"

    HELIX_PROC_ROOT=/proc HELIX_LAUNCHER_PREDECESSOR_WAIT=0 \
        sh "$MI/bin/helix-launcher.sh" > "$BATS_TEST_TMPDIR/l1.log" 2>&1 3>&- &
    l1=$!
    OWNED_PIDS="$OWNED_PIDS $l1"
    wait_lines "$BATS_TEST_TMPDIR/screens" 1

    HELIX_PROC_ROOT=/proc HELIX_LAUNCHER_PREDECESSOR_WAIT=0 \
        sh "$MI/bin/helix-launcher.sh" > "$BATS_TEST_TMPDIR/l2.log" 2>&1 3>&- &
    l2=$!
    OWNED_PIDS="$OWNED_PIDS $l2"
    wait_lines "$BATS_TEST_TMPDIR/screens" 2

    h1=$(awk -v l="$l1" '$1 == l {print $2}' "$BATS_TEST_TMPDIR/screens")
    h2=$(awk -v l="$l2" '$1 == l {print $2}' "$BATS_TEST_TMPDIR/screens")
    [ -n "$h1" ] && [ -n "$h2" ]
    OWNED_PIDS="$OWNED_PIDS $h1 $h2"

    # Z-Mod's killall reaches helix-screen; the old launcher then exits and
    # runs its trap with the successor already up.
    kill "$h1"
    wait "$l1" || true

    kill -0 "$h2" || fail "old launcher's cleanup killed the successor's helix-screen"
    if grep -q 'helix-' "$BATS_TEST_TMPDIR/killall.log" 2>/dev/null; then
        fail "cleanup swept by name: $(cat "$BATS_TEST_TMPDIR/killall.log")"
    fi
}

@test "cleanup stops a child of its own left behind by its watchdog" {
    install_resident "$MI/bin/helix-splash" "$BATS_TEST_TMPDIR/splashes"
    # helix-screen leaves a splash running and exits, the shape of an orphan
    # whose supervisor died.
    cat > "$MI/bin/helix-screen" <<EOF
#!/bin/sh
"$MI/bin/helix-splash" >/dev/null 2>&1 &
exit 0
EOF
    chmod +x "$MI/bin/helix-screen"

    HELIX_PROC_ROOT=/proc HELIX_LAUNCHER_PREDECESSOR_WAIT=0 HELIX_NO_SPLASH=1 \
        sh "$MI/bin/helix-launcher.sh" > "$BATS_TEST_TMPDIR/l1.log" 2>&1 3>&- || true
    wait_lines "$BATS_TEST_TMPDIR/splashes" 1
    splash=$(awk '{print $2}' "$BATS_TEST_TMPDIR/splashes")
    OWNED_PIDS="$OWNED_PIDS $splash"

    wait_gone "$splash" || fail "launcher left its own helix-splash (PID $splash) running"
}

@test "launcher waits for an exiting predecessor before starting anything" {
    cat > "$MI/bin/helix-screen" <<EOF
#!/bin/sh
if kill -0 "\$OLD_PID" 2>/dev/null; then echo alive; else echo gone; fi > "$BATS_TEST_TMPDIR/at_start"
EOF
    chmod +x "$MI/bin/helix-screen"

    old=$(start_fake_launcher 15)
    OWNED_PIDS="$OWNED_PIDS $old"
    expose "$old"

    OLD_PID="$old" HELIX_PROC_ROOT="$FAKE_PROC" HELIX_LAUNCHER_PREDECESSOR_WAIT=10 \
        run sh "$MI/bin/helix-launcher.sh"

    contains "Waiting up to 10s for exiting launcher (PID $old)" "$output"
    contains "Previous launcher exited after" "$output"
    [ "$(cat "$BATS_TEST_TMPDIR/at_start")" = "gone" ]
}

@test "launcher gives up on a predecessor after the bound and starts" {
    cat > "$MI/bin/helix-screen" <<EOF
#!/bin/sh
if kill -0 "\$OLD_PID" 2>/dev/null; then echo alive; else echo gone; fi > "$BATS_TEST_TMPDIR/at_start"
EOF
    chmod +x "$MI/bin/helix-screen"

    old=$(start_fake_launcher 600)
    OWNED_PIDS="$OWNED_PIDS $old"
    expose "$old"

    start=$(date +%s)
    OLD_PID="$old" HELIX_PROC_ROOT="$FAKE_PROC" HELIX_LAUNCHER_PREDECESSOR_WAIT=2 \
        run sh "$MI/bin/helix-launcher.sh"
    elapsed=$(( $(date +%s) - start ))

    contains "Launcher (PID $old) still running after 2s; proceeding" "$output"
    [ "$(cat "$BATS_TEST_TMPDIR/at_start")" = "alive" ]
    [ "$elapsed" -lt 10 ]
}

@test "a launcher running the query is not its own predecessor" {
    # A launcher-named parent asks the question, as the launcher does when it
    # runs a platform hook that calls --wait-for-predecessor.
    mkdir -p "$BATS_TEST_TMPDIR/parent/bin"
    cat > "$BATS_TEST_TMPDIR/parent/bin/helix-launcher.sh" <<EOF
#!/bin/sh
ln -s "/proc/\$\$" "$FAKE_PROC/\$\$"
HELIX_PROC_ROOT="$FAKE_PROC" HELIX_LAUNCHER_PREDECESSOR_WAIT=5 \
    sh "$MI/bin/helix-launcher.sh" --wait-for-predecessor
EOF
    : > "$MI/bin/helix-screen"
    chmod +x "$MI/bin/helix-screen"

    start=$(date +%s)
    run sh "$BATS_TEST_TMPDIR/parent/bin/helix-launcher.sh"
    elapsed=$(( $(date +%s) - start ))

    [ "$status" -eq 0 ]
    [[ "$output" != *"Waiting"* ]] || fail "query waited on its own parent: $output"
    [ "$elapsed" -lt 3 ]
}

# Runs platform_pre_start from hook file $1 with the side effects neutralized.
run_hook_pre_start() {
    run sh -c '
        . "$1"
        mkdir() { :; }
        touch() { :; }
        LAUNCHER="$2"
        platform_pre_start
    ' sh "$1" "$MI/bin/helix-launcher.sh"
}

@test "zmod hooks do not wait when a healthy instance is up" {
    : > "$MI/bin/helix-screen"
    chmod +x "$MI/bin/helix-screen"
    install_resident "$BATS_TEST_TMPDIR/helix-watchdog" "$BATS_TEST_TMPDIR/watchdogs"
    sh "$BATS_TEST_TMPDIR/helix-watchdog" >/dev/null 2>&1 3>&- &
    wd=$!
    OWNED_PIDS="$OWNED_PIDS $wd"
    old=$(start_fake_launcher 600)
    OWNED_PIDS="$OWNED_PIDS $old"
    expose "$old"
    expose "$wd"

    for hook in hooks-ad5x.sh hooks-ad5m-zmod.sh; do
        start=$(date +%s)
        HELIX_PROC_ROOT="$FAKE_PROC" HELIX_LAUNCHER_PREDECESSOR_WAIT=5 \
            run_hook_pre_start "$HOOKS_DIR/$hook"
        elapsed=$(( $(date +%s) - start ))
        [ "$status" -eq 0 ]
        [[ "$output" != *"Waiting"* ]] || fail "$hook waited on a healthy instance: $output"
        [ "$elapsed" -lt 3 ] || fail "$hook took ${elapsed}s with a healthy instance up"
    done
}

@test "zmod hooks wait for an exiting launcher with no UI under it" {
    : > "$MI/bin/helix-screen"
    chmod +x "$MI/bin/helix-screen"

    for hook in hooks-ad5x.sh hooks-ad5m-zmod.sh; do
        old=$(start_fake_launcher 15)
        OWNED_PIDS="$OWNED_PIDS $old"
        expose "$old"
        HELIX_PROC_ROOT="$FAKE_PROC" HELIX_LAUNCHER_PREDECESSOR_WAIT=10 \
            run_hook_pre_start "$HOOKS_DIR/$hook"
        [ "$status" -eq 0 ]
        contains "Waiting up to 10s for exiting launcher (PID $old)" "$output"
        kill -0 "$old" 2>/dev/null && fail "$hook returned while launcher $old was alive"
        rm -f "$FAKE_PROC/$old"
    done
}
