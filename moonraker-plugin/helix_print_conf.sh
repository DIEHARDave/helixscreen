#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Where the [helix_print] section lives in a Moonraker config, and how this
# host reaches Moonraker over HTTP: shared by the plugin's install.sh and the
# main installer (bundled into it from here), so both find the section in the
# same files and talk to Moonraker the same way.

[ -n "${_HELIX_PRINT_CONF_SOURCED:-}" ] && return 0
_HELIX_PRINT_CONF_SOURCED=1

# Print every Moonraker config file reachable from @1 through [include] lines,
# @1 first, one per line, each once. A relative include resolves against the
# directory of the file that names it, as the path was given (a symlinked
# moonraker.conf resolves against the link's directory). A glob expands the way
# Moonraker expands it. @2, when set, is prefixed to an absolute include: the
# root Moonraker runs under, seen from here. A file is walked once however
# many paths lead to it, so an include cycle ends. Paths holding blanks are
# not supported; no Moonraker layout uses them.
moonraker_conf_files() {
    _mcf_root="${2:-}"
    _mcf_todo="$1"
    _mcf_seen=" "
    while [ -n "$_mcf_todo" ]; do
        _mcf_file="${_mcf_todo%% *}"
        case "$_mcf_todo" in
            *" "*) _mcf_todo="${_mcf_todo#* }" ;;
            *) _mcf_todo="" ;;
        esac
        [ -n "$_mcf_file" ] || continue
        [ -f "$_mcf_file" ] || continue
        _mcf_key=$(readlink -f "$_mcf_file" 2>/dev/null) || _mcf_key=""
        [ -n "$_mcf_key" ] || _mcf_key="$_mcf_file"
        case "$_mcf_seen" in *" $_mcf_key "*) continue ;; esac
        _mcf_seen="$_mcf_seen$_mcf_key "
        printf '%s\n' "$_mcf_file"
        _mcf_dir=$(dirname "$_mcf_file")
        # Trailing blanks after "]" include the CR of a file saved on Windows.
        _mcf_incs=$(sed -n 's/^\[include[[:space:]]\{1,\}\([^]]*[^][:space:]]\)[[:space:]]*\][[:space:]]*$/\1/p' "$_mcf_file" 2>/dev/null)
        while IFS= read -r _mcf_pat; do
            [ -n "$_mcf_pat" ] || continue
            case "$_mcf_pat" in
                /*) _mcf_pat="$_mcf_root$_mcf_pat" ;;
                *) _mcf_pat="$_mcf_dir/$_mcf_pat" ;;
            esac
            # Unquoted on purpose: the pattern expands here, against its own directory.
            for _mcf_inc in $_mcf_pat; do
                if [ -f "$_mcf_inc" ]; then
                    _mcf_todo="${_mcf_todo:+$_mcf_todo }$_mcf_inc"
                fi
            done
        done <<MCF_INCLUDES
$_mcf_incs
MCF_INCLUDES
    done
    return 0
}

# Print each file in moonraker_conf_files @1 @2 holding a line that matches
# extended regex @3, one per line; nothing when none does.
helix_conf_files_matching() {
    moonraker_conf_files "$1" "${2:-}" | while IFS= read -r _hcm_file; do
        if grep -Eq "$3" "$_hcm_file" 2>/dev/null; then
            printf '%s\n' "$_hcm_file"
        fi
    done
    return 0
}

# Print the port Moonraker serves HTTP on: [server] port in the first of
# moonraker_conf_files @1 @2 that sets it, else 7125, Moonraker's default.
# Firmware moves it (some serve Moonraker on port 80).
moonraker_conf_port() {
    _mcp_port=$(moonraker_conf_files "$1" "${2:-}" | while IFS= read -r _mcp_file; do
        awk '/^\[/ { s = ($0 ~ /^\[server\][ \t\r]*$/) }
            s && /^port[ \t]*[:=]/ { sub(/^port[ \t]*[:=][ \t]*/, ""); if (match($0, /^[0-9]+/)) print substr($0, 1, RLENGTH); exit }' \
            "$_mcp_file" 2>/dev/null
    done | awk 'NR == 1')
    printf '%s' "${_mcp_port:-7125}"
}

# Print the URL Moonraker answers at from this host: MOONRAKER_URL when set,
# else loopback on moonraker_conf_port @1 @2.
moonraker_local_url() {
    if [ -n "${MOONRAKER_URL:-}" ]; then
        printf '%s' "$MOONRAKER_URL"
        return 0
    fi
    printf 'http://127.0.0.1:%s' "$(moonraker_conf_port "$1" "${2:-}")"
}

# The awk program that reads a process's argv, one element per line, and
# prints what makes it Moonraker: the moonraker.py it runs, or "-m" and the
# module it runs (python3 -m moonraker.moonraker on the CC1). Moonraker is a
# python interpreter (argv[0] python, python3, python3.11, ...); an init
# script, editor or log viewer whose argv names the file comes and goes
# beside it and is not Moonraker. scripts/helix-launcher.sh matches the same
# two shapes. Prints nothing for any other process.
# shellcheck disable=SC2016  # awk's own $
HELIX_MOONRAKER_ARGV_AWK='
    NR == 1 { n = split($0, part, "/"); if (part[n] !~ /^python[0-9.]*$/) exit; next }
    module { if ($0 ~ /^moonraker(\.moonraker)?$/) print "-m " $0; exit }
    $0 == "-m" { module = 1; next }
    /^-m/ { if ($0 ~ /^-mmoonraker(\.moonraker)?$/) print "-m " substr($0, 3); exit }
    /(^|\/)moonraker\.py$/ { print; exit }'

# Print the running Moonraker's /proc/<pid> directory under HELIX_PROC_ROOT
# (tests point it at a fixture tree), else /proc, and, on the next line, what
# HELIX_MOONRAKER_ARGV_AWK printed for it: the moonraker.py its argv names, or
# "-m <module>". Returns 1, printing nothing, when no Moonraker runs.
running_moonraker_process() {
    _rmp_root="${HELIX_PROC_ROOT:-/proc}"
    for _rmp_f in "$_rmp_root"/[0-9]*/cmdline; do
        [ -r "$_rmp_f" ] || continue
        _rmp_script=$(tr '\000' '\n' < "$_rmp_f" 2>/dev/null | awk "$HELIX_MOONRAKER_ARGV_AWK")
        [ -n "$_rmp_script" ] || continue
        printf '%s\n%s\n' "$(dirname "$_rmp_f")" "$_rmp_script"
        return 0
    done
    return 1
}

# Print where the running Moonraker's "/" is from here: empty when it shares
# ours or none runs, the chroot's directory when it runs in one (Forge-X runs
# Moonraker in /data/.mod/.forge-x). Every path in its argv and every absolute
# [include] it reads is relative to that.
running_moonraker_root() {
    _rmd_proc=$(running_moonraker_process) || return 0
    _rmd_proc="${_rmd_proc%%
*}"
    _rmd_root=$(readlink "$_rmd_proc/root" 2>/dev/null) || return 0
    [ "$_rmd_root" = "/" ] && return 0
    printf '%s' "${_rmd_root%/}"
}

# Print the running Moonraker's effective uid, then its groups (primary
# first), space-separated. Empty when no Moonraker runs here.
running_moonraker_ids() {
    _rmi_proc=$(running_moonraker_process) || return 0
    _rmi_proc="${_rmi_proc%%
*}"
    awk '/^Uid:/ { u = $3 } /^Gid:/ { g = $3 }
        /^Groups:/ { for (i = 2; i <= NF; i++) gs = gs " " $i }
        END { if (u != "") print u " " g gs }' "$_rmi_proc/status" 2>/dev/null || true
}

# Print the start time of the process at /proc directory @1, in clock ticks
# since boot: field 22 of its stat, counted after the last ')' since the
# command name may hold spaces.
proc_start_ticks() {
    sed 's/.*) //' "$1/stat" 2>/dev/null | awk '{ print $20 }'
}

# Print the running Moonraker's pid and start time, which tell one Moonraker
# process from the next even when a pid is reused. Empty when none runs.
running_moonraker_identity() {
    _rmy_proc=$(running_moonraker_process) || return 0
    _rmy_proc="${_rmy_proc%%
*}"
    printf '%s %s' "${_rmy_proc##*/}" "$(proc_start_ticks "$_rmy_proc")"
}

# Whether the process whose running_moonraker_identity was @1 still runs:
# one read of its stat, with no walk over /proc.
moonraker_identity_alive() {
    [ -n "$1" ] || return 1
    _mia_proc="${HELIX_PROC_ROOT:-/proc}/${1%% *}"
    [ -d "$_mia_proc" ] && [ "$(proc_start_ticks "$_mia_proc")" = "${1#* }" ]
}

# Print the time in whole seconds. Every wait here runs on this clock, so an
# HTTP probe or a /proc walk that takes seconds of its own counts against the
# wait instead of stretching it.
helix_now() {
    date +%s
}

# Wait up to @2 seconds for the Moonraker process whose identity was @1 to
# exit. True once it has, and at once when @1 is empty.
moonraker_wait_gone() {
    _mwg_end=$(($(helix_now) + $2))
    while moonraker_identity_alive "$1"; do
        [ "$(helix_now)" -lt "$_mwg_end" ] || return 1
        sleep 1
    done
    return 0
}

# Watch the Moonraker at base URL @2, whose process had identity @1 before a
# restart was asked of it, for up to @3 seconds. Returns 0 once the restart is
# seen under way: that process is gone, or, when @4 says Moonraker answered
# before, it failed to answer two checks in a row. Returns 1 when nothing
# changed by the end.
moonraker_restart_seen() {
    _mrs_end=$(($(helix_now) + $3))
    _mrs_quiet=0
    while :; do
        if [ -n "$1" ] && ! moonraker_identity_alive "$1"; then
            return 0
        fi
        if [ -z "${4:-}" ]; then
            :
        elif moonraker_answers "$2/server/info"; then
            _mrs_quiet=0
        else
            _mrs_quiet=$((_mrs_quiet + 1))
            [ "$_mrs_quiet" -lt 2 ] || return 0
        fi
        [ "$(helix_now)" -lt "$_mrs_end" ] || return 1
        sleep 1
    done
}

# Whether systemd is the init of the host whose /run is @1, judged by its
# runtime directory there. A systemctl on PATH proves nothing:
# Creality K1 firmware ships /usr/bin/systemctl as a shell script that handles
# reboot and poweroff and exits 0 for anything else.
systemd_is_init() {
    [ -d "$1/systemd/system" ]
}

# The same probe as _has_real_curl in scripts/lib/installer/release.sh and
# moonraker-plugin/remote-install.sh, which run where this file is not sourced.
# Detect whether `curl` is curl proper, with the flags we pass it. Creality K1
# firmware ships libhv's example client as /usr/bin/curl: it answers to the name,
# prints "curl version 1.0.0" and rejects -s, -S, -L, -o and -f. Real curl's first
# --version output starts "curl <digit>...". Shell builtins only, so it holds on
# a PATH with no head or grep. Cache the result so we only probe once.
_REAL_CURL=""
_has_real_curl() {
    if [ -z "$_REAL_CURL" ]; then
        _REAL_CURL=no
        if command -v curl >/dev/null 2>&1; then
            case "$(curl --version 2>/dev/null)" in
                "curl "[0-9]*) _REAL_CURL=yes ;;
            esac
        fi
    fi
    [ "$_REAL_CURL" = "yes" ]
}

# The python an HTTP request falls back on when this host has neither real
# curl nor wget (Creality K2 firmware ships neither): python3, else python,
# whichever imports urllib.request in isolated mode (-I, Python 3.4 on), which
# leaves the current directory and PYTHON* variables out of its imports.
# Cached like _REAL_CURL.
_HTTP_PY=""
_HTTP_PY_PROBED=""
_has_http_python() {
    if [ -z "$_HTTP_PY_PROBED" ]; then
        _HTTP_PY_PROBED=1
        for _hhp_cand in python3 python; do
            if command -v "$_hhp_cand" >/dev/null 2>&1 &&
                "$_hhp_cand" -I -c 'import urllib.request' >/dev/null 2>&1; then
                _HTTP_PY="$_hhp_cand"
                break
            fi
        done
    fi
    [ -n "$_HTTP_PY" ]
}

# Request URL @2 with method @1 (GET prints the body; POST sends an empty
# one) through _has_http_python's python, waiting at most @3 seconds. An HTTP
# error is a failure, as curl -f makes it.
python_http() {
    _has_http_python || return 1
    HELIX_HTTP_METHOD="$1" HELIX_HTTP_URL="$2" HELIX_HTTP_TIMEOUT="$3" "$_HTTP_PY" -I - <<'PYEOF'
import os, sys, urllib.request
method = os.environ["HELIX_HTTP_METHOD"]
try:
    req = urllib.request.Request(os.environ["HELIX_HTTP_URL"],
                                 data=b"" if method == "POST" else None, method=method)
    with urllib.request.urlopen(req, timeout=float(os.environ["HELIX_HTTP_TIMEOUT"])) as resp:
        sys.stdout.buffer.write(resp.read())
except Exception:
    sys.exit(1)
PYEOF
}

# Print the body of a GET of URL @1, waiting at most @2 seconds (default 2):
# real curl, then wget, and python only when neither is here. BusyBox systems
# (Forge-X outside its chroot) carry wget but no curl, a K1's /usr/bin/curl is
# an impostor that rejects -s and -f, and a K2 has python3 alone. -f, wget's
# own status and urllib make an HTTP error a failure.
moonraker_get() {
    _mg_wait="${2:-2}"
    if _has_real_curl && curl -sf --max-time "$_mg_wait" "$1" 2>/dev/null; then
        return 0
    fi
    if command -v wget > /dev/null 2>&1; then
        wget -q -T "$_mg_wait" -O - "$1" 2>/dev/null
        return
    fi
    ! _has_real_curl && python_http GET "$1" "$_mg_wait"
}

# Whether a GET of URL @1 succeeds within @2 seconds (default 2).
moonraker_answers() {
    moonraker_get "$1" "${2:-}" > /dev/null
}

# Print what the Moonraker at base URL @1 reports of a print: "active" for one
# printing or paused, "idle" for none, "unknown" when it cannot be asked in
# time. /server/info's body @2, when it names a Klipper that is not ready,
# means no print runs.
moonraker_job_state() {
    if printf '%s' "${2:-}" | grep -q '"klippy_state"' &&
        ! printf '%s' "${2:-}" | grep -Eq '"klippy_state"[[:space:]]*:[[:space:]]*"ready"'; then
        printf 'idle'
        return 0
    fi
    if ! _mjs_body=$(moonraker_get "$1/printer/objects/query?print_stats=state" 5); then
        printf 'unknown'
    elif printf '%s' "$_mjs_body" | grep -Eq '"state"[[:space:]]*:[[:space:]]*"(printing|paused)"'; then
        printf 'active'
    else
        printf 'idle'
    fi
}

# Whether a print holds off a restart of the Moonraker at base URL @1: one is
# printing or paused, or one cannot be ruled out, because a Moonraker process
# runs and does not answer in time (a busy board answers slowly mid-print). A
# Moonraker with no process and no answer holds nothing off: no client of it
# is left to drop. Sets MOONRAKER_UP to 1 when Moonraker answered, else
# empty, and MOONRAKER_INFO to the body of its /server/info.
moonraker_restart_held() {
    MOONRAKER_UP=1
    if ! MOONRAKER_INFO=$(moonraker_get "$1/server/info" 2); then
        MOONRAKER_INFO=""
        MOONRAKER_UP=""
        running_moonraker_process >/dev/null || return 1
        MOONRAKER_INFO=$(moonraker_get "$1/server/info" 10) || return 0
        MOONRAKER_UP=1
    fi
    [ "$(moonraker_job_state "$1" "$MOONRAKER_INFO")" != idle ]
}

# POST an empty body to URL @1, waiting at most @2 seconds (default 5).
# Clients in moonraker_get's order; an HTTP error (a 401) is a failure.
moonraker_post() {
    _mp_wait="${2:-5}"
    if _has_real_curl && curl -sf --max-time "$_mp_wait" -X POST "$1" >/dev/null 2>&1; then
        return 0
    fi
    if command -v wget >/dev/null 2>&1; then
        wget -q -T "$_mp_wait" -O /dev/null --post-data='' "$1" >/dev/null 2>&1
        return
    fi
    ! _has_real_curl && python_http POST "$1" "$_mp_wait" >/dev/null 2>&1
}

# POST /server/restart to the Moonraker at base URL @1 (default: MOONRAKER_URL,
# else port 7125). Moonraker re-launches itself in place, the one restart that
# works where no service manager reaches it (Forge-X's chroot) or where sudo
# wants a password. Returns 0 when Moonraker accepted it.
request_moonraker_restart() {
    moonraker_post "${1:-$(moonraker_local_url "")}/server/restart" 5
}

# request_moonraker_restart at base URL @1 (default as there), then up to 5s
# for the instance that accepted it to stop answering: it answers for a moment
# after the request, and a wait for Moonraker that starts then would take it
# for the one that came back. Returns 0 when Moonraker accepted the request.
moonraker_restart_over_http() {
    _mro_url="${1:-$(moonraker_local_url "")}"
    request_moonraker_restart "$_mro_url" || return 1
    _mro_end=$(($(helix_now) + 5))
    while [ "$(helix_now)" -lt "$_mro_end" ] && moonraker_answers "$_mro_url/server/info"; do
        sleep 1
    done
    return 0
}

# Print each way this host's service manager can restart Moonraker, one per
# line, in the order to try them: "systemd" when systemd is init
# (systemd_is_init on HELIX_RUN_DIR, else /run) and the moonraker unit is
# active, then the first init script for it under HELIX_INITD_DIR, then
# /etc/init.d (S56moonraker_service on Creality K1 and Simple AF, a plain
# moonraker on COSMOS and the rest). With @1 "start", the ways to start a
# stopped Moonraker instead: systemd counts when the unit exists at all.
# Prints nothing when no service manager runs it, as for Moonraker in
# Forge-X's chroot.
moonraker_service() {
    if systemd_is_init "${HELIX_RUN_DIR:-/run}" && command -v systemctl >/dev/null 2>&1; then
        if [ "${1:-}" = start ]; then
            systemctl cat moonraker >/dev/null 2>&1 && printf 'systemd\n'
        else
            systemctl is-active --quiet moonraker 2>/dev/null && printf 'systemd\n'
        fi
    fi
    for _ms_script in "${HELIX_INITD_DIR:-/etc/init.d}/S56moonraker_service" \
                      "${HELIX_INITD_DIR:-/etc/init.d}/moonraker" \
                      /etc/init.d/S56moonraker_service /etc/init.d/moonraker; do
        if [ -x "$_ms_script" ] || [ -f "$_ms_script" ]; then
            printf '%s\n' "$_ms_script"
            return 0
        fi
    done
    return 0
}

# Whether systemd reports the moonraker unit between states (activating, as
# during its Restart= delay, or deactivating): a missing process is then no
# sign that nothing is coming back.
moonraker_unit_in_transition() {
    systemd_is_init "${HELIX_RUN_DIR:-/run}" && command -v systemctl >/dev/null 2>&1 || return 1
    case "$(systemctl is-active moonraker 2>/dev/null)" in
        activating | deactivating | reloading) return 0 ;;
    esac
    return 1
}

# Wait for the Moonraker at base URL @1 to answer, up to HELIX_MOONRAKER_UP_SECS
# seconds (default 90: a slow board logs "Starting Moonraker" 3 to 5s after
# launch and answers well after that, and a restart's own component and
# database loads come on top). With @2 "watch", stop early once no Moonraker
# process has been seen for 3 seconds and systemd is not bringing one back:
# nothing is coming back then. Returns 0 when it answers, 2 when no process
# is left, 1 on the timeout.
moonraker_wait_up() {
    _mwu_end=$(($(helix_now) + ${HELIX_MOONRAKER_UP_SECS:-90}))
    _mwu_gone=""
    while ! moonraker_answers "$1/server/info"; do
        _mwu_now=$(helix_now)
        if [ "${2:-}" = watch ]; then
            if running_moonraker_process >/dev/null || moonraker_unit_in_transition; then
                _mwu_gone=""
            else
                [ -n "$_mwu_gone" ] || _mwu_gone=$_mwu_now
                [ $((_mwu_now - _mwu_gone)) -lt 3 ] || return 2
            fi
        fi
        [ "$_mwu_now" -lt "$_mwu_end" ] || return 1
        sleep 1
    done
    return 0
}

# Whether Moonraker at base URL @1 may be started: no Moonraker process runs
# and nothing answers on its port. A start beside a Moonraker that runs (one
# still coming up, or one whose argv this host's matcher misses) would run a
# second instance against the same port and database.
moonraker_start_safe() {
    ! running_moonraker_process >/dev/null && ! moonraker_answers "$1/server/info" 5
}

# Run action @3 (restart or start) of Moonraker's service way @2 (systemd, or
# an init script's path) under prefix @1. systemd starts Moonraker under its
# own context. An init script starts it as a child of this script, so it runs
# detached: a new session where setsid exists, nice 0, oom_score_adj 0, stdio
# on /dev/null, and nothing from the environment but PATH. A Moonraker it
# starts then carries nothing of the caller's: HelixScreen's raised nice and
# OOM score, its malloc settings, or the output pipe that closes when this
# script exits.
moonraker_service_run() {
    if [ "$2" = systemd ]; then
        # shellcheck disable=SC2086  # the prefix is a word list
        $1 systemctl "$3" moonraker < /dev/null
        return
    fi
    # shellcheck disable=SC2016,SC2086  # the -c string's own $; a word-list prefix
    $1 sh -c 'renice 0 -p $$ > /dev/null 2>&1
echo 0 2> /dev/null > "/proc/$$/oom_score_adj"
cd /
if command -v setsid > /dev/null 2>&1; then
    exec env -i PATH="$PATH" HOME=/ setsid "$1" "$2"
fi
exec env -i PATH="$PATH" HOME=/ "$1" "$2"' sh "$2" "$3" < /dev/null > /dev/null 2>&1
}

# After a restart attempt, make sure the Moonraker at base URL @2 answers
# again, starting it through moonraker_service start, run under prefix @1,
# when it is down: a service script's restart can stop it and fail to start
# it (start-stop-daemon refuses while the old process still holds its
# pidfile). @3, when set, is called with a line for each problem. @4 is the
# identity of the Moonraker process before the restart, empty when none was
# seen: then a missing process proves nothing, so Moonraker gets the whole
# wait before anything starts it. Nothing is started while a Moonraker
# process runs or anything answers on its port (moonraker_start_safe).
# Returns 0 when it answers, 1 when it answers because this started it, 2
# when it does not answer yet but a process runs or none can be started, and
# 3 when it is down and could not be started.
moonraker_bring_back() {
    _mbb_pre="$1"
    _mbb_url="$2"
    _mbb_warn="${3:-}"
    _mbb_secs="${HELIX_MOONRAKER_UP_SECS:-90}"
    _mbb_ways=$(moonraker_service start)
    _mbb_watch=""
    [ -z "$_mbb_ways" ] || [ -z "${4:-}" ] || _mbb_watch=watch
    _mbb_up=0
    moonraker_wait_up "$_mbb_url" "$_mbb_watch" || _mbb_up=$?
    [ "$_mbb_up" -ne 0 ] || return 0
    if [ -z "$_mbb_ways" ] || ! moonraker_start_safe "$_mbb_url"; then
        [ -z "$_mbb_warn" ] || "$_mbb_warn" "Moonraker did not respond within ${_mbb_secs} seconds of the restart"
        return 2
    fi
    _mbb_hint=""
    _mbb_started=""
    _mbb_ifs=$IFS
    IFS='
'
    for _mbb_way in $_mbb_ways; do
        IFS=$_mbb_ifs
        if [ "$_mbb_way" = systemd ]; then
            _mbb_name="via systemctl"
            _mbb_hint=${_mbb_hint:-"sudo systemctl start moonraker"}
        else
            _mbb_name="with $_mbb_way"
            _mbb_hint=${_mbb_hint:-"$_mbb_way start"}
        fi
        # A start that failed may still have started it.
        moonraker_start_safe "$_mbb_url" || break
        if moonraker_service_run "$_mbb_pre" "$_mbb_way" start; then
            _mbb_started="$_mbb_name"
            break
        fi
        [ -z "$_mbb_warn" ] || "$_mbb_warn" "Could not start Moonraker $_mbb_name"
    done
    IFS=$_mbb_ifs
    if [ -n "$_mbb_started" ] || ! moonraker_start_safe "$_mbb_url"; then
        _mbb_up=0
        moonraker_wait_up "$_mbb_url" "${_mbb_started:+watch}" || _mbb_up=$?
        if [ "$_mbb_up" -eq 0 ]; then
            [ -z "$_mbb_started" ] || [ -z "$_mbb_warn" ] ||
                "$_mbb_warn" "Moonraker was not running after the restart, so it was started $_mbb_started"
            [ -z "$_mbb_started" ] || return 1
            return 0
        fi
        if ! moonraker_start_safe "$_mbb_url"; then
            [ -z "$_mbb_warn" ] || "$_mbb_warn" "Moonraker runs but did not respond within ${_mbb_secs} seconds"
            return 2
        fi
    fi
    if [ -n "$_mbb_warn" ]; then
        "$_mbb_warn" "MOONRAKER IS NOT RUNNING and could not be started. Start it now:"
        "$_mbb_warn" "  $_mbb_hint"
    fi
    return 3
}

# Moonraker restart timing, in seconds on helix_now's clock, each overridable
# from the environment:
#   HELIX_MOONRAKER_UP_SECS (90)    how long Moonraker gets to answer after a
#                                   restart or a start, and an old process to exit
#   HELIX_MOONRAKER_STOP_SECS (30)  how long a restart that reported success
#                                   gets to be seen under way
#   HELIX_MOONRAKER_SETTLE_SECS (5) the same for a restart that failed, which
#                                   may still have signalled Moonraker to stop
# A probe overruns a wait by at most its own cap, doubled where curl and wget
# both try: 4s for /server/info. At the defaults restart_moonraker_via waits
# at most about 700 seconds in all: four print checks (34s each: 4 + 20 +
# 10), three attempts that restart nothing (44s each: a 10s request and the
# 34s watch), the last attempt (44s), the old process's exit (94s),
# Moonraker's answer (94s), up to four start checks (14s each) and a started
# Moonraker's answer (94s), rounded up. The run
# time of the service commands themselves comes on top: systemd bounds one by
# the unit's stop and start timeouts, and an init script returns once it has
# signalled. HelixPluginInstaller's timeout for --restart-moonraker exceeds
# both.

# Restart Moonraker, the command run under prefix @1 (a word list: a root
# wrapper, or empty) for the service manager, Moonraker at base URL @2
# (default as for request_moonraker_restart); command @3, when set, is called
# with a line for each problem. A print printing or paused holds every
# attempt off (it drops Klipper's API clients mid-job), checked again right
# before each one (moonraker_restart_held). While Moonraker answers, its own
# in-place restart over HTTP comes first: it needs no root and no service
# manager, and races no pidfile. The service manager (moonraker_service) comes
# first instead when Moonraker does not answer, or when /server/info lists
# helix_print among its components (an in-place restart keeps every module it
# has imported), and then Moonraker is asked to restart its own service
# (POST /machine/services/restart) before its in-place restart.
# One restart at a time: an attempt that Moonraker accepted, or that is seen
# under way (moonraker_restart_seen), is the last, so no second restart lands
# on one in flight. Every attempt that may have stopped Moonraker ends in
# moonraker_bring_back, after the old process exits, so Moonraker is never
# left down while it can be started. Returns 0 when a service manager
# restarted its process, 3 when Moonraker restarted itself, 4 when a print
# holds the restart off or cannot be ruled out, 5 when Moonraker is down and
# could not be started, and 1 when nothing restarted it.
restart_moonraker_via() {
    _rmv_pre="$1"
    _rmv_url="${2:-$(moonraker_local_url "")}"
    _rmv_warn="${3:-}"
    _rmv_before=$(running_moonraker_identity)
    moonraker_restart_held "$_rmv_url" && return 4
    _rmv_order=$(moonraker_service)
    case "$MOONRAKER_UP$MOONRAKER_INFO" in
        '') ;;
        *'"helix_print"'*) _rmv_order="$_rmv_order
services
inplace" ;;
        *) _rmv_order="inplace
$_rmv_order" ;;
    esac
    _rmv_kind=""
    _rmv_held=""
    _rmv_first=1
    _rmv_ifs=$IFS
    IFS='
'
    for _rmv_way in $_rmv_order; do
        IFS=$_rmv_ifs
        # Moonraker's own service restart can be judged only by its process.
        [ "$_rmv_way" != services ] || [ -n "$_rmv_before" ] || continue
        if [ -z "$_rmv_first" ] && moonraker_restart_held "$_rmv_url"; then
            _rmv_held=1
            break
        fi
        _rmv_first=""
        _rmv_ok=""
        case "$_rmv_way" in
            inplace)
                _rmv_name="over HTTP"
                if moonraker_restart_over_http "$_rmv_url"; then
                    _rmv_kind=inplace
                    break
                fi
                ;;
            services)
                _rmv_name="through Moonraker's own service restart"
                if moonraker_post "$_rmv_url/machine/services/restart?service=moonraker" 5; then
                    _rmv_kind=service
                    break
                fi
                ;;
            *)
                if [ "$_rmv_way" = systemd ]; then
                    _rmv_name="via systemctl"
                else
                    _rmv_name="with $_rmv_way"
                fi
                ! moonraker_service_run "$_rmv_pre" "$_rmv_way" restart || _rmv_ok=1
                ;;
        esac
        _rmv_secs="${HELIX_MOONRAKER_SETTLE_SECS:-5}"
        [ -z "$_rmv_ok" ] || _rmv_secs="${HELIX_MOONRAKER_STOP_SECS:-30}"
        # With no process seen and no answer before it, nothing tells an
        # attempt's effect but its own exit status.
        if [ -z "$_rmv_before" ] && [ -z "$MOONRAKER_UP" ]; then
            if [ -n "$_rmv_ok" ]; then
                _rmv_kind=service
                break
            fi
        elif moonraker_restart_seen "$_rmv_before" "$_rmv_url" "$_rmv_secs" "$MOONRAKER_UP"; then
            _rmv_kind=service
            break
        fi
        if [ -z "$_rmv_warn" ]; then
            :
        elif [ -n "$_rmv_ok" ]; then
            "$_rmv_warn" "Moonraker kept running through a restart $_rmv_name"
        else
            "$_rmv_warn" "Could not restart Moonraker $_rmv_name"
        fi
    done
    IFS=$_rmv_ifs
    if [ -z "$_rmv_kind" ]; then
        [ -z "$_rmv_held" ] || return 4
        return 1
    fi
    # A service restart's old process exits before a new one can start.
    if [ "$_rmv_kind" = service ] &&
        ! moonraker_wait_gone "$_rmv_before" "${HELIX_MOONRAKER_UP_SECS:-90}"; then
        [ -z "$_rmv_warn" ] || "$_rmv_warn" "Moonraker kept running through a restart $_rmv_name"
    fi
    _rmv_up=0
    moonraker_bring_back "$_rmv_pre" "$_rmv_url" "$_rmv_warn" "$_rmv_before" || _rmv_up=$?
    [ "$_rmv_up" -ne 3 ] || return 5
    [ "$_rmv_kind" != inplace ] || return 3
    ! moonraker_identity_alive "$_rmv_before" || return 1
    return 0
}

# Section headers this plugin owns in a Moonraker config: its own section, and
# the [update_manager helix_print] a git-clone install of the plugin wrote.
# shellcheck disable=SC2034  # read by the scripts that source this file
HELIX_PRINT_SECTION_RE='^\[helix_print\]'
HELIX_PRINT_UPDATER_RE='^\[update_manager helix_print\]'
HELIX_PRINT_OWNED_RE='^\[(helix_print|update_manager helix_print)\]'

# Print each file in moonraker_conf_files @1 @2 that holds a [helix_print]
# section, one per line; nothing when none does.
# UNCALLED_OK: moonraker-plugin/install.sh calls it; the main installer only
# removes sections, through helix_print_owned_files.
helix_print_section_files() {
    helix_conf_files_matching "$1" "${2:-}" "$HELIX_PRINT_SECTION_RE"
}

# Print each file in moonraker_conf_files @1 @2 that holds any section this
# plugin owns, one per line: what an uninstall has to clean.
helix_print_owned_files() {
    helix_conf_files_matching "$1" "${2:-}" "$HELIX_PRINT_OWNED_RE"
}

# Replace config file @1 with the output of awk program @2 run over it, each
# command run under prefix @3 when set (the main installer's sudo). The new
# text is written beside the real file (a symlinked config is followed and
# stays a symlink) into a copy that keeps its mode and owner, then renamed over
# it, so a write that fails part-way leaves the original whole. BusyBox awk
# exits 0 when a write fails, so the staged copy must also hold as many bytes
# as a second run of the program writes into a pipe, which no full disk cuts
# short. Returns 1 when the file could not be rewritten.
helix_rewrite_conf() {
    _rw_pre="${3:-}"
    _rw_real=$(readlink -f "$1" 2>/dev/null) || _rw_real=""
    [ -n "$_rw_real" ] || _rw_real="$1"
    _rw_tmp="$_rw_real.helixtmp.$$"
    # The program is an argument rather than part of the -c string, so its $0
    # and $1 stay awk's and are never expanded by a shell.
    # shellcheck disable=SC2016,SC2086  # the -c string's own $; a word-list prefix
    if $_rw_pre cp -p "$_rw_real" "$_rw_tmp" 2>/dev/null &&
        $_rw_pre sh -c 'awk "$1" "$2" > "$3" &&
            [ "$(awk "$1" "$2" | wc -c)" -eq "$(wc -c < "$3")" ]' sh "$2" "$_rw_real" "$_rw_tmp" &&
        $_rw_pre mv -f "$_rw_tmp" "$_rw_real"; then
        return 0
    fi
    # shellcheck disable=SC2086
    $_rw_pre rm -f "$_rw_tmp"
    return 1
}

# Print an awk program that prints a config file without the sections whose
# header matches extended regex @1 (anchored, bracketed: ^\[(a|b)\]). A run of
# such sections goes as one, along with the "#~# --- HelixPrint Plugin ---"
# line a git-clone install wrote above it. A run that reaches the end of the
# file also takes the one blank line before it, which is what an install writes
# ahead of the section it appends, so an install and uninstall leave the file as
# it was. A run between two other sections keeps that line: it separates them.
helix_strip_awk() {
    # shellcheck disable=SC2016  # awk's own $ fields
    printf '%s' '
    { line[NR] = $0 }
    END {
        skip = 0
        for (i = 1; i <= NR; i++) {
            if (line[i] ~ /'"$1"'/) {
                if (!skip) {
                    start = i
                    if (i > 1 && line[i - 1] == "#~# --- HelixPrint Plugin ---") { drop[i - 1] = 1; start = i - 1 }
                }
                skip = 1
            }
            else if (line[i] ~ /^\[/) { skip = 0 }
            if (skip) { drop[i] = 1 }
        }
        if (skip && start > 1 && line[start - 1] ~ /^[ \t\r]*$/) { drop[start - 1] = 1 }
        for (i = 1; i <= NR; i++) { if (!drop[i]) print line[i] }
    }
'
}

# Programs for helix_strip_awk: every section the plugin owns (uninstall), and
# only the git-clone install's updater entry (an install, whose link makes that
# entry follow a checkout Moonraker no longer loads).
# shellcheck disable=SC2034  # read by the scripts that source this file
HELIX_PRINT_STRIP_AWK=$(helix_strip_awk "$HELIX_PRINT_OWNED_RE")
# shellcheck disable=SC2034
HELIX_PRINT_UPDATER_STRIP_AWK=$(helix_strip_awk "$HELIX_PRINT_UPDATER_RE")
