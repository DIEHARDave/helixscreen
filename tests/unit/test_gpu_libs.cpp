// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gpu_libs.h"

#include <cerrno>
#include <cstring>
#include <dlfcn.h>

#include "../catch_amalgamated.hpp"

using helix::gpu_libs::Failure;
using helix::gpu_libs::ReturnKind;

namespace {

using IntFn = int (*)();
using PtrFn = void* (*)();
using DoubleFn = double (*)();
using CosFn = double (*)(double);

// Sorted by name, as scripts/gpu_lib_return_kinds.py writes the build's table.
const ReturnKind kTable[] = {
    {"drmIoctl", 'i'},
    {"drmModeGetResources", 'z'},
    {"gbm_bo_get_fd", 'i'},
    {"libinput_event_pointer_get_dx", 'd'},
};

} // namespace

TEST_CASE("Each function's failure comes from the return-kind table", "[gpu_libs]") {
    const size_t n = std::size(kTable);
    CHECK(helix::gpu_libs::failure_for("drmIoctl", kTable, n) == Failure::Negative);
    CHECK(helix::gpu_libs::failure_for("drmModeGetResources", kTable, n) == Failure::Zero);
    CHECK(helix::gpu_libs::failure_for("gbm_bo_get_fd", kTable, n) == Failure::Negative);
    CHECK(helix::gpu_libs::failure_for("libinput_event_pointer_get_dx", kTable, n) ==
          Failure::ZeroDouble);
    // Not in the table: 0 / NULL.
    CHECK(helix::gpu_libs::failure_for("eglGetDisplay", kTable, n) == Failure::Zero);
    CHECK(helix::gpu_libs::failure_for("aaa", kTable, n) == Failure::Zero);
    CHECK(helix::gpu_libs::failure_for("zzz", kTable, n) == Failure::Zero);
}

TEST_CASE("A signed libdrm/libinput call or a *_get_fd* fails with -1 and ENOSYS", "[gpu_libs]") {
    errno = 0;
    auto fn = reinterpret_cast<IntFn>(helix::gpu_libs::stand_in(Failure::Negative));
    CHECK(fn() == -1);
    CHECK(errno == ENOSYS);
}

TEST_CASE("A double-returning call fails with 0.0", "[gpu_libs]") {
    auto fn = reinterpret_cast<DoubleFn>(helix::gpu_libs::stand_in(Failure::ZeroDouble));
    CHECK(fn() == 0.0);
}

TEST_CASE("A pointer or EGL call fails with NULL / 0", "[gpu_libs]") {
    void* zero = helix::gpu_libs::stand_in(Failure::Zero);
    CHECK(reinterpret_cast<PtrFn>(zero)() == nullptr);
    CHECK(reinterpret_cast<IntFn>(zero)() == 0);
}

TEST_CASE("A missing GPU library gets a handle, not an abort", "[gpu_libs]") {
    void* lib = helix_gpu_lib_open("libhelix-does-not-exist.so.9");
    REQUIRE(lib != nullptr);
    dlclose(lib);
}

TEST_CASE("A missing library's handle never resolves another library's function", "[gpu_libs]") {
    // strlen is loaded in this process; a search of the missing library's
    // handle must not find it.
    void* lib = helix_gpu_lib_open("libhelix-does-not-exist.so.9");
    REQUIRE(lib != nullptr);
    void* fn = helix_gpu_lib_symbol(lib, "strlen");
    REQUIRE(fn != nullptr);
    CHECK(fn != dlsym(RTLD_DEFAULT, "strlen"));
    CHECK(fn == helix::gpu_libs::stand_in(Failure::Zero));
    dlclose(lib);
}

TEST_CASE("A library that loads resolves its real functions", "[gpu_libs]") {
    void* libm = helix_gpu_lib_open("libm.so.6");
    REQUIRE(libm != nullptr);
    void* fn = helix_gpu_lib_symbol(libm, "cos");
    REQUIRE(fn != nullptr);
    CHECK(reinterpret_cast<CosFn>(fn)(0.0) == 1.0);
    // A name that library lacks falls back to a stand-in.
    CHECK(helix_gpu_lib_symbol(libm, "helix_no_such_function") ==
          helix::gpu_libs::stand_in(Failure::Zero));
    dlclose(libm);
}

TEST_CASE("Every GPU library has its soname", "[gpu_libs]") {
    using helix::gpu_libs::Lib;
    CHECK(std::strcmp(helix::gpu_libs::soname(Lib::Drm), "libdrm.so.2") == 0);
    CHECK(std::strcmp(helix::gpu_libs::soname(Lib::Gbm), "libgbm.so.1") == 0);
    CHECK(std::strcmp(helix::gpu_libs::soname(Lib::Egl), "libEGL.so.1") == 0);
    CHECK(std::strcmp(helix::gpu_libs::soname(Lib::Gles2), "libGLESv2.so.2") == 0);
    CHECK(std::strcmp(helix::gpu_libs::soname(Lib::Input), "libinput.so.10") == 0);
}
