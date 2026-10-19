#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The HelixPrint plugin install/uninstall matrix, against real Moonrakers in
# containers: one image per Moonraker vintage (tests/plugin-host/Dockerfile),
# one container per cell, laid out as one kind of printer
# (tests/plugin-host/cell.sh). No printer, no Klipper.
#
#   scripts/plugin-host-matrix.sh                       # every vintage x layout
#   scripts/plugin-host-matrix.sh --vintages v0.9.3 --layouts "plain k1"
#   scripts/plugin-host-matrix.sh --build-only          # just the images
#   scripts/test-host-run.sh plugin-host [args...]      # the same, on the test host
#
# Options:
#   --vintages "A B"   Moonraker refs (default: v0.8.0 v0.9.3 master)
#   --layouts "A B"    cell.sh layouts (default: plain legacy k1 forgex modconf incsec)
#   --out DIR          results (default: build/plugin-host/<timestamp>)
#   --no-strict        run install.sh unconfined even where its service confines it
#   --keep             leave each cell's container running for a look inside
#   --build-only       build the images and stop
#
# Each cell, in order; a failed up or install ends the cell, and every other
# step runs whatever came before it, so one cell can show several failures:
#   up         Moonraker answers /server/info; its warnings and failed
#              components are the baseline
#   install    the plugin copied into INSTALL_DIR, install.sh --no-restart
#              --auto as the app user exits 0
#   loaded     after POST /server/restart: helix_print in components,
#              /server/helix/status answers, no failed component, no warning
#              the baseline lacked
#   reinstall  a second install exits 0 and leaves one [helix_print]
#   uninstall  install.sh --no-restart --uninstall-auto exits 0
#   unloaded   after a restart: helix_print gone from components and failed
#              components, no new warning, the symlink gone
#   pristine   every config file byte-identical to the pre-install copy,
#              *.bak* backups aside (they are listed in the cell's results)
#
# DOCKER names the docker client (default docker; the test host passes
# "sudo -n docker"). Exits 0 when every cell passes, 1 when any fails, 2 on a
# usage or build error.
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
HARNESS="$REPO/tests/plugin-host"
read -r -a DOCKER <<< "${DOCKER:-docker}"

VINTAGES="v0.8.0 v0.9.3 master"
ALL_LAYOUTS="plain legacy k1 forgex modconf incsec"
LAYOUTS=$ALL_LAYOUTS
OUT=""
STRICT=1
KEEP=false
BUILD_ONLY=false
UP_SECS=120          # a first start creates the database
RESTART_SECS=90

usage() { sed -n '3,/^set -uo pipefail$/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 2; }

while [ $# -gt 0 ]; do
    case "$1" in
        --vintages) VINTAGES=${2:?}; shift 2 ;;
        --layouts) LAYOUTS=${2:?}; shift 2 ;;
        --out) OUT=${2:?}; shift 2 ;;
        --no-strict) STRICT=0; shift ;;
        --keep) KEEP=true; shift ;;
        --build-only) BUILD_ONLY=true; shift ;;
        -h|--help) usage ;;
        *) echo "plugin-host-matrix: unknown argument $1" >&2; usage ;;
    esac
done
for l in $LAYOUTS; do
    case " $ALL_LAYOUTS " in *" $l "*) ;; *) echo "plugin-host-matrix: unknown layout $l" >&2; exit 2 ;; esac
done
command -v jq >/dev/null 2>&1 || { echo "plugin-host-matrix: needs jq" >&2; exit 2; }
OUT=${OUT:-$REPO/build/plugin-host/$(date +%Y%m%d-%H%M%S)}
mkdir -p "$OUT"

ts() { date '+%H:%M:%S'; }
say() { printf '%s %s\n' "$(ts)" "$*"; }

# The Python each vintage was released against: v0.8.0's pinned wheels
# predate Python 3.11.
python_for() { # <vintage>
    case "$1" in
        v0.8.*) echo python:3.9-slim-bookworm ;;
        *) echo python:3.11-slim-bookworm ;;
    esac
}

image_for() { echo "helix-plugin-host:$1"; }

# master is built at the commit it names now, so the cache follows it.
build_image() { # <vintage>
    local v=$1 ref=$1
    if [ "$v" = master ]; then
        ref=$(git ls-remote https://github.com/Arksine/moonraker.git refs/heads/master | cut -c1-40)
        [ -n "$ref" ] || { say "✗ cannot resolve Moonraker master"; return 1; }
    fi
    say "→ image $(image_for "$v") (Moonraker $ref, $(python_for "$v"))"
    "${DOCKER[@]}" build --progress=plain -t "$(image_for "$v")" \
        --build-arg PYTHON_IMAGE="$(python_for "$v")" --build-arg MOONRAKER_REF="$ref" \
        "$HARNESS" > "$OUT/build-$v.log" 2>&1 || { say "✗ image build failed: $OUT/build-$v.log"; cat "$OUT/build-$v.log"; return 1; }
}

for v in $VINTAGES; do build_image "$v" || exit 2; done
$BUILD_ONLY && exit 0

# --- one cell --------------------------------------------------------------------

C=""          # the cell's container
CDIR=""       # the cell's results
# A cell's container goes with the run, however the run ends.
cleanup() { [ -z "$C" ] || $KEEP || "${DOCKER[@]}" rm -f "$C" > /dev/null 2>&1; }
trap cleanup EXIT
trap 'exit 130' INT TERM HUP
x() { "${DOCKER[@]}" exec "$C" "$@"; }
info() { x curl -s --max-time 5 http://127.0.0.1:7125/server/info; }

# Wait until Moonraker answers /server/info with a result.
wait_up() { # <seconds>
    local end=$(( $(date +%s) + $1 ))
    while [ "$(date +%s)" -lt "$end" ]; do
        if info 2>/dev/null | jq -e '.result.components' >/dev/null 2>&1; then return 0; fi
        sleep 1
    done
    return 1
}

# Ask Moonraker to restart and wait for it to answer again. A vintage that
# restarts in place answers between the request and the restart, so the wait
# starts after Moonraker has stopped answering, or after a few seconds.
restart() {
    x curl -s --max-time 5 -X POST http://127.0.0.1:7125/server/restart > /dev/null 2>&1 || true
    local n=0
    while [ "$n" -lt 10 ] && info >/dev/null 2>&1; do
        sleep 0.5
        n=$((n + 1))
    done
    wait_up "$RESTART_SECS"
}

# Wait until server/info's components (or failed components) <want: has|lacks> helix_print.
wait_component() { # <has|lacks> <seconds>
    local end=$(( $(date +%s) + $2 )) j
    while [ "$(date +%s)" -lt "$end" ]; do
        j=$(info 2>/dev/null) || j=""
        case "$1" in
            has) printf '%s' "$j" | jq -e '(.result.components + (.result.failed_components // [])) | index("helix_print")' >/dev/null 2>&1 && return 0 ;;
            lacks) printf '%s' "$j" | jq -e '.result.components and ((.result.components + (.result.failed_components // [])) | index("helix_print") | not)' >/dev/null 2>&1 && return 0 ;;
        esac
        sleep 1
    done
    return 1
}

STEP_LOG=""
record() { # <step> <PASS|FAIL> <detail>
    printf '%-10s %s %s\n' "$1" "$2" "$3" >> "$STEP_LOG"
    say "  $1: $2${3:+ - $3}"
    [ "$2" = PASS ]
}

# The warnings in server/info <json> that <baseline json> lacks.
new_warnings() { # <json> <baseline json>
    jq -n -r --argjson a "$1" --argjson b "$2" '($a.result.warnings // []) - ($b.result.warnings // []) | .[]'
}

check_loaded() { # <baseline json>
    local j failed new
    if ! wait_component has 30; then
        record loaded FAIL "helix_print never appeared after the restart"; return 1
    fi
    j=$(info)
    printf '%s\n' "$j" | jq . > "$CDIR/server_info_installed.json"
    failed=$(printf '%s' "$j" | jq -r '.result.failed_components // [] | join(" ")')
    new=$(new_warnings "$j" "$1")
    x curl -s --max-time 5 http://127.0.0.1:7125/server/helix/status > "$CDIR/helix_status.json" 2>&1
    if [ -n "$failed" ]; then record loaded FAIL "failed_components: $failed; warnings: ${new:-none}"; return 1; fi
    if ! printf '%s' "$j" | jq -e '.result.components | index("helix_print")' >/dev/null; then
        record loaded FAIL "helix_print not in components"; return 1
    fi
    if [ -n "$new" ]; then record loaded FAIL "new warnings: $(printf '%s' "$new" | tr '\n' '|')"; return 1; fi
    if ! jq -e '.result' "$CDIR/helix_status.json" >/dev/null 2>&1; then
        record loaded FAIL "/server/helix/status: $(head -c 200 "$CDIR/helix_status.json")"; return 1
    fi
    record loaded PASS ""
}

check_unloaded() { # <baseline json>
    local j new link
    if ! wait_component lacks 30; then
        j=$(info 2>/dev/null)
        record unloaded FAIL "helix_print still listed: components $(printf '%s' "$j" | jq -c '.result.components'), failed $(printf '%s' "$j" | jq -c '.result.failed_components')"
        return 1
    fi
    j=$(info)
    printf '%s\n' "$j" | jq . > "$CDIR/server_info_uninstalled.json"
    new=$(new_warnings "$j" "$1")
    link=$(x sh /harness/cell.sh link)
    if [ -n "$new" ]; then record unloaded FAIL "new warnings: $(printf '%s' "$new" | tr '\n' '|')"; return 1; fi
    if [ "$link" != absent ]; then record unloaded FAIL "plugin link left: $link"; return 1; fi
    record unloaded PASS ""
}

run_cell() { # <vintage> <layout>
    local v=$1 l=$2 name rc
    name="$v-$l"
    CDIR="$OUT/$name"
    STEP_LOG="$CDIR/steps.txt"
    mkdir -p "$CDIR"
    : > "$STEP_LOG"
    C="helix-plugin-host-${name//./_}-$$"
    say "▶ $name"
    # SYS_ADMIN for the Forge-X bind mounts, and seccomp/apparmor unconfined so
    # bwrap may create the user namespace ProtectSystem=strict is rebuilt in.
    "${DOCKER[@]}" run -d --name "$C" --cap-add SYS_ADMIN \
        --security-opt seccomp=unconfined --security-opt apparmor=unconfined \
        -e CELL_STRICT="$STRICT" \
        -v "$HARNESS:/harness:ro" -v "$REPO/moonraker-plugin:/plugin-src:ro" \
        "$(image_for "$v")" sh /harness/cell.sh start "$l" > /dev/null || { record up FAIL "docker run failed"; return 1; }

    cell_steps
    rc=$?
    x cat /var/log/moonraker.out > "$CDIR/moonraker.out" 2>&1
    # shellcheck disable=SC2016  # expands in the container
    x sh -c '. /run/cell.env; cat "$(dirname "$CONFIG_DIR")/logs/moonraker.log"' > "$CDIR/moonraker.log" 2>&1
    x sh /harness/cell.sh env > "$CDIR/cell.env" 2>&1
    if $KEEP; then say "  kept container $C"; else "${DOCKER[@]}" rm -f "$C" > /dev/null; fi
    C=""
    if [ "$rc" -eq 0 ]; then echo PASS > "$CDIR/result"; else echo FAIL > "$CDIR/result"; fi
    return "$rc"
}

cell_steps() {
    local base rc n
    if ! wait_up "$UP_SECS"; then record up FAIL "no /server/info within ${UP_SECS}s"; return 1; fi
    base=$(info)
    printf '%s\n' "$base" | jq . > "$CDIR/server_info_baseline.json"
    record up PASS "moonraker $(printf '%s' "$base" | jq -r '.result.moonraker_version'), baseline failed [$(printf '%s' "$base" | jq -r '.result.failed_components // [] | join(" ")')], $(printf '%s' "$base" | jq -r '.result.warnings // [] | length') warning(s)"

    x sh /harness/cell.sh stage
    x sh /harness/cell.sh install > "$CDIR/install.out" 2>&1
    rc=$?
    if [ "$rc" -ne 0 ]; then record install FAIL "exit $rc: $(grep -m1 -E 'ERROR|WARN' "$CDIR/install.out" | sed 's/\x1b\[[0-9;]*m//g')"; return 1; fi
    record install PASS "$(x sh /harness/cell.sh link)"

    local ok=0
    if restart; then check_loaded "$base" || ok=1
    else record loaded FAIL "Moonraker did not come back after the restart"; ok=1
    fi

    x sh /harness/cell.sh install > "$CDIR/reinstall.out" 2>&1
    rc=$?
    x sh /harness/cell.sh sections > "$CDIR/sections.txt" 2>&1
    n=$(awk '{ s += $1 } END { print s + 0 }' "$CDIR/sections.txt")
    if [ "$rc" -ne 0 ]; then record reinstall FAIL "exit $rc"; ok=1
    elif [ "$n" -ne 1 ]; then record reinstall FAIL "$n [helix_print] sections: $(tr '\n' ' ' < "$CDIR/sections.txt")"; ok=1
    else record reinstall PASS ""
    fi

    x sh /harness/cell.sh uninstall > "$CDIR/uninstall.out" 2>&1
    rc=$?
    if [ "$rc" -ne 0 ]; then record uninstall FAIL "exit $rc: $(grep -m1 -E 'ERROR|WARN' "$CDIR/uninstall.out" | sed 's/\x1b\[[0-9;]*m//g')"; ok=1
    else record uninstall PASS ""
    fi

    if restart; then check_unloaded "$base" || ok=1
    else record unloaded FAIL "Moonraker did not come back after the restart"; ok=1
    fi

    x sh /harness/cell.sh backups > "$CDIR/backups.txt" 2>&1
    if ! x sh /harness/cell.sh confdiff > "$CDIR/confdiff.txt" 2>&1; then
        record pristine FAIL "config differs from the pre-install copy: $(grep -v '^diff ' "$CDIR/confdiff.txt" | tr '\n' ' ' | cut -c1-160)"
        return 1
    fi
    record pristine PASS "$(grep -c '' "$CDIR/backups.txt") backup(s) left"
    return "$ok"
}

fails=0
for v in $VINTAGES; do
    for l in $LAYOUTS; do
        run_cell "$v" "$l" || fails=$((fails + 1))
    done
done

# --- summary -----------------------------------------------------------------------
{
    printf '\n%-18s %-6s %s\n' CELL RESULT "FAILED STEPS"
    for v in $VINTAGES; do
        for l in $LAYOUTS; do
            d="$OUT/$v-$l"
            printf '%-18s %-6s %s\n' "$v-$l" "$(cat "$d/result" 2>/dev/null || echo '?')" \
                "$(awk '$2 == "FAIL" { printf "%s%s", sep, $1; sep = " " }' "$d/steps.txt" 2>/dev/null)"
        done
    done
    printf '\nresults: %s\n' "$OUT"
} | tee "$OUT/summary.txt"
[ "$fails" -eq 0 ]
