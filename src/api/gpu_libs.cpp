// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gpu_libs.h"

#include <spdlog/spdlog.h>

#include <array>
#include <dlfcn.h>
#include <mutex>

namespace helix::gpu_libs {

namespace {

constexpr size_t kLibCount = 5;

struct Slot {
    std::once_flag once;
    void* handle = nullptr;
};

std::array<Slot, kLibCount>& slots() {
    static std::array<Slot, kLibCount> s;
    return s;
}

size_t index(Lib lib) {
    return static_cast<size_t>(lib);
}

} // namespace

const char* soname(Lib lib) {
    switch (lib) {
    case Lib::Drm:
        return "libdrm.so.2";
    case Lib::Gbm:
        return "libgbm.so.1";
    case Lib::Egl:
        return "libEGL.so.1";
    case Lib::Gles2:
        return "libGLESv2.so.2";
    case Lib::Input:
        return "libinput.so.10";
    }
    return "";
}

void* handle(Lib lib) {
    Slot& slot = slots()[index(lib)];
    // RTLD_GLOBAL so the trampolines' own dlopen of the same soname, and any
    // library that needs this one, resolve to the copy already loaded.
    std::call_once(slot.once, [&] {
        slot.handle = dlopen(soname(lib), RTLD_LAZY | RTLD_GLOBAL);
        if (slot.handle == nullptr) {
            const char* err = dlerror();
            spdlog::info("[GpuLibs] {} does not load: {}", soname(lib), err ? err : "unknown");
        }
    });
    return slot.handle;
}

bool available(Lib lib) {
#ifdef HELIX_DLOPEN_GPU_LIBS
    return handle(lib) != nullptr;
#else
    // Linked directly: the binary would not have started without it.
    (void)lib;
    return true;
#endif
}

bool available_all(std::initializer_list<Lib> libs) {
    for (Lib lib : libs) {
        if (!available(lib)) {
            return false;
        }
    }
    return true;
}

void* gles2_proc(const char* name) {
    void* lib = handle(Lib::Gles2);
    return lib != nullptr ? dlsym(lib, name) : nullptr;
}

} // namespace helix::gpu_libs
