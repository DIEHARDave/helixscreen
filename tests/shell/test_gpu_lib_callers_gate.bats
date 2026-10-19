#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_gpu_lib_callers.py: only reviewed objects may
# call into the runtime-loaded GPU libraries. Each test builds a scratch build
# directory with real objects compiled by the host compiler and a trampoline
# table in the shape Implib.so writes.

GATE="scripts/check_gpu_lib_callers.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    load helpers.bash
    REPO="$PWD"
    BUILD="$BATS_TEST_TMPDIR/build"
    mkdir -p "$BUILD/implib"
    printf 'static const char *const sym_names[] = {\n  "drmModeGetResources",\n  "gbm_create_device",\n  0\n};\n' \
        > "$BUILD/implib/libdrm.so.2.init.c"
}

# Compile a one-function object at $BUILD/$1 that calls $2.
caller_object() {
    local obj="$BUILD/$1" fn="$2"
    mkdir -p "$(dirname "$obj")"
    printf 'void %s(void);\nvoid call_it(void) { %s(); }\n' "$fn" "$fn" > "$BATS_TEST_TMPDIR/src.c"
    cc -c "$BATS_TEST_TMPDIR/src.c" -o "$obj"
}

@test "passes when only reviewed objects call a stubbed function" {
    caller_object obj/api/display_backend_drm.o drmModeGetResources
    caller_object obj/ui/backdrop_blur.o gbm_create_device
    caller_object obj/system/other.o printf
    run python3 "$GATE" --build-dir "$BUILD"
    [ "$status" -eq 0 ] || fail "expected a pass: $output"
    contains "2 reviewed objects" "$output"
}

@test "fails, naming the object and function, when an unreviewed object calls one" {
    caller_object obj/api/display_backend_drm.o drmModeGetResources
    caller_object obj/ui/new_panel.o drmModeGetResources
    run python3 "$GATE" --build-dir "$BUILD"
    [ "$status" -eq 1 ] || fail "expected a failure: $output"
    contains "obj/ui/new_panel.o: drmModeGetResources" "$output"
}

@test "ignores the trampoline objects themselves" {
    caller_object implib/libdrm.so.2.tramp.o drmModeGetResources
    run python3 "$GATE" --build-dir "$BUILD"
    [ "$status" -eq 0 ] || fail "expected a pass: $output"
}

@test "fails when there is no trampoline table to read" {
    rm "$BUILD/implib/libdrm.so.2.init.c"
    caller_object obj/ui/new_panel.o drmModeGetResources
    run python3 "$GATE" --build-dir "$BUILD"
    [ "$status" -eq 1 ] || fail "a gate with nothing to compare must not pass: $output"
    contains "no trampoline tables" "$output"
}
