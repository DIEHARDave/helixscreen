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

ALL_STEPS="app-build sweep ci-shards bats ui plugin python theme-tokens xml-schema lint-xml lvgl-event-codes"

# Every make here is a stub, and a stub must never reach the real make or the
# jobpool shim: JOBPOOL=0 keeps the shim out of any make that does run, and the
# stubs refuse to nest (FAKE_MAKE_DEPTH), so a stub that re-enters make fails
# loudly instead of forking without bound.
setup() {
    export JOBPOOL=0
    unset FAKE_MAKE_DEPTH LAND_GATE_ACTIVE
    export TMPDIR="$BATS_TEST_TMPDIR"
    export CALLS="$BATS_TEST_TMPDIR/calls"
    export LAND_GATE_LOGDIR="$BATS_TEST_TMPDIR/logs"
    : >"$CALLS"
    # A target exits with $RC_<target, - as _>, default 0; no target is "app".
    mock_command_script fake-make '
[ "${FAKE_MAKE_DEPTH:-0}" -lt 1 ] || { echo "fake-make re-entered" >&2; exit 97; }
export FAKE_MAKE_DEPTH=$((${FAKE_MAKE_DEPTH:-0} + 1))
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
    # take exits $CLAIM_TAKE_RC (default 0); every call is recorded.
    mock_command_script fake-claim '
echo "claim $*" >> "$CALLS"
case "$1" in take) exit "${CLAIM_TAKE_RC:-0}" ;; esac'
    export MAKE=fake-make TEST_HOST_RUN=fake-test-host-run HELIX_CLAIM=fake-claim
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
    grep -qx "make test-shell BATS_REQUIRED=1" "$CALLS"
    grep -qx "make test-ui-ci" "$CALLS"
    grep -qx "make test-plugin" "$CALLS"
    grep -qx "make test-python" "$CALLS"
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
    for pair in app:app-build unit-sweep:sweep test-shell:bats test-ui-ci:ui test-plugin:plugin test-python:python \
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
    grep -qx "make test-shell BATS_REQUIRED=1" "$CALLS"
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

@test "a step past LAND_GATE_STEP_TIMEOUT is killed and reads TIMEOUT, failing the gate" {
    mock_command_script fake-make '
[ "${FAKE_MAKE_DEPTH:-0}" -lt 1 ] || { echo "fake-make re-entered" >&2; exit 97; }
export FAKE_MAKE_DEPTH=$((${FAKE_MAKE_DEPTH:-0} + 1))
t=app; for a in "$@"; do case "$a" in -*|*=*) ;; *) t=$a ;; esac; done
echo "make $t" >> "$CALLS"
[ "$t" = lint-xml ] && sleep 30
exit 0'
    LAND_GATE_STEP_TIMEOUT=1 run "$GATE"
    [ "$status" -eq 1 ]
    row TIMEOUT lint-xml
    row PASS bats
}

@test "the gate holds build:<tree> for the run and releases it; a held tree refuses" {
    run "$GATE"
    [ "$status" -eq 0 ]
    grep -q "^claim take build:" "$CALLS"
    grep -q "^claim release-if-owned-by .* build:" "$CALLS"
    : >"$CALLS"
    CLAIM_TAKE_RC=1 run "$GATE"
    [ "$status" -eq 2 ]
    contains "is held" "$output"
    refute_grep "^make" "$CALLS"
}

@test "a tree claimed by the session running the gate is taken as that session" {
    # take succeeds only for the pid named in $CLAIM_OWNER, as helix-claim
    # exempts the holder of the sibling worktree: claim.
    mock_command_script fake-claim '
echo "claim $*" >> "$CALLS"
case "$1" in take) [ "$5" = "$CLAIM_OWNER" ] ;; esac'
    CLAIM_OWNER=$$ run "$GATE"
    [ "$status" -eq 0 ]
    grep -q "^claim take build:.* --pid $$\$" "$CALLS"
    grep -q "^claim release-if-owned-by $$ build:" "$CALLS"
}

@test "the verdict names the commit, marked +dirty when the tree has uncommitted changes" {
    local T="$BATS_TEST_TMPDIR/tree"
    git init -q "$T"
    echo a >"$T/f"; git -C "$T" add f
    git -C "$T" -c user.email=t@t -c user.name=t commit -q -m init
    local sha; sha=$(git -C "$T" rev-parse --short HEAD)
    run bash -c "cd '$T' && '$GATE'"
    [ "$status" -eq 0 ]
    contains "Land gate on $sha," "$output"
    lacks "+dirty" "$output"
    echo b >"$T/f"
    run bash -c "cd '$T' && '$GATE'"
    contains "Land gate on $sha+dirty" "$output"
    contains "LAND GATE: PASS ($sha+dirty)" "$output"
}

@test "test-shell with BATS_REQUIRED=1 fails when bats is missing; without it, skips" {
    run make -C "$REPO_ROOT" --no-print-directory test-shell BATS_BIN=no-such-bats BATS_REQUIRED=1
    [ "$status" -ne 0 ]
    contains "BATS_REQUIRED=1" "$output"
    run make -C "$REPO_ROOT" --no-print-directory test-shell BATS_BIN=no-such-bats
    [ "$status" -eq 0 ]
    contains "skipping shell tests" "$output"
}

# A stubbed step that re-enters the gate (a make target running land-gate, or a
# stub calling make again) must end in a FAIL row, never in unbounded recursion.
@test "stubbed lanes cannot recurse: a nested gate refuses and a re-entering stub stops" {
    mock_command_script fake-make '
[ "${FAKE_MAKE_DEPTH:-0}" -lt 1 ] || { echo "fake-make re-entered" >&2; exit 97; }
export FAKE_MAKE_DEPTH=$((${FAKE_MAKE_DEPTH:-0} + 1))
t=app; for a in "$@"; do case "$a" in -*|*=*) ;; *) t=$a ;; esac; done
echo "make $t" >> "$CALLS"
case "$t" in
    test-shell) "$NESTED_GATE"; exit $? ;;
    lint-xml) fake-make --no-print-directory again; exit $? ;;
esac
exit 0'
    NESTED_GATE="$GATE" run /usr/bin/timeout 120 "$GATE"
    [ "$status" -eq 1 ]
    row FAIL bats
    row FAIL lint-xml
    grep -q "land-gate is already running" "$LAND_GATE_LOGDIR/bats.log"
    grep -q "fake-make re-entered" "$LAND_GATE_LOGDIR/lint-xml.log"
    [ "$(grep -c '^make' "$CALLS")" -lt 20 ]
}
