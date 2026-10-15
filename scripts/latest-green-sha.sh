#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The newest commit on origin/<branch> whose CI is fully green: the commit a
# release can be cut from without freezing the branch while CI catches up.
#
#   scripts/latest-green-sha.sh [--branch B] [--depth N] [--require "W1,W2"]
#
# Takes the commits CI ran on for the last N pushes to B (default 30, newest
# first, from the first --require workflow's runs), and prints the full SHA of
# the first one still on origin/B where
#   - every push-triggered workflow run on B for that commit has completed,
#   - each one concluded success or skipped, and
#   - every workflow named in --require ran at all (default "Build,Code Quality",
#     the two that run on every push; path-filtered workflows such as XML Lint
#     count when they ran and are not required).
# A commit that fails a rule is named on stderr with the reason, so the caller
# sees what was passed over. Exit 1 when no commit in the window qualifies.
#
# It reads origin/B as it is locally: `git fetch origin` first.
set -uo pipefail

GH=${GH:-gh}
BRANCH=main
DEPTH=30
REQUIRE="Build,Code Quality"

while [ $# -gt 0 ]; do
    case "$1" in
        --branch)  BRANCH=${2:?--branch needs a value}; shift 2 ;;
        --depth)   DEPTH=${2:?--depth needs a value}; shift 2 ;;
        --require) REQUIRE=${2?--require needs a value}; shift 2 ;;
        -h | --help) awk 'NR > 2 && /^set -uo/ { exit } NR > 2' "$0" | sed 's/^# \?//'; exit 0 ;;
        *) echo "✗ unknown argument '$1'" >&2; exit 2 ;;
    esac
done
case "$DEPTH" in '' | *[!0-9]*) echo "✗ --depth takes a number" >&2; exit 2 ;; esac

git rev-parse -q --verify "origin/$BRANCH^{commit}" >/dev/null || {
    echo "✗ cannot read origin/$BRANCH" >&2
    exit 2
}
# The candidates are the commits CI ran on: the tips of pushes to B, newest
# first. Walking history instead would wander into a merged branch's commits,
# none of which CI ever saw on B.
IFS=, read -r first_required _ <<<"$REQUIRE"
shas=$("$GH" run list --branch "$BRANCH" --event push --limit "$DEPTH" \
    ${first_required:+--workflow "$first_required"} --json headSha --jq '.[].headSha') || {
    echo "✗ gh run list failed" >&2
    exit 2
}

# verdict <sha>: empty when green, else the reason it is not.
verdict() {
    local runs name status conclusion req seen=$'\n' bad=""
    runs=$("$GH" run list --commit "$1" --branch "$BRANCH" --event push --limit 100 \
        --json workflowName,status,conclusion \
        --jq '.[] | [.workflowName, .status, .conclusion] | @tsv') || {
        echo "gh run list failed"
        return
    }
    [ -n "$runs" ] || { echo "no CI runs"; return; }
    while IFS=$'\t' read -r name status conclusion; do
        seen+="$name"$'\n'
        if [ "$status" != completed ]; then
            bad+="${bad:+, }$name $status"
        else
            case "$conclusion" in
                success | skipped) ;;
                *) bad+="${bad:+, }$name $conclusion" ;;
            esac
        fi
    done <<<"$runs"
    IFS=, read -ra req <<<"$REQUIRE"
    for name in "${req[@]}"; do
        [ -z "$name" ] && continue
        case "$seen" in *$'\n'"$name"$'\n'*) ;; *) bad+="${bad:+, }$name never ran" ;; esac
    done
    echo "$bad"
}

tried=" "
for sha in $shas; do
    case "$tried" in *" $sha "*) continue ;; esac
    tried+="$sha "
    # A push later force-pushed away is not on B any more.
    git merge-base --is-ancestor "$sha" "origin/$BRANCH" 2>/dev/null || {
        echo "  skip ${sha:0:9}: not on origin/$BRANCH" >&2
        continue
    }
    why=$(verdict "$sha")
    if [ -z "$why" ]; then
        echo "$sha"
        exit 0
    fi
    echo "  skip ${sha:0:9} $(git log -1 --format=%s "$sha" | cut -c1-60): $why" >&2
done
echo "✗ none of the last $DEPTH pushes to $BRANCH has fully green CI" >&2
exit 1
