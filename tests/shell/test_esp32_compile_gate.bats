#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for the commit-time ESP32 compile check, scripts/qc/esp32_compile.sh
# driving `scripts/esp32_syntax_check.py --staged`.
#
# The compile itself runs in the espressif/idf image, which no shell test may
# start; unit selection is pinned in tests/python/test_esp32_syntax_check.py.
# What is pinned here is the hook's side of the contract: firmware source and
# header paths reach the gate and nothing else does, the full sweep leaves the
# check to pre-push, a commit touching no source starts no container, and the
# hook never pays for a firmware configure.

load helpers

REPO_ROOT="$(cd "${BATS_TEST_DIRNAME}/../.." && pwd)"

setup() {
    DOCKER_LOG="$BATS_TEST_TMPDIR/docker.log"
    export DOCKER_LOG
    mkdir -p "$BATS_TEST_TMPDIR/bin"
    # Answers `image inspect` as present and records every call.
    printf '#!/bin/sh\necho "$*" >> "%s"\n[ "$1 $2" = "image inspect" ] && exit 0\nexit 1\n' \
        "$DOCKER_LOG" > "$BATS_TEST_TMPDIR/bin/docker"
    chmod +x "$BATS_TEST_TMPDIR/bin/docker"
    export PATH="$BATS_TEST_TMPDIR/bin:$PATH"
    export XDG_CACHE_HOME="$BATS_TEST_TMPDIR/cache"

    FIX="$BATS_TEST_TMPDIR/fw"
    mkdir -p "$FIX/src/printer" "$FIX/firmware/helixscreen-esp32"
    cd "$FIX" || return 1
    git init -q .
    git config user.email t@example.com
    git config user.name tester
    printf 'int a() { return 1; }\n' > src/printer/a.cpp
    printf 'cmake_minimum_required(VERSION 3.16)\n' > firmware/helixscreen-esp32/CMakeLists.txt
    git add -A
    git commit -qm base --no-verify
}

run_gate() {
    run bash -c '
        STAGED_ONLY="$1"; QC_TMP="$(mktemp -d)"; QC_COUNTS="$QC_TMP/counts"
        . "$2/scripts/qc/_lib.sh"
        . "$2/scripts/qc/esp32_compile.sh"
        cd "$3" && mkdir -p scripts && ln -sf "$2/scripts/esp32_syntax_check.py" scripts/
        qc_esp32_compile' _ "$1" "$REPO_ROOT" "$FIX"
}

@test "the trigger takes firmware sources and headers and nothing else" {
    cd "$REPO_ROOT" || return 1
    re="$(qc_trigger esp32_compile)"
    [ -n "$re" ]
    for p in src/printer/ams_state.cpp include/printer_state.h src/lib.c \
             firmware/helixscreen-esp32/main/main.cpp; do
        echo "$p" | grep -qE "$re" || fail "$p should trigger"
    done
    for p in ui_xml/globals.xml docs/devel/README.md tests/unit/test_x.cpp scripts/foo.py \
             src/printer/notes.md; do
        if echo "$p" | grep -qE "$re"; then fail "$p should not trigger"; fi
    done
}

@test "a staged firmware source with no cached flags is left to pre-push, never configured" {
    printf 'int a() { return 2; }\n' > src/printer/a.cpp
    git add src/printer/a.cpp
    run_gate true
    [ "$status" -eq 0 ]
    contains "no cached firmware flags" "$output"
    contains "pre-push configures them" "$output"
    run grep -c '^run' "$DOCKER_LOG"
    [ "$output" = "0" ] || fail "the hook started a container: $(cat "$DOCKER_LOG")"
}

@test "a commit staging no source or header starts nothing and prints nothing" {
    printf 'notes\n' > README.md
    git add README.md
    run_gate true
    [ "$status" -eq 0 ]
    [ -z "$output" ]
    [ ! -s "$DOCKER_LOG" ]
}

@test "the full sweep leaves the check to pre-push" {
    printf 'int a() { return 2; }\n' > src/printer/a.cpp
    git add src/printer/a.cpp
    run_gate false
    [ "$status" -eq 0 ]
    [ -z "$output" ]
    [ ! -s "$DOCKER_LOG" ]
}

@test "HELIX_ESP32_SYNTAX=0 skips it" {
    printf 'int a() { return 2; }\n' > src/printer/a.cpp
    git add src/printer/a.cpp
    HELIX_ESP32_SYNTAX=0 run_gate true
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}
