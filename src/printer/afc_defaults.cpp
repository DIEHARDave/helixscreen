// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "afc_defaults.h"

#include <algorithm>

namespace helix::printer {

std::vector<DeviceSection> afc_default_sections() {
    return {
        {"setup", "Setup", 0, "Calibration, LED, and system configuration"},
        {"speed", "Speed Settings", 1, "Move speed multipliers"},
        {"toolhead", "Toolhead", 2, "Extruder distances and sensor configuration"},
        {"maintenance", "Maintenance", 3, "Lane tests and motor resets"},
        {"hub", "Hub & Cutter", 4, "Blade change and parking"},
        {"tip_forming", "Tip Forming", 5, "Tip shaping configuration"},
        {"purge", "Purge & Wipe", 6, "Purge tower and brush settings"},
    };
}

std::vector<DeviceAction> afc_default_actions() {
    using DA = DeviceAction;
    return {
        // Setup section (calibration + LED)
        DA::button("calibration_wizard", "Run Calibration Wizard", "setup", "play",
                   "Interactive calibration for all lanes"),
        DA::slider("bowden_length", "Bowden Length", "setup", 450.0f, 100, 2000, "mm", "ruler",
                   "Distance from hub to toolhead"),

        // Speed section
        DA::slider("speed_fwd", "Forward Multiplier", "speed", 1.0f, 0.5f, 2.0f, "x", "speed_up",
                   "Speed multiplier for forward moves")
            .with_step(0.05f),
        DA::slider("speed_rev", "Reverse Multiplier", "speed", 1.0f, 0.5f, 2.0f, "x", "speed_down",
                   "Speed multiplier for reverse moves")
            .with_step(0.05f),

        // Toolhead section (extruder distances)
        DA::slider("tool_stn", "Sensor to Nozzle", "toolhead", 72.0f, 0.0f, 200.0f, "mm", "ruler",
                   "Distance from toolhead sensor to nozzle tip"),
        DA::slider("tool_stn_unload", "Unload Distance", "toolhead", 100.0f, 0.0f, 200.0f, "mm",
                   "ruler", "Retraction distance to clear extruder gears"),
        DA::slider("tool_sensor_after_extruder", "Post-Sensor Clear", "toolhead", 0.0f, 0.0f,
                   100.0f, "mm", "ruler", "Extra distance to move once sensors clear"),

        // Maintenance section
        DA::button("test_lanes", "Test All Lanes", "maintenance", "play_circle",
                   "Run test sequence on all lanes"),
        DA::button("change_blade", "Change Blade", "maintenance", "wrench",
                   "Initiate blade change procedure"),
        DA::button("park", "Park", "maintenance", "home_import", "Park the AFC system"),
        DA::button("brush", "Clean Brush", "maintenance", "sync", "Run brush cleaning sequence"),
        DA::button("reset_motor", "Reset Motor Timer", "maintenance", "progress_clock",
                   "Reset motor run-time counter"),

        // Setup section (LED & Modes)
        DA::button("led_toggle", "Turn On LEDs", "setup", "lightbulb_on", "Toggle AFC LED strip"),
        DA::button("led_extruder", "Toolhead LED", "setup", "lightbulb_on",
                   "Toggle toolhead LED for an extruder"),
        DA::button("quiet_mode", "Toggle Quiet Mode", "setup", "volume_off",
                   "Enable/disable quiet operation mode"),

        // Hub & Cutter section
        DA::toggle("hub_cut_enabled", "Cutter Enabled", "hub", false, "tune",
                   "Enable or disable the hub cutter"),
        DA::slider("hub_cut_dist", "Cut Distance", "hub", 50.0f, 0.0f, 100.0f, "mm", "ruler",
                   "Distance for hub cutter operation"),
        DA::slider("hub_bowden_length", "Hub Bowden Length", "hub", 450.0f, 100.0f, 2000.0f, "mm",
                   "ruler", "Bowden tube length from hub to toolhead"),
        DA::toggle("assisted_retract", "Assisted Retract", "hub", false, "undo",
                   "Enable assisted retraction at hub"),

        // Tip Forming section
        DA::slider("ramming_volume", "Ramming Volume", "tip_forming", 0.0f, 0.0f, 100.0f, "mm³",
                   "water", "Volume of filament used during ramming"),
        DA::slider("unloading_speed_start", "Unloading Start Speed", "tip_forming", 80.0f, 0.0f,
                   200.0f, "mm/s", "speed", "Initial speed for filament unloading"),
        DA::slider("cooling_tube_length", "Cooling Tube Length", "tip_forming", 15.0f, 0.0f, 100.0f,
                   "mm", "thermometer_minus", "Length of the cooling tube section"),
        DA::slider("cooling_tube_retraction", "Cooling Tube Retraction", "tip_forming", 0.0f, 0.0f,
                   100.0f, "mm", "thermometer_minus", "Retraction distance in the cooling tube"),

        // Purge & Wipe section
        DA::toggle("purge_enabled", "Enable Purge", "purge", false, "water",
                   "Enable purging during tool changes"),
        DA::slider("purge_length", "Purge Length", "purge", 50.0f, 0.0f, 200.0f, "mm", "ruler",
                   "Length of filament to purge"),
        DA::toggle("brush_enabled", "Enable Brush Wipe", "purge", false, "sync",
                   "Enable brush wipe after purging"),
    };
}

void afc_add_toolhead_calibration_action(std::vector<DeviceAction>& actions) {
    const auto first_toolhead = std::find_if(actions.begin(), actions.end(),
                                             [](const auto& a) { return a.section == "toolhead"; });
    actions.insert(first_toolhead,
                   DeviceAction::button("toolhead_calibration", "Calibrate Toolhead", "toolhead",
                                        "play",
                                        "Guided sensor-to-nozzle, unload and cutter retract "
                                        "calibration"));
}

AfcCapabilities afc_default_capabilities() {
    return {
        .supports_endless_spool = true,
        .supports_bypass = true,
        .supports_purge = true,
        .tip_method = TipMethod::CUT,
    };
}

} // namespace helix::printer
