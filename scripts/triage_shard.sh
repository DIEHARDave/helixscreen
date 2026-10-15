#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# triage_shard.sh BIN DIR SHARD COUNT FILTER [ORDER_FLAGS...]
#
# Post-mortem for one unit-sweep shard that failed, crashed or timed out
# (`diagnose_shards` in mk/tests.mk calls it per suspect shard). DIR holds the
# shard's console log as DIR/SHARD.log; FILTER and ORDER_FLAGS are what the
# sweep passed, so `--shard-count COUNT --shard-index SHARD` selects the same
# cases in the same order.
#
# It names the failing case(s) and re-runs them in up to four ways, cheapest
# first, stopping at the first that fails:
#
#   alone              the failing cases with nothing else in the process
#   alone, pinned      the same on one CPU (taskset -c)
#   composition        the whole shard again, same cases and order
#   composition, pinned
#
# and prints one verdict:
#
#   fails alone                      a real bug in the case or the code it covers
#   fails only pinned                timing: it needs a slow or contended CPU
#   fails only in shard composition  order dependence or state leaked by an
#                                    earlier case in the shard
#   did not reproduce
#
# An idle machine with free cores is the condition a timing flake never fails
# under, which is why the pinned runs exist: one core makes every thread the
# test starts compete with it, as a loaded sweep does.
#
# Bounded: each run is capped by TRIAGE_TIMEOUT seconds (default 60) and the
# whole triage by TRIAGE_BUDGET (default 150); a stage that would start past
# the budget is skipped and named. TRIAGE_RETRIES (default 3) runs per stage.
# TRIAGE_CPU picks the core (default: the first one this process may use).
# Without taskset (macOS) the pinned stages are skipped and say so.
#
# Always exits 0: the sweep's own verdict decides whether the run failed.

set -u

if [ $# -lt 5 ]; then
    echo "usage: $0 BIN DIR SHARD COUNT FILTER [ORDER_FLAGS...]" >&2
    exit 2
fi
bin=$1 dir=$2 shard=$3 count=$4 filter=$5
shift 5
order=("$@")

here=$(cd "$(dirname "$0")" && pwd)
retries=${TRIAGE_RETRIES:-3}
run_timeout=${TRIAGE_TIMEOUT:-60}
budget=${TRIAGE_BUDGET:-150}
taskset_cmd=${TRIAGE_TASKSET:-taskset}
log="$dir/$shard.log"
started=$SECONDS

timeout_cmd=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)

# One core this process is allowed to run on: the first entry of its affinity
# list ("0-31", "3,5-7"), or 0.
pick_cpu() {
    if [ -n "${TRIAGE_CPU:-}" ]; then
        echo "$TRIAGE_CPU"
        return
    fi
    local list
    list=$("$taskset_cmd" -cp $$ 2>/dev/null | sed -n 's/.*: *//p')
    list=${list%%[,-]*}
    case $list in '' | *[!0-9]*) echo 0 ;; *) echo "$list" ;; esac
}

pinned=()
if command -v "$taskset_cmd" >/dev/null 2>&1; then
    cpu=$(pick_cpu)
    pinned=("$taskset_cmd" -c "$cpu")
fi

shard_args=("$filter" ${order[@]+"${order[@]}"} --shard-count "$count" --shard-index "$shard")

# Print a command so it can be pasted back into a shell: plain words as they
# are, anything else single-quoted.
quote() {
    local a out=""
    for a in "$@"; do
        case $a in
            '' | *[!A-Za-z0-9_./:=,+@%-]*) a="'${a//\'/\'\\\'\'}'" ;;
        esac
        out+="$a "
    done
    printf '%s' "${out% }"
}

# --- what the shard ran, and what failed --------------------------------------
listing=$("$bin" "${shard_args[@]}" --list-tests --reporter xml 2>/dev/null)
printf '%s' "$listing" | python3 "$here/catch2_shard_tests.py" > "$dir/$shard.tests" 2>/dev/null || true
printf '%s' "$listing" | python3 "$here/catch2_shard_tests.py" --failed-in "$log" \
    > "$dir/$shard.failed" 2>/dev/null || : > "$dir/$shard.failed"
n=$(wc -l < "$dir/$shard.tests" | tr -d ' ')
nfail=$(wc -l < "$dir/$shard.failed" | tr -d ' ')

echo "  ran ${n:-?} test case(s) -> $dir/$shard.tests ($bin --input-file replays it)"
asserts=$(grep -oE '^[A-Za-z0-9_/.-]+\.cpp:[0-9]+: FAILED' "$log" 2>/dev/null |
    sed 's/: FAILED$//' | sort -u | tr '\n' ' ')
if [ -n "$asserts" ]; then
    echo "  failing assertion(s): $asserts"
else
    echo "  no FAILED marker: died after its assertions passed (teardown/static dtor)"
fi
if [ "$nfail" -gt 0 ]; then
    echo "  failing case(s) -> $dir/$shard.failed:"
    sed 's/^/    /' "$dir/$shard.failed"
fi
echo "  shard repro: $(quote "$bin" "${shard_args[@]}")"

# --- re-runs ------------------------------------------------------------------
# stage NAME -- CMD...: run CMD up to $retries times. Prints the number of
# failing runs; keeps the last failing run's output as $dir/$shard.<NAME>.log.
# A run that selected no test case is not a failure of the case, and stops
# the stage (Catch2 says "No test cases matched").
stage_hits=0
stage_ran=0
stage() {
    local name=$1
    shift 2
    stage_hits=0
    stage_ran=0
    local out="$dir/$shard.$name.run.log" i rc limit
    for ((i = 1; i <= retries; i++)); do
        limit=$((budget - (SECONDS - started)))
        [ "$limit" -gt 0 ] || break
        [ "$limit" -lt "$run_timeout" ] || limit=$run_timeout
        if [ -n "$timeout_cmd" ]; then
            "$timeout_cmd" -k 5 "$limit" "$@" > "$out" 2>&1
        else
            "$@" > "$out" 2>&1
        fi
        rc=$?
        if grep -q "No test cases matched" "$out" 2>/dev/null; then
            echo "  (could not select the failing case(s) on their own; $name skipped)"
            break
        fi
        stage_ran=$((stage_ran + 1))
        if [ "$rc" -ne 0 ]; then
            stage_hits=$((stage_hits + 1))
            mv -f "$out" "$dir/$shard.$name.log"
        fi
    done
    rm -f "$out"
    if [ "$stage_ran" -gt 0 ]; then
        echo "  $name: failed $stage_hits/$stage_ran"
    fi
}

verdict=""
repro=""
repro_log=""
skipped=()
try_stage() { # name description -- cmd...
    local name=$1 desc=$2
    shift 2
    if [ -n "$verdict" ]; then
        return
    fi
    if [ $((SECONDS - started)) -ge "$budget" ]; then
        skipped+=("$name (budget)")
        return
    fi
    stage "$name" "$@"
    if [ "$stage_hits" -gt 0 ]; then
        verdict=$desc
        shift
        repro=$(quote "$@")
        repro_log="$dir/$shard.$name.log"
    fi
}

alone=("$bin" --input-file "$dir/$shard.failed")
whole=("$bin" "${shard_args[@]}")

if [ "$nfail" -gt 0 ]; then
    try_stage alone "fails alone: a real bug in the case or the code it covers" -- "${alone[@]}"
fi
if [ "$nfail" -gt 0 ] && [ ${#pinned[@]} -gt 0 ]; then
    try_stage alone-pinned "fails only pinned: timing, it needs a slow or contended CPU" -- \
        "${pinned[@]}" "${alone[@]}"
fi
try_stage composition \
    "fails only in shard composition: order dependence or state leaked by an earlier case" -- \
    "${whole[@]}"
if [ ${#pinned[@]} -gt 0 ]; then
    try_stage composition-pinned \
        "fails only pinned: timing, it needs a slow or contended CPU (and this shard's cases)" -- \
        "${pinned[@]}" "${whole[@]}"
fi

[ ${#pinned[@]} -gt 0 ] || skipped+=("pinned stages ($taskset_cmd not available)")

if [ -n "$verdict" ]; then
    echo "  verdict: $verdict"
    echo "  repro: $repro"
    echo "  repro log: $repro_log"
    tail -25 "$repro_log" 2>/dev/null | sed 's/^/     | /'
else
    echo "  verdict: did not reproduce ($retries run(s) per stage, $((SECONDS - started))s)"
    echo "     An intermittent fault can pass every re-run: this does not prove the diff"
    echo "     is clean. Raise TRIAGE_RETRIES or TRIAGE_BUDGET to sample harder."
fi
if [ ${#skipped[@]} -gt 0 ]; then
    echo "  skipped: ${skipped[*]}"
fi
exit 0
