#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# make land-gate: the pre-landing gate. What CI judges a branch on, run before
# the branch reaches main, so a red CI run is found here instead of on main.
#
#   app-build         make (the app; tests/ui drives the real binary)
#   sweep             make unit-sweep, the local gate's shard layout
#   ci-shards         the sweep again as CI_SHARDS shards (default 8: build.yml's
#                     nproc*2 on 4-core runners). Which tests share a process
#                     decides what a use-after-free corrupts, so a layout nobody
#                     ran locally can crash where the local one passed.
#   bats              make test-shell
#   ui                make test-ui-ci: CI's exact tests/ui pytest slice, headless
#   plugin            make test-plugin: the moonraker plugin pytest, also in CI's job
#   theme-tokens      make check-theme-tokens: generated token table vs ui_xml/
#   xml-schema        make check-xml-schema: linter schema snapshot vs a regeneration
#   lint-xml          make lint-xml: CI's XML Lint job, against the committed snapshot
#   lvgl-event-codes  make check-lvgl-event-codes: crash worker table vs lv_event_code_t
#
# The two C++ steps run on the test host when full-test-run's would
# (scripts/lib/sweep_location.sh: TEST_HOST=1, 0 or unset for the probe), one
# after the other, since both use one mirror. Everything else runs here, in
# parallel lanes beside them. Each step logs to $LAND_GATE_LOGDIR/<step>.log
# (default build/land-gate); the table at the end names every step with its
# duration, and the exit status is non-zero if any step did not pass.
#
# Outside this gate: [.] (the commit hook), [slow] and the goldens (nightly and
# local-only), and the full quality-checks.sh run (pre-push and CI).
set -uo pipefail

MAKE=${MAKE:-make}
SELF="${BASH_SOURCE[0]}"
TEST_HOST_RUN=${TEST_HOST_RUN:-$(dirname "$SELF")/test-host-run.sh}
CI_SHARDS=${CI_SHARDS:-8}
LAND_GATE_LOGDIR=${LAND_GATE_LOGDIR:-build/land-gate}
ROWS_DIR="$LAND_GATE_LOGDIR/rows"

STEPS="app-build sweep ci-shards bats ui plugin theme-tokens xml-schema lint-xml lvgl-event-codes"

# step <name> <command...>: run it, log it, record its row.
step() {
    local name=$1 log t0 status
    shift
    log="$LAND_GATE_LOGDIR/$name.log"
    t0=$SECONDS
    echo "[$(date +%H:%M:%S)] $name: started"
    if "$@" >"$log" 2>&1; then
        status=PASS
    else
        status=FAIL
    fi
    printf '%s %s %s\n' "$status" $((SECONDS - t0)) "$log" >"$ROWS_DIR/$name"
    echo "[$(date +%H:%M:%S)] $name: $status in $((SECONDS - t0))s"
    [ "$status" = PASS ]
}

skip() { printf 'SKIP - %s\n' "$2" >"$ROWS_DIR/$1"; }

mk() { "$MAKE" --no-print-directory "$@"; }

lane() {
    case "$1" in
        unit)
            if [ "$SWEEP_WHERE" = remote ]; then
                step sweep "$TEST_HOST_RUN" sweep
                step ci-shards "$TEST_HOST_RUN" sweep "NPROCS=$CI_SHARDS"
            else
                step sweep mk unit-sweep
                step ci-shards mk unit-sweep "NPROCS=$CI_SHARDS"
            fi ;;
        bats) step bats mk test-shell ;;
        ui)   step ui mk test-ui-ci
              step plugin mk test-plugin ;;
        fresh)
            step theme-tokens mk check-theme-tokens
            step xml-schema mk check-xml-schema
            step lint-xml mk lint-xml
            step lvgl-event-codes mk check-lvgl-event-codes ;;
        *) echo "✗ unknown lane '$1'" >&2; return 2 ;;
    esac
    return 0
}

if [ "${1:-}" = --lane ]; then
    lane "$2"
    exit $?
fi

# shellcheck source-path=SCRIPTDIR source=lib/build_hosts.sh
. "$(dirname "$SELF")/lib/build_hosts.sh"
# shellcheck source-path=SCRIPTDIR source=lib/sweep_location.sh
. "$(dirname "$SELF")/lib/sweep_location.sh"
resolve_sweep_location || exit $?
export SWEEP_WHERE MAKE TEST_HOST_RUN CI_SHARDS LAND_GATE_LOGDIR ROWS_DIR

mkdir -p "$ROWS_DIR" || exit 2
rm -f "$ROWS_DIR"/*
gate_t0=$SECONDS

# Each lane is its own process group, so one signal reaches everything under
# it: a test-host ssh, a pytest and its app, a bats run. A lane left behind
# keeps a mirror lock and a run nobody is waiting for.
PIDS=()
start_lane() {
    if command -v setsid >/dev/null 2>&1; then
        setsid bash "$SELF" --lane "$1" &
    else
        bash "$SELF" --lane "$1" &
    fi
    PIDS+=("$!")
}
stop_lanes() {
    local p
    for p in "${PIDS[@]}"; do kill -TERM -- "-$p" 2>/dev/null || kill -TERM "$p" 2>/dev/null; done
    exit 130
}
trap stop_lanes INT TERM HUP

if [ "$SWEEP_WHERE" = remote ]; then
    echo "→ C++ sweeps on $HELIX_TEST_HOST, everything else here (logs: $LAND_GATE_LOGDIR)"
    start_lane unit
else
    echo "→ every step runs here (logs: $LAND_GATE_LOGDIR)"
fi
start_lane bats
start_lane fresh

# The app builds before anything else builds in this tree: two makes compiling
# one tree's objects at once race on the same files.
if step app-build mk; then
    start_lane ui
else
    skip ui "app-build failed"
    skip plugin "app-build failed"
fi
[ "$SWEEP_WHERE" = remote ] || start_lane unit

for p in "${PIDS[@]}"; do wait "$p"; done
trap - INT TERM HUP

failed=0
echo
echo "Land gate ($(git rev-parse --short HEAD 2>/dev/null || echo '?'), C++ sweeps: $SWEEP_WHERE)"
for s in $STEPS; do
    if [ -f "$ROWS_DIR/$s" ]; then
        read -r status secs log <"$ROWS_DIR/$s"
    else
        status=FAIL secs=- log="no result: its lane died"
    fi
    [ "$status" = PASS ] || failed=1
    if [ "$secs" = - ]; then dur=-; else dur="${secs}s"; fi
    printf '  %-4s  %-17s %7s  %s\n' "$status" "$s" "$dur" "$log"
done
for s in $STEPS; do
    [ -f "$ROWS_DIR/$s" ] || continue
    read -r status _ log <"$ROWS_DIR/$s"
    [ "$status" = FAIL ] || continue
    echo
    echo "--- $s: last 25 lines of $log ---"
    tail -n 25 "$log"
done
echo
echo "  total $((SECONDS - gate_t0))s"
if [ "$failed" -ne 0 ]; then
    echo "LAND GATE: FAIL"
    exit 1
fi
echo "LAND GATE: PASS"
