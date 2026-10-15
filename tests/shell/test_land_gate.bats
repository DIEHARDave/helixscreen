#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# make land-gate's wiring (scripts/land-gate.sh). make and test-host-run.sh are
# stubbed: each target exits with a verdict the test chooses and records that it
# ran, so these prove which steps run where and that any red step reds the gate
# and shows in the table, without running a real suite.

load helpers

REPO_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
GATE="$REPO_ROOT/scripts/land-gate.sh"

ALL_STEPS="app-build sweep ci-shards bats ui plugin theme-tokens xml-schema lint-xml lvgl-event-codes"

setup() {
    export TMPDIR="$BATS_TEST_TMPDIR"
    export CALLS="$BATS_TEST_TMPDIR/calls"
    export LAND_GATE_LOGDIR="$BATS_TEST_TMPDIR/logs"
    : >"$CALLS"
    # A target exits with $RC_<target, - as _>, default 0; no target is "app".
    mock_command_script fake-make '
t=app; vars=""
for a in "$@"; do case "$a" in -*) ;; *=*) vars="$vars $a" ;; *) t=$a ;; esac; done
echo "make $t$vars" >> "$CALLS"
echo "output of $t"
eval "rc=\${RC_$(echo "$t" | tr - _):-0}"
exit "$rc"'
    mock_command_script fake-test-host-run '
echo "host-run $*" >> "$CALLS"
case "$1" in
    sweep) echo "host sweep $*"; case "$*" in *NPROCS=*) exit "${RC_host_ci:-0}" ;; *) exit "${RC_host_sweep:-0}" ;; esac ;;
    --probe) exit 1 ;;
esac'
    export MAKE=fake-make TEST_HOST_RUN=fake-test-host-run
    export HELIX_TEST_HOST=testhost.invalid HELIX_BUILD_HOSTS_FILE=/nonexistent
    unset CI_SHARDS
    export TEST_HOST=0
}

row() { # <status> <step>: the table row for step has that status
    printf '%s\n' "$output" | grep -Eq "^  $1 +$2 "
}

@test "all green: every step runs, every row passes, and the gate passes" {
    run "$GATE"
    [ "$status" -eq 0 ]
    for s in $ALL_STEPS; do row PASS "$s"; done
    contains "LAND GATE: PASS" "$output"
    grep -qx "make app" "$CALLS"
    grep -qx "make unit-sweep" "$CALLS"
    grep -qx "make test-shell" "$CALLS"
    grep -qx "make test-ui-ci" "$CALLS"
    grep -qx "make test-plugin" "$CALLS"
    grep -qx "make check-theme-tokens" "$CALLS"
    grep -qx "make check-xml-schema" "$CALLS"
    grep -qx "make lint-xml" "$CALLS"
    grep -qx "make check-lvgl-event-codes" "$CALLS"
}

@test "the second sweep runs in CI's 8-shard layout, and CI_SHARDS changes it" {
    run "$GATE"
    [ "$status" -eq 0 ]
    grep -qx "make unit-sweep NPROCS=8" "$CALLS"
    : >"$CALLS"
    CI_SHARDS=12 run "$GATE"
    grep -qx "make unit-sweep NPROCS=12" "$CALLS"
}

@test "each failing step fails the gate and shows FAIL in its own row" {
    local target step
    for pair in app:app-build unit-sweep:sweep test-shell:bats test-ui-ci:ui test-plugin:plugin \
                check-theme-tokens:theme-tokens check-xml-schema:xml-schema lint-xml:lint-xml \
                check-lvgl-event-codes:lvgl-event-codes; do
        target=${pair%%:*} step=${pair#*:}
        eval "export RC_$(echo "$target" | tr - _)=3"
        run "$GATE"
        eval "unset RC_$(echo "$target" | tr - _)"
        [ "$status" -eq 1 ] || { echo "red $target did not fail the gate"; return 1; }
        row FAIL "$step" || { echo "no FAIL row for $step"; return 1; }
        contains "LAND GATE: FAIL" "$output"
        contains "output of $target" "$output"
    done
}

@test "a red CI-layout pass alone fails the gate while the local layout passes" {
    TEST_HOST=1 RC_host_ci=1 run "$GATE"
    [ "$status" -eq 1 ]
    row PASS sweep
    row FAIL ci-shards
}

@test "a failed app build skips the UI steps and fails the gate" {
    RC_app=2 run "$GATE"
    [ "$status" -eq 1 ]
    row FAIL app-build
    row SKIP ui
    row SKIP plugin
    refute_grep "make test-ui-ci" "$CALLS"
    row PASS bats
}

@test "TEST_HOST=1 sends both sweeps to the test host and runs neither here" {
    TEST_HOST=1 run "$GATE"
    [ "$status" -eq 0 ]
    grep -qx "host-run sweep" "$CALLS"
    grep -qx "host-run sweep NPROCS=8" "$CALLS"
    refute_grep "make unit-sweep" "$CALLS"
    grep -qx "make test-shell" "$CALLS"
    grep -qx "make test-ui-ci" "$CALLS"
}

@test "a bad TEST_HOST refuses before running anything" {
    TEST_HOST=maybe run "$GATE"
    [ "$status" -eq 2 ]
    [ ! -s "$CALLS" ]
}

@test "make land-gate hands off to scripts/land-gate.sh with CI_SHARDS" {
    run make -C "$REPO_ROOT" -n --no-print-directory land-gate
    [ "$status" -eq 0 ]
    contains "scripts/land-gate.sh" "$output"
    contains 'CI_SHARDS="8"' "$output"
}

# The gate is only worth its name while it runs what CI runs.
@test "test-ui-ci runs build.yml's exact tests/ui pytest invocation, headless" {
    local ci mk args
    ci=$(grep -E '^\s*run: pytest tests/ui/' "$REPO_ROOT/.github/workflows/build.yml" | sed 's/^ *run: pytest //')
    [ -n "$ci" ]
    args=$(make -C "$REPO_ROOT" -pn --no-print-directory test-ui-ci 2>/dev/null |
        sed -n 's/^UI_CI_PYTEST_ARGS := //p')
    [ "$args" = "$ci" ] || { printf 'CI:   %s\nmake: %s\n' "$ci" "$args"; return 1; }
    mk=$(make -C "$REPO_ROOT" -n --no-print-directory test-ui-ci 2>/dev/null)
    contains "pytest $ci;" "$mk"
    contains "HELIX_HEADLESS=1 SDL_VIDEODRIVER=dummy" "$mk"
}

@test "CI's unit-test step uses the filter the sweep passes, with explicit shard flags" {
    local ci
    ci=$(grep -F 'helix-tests "~[.] ~[slow]" --shard-count $NPROCS --shard-index $i' \
        "$REPO_ROOT/.github/workflows/build.yml")
    [ -n "$ci" ]
    grep -qF 'NPROCS=$(( $(nproc) * 2 ))' "$REPO_ROOT/.github/workflows/build.yml"
}
