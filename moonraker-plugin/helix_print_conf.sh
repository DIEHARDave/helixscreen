#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Where the [helix_print] section lives in a Moonraker config: shared by the
# plugin's install.sh and the main installer's uninstall (bundled into it from
# here), so both find the section in the same files.

[ -n "${_HELIX_PRINT_CONF_SOURCED:-}" ] && return 0
_HELIX_PRINT_CONF_SOURCED=1

# Print every Moonraker config file reachable from @1 through [include] lines,
# @1 first, one per line, each once. A relative include resolves against the
# directory of the file that names it, as the path was given (a symlinked
# moonraker.conf resolves against the link's directory). A glob expands the way
# Moonraker expands it. @2, when set, is prefixed to an absolute include: the
# root Moonraker runs under, seen from here. Paths holding blanks are not
# supported; no Moonraker layout uses them.
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
        case "$_mcf_seen" in *" $_mcf_file "*) continue ;; esac
        _mcf_seen="$_mcf_seen$_mcf_file "
        [ -f "$_mcf_file" ] || continue
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

# Print each file in moonraker_conf_files @1 @2 that holds a [helix_print]
# section, one per line; nothing when none does.
helix_print_section_files() {
    moonraker_conf_files "$1" "${2:-}" | while IFS= read -r _hps_file; do
        if grep -q '^\[helix_print\]' "$_hps_file" 2>/dev/null; then
            printf '%s\n' "$_hps_file"
        fi
    done
    return 0
}
