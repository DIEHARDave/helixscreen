// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "macro_param_cache.h"

#include "hv/json.hpp"

namespace helix::test {

/// The printer's macros read (none of them stop the printer), so the file
/// scan's printer-stopping command check answers instead of reporting it
/// could not run. Cleared again on destruction.
struct ScopedMacrosRead {
    ScopedMacrosRead() {
        MacroParamCache::instance().populate_from_configfile(nlohmann::json::object(), {});
    }
    ~ScopedMacrosRead() {
        MacroParamCache::instance().clear();
    }
    ScopedMacrosRead(const ScopedMacrosRead&) = delete;
    ScopedMacrosRead& operator=(const ScopedMacrosRead&) = delete;
};

} // namespace helix::test
