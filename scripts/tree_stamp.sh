#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# tree_stamp.sh [BASE]
#
# Prints "<BASE> <content-hash>": the commit the source tree is measured
# against, and a hash naming every source file under src/, include/ and tests/
# whose working-tree content differs from BASE (tracked edits, deletions and
# untracked sources alike). BASE defaults to HEAD. A tree identical to BASE
# hashes to "clean".
#
# The link rule stamps a binary with `tree_stamp.sh`; the binary runs
# `tree_stamp.sh <its BASE>` at startup and warns when the two differ. Measuring
# against the BASE the binary recorded, not the current HEAD, is what lets a
# commit of already-built edits leave the binary reading as current: the same
# files differ from that BASE with the same content either way.
#
# Exit 1 outside a git work tree (an rsynced mirror, an unpacked artifact),
# exit 2 when BASE is not a commit this repository has. Both print nothing.

set -u

top=$(git rev-parse --show-toplevel 2>/dev/null) && [ -n "$top" ] && cd "$top" || exit 1
base=$(git rev-parse --verify --quiet "${1:-HEAD}^{commit}" 2>/dev/null) || exit 2

paths=(src include tests)

# Every changed or untracked path, NUL-separated, deduplicated, filtered to
# what the compiler reads. Renames are a delete plus an add, so both ends count.
changed=$(
    {
        git diff --no-ext-diff --no-renames --name-only -z "$base" -- "${paths[@]}"
        git ls-files -z -o --exclude-standard -- "${paths[@]}"
    } | while IFS= read -r -d '' f; do
        case $f in
            *.c | *.cc | *.cpp | *.h | *.hh | *.hpp | *.inc | *.m | *.mm) printf '%s\n' "$f" ;;
        esac
    done | LC_ALL=C sort -u
)

if [ -z "$changed" ]; then
    printf '%s clean\n' "$base"
    exit 0
fi

listing=$(
    printf '%s\n' "$changed" | while IFS= read -r f; do
        if [ -f "$f" ]; then
            printf '%s\t%s\n' "$(git hash-object -- "$f")" "$f"
        else
            printf 'deleted\t%s\n' "$f"
        fi
    done
)
printf '%s %s\n' "$base" "$(printf '%s\n' "$listing" | git hash-object --stdin)"
