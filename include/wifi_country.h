// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <string>

/**
 * @file wifi_country.h
 * @brief WiFi regulatory country: the ISO 3166-1 alpha-2 table, the parsers
 *        for what the backends report, and the startup policy.
 *
 * A radio with no country set runs in the world regulatory domain ("00"),
 * where 5 GHz channels are passive-only: an active scan finds nothing there
 * until an access point's country element unlocks them, which in practice
 * means 5 GHz networks appear only after the first 2.4 GHz association.
 * Setting the country lifts that restriction.
 *
 * Everything here is pure: no I/O, no LVGL, no backend.
 */
namespace helix::wifi::country {

struct Entry {
    const char* code; ///< ISO 3166-1 alpha-2, upper case
    const char* name; ///< English short name
};

/// Number of entries in the country table (sorted by English name).
size_t table_size();

/// The table entry at @p index, which must be below table_size().
const Entry& table_entry(size_t index);

/// @p code upper-cased when it names a country in the table, else "".
/// World ("00") and anything malformed answer "".
std::string normalize(const std::string& code);

/// True for "" and "00": no country, the world regulatory domain.
bool is_world(const std::string& code);

/// Dropdown option string: @p not_set_label, then "Name (CC)" per country,
/// newline-separated.
std::string dropdown_options(const std::string& not_set_label);

/// Dropdown index for @p code: 0 (the not-set row) for world or unknown,
/// else 1 + its table index.
int dropdown_index(const std::string& code);

/// The code a dropdown index selects: "" for 0 or out of range.
std::string code_at_dropdown_index(int index);

/// Parse a wpa_supplicant `GET country` reply: "US" (no newline) when one is
/// configured, "FAIL" when none is. "" for world, failure or junk.
std::string parse_wpa_get_country(const std::string& reply);

/// Parse `iw reg get`: the country of the global section (the first
/// "country XX:" line). "" when it is world, absent or unparseable.
std::string parse_iw_reg_get(const std::string& output);

/// The alpha2= field of a CTRL-EVENT-REGDOM-CHANGE line, "" if absent.
std::string parse_regdom_change_alpha2(const std::string& event);

/// What to do with the radio's country once a backend is up.
struct Plan {
    /// Country to send to the backend; "" means leave the radio alone.
    std::string apply;
    /// Country the UI shows as current; "" means not set (world).
    std::string display;
};

/// Startup and on-change policy. @p stored is the user's saved choice
/// ("" = never chosen); @p system is what the radio reports now.
///
/// A saved choice wins and is sent only when the radio disagrees. With no
/// saved choice the radio is left alone: a country the device's own
/// configuration set is shown as current, never overwritten.
Plan plan(const std::string& stored, const std::string& system);

/// Whether WiFi settings should explain that a country unlocks 5 GHz: the
/// adapter can use 5 GHz, but the radio runs in the world domain.
bool should_hint_5ghz(bool supports_5ghz, const std::string& effective_country);

} // namespace helix::wifi::country
