// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string_view>
#include <vector>

/**
 * @file screensaver_registry.h
 * @brief The screensavers the app knows, as data
 *
 * Always compiled, including in builds without screensavers, so settings code can clamp a
 * stored type without the saver sources. The saver implementations are created in
 * src/ui/screensaver_manager.cpp.
 */

/**
 * @brief Available screensaver types
 *
 * In-app identity only. Config, HELIX_SCREENSAVER_NOW, ctl and the level store name a saver
 * by its stable SCREENSAVERS name instead, so reordering or compiling out a saver never
 * changes what a stored value means.
 */
enum class ScreensaverType : int {
    OFF = 0,
    FLYING_TOASTERS = 1,
    STARFIELD = 2,
    PIPES_3D = 3,
    BOUNCING_PRINTER = 4,
    FIREWORKS = 5,
    BOUNCING_LOGO = 6,
    MAZE_3D = 7,
    HYPERSPACE = 8,
    FLIP_CLOCK = 9,
};

namespace helix::ui {

/// Color depths a saver draws at, as bits of ScreensaverInfo::depths.
enum SaverDepth : uint8_t {
    SAVER_DEPTH_16 = 1u << 0,
    SAVER_DEPTH_32 = 1u << 1,
};

/// One registered screensaver.
struct ScreensaverInfo {
    ScreensaverType type;
    /// Stable name: the level store key and a HELIX_SCREENSAVER_NOW value.
    const char* name;
    /// Translation key, which is also the English label in the settings dropdown.
    const char* label_key;
    /// SaverDepth bits the saver draws at.
    uint8_t depths;
};

/// Every screensaver, in type order starting at 1: row i has type i + 1. The settings
/// dropdown in ui_xml/settings_display_overlay.xml lists "Off" and then these labels in
/// this order, which a test checks.
inline constexpr ScreensaverInfo SCREENSAVERS[] = {
    {ScreensaverType::FLYING_TOASTERS, "toasters", "Flying Toasters",
     SAVER_DEPTH_16 | SAVER_DEPTH_32},
    {ScreensaverType::STARFIELD, "starfield", "Starfield", SAVER_DEPTH_16 | SAVER_DEPTH_32},
    {ScreensaverType::PIPES_3D, "pipes", "3D Pipes", SAVER_DEPTH_16 | SAVER_DEPTH_32},
    {ScreensaverType::BOUNCING_PRINTER, "bounce", "Bouncing Printer",
     SAVER_DEPTH_16 | SAVER_DEPTH_32},
    {ScreensaverType::FIREWORKS, "fireworks", "Fireworks", SAVER_DEPTH_16 | SAVER_DEPTH_32},
    {ScreensaverType::BOUNCING_LOGO, "logo", "Bouncing Logo", SAVER_DEPTH_16 | SAVER_DEPTH_32},
    {ScreensaverType::MAZE_3D, "maze", "3D Maze", SAVER_DEPTH_16 | SAVER_DEPTH_32},
    {ScreensaverType::HYPERSPACE, "hyperspace", "Hyperspace", SAVER_DEPTH_16 | SAVER_DEPTH_32},
    {ScreensaverType::FLIP_CLOCK, "flipclock", "Flip Clock", SAVER_DEPTH_16 | SAVER_DEPTH_32},
};

inline constexpr size_t SCREENSAVER_COUNT = std::size(SCREENSAVERS);

/// Translation key and English label of the dropdown's first option.
inline constexpr const char* SCREENSAVER_OFF_LABEL_KEY = "Off";

/// Stable name of OFF, the persisted value of a screensaver turned off.
inline constexpr const char* SCREENSAVER_OFF_NAME = "off";

/// What each integer /display/screensaver_type value meant, indexed by that number. Frozen:
/// these numbers live on in old configs and backups, so the table never reorders or grows,
/// whatever happens to ScreensaverType or SCREENSAVERS.
inline constexpr const char* LEGACY_SCREENSAVER_TYPE_NAMES[] = {
    "off",       "toasters", "starfield", "pipes",      "bounce",
    "fireworks", "logo",     "maze",      "hyperspace", "flipclock",
};

/// Type a fresh install prefers, when this build can draw it. The screensaver gate measures it
/// and steps it down, or runs a lighter saver in its place, when it costs too much.
inline constexpr ScreensaverType DEFAULT_SCREENSAVER_TYPE = ScreensaverType::FLYING_TOASTERS;

/// Highest valid type value.
constexpr int screensaver_last_type() {
    return static_cast<int>(SCREENSAVERS[SCREENSAVER_COUNT - 1].type);
}

/// A stored type made valid: below OFF reads as OFF, past the last type as the last type.
constexpr int clamp_screensaver_type(int value) {
    if (value < 0) {
        return 0;
    }
    if (value > screensaver_last_type()) {
        return screensaver_last_type();
    }
    return value;
}

/// The row for `type`, or nullptr for OFF and unregistered values.
constexpr const ScreensaverInfo* find_screensaver(ScreensaverType type) {
    for (const ScreensaverInfo& info : SCREENSAVERS) {
        if (info.type == type) {
            return &info;
        }
    }
    return nullptr;
}

/// Type a fresh install runs at `build_depth` (a SaverDepth bit): the preferred default when it
/// draws at that depth, otherwise the first registered saver that does, and OFF if none do.
/// A 16 bpp build excludes the 32 bpp-only savers, so a default naming one would start nothing.
constexpr ScreensaverType default_screensaver_type(uint8_t build_depth) {
    const ScreensaverInfo* preferred = find_screensaver(DEFAULT_SCREENSAVER_TYPE);
    if (preferred != nullptr && (preferred->depths & build_depth) != 0U) {
        return DEFAULT_SCREENSAVER_TYPE;
    }
    for (const ScreensaverInfo& row : SCREENSAVERS) {
        if ((row.depths & build_depth) != 0U) {
            return row.type;
        }
    }
    return ScreensaverType::OFF;
}

/// The row whose stable name is `name`, or nullptr.
constexpr const ScreensaverInfo* find_screensaver_by_name(std::string_view name) {
    for (const ScreensaverInfo& info : SCREENSAVERS) {
        if (name == info.name) {
            return &info;
        }
    }
    return nullptr;
}

/// Persisted name of `type`: its row's stable name, or "off" for OFF and unregistered values.
constexpr const char* screensaver_name(ScreensaverType type) {
    const ScreensaverInfo* info = find_screensaver(type);
    return info != nullptr ? info->name : SCREENSAVER_OFF_NAME;
}

/// ASCII case-insensitive equality.
constexpr bool equals_ignoring_case(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); i++) {
        const char x = (a[i] >= 'A' && a[i] <= 'Z') ? static_cast<char>(a[i] - 'A' + 'a') : a[i];
        const char y = (b[i] >= 'A' && b[i] <= 'Z') ? static_cast<char>(b[i] - 'A' + 'a') : b[i];
        if (x != y) {
            return false;
        }
    }
    return true;
}

/// The type a stored name selects, ignoring case: "off" or a registered name. nullopt for
/// anything else.
constexpr std::optional<ScreensaverType> screensaver_from_name(std::string_view name) {
    if (equals_ignoring_case(name, SCREENSAVER_OFF_NAME)) {
        return ScreensaverType::OFF;
    }
    for (const ScreensaverInfo& info : SCREENSAVERS) {
        if (equals_ignoring_case(name, info.name)) {
            return info.type;
        }
    }
    return std::nullopt;
}

/// The name a legacy integer screensaver_type value stood for, or nullptr out of range.
constexpr const char* legacy_screensaver_name(long long value) {
    if (value < 0 || value >= static_cast<long long>(std::size(LEGACY_SCREENSAVER_TYPE_NAMES))) {
        return nullptr;
    }
    return LEGACY_SCREENSAVER_TYPE_NAMES[value];
}

/**
 * @brief Saver a HELIX_SCREENSAVER_NOW value asks for
 *
 * A registered name starts that saver. Any other value starts the configured saver, or
 * flying toasters when the configured type is OFF.
 */
constexpr ScreensaverType resolve_screensaver_now(std::string_view value,
                                                  ScreensaverType configured) {
    if (const ScreensaverInfo* info = find_screensaver_by_name(value)) {
        return info->type;
    }
    return configured != ScreensaverType::OFF ? configured : ScreensaverType::FLYING_TOASTERS;
}

/**
 * @brief The saver that runs when `configured` is chosen
 *
 * `configured` itself unless `too_heavy(type)` rejects it; then `preferred`, then the first
 * registered saver it does not reject. When it rejects every saver, `configured`,
 * which the manager shows as a static black screen. OFF stays OFF. The stored choice is never
 * rewritten, so a config moved to a faster board runs the saver it names.
 */
template <typename TooHeavy>
constexpr ScreensaverType
runnable_screensaver(ScreensaverType configured, TooHeavy&& too_heavy,
                     ScreensaverType preferred = DEFAULT_SCREENSAVER_TYPE) {
    if (configured == ScreensaverType::OFF || !too_heavy(configured)) {
        return configured;
    }
    if (!too_heavy(preferred)) {
        return preferred;
    }
    for (const ScreensaverInfo& row : SCREENSAVERS) {
        if (!too_heavy(row.type)) {
            return row.type;
        }
    }
    return configured;
}

/// What the settings dropdown offers, in order: OFF, then every registered saver
/// `too_heavy(type)` does not reject.
template <typename TooHeavy>
std::vector<ScreensaverType> screensaver_choices(TooHeavy&& too_heavy) {
    std::vector<ScreensaverType> choices{ScreensaverType::OFF};
    for (const ScreensaverInfo& row : SCREENSAVERS) {
        if (!too_heavy(row.type)) {
            choices.push_back(row.type);
        }
    }
    return choices;
}

/// Dropdown index of `runs` in `choices`. A saver missing from them runs only as the black
/// screen of a board where every saver is too heavy, which reads as Off: index 0.
inline size_t screensaver_choice_index(const std::vector<ScreensaverType>& choices,
                                       ScreensaverType runs) {
    for (size_t i = 0; i < choices.size(); i++) {
        if (choices[i] == runs) {
            return i;
        }
    }
    return 0;
}

} // namespace helix::ui
