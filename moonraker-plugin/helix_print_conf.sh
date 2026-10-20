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

# Print one line per running Moonraker: its /proc/<pid> directory under
# HELIX_PROC_ROOT (tests point it at a fixture tree), else /proc, a blank, and
# the moonraker.py its argv names. Moonraker is a python interpreter (argv[0]
# python, python3, python3.11, ...) running moonraker.py; an init script, editor
# or log viewer whose argv names the file comes and goes beside it and is not
# Moonraker. Prints nothing when no Moonraker runs.
running_moonrakers() {
    _rms_root="${HELIX_PROC_ROOT:-/proc}"
    for _rms_f in "$_rms_root"/[0-9]*/cmdline; do
        [ -r "$_rms_f" ] || continue
        _rms_script=$(tr '\000' '\n' < "$_rms_f" 2>/dev/null | awk '
            NR == 1 { n = split($0, part, "/"); if (part[n] !~ /^python[0-9.]*$/) exit; next }
            /(^|\/)moonraker\.py$/ { print; exit }')
        [ -n "$_rms_script" ] || continue
        printf '%s %s\n' "$(dirname "$_rms_f")" "$_rms_script"
    done
    return 0
}

# Print the first running Moonraker's /proc/<pid> directory and, on the next
# line, its moonraker.py (running_moonrakers). Returns 1, printing nothing,
# when no Moonraker runs.
running_moonraker_process() {
    _rmp_first=$(running_moonrakers | sed -n 1p)
    [ -n "$_rmp_first" ] || return 1
    printf '%s\n%s\n' "${_rmp_first%% *}" "${_rmp_first#* }"
}

# Print the running Moonraker's /proc directory (running_moonraker_process).
running_moonraker_proc() {
    _rmq_proc=$(running_moonraker_process) || return 1
    printf '%s' "${_rmq_proc%%
*}"
}

# Print the running Moonraker's argv, one argument per line.
running_moonraker_args() {
    _rmg_proc=$(running_moonraker_proc) || return 1
    tr '\000' '\n' < "$_rmg_proc/cmdline" 2>/dev/null
}

# Print where the running Moonraker's "/" is from here: empty when it shares
# ours or none runs, the chroot's directory when it runs in one (Forge-X runs
# Moonraker in /data/.mod/.forge-x). Every path in its argv and every absolute
# [include] it reads is relative to that.
running_moonraker_root() {
    _rmd_proc=$(running_moonraker_proc) || return 0
    _rmd_root=$(readlink "$_rmd_proc/root" 2>/dev/null) || return 0
    [ "$_rmd_root" = "/" ] && return 0
    printf '%s' "${_rmd_root%/}"
}

# Print the value the running Moonraker's argv gives option @1 (its short
# form) or @2 (its long form): the next argument, or what follows "=" or the
# short flag in the same one. A long form may be abbreviated, as Python's
# argparse accepts. Empty when no Moonraker runs or neither form appears.
running_moonraker_arg() {
    running_moonraker_args | awk -v s="$1" -v l="$2" '
        function is_long(a) { return length(a) > 2 && index(l, a) == 1 }
        take { print; exit }
        $0 == s || is_long($0) { take = 1; next }
        /^--[^=]+=/ { eq = index($0, "="); if (is_long(substr($0, 1, eq - 1))) { print substr($0, eq + 1); exit } }
        length($0) > 2 && substr($0, 1, 2) == s { print substr($0, 3); exit }'
}

# Print the moonraker.conf the running Moonraker loads, as a path from here:
# the file its -c names, else config/moonraker.conf under the data path its -d
# names, whichever is first a file. Paths are absolute in its root; a relative
# one names nothing from here and is skipped. Empty when no Moonraker runs,
# when it names neither (it then loads its user's ~/printer_data, which a
# caller finds by home), or when more than one runs: several instances
# (KIAUH printer_1_data, printer_2_data) list in no stable order, so the
# caller's own lookup decides. On Creality K2 this is the only way to the
# answer: procd starts Moonraker with -c /usr/share/moonraker/moonraker.conf.
running_moonraker_conf() {
    running_moonrakers | awk 'END { exit NR != 1 }' || return 0
    _rmc_root=$(running_moonraker_root)
    _rmc_data=$(running_moonraker_arg -d --datapath)
    for _rmc_conf in "$(running_moonraker_arg -c --configfile)" \
                     "${_rmc_data:+${_rmc_data%/}/config/moonraker.conf}"; do
        case "$_rmc_conf" in
            /*) ;;
            *) continue ;;
        esac
        if [ -f "$_rmc_root$_rmc_conf" ]; then
            printf '%s' "$_rmc_root$_rmc_conf"
            return 0
        fi
    done
    return 0
}

# Print the running Moonraker's effective uid, then its groups (primary
# first), space-separated. Empty when no Moonraker runs here.
running_moonraker_ids() {
    _rmi_proc=$(running_moonraker_proc) || return 0
    awk '/^Uid:/ { u = $3 } /^Gid:/ { g = $3 }
        /^Groups:/ { for (i = 2; i <= NF; i++) gs = gs " " $i }
        END { if (u != "") print u " " g gs }' "$_rmi_proc/status" 2>/dev/null || true
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

# Print the body of a GET of URL @1, waiting at most @2 seconds (default 2):
# real curl, else wget. BusyBox systems (Forge-X outside its chroot) carry
# wget but no curl, and a K1's /usr/bin/curl is an impostor that rejects -s
# and -f. -f and wget's own status make an HTTP error a failure.
moonraker_get() {
    _mg_wait="${2:-2}"
    if _has_real_curl && curl -sf --max-time "$_mg_wait" "$1" 2>/dev/null; then
        return 0
    fi
    command -v wget > /dev/null 2>&1 && wget -q -T "$_mg_wait" -O - "$1" 2>/dev/null
}

# Whether a GET of URL @1 succeeds within @2 seconds (default 2).
moonraker_answers() {
    moonraker_get "$1" "${2:-}" > /dev/null
}

# Whether the Moonraker at base URL @1 reports a print that is printing or
# paused. False when it cannot be asked: then no job is known to be at risk.
moonraker_job_active() {
    moonraker_get "$1/printer/objects/query?print_stats=state" 5 |
        grep -Eq '"state"[[:space:]]*:[[:space:]]*"(printing|paused)"'
}

# POST /server/restart to the Moonraker at base URL @1 (default: MOONRAKER_URL,
# else port 7125). Moonraker re-launches itself in place, the one restart that
# works where no service manager reaches it (Forge-X's chroot) or where sudo
# wants a password. Real curl first, then wget; -sf and wget's own status make
# an HTTP error (a 401) a failure. Returns 0 when Moonraker accepted it.
request_moonraker_restart() {
    _rmr_url="${1:-$(moonraker_local_url "")}/server/restart"
    if _has_real_curl && curl -sf --max-time 5 -X POST "$_rmr_url" >/dev/null 2>&1; then
        return 0
    fi
    command -v wget >/dev/null 2>&1 &&
        wget -q -T 5 -O /dev/null --post-data='' "$_rmr_url" >/dev/null 2>&1
}

# request_moonraker_restart at base URL @1 (default as there), then up to 5s
# for the instance that accepted it to stop answering: it answers for a moment
# after the request, and a wait for Moonraker that starts then would take it
# for the one that came back. Returns 0 when Moonraker accepted the request.
moonraker_restart_over_http() {
    _mro_url="${1:-$(moonraker_local_url "")}"
    request_moonraker_restart "$_mro_url" || return 1
    _mro_quiet=0
    while [ "$_mro_quiet" -lt 5 ] && moonraker_answers "$_mro_url/server/info"; do
        sleep 1
        _mro_quiet=$((_mro_quiet + 1))
    done
    return 0
}

# Print how this host's service manager restarts Moonraker: "systemd" when the
# moonraker unit is active, else the first init script for it under
# HELIX_INITD_DIR, then /etc/init.d (S56moonraker_service on Creality K1 and
# Simple AF, a plain moonraker on COSMOS and the rest). Prints nothing when no
# service manager runs it, as for Moonraker in Forge-X's chroot.
moonraker_service() {
    if command -v systemctl >/dev/null 2>&1 && systemctl is-active --quiet moonraker 2>/dev/null; then
        printf 'systemd'
        return 0
    fi
    for _ms_script in "${HELIX_INITD_DIR:-/etc/init.d}/S56moonraker_service" \
                      "${HELIX_INITD_DIR:-/etc/init.d}/moonraker" \
                      /etc/init.d/S56moonraker_service /etc/init.d/moonraker; do
        if [ -x "$_ms_script" ] || [ -f "$_ms_script" ]; then
            printf '%s' "$_ms_script"
            return 0
        fi
    done
    return 0
}

# Restart Moonraker through moonraker_service, the command run under prefix
# @1 (a word list: a root wrapper, or empty), else by asking the Moonraker at
# base URL @2 (default as for request_moonraker_restart) to restart itself,
# which needs no root. Command @3, when set, is called with a line saying why
# a service-manager restart failed. The request over HTTP is never made while
# a print is printing or paused: a Moonraker restart drops Klipper's API
# clients mid-job. Returns 0 when the service manager restarted it, 3 when
# Moonraker accepted the request, 4 when an active print holds the restart
# off, and 1 when nothing restarted it.
restart_moonraker_via() {
    _rmv_pre="$1"
    _rmv_url="${2:-$(moonraker_local_url "")}"
    _rmv_service=$(moonraker_service)
    case "$_rmv_service" in
        '') ;;
        systemd)
            # shellcheck disable=SC2086  # the prefix is a word list
            $_rmv_pre systemctl restart moonraker && return 0
            [ -z "${3:-}" ] || "$3" "Could not restart Moonraker via systemctl"
            ;;
        *)
            # shellcheck disable=SC2086
            $_rmv_pre "$_rmv_service" restart && return 0
            [ -z "${3:-}" ] || "$3" "Could not restart Moonraker with $_rmv_service"
            ;;
    esac
    moonraker_job_active "$_rmv_url" && return 4
    moonraker_restart_over_http "$_rmv_url" && return 3
    return 1
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
