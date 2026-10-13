// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

namespace helix {

class Config;
class PanelWidgetConfig;

/// The `led` value of every placed home light button, page by page. A button
/// with no value yet reads as @p pending.
std::vector<std::string> home_light_button_keys(const PanelWidgetConfig& home,
                                                const std::string& pending);

/// Copy leds/light_button_pending into every home light button with no `led`
/// value, then clear it. True when a button was written.
bool adopt_pending_light_button(Config& cfg, PanelWidgetConfig& home);

/// What LED on at Start turns on: every device the home light buttons drive,
/// or the chamber light when there are none.
std::vector<std::string> home_light_button_targets();

/// Whether any device in home_light_button_targets() is on: the light the
/// print-status Light button drives and shows.
bool home_light_buttons_lit();

/// What a light button tap did.
enum class LightToggle {
    Busy,     ///< a light command awaits its ACK; a toast says the tap waits for it
    NoTarget, ///< nothing to switch
    On,       ///< switched on
    Off,      ///< switched off
};

/// Toggle @p ids as every light button does: refused with a toast while any
/// light command is in flight, and nothing when @p ids is empty.
LightToggle toggle_lights(const std::vector<std::string>& ids);

/// Resolve leds/light_button_pending against the home layout, clearing it even
/// when no light button is placed, then offer LED on at Start the targets the
/// home light buttons drive. Runs at discovery-complete and when a WLED
/// discovery settles, and waits while one is in flight; LED on at Start applies
/// at most once per printer session.
void settle_light_buttons();

} // namespace helix
