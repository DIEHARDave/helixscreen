// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "platform_info.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace helix::platform {

/// Everything keyed on a platform key: its display name, the ELF header its
/// release binary must carry, and the Moonraker files a debug bundle captures.
/// A new platform is one row in platform_table.cpp.
struct Info {
    const char* key;
    const char* display_name;
    bool has_printer_hardware; ///< false for generic hosts (Pi, x86) and the K-Touch
    uint8_t elf_class;         ///< 1 = ELFCLASS32, 2 = ELFCLASS64, 0 = no ELF release to check
    uint8_t elf_data;          ///< EI_DATA: 1 = little-endian, 2 = big-endian
    uint16_t elf_machine;      ///< e_machine
    std::vector<std::string> diagnostic_files; ///< Moonraker paths; basename is the bundle key
};

/// Platform key of this build ("pi", "ad5m", "mips", ...), from the compile-time
/// HELIX_PLATFORM_* define. Self-update asset names, telemetry and crash reports
/// all use it.
std::string current_key();

/// Asset keys a release may list the @p key build under, best match first.
/// Releases cut before the unified MIPS build carry no "mips" asset, only the
/// board's own ("ad5x" when @p mod_layout, else "k1"), so a MIPS build moving
/// back to one of those releases needs that key too. A Creator 5 or Creator 5
/// Pro (@p machine) has no pre-unification build of its own, and the AD5X one
/// is another board's binary, so it answers to "mips" alone. Every other key
/// maps to itself alone.
std::vector<std::string> release_asset_keys(const std::string& key,
                                            std::optional<FlashForgeMachine> machine,
                                            bool mod_layout);

/// Table row for @p key, or nullptr for an unknown key.
const Info* find(const std::string& key);

/// Display name for @p key; the key itself when unrecognised.
std::string display_name(const std::string& key);

/// True when the first 20 bytes of an ELF file match @p platform's expectation.
bool elf_header_matches(const Info& platform, const uint8_t (&header)[20]);

} // namespace helix::platform
