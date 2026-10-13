// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// One classification for a value a widget judges, shared by every widget that
// colours a reading. Pure functions, no LVGL: each returns a ValueStatus and the
// caller maps it to its own appearance with bind_style_if_eq.

namespace helix {

/// The ints match the ams_env_ind_<n>_humidity_status encoding (0 ok, 1 warn, 2 danger).
enum class ValueStatus : int { Normal = 0, Warn = 1, Danger = 2 };

} // namespace helix

namespace helix::status {

/// A speed or flow override is Warn whenever it is off 100%.
constexpr ValueStatus override_status(int pct) {
    return pct != 100 ? ValueStatus::Warn : ValueStatus::Normal;
}

/// A Z-offset is Warn while there is an unsaved one to save.
constexpr ValueStatus z_offset_status(bool save_available) {
    return save_available ? ValueStatus::Warn : ValueStatus::Normal;
}

} // namespace helix::status
