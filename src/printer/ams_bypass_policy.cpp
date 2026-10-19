// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_bypass_policy.h"

#include "macro_choice.h"
#include "printer_discovery.h"
#include "settings_manager.h"

#include <algorithm>

namespace helix {

namespace {

/// The conventional wrapper names on hardware whose filament path has a master
/// switch but no bypass command. An owner whose macros are named differently
/// picks theirs in the device operations.
constexpr const char* kConventionalBypassOn = "ACE_BYPASS_ON";
constexpr const char* kConventionalBypassOff = "ACE_BYPASS_OFF";

} // namespace

BypassMacros detect_bypass_macros(const PrinterDiscovery& hw) {
    return {hw.has_macro(kConventionalBypassOn) ? std::string(kConventionalBypassOn) : "",
            hw.has_macro(kConventionalBypassOff) ? std::string(kConventionalBypassOff) : ""};
}

std::vector<std::string> bypass_macro_options(const PrinterDiscovery& hw) {
    std::vector<std::string> out;
    for (const auto& macro : hw.macros()) {
        if (macro.find("BYPASS") != std::string::npos) {
            out.push_back(macro);
        }
    }
    if (out.empty()) {
        return out;
    }
    std::sort(out.begin(), out.end());
    out.insert(out.begin(), kAutoMacro);
    return out;
}

BypassMacros resolve_bypass_macros(const BypassMacros& detected, const std::string& on_choice,
                                   const std::string& off_choice) {
    BypassMacros macros{resolve_macro_choice(on_choice, detected.on),
                        resolve_macro_choice(off_choice, detected.off)};

    // Half a pair cannot round-trip: engaging bypass with no way back strands
    // the machine in a state the UI offers no exit from.
    if (macros.on.empty() || macros.off.empty()) {
        return {};
    }
    return macros;
}

BypassMacros resolve_bypass_macros(const PrinterDiscovery& hw, const std::string& on_override,
                                   const std::string& off_override) {
    return resolve_bypass_macros(detect_bypass_macros(hw), on_override, off_override);
}

BypassMacros resolve_bypass_macros_for(const PrinterDiscovery& hw) {
    auto& settings = SettingsManager::instance();
    return resolve_bypass_macros(hw, settings.get_ace_bypass_on_macro(),
                                 settings.get_ace_bypass_off_macro());
}

bool bypass_available_for(bool firmware_supports_bypass) {
    if (firmware_supports_bypass) {
        return true; // no need to touch settings when the firmware already says yes
    }
    return bypass_available(firmware_supports_bypass,
                            SettingsManager::instance().get_ams_force_bypass_controls());
}

} // namespace helix
