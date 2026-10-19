// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <initializer_list>

namespace helix::gpu_libs {

/// The display and GPU libraries a DRM build can use. Where the build carries
/// them as lazy-loading trampolines (HELIX_DLOPEN_GPU_LIBS, tools/implib), the
/// binary starts without them, and the first call into a missing one aborts the
/// process. Every path into them therefore asks available() first.
enum class Lib { Drm, Gbm, Egl, Gles2, Input };

/// The soname a library is loaded by.
const char* soname(Lib lib);

/// Whether the library loads on this system. Probed once per library and
/// cached, since the answer cannot change while the process runs; a library
/// that does not load is named in the log once. Always true in a build that
/// links the libraries directly.
bool available(Lib lib);

/// Whether every one of @p libs loads.
bool available_all(std::initializer_list<Lib> libs);

/// Handle from dlopen() for the library, or nullptr when it does not load.
void* handle(Lib lib);

/// Resolve a GLES2 entry point from the loaded libGLESv2, for
/// helix::gles2::load(). nullptr when the library or the symbol is missing.
void* gles2_proc(const char* name);

} // namespace helix::gpu_libs
