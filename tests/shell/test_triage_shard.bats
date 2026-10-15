#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# scripts/triage_shard.sh, the per-shard post-mortem the unit sweep runs on a
# shard that failed. It names the failing case from the shard's console log,
# re-runs it alone, alone on one pinned CPU, as the whole shard, and as the
# whole shard pinned, and prints one verdict.
#
# The fake binary lists three cases for the shard (one with a comma, which only
# reaches --input-file intact escaped) and decides each run from FAKE_MODE:
#   alone        VICTIM fails whenever it runs
#   composition  VICTIM fails only after POLLUTER ran in the same process
#   pinned       VICTIM fails only under the fake taskset
#   composition-pinned  both conditions at once
#   never        every re-run passes
# The fake taskset reports an affinity list and records the core it pinned to.

load helpers

SCRIPT="scripts/triage_shard.sh"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    DIR="$BATS_TEST_TMPDIR/shards"
    mkdir -p "$DIR"
    FAKE="$BATS_TEST_TMPDIR/fake-tests"
    CALLS="$BATS_TEST_TMPDIR/calls.log"
    export CALLS
    cat > "$FAKE" <<'EOF'
#!/usr/bin/env bash
echo "$* pinned=${FAKE_PINNED:-0}" >> "$CALLS"
input="" listing=0 shard=""
while [ $# -gt 0 ]; do
    case "$1" in
        --input-file) input=$2; shift ;;
        --list-tests) listing=1 ;;
        --shard-index) shard=$2; shift ;;
    esac
    shift
done
if [ "$listing" -eq 1 ]; then
    cat <<'XML'
<?xml version="1.0" encoding="UTF-8"?>
<MatchingTests>
  <TestCase><Name>polluter</Name><SourceInfo><File>tests/unit/test_a.cpp</File><Line>10</Line></SourceInfo></TestCase>
  <TestCase><Name>bystander, with a comma</Name><SourceInfo><File>tests/unit/test_a.cpp</File><Line>20</Line></SourceInfo></TestCase>
  <TestCase><Name>victim</Name><SourceInfo><File>tests/unit/test_b.cpp</File><Line>30</Line></SourceInfo></TestCase>
</MatchingTests>
XML
    exit 0
fi
if [ -n "$input" ]; then
    selected=$(sed -e 's/\\\(.\)/\1/g' "$input")
    if ! printf '%s\n' "$selected" | grep -qx victim; then
        echo "No test cases matched '$(head -1 "$input")'"
        exit 2
    fi
else
    selected=$(printf '%s\n' polluter "bystander, with a comma" victim)
fi
has() { printf '%s\n' "$selected" | grep -qx "$1"; }
fail=0
case ${FAKE_MODE:-never} in
    alone) fail=1 ;;
    composition) has polluter && fail=1 ;;
    pinned) [ "${FAKE_PINNED:-0}" = 1 ] && fail=1 ;;
    composition-pinned) has polluter && [ "${FAKE_PINNED:-0}" = 1 ] && fail=1 ;;
esac
if [ "$fail" = 1 ]; then
    echo "tests/unit/test_b.cpp:33: FAILED:"
    echo "test cases: 1 | 1 failed"
    exit 1
fi
echo "All tests passed"
exit 0
EOF
    chmod +x "$FAKE"
    TASKSET="$BATS_TEST_TMPDIR/fake-taskset"
    PINS="$BATS_TEST_TMPDIR/pins.log"
    export PINS
    cat > "$TASKSET" <<'EOF'
#!/usr/bin/env bash
if [ "$1" = -cp ]; then
    echo "pid $2's current affinity list: 4-7,9"
    exit 0
fi
[ "$1" = -c ] || exit 64
echo "$2" >> "$PINS"
shift 2
FAKE_PINNED=1 exec "$@"
EOF
    chmod +x "$TASKSET"
    export TRIAGE_TASKSET="$TASKSET"
    unset TRIAGE_CPU TRIAGE_RETRIES TRIAGE_BUDGET TRIAGE_TIMEOUT FAKE_MODE
    write_failing_log
}

# The shard's console log: the victim's header naming a section, the
# section's location (not the case's), and a FAILED line.
write_failing_log() {
    local rule
    rule=$(printf -- '-%.0s' $(seq 79))
    cat > "$DIR/5.log" <<EOF
=== shard 5/12 host=fake nproc=32 git=abc1234 ts=now order=decl seed=0
[spdlog] noise that is not a header
$rule
victim
  a section
$rule
tests/unit/test_b.cpp:31
...............................................................................

tests/unit/test_b.cpp:33: FAILED:
  REQUIRE( icon_h > 0 )
test cases: 3 | 2 passed | 1 failed
EOF
}

triage() {
    run bash "$SCRIPT" "$FAKE" "$DIR" 5 12 "~[.] ~[slow]" "$@"
}

@test "a case that fails on its own is a real bug, and is the only stage that runs" {
    FAKE_MODE=alone triage
    [ "$status" -eq 0 ]
    contains "verdict: fails alone" "$output"
    contains "alone: failed 3/3" "$output"
    lacks "composition:" "$output"
    [ "$(cat "$DIR/5.failed")" = victim ]
    contains "repro: $FAKE --input-file $DIR/5.failed" "$output"
    [ ! -e "$PINS" ]
}

@test "a case that fails only on one core is timing, pinned to a core this process may use" {
    FAKE_MODE=pinned triage
    contains "alone: failed 0/3" "$output"
    contains "alone-pinned: failed 3/3" "$output"
    contains "verdict: fails only pinned" "$output"
    contains "repro: $TASKSET -c 4 $FAKE --input-file" "$output"
    [ "$(sort -u "$PINS")" = 4 ]
}

@test "a case that fails only after an earlier case is order dependence, with the shard repro" {
    FAKE_MODE=composition triage --order rand --rng-seed 29499066
    contains "alone: failed 0/3" "$output"
    contains "alone-pinned: failed 0/3" "$output"
    contains "composition: failed 3/3" "$output"
    contains "verdict: fails only in shard composition" "$output"
    contains "repro: $FAKE '~[.] ~[slow]' --order rand --rng-seed 29499066 --shard-count 12 --shard-index 5" "$output"
    # The whole-shard re-runs carry the sweep's own order flags.
    grep -q -- "--order rand --rng-seed 29499066 --shard-count 12 --shard-index 5 pinned=0" "$CALLS"
}

@test "a shard that needs both its composition and one core is reported as timing" {
    FAKE_MODE=composition-pinned triage
    contains "composition: failed 0/3" "$output"
    contains "composition-pinned: failed 3/3" "$output"
    contains "verdict: fails only pinned" "$output"
    contains "repro: $TASKSET -c 4 $FAKE" "$output"
}

@test "nothing reproducing says so and does not claim the diff is clean" {
    FAKE_MODE=never TRIAGE_RETRIES=2 triage
    contains "alone: failed 0/2" "$output"
    contains "composition-pinned: failed 0/2" "$output"
    contains "verdict: did not reproduce" "$output"
    contains "does not prove the diff" "$output"
}

@test "TRIAGE_CPU picks the pinned core" {
    FAKE_MODE=pinned TRIAGE_CPU=2 triage
    contains "repro: $TASKSET -c 2 " "$output"
}

@test "without taskset the pinned stages are skipped and named" {
    FAKE_MODE=pinned TRIAGE_TASKSET="$BATS_TEST_TMPDIR/no-such-taskset" triage
    [ "$status" -eq 0 ]
    lacks "pinned: failed" "$output"
    contains "verdict: did not reproduce" "$output"
    contains "pinned stages (" "$output"
}

@test "a shard that died with no FAILED case skips the alone stages" {
    printf '=== shard 5/12\nSegmentation fault\n' > "$DIR/5.log"
    FAKE_MODE=composition triage
    contains "no FAILED marker" "$output"
    lacks "alone: failed" "$output"
    contains "verdict: fails only in shard composition" "$output"
    [ ! -s "$DIR/5.failed" ]
}

@test "the budget bounds the triage and names the stages it skipped" {
    FAKE_MODE=never TRIAGE_BUDGET=0 triage
    [ "$status" -eq 0 ]
    contains "skipped: alone (budget)" "$output"
    lacks "failed 0/" "$output"
}

@test "the sweep's diagnostics run the triage on a failing shard" {
    FAKE_MODE=alone run make --no-print-directory TEST_BIN="$FAKE" SHARD_RETRIES=1 \
        --eval 'triage-under-test: ; @$(call diagnose_shards,'"$DIR"',5,"~[.]",12,)' \
        triage-under-test
    [ "$status" -eq 0 ]
    contains "shard diagnostics" "$output"
    contains "alone: failed 1/1" "$output"
    contains "verdict: fails alone" "$output"
}
