// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gpu_libs.h"

#include <cstring>
#include <dlfcn.h>

#include "../catch_amalgamated.hpp"

namespace {

using LongFn = long (*)();
using StrlenFn = size_t (*)(const char*);

} // namespace

TEST_CASE("A missing GPU library gets a handle, not an abort", "[gpu_libs]") {
    void* lib = helix_gpu_lib_open("libhelix-does-not-exist.so.9");
    REQUIRE(lib != nullptr);
    dlclose(lib);
}

TEST_CASE("A function of a missing GPU library returns 0", "[gpu_libs]") {
    void* fn = helix_gpu_lib_symbol(nullptr, "drmModeGetResources");
    REQUIRE(fn != nullptr);
    CHECK(reinterpret_cast<LongFn>(fn)() == 0);
}

TEST_CASE("A symbol a loaded library lacks returns the same stand-in", "[gpu_libs]") {
    void* libc = dlopen("libc.so.6", RTLD_LAZY);
    REQUIRE(libc != nullptr);
    void* fn = helix_gpu_lib_symbol(libc, "helix_no_such_function");
    REQUIRE(fn != nullptr);
    CHECK(fn == helix_gpu_lib_symbol(nullptr, "anything_else"));
    CHECK(reinterpret_cast<LongFn>(fn)() == 0);
    dlclose(libc);
}

TEST_CASE("A library that loads resolves its real functions", "[gpu_libs]") {
    void* libc = helix_gpu_lib_open("libc.so.6");
    REQUIRE(libc != nullptr);
    void* fn = helix_gpu_lib_symbol(libc, "strlen");
    REQUIRE(fn != nullptr);
    CHECK(reinterpret_cast<StrlenFn>(fn)("four") == 4);
    dlclose(libc);
}

TEST_CASE("Every GPU library has its soname", "[gpu_libs]") {
    using helix::gpu_libs::Lib;
    CHECK(std::strcmp(helix::gpu_libs::soname(Lib::Drm), "libdrm.so.2") == 0);
    CHECK(std::strcmp(helix::gpu_libs::soname(Lib::Gbm), "libgbm.so.1") == 0);
    CHECK(std::strcmp(helix::gpu_libs::soname(Lib::Egl), "libEGL.so.1") == 0);
    CHECK(std::strcmp(helix::gpu_libs::soname(Lib::Gles2), "libGLESv2.so.2") == 0);
    CHECK(std::strcmp(helix::gpu_libs::soname(Lib::Input), "libinput.so.10") == 0);
}
