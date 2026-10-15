// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

/**
 * @file belt_settle.h
 * @brief Settling laps before a belt sweep
 *
 * After a tensioner turn the change stays mostly in the span beside the
 * tensioner until the gantry runs the belt loop through. Every sweep that can
 * follow a belt change runs these laps first (#1736). Pure: no LVGL, no I/O.
 */

namespace helix::calibration {

namespace belt_settle {
inline constexpr int LAPS = 3;
inline constexpr float INSET_MIN_MM = 20.0f; ///< laps stay at least this far inside the limits
inline constexpr float INSET_SPAN_FRACTION =
    0.07f; ///< ...or this share of the axis span, if larger
/// With several tools, the laps keep to the central half of each axis: parked
/// tools and their docks sit inside the travel.
inline constexpr float MULTI_TOOL_INSET_SPAN_FRACTION = 0.25f;
inline constexpr float SPEED_MAX_MM_S = 300.0f;
inline constexpr float SPEED_VELOCITY_FRACTION = 0.6f; ///< of toolhead max_velocity
inline constexpr float MIN_SIDE_MM = 20.0f; ///< a shorter lap side cannot run the belt loop
inline constexpr uint32_t MIN_TIMEOUT_MS = 60000;
inline constexpr float TIMEOUT_FACTOR = 4.0f;                  ///< of the planned duration
inline constexpr const char* STATE_NAME = "helix_belt_settle"; ///< SAVE_GCODE_STATE NAME=
} // namespace belt_settle

struct BeltSettleInputs {
    /// toolhead axis_minimum / axis_maximum, machine coordinates
    float x_min = 0.0f;
    float x_max = 0.0f;
    float y_min = 0.0f;
    float y_max = 0.0f;
    bool has_xy_limits = false;
    /// [resonance_tester] probe point (x, y, z), machine coordinates
    std::optional<std::array<float, 3>> probe_point;
    /// gcode_move.homing_origin: a G-code coordinate is machine minus origin
    float origin_x = 0.0f;
    float origin_y = 0.0f;
    float origin_z = 0.0f;
    float max_velocity_mm_s = 0.0f; ///< toolhead max_velocity; 0 when not yet reported
    int tool_count = 1;             ///< physical toolheads; docks are in the travel past one
};

enum class BeltSettleRefusal {
    NONE,
    NO_AXIS_LIMITS,
    NO_PROBE_POINT,
    NO_VELOCITY,
    SPAN_TOO_SMALL, ///< a lap side would be under belt_settle::MIN_SIDE_MM
};

struct BeltSettlePlan {
    BeltSettleRefusal refusal = BeltSettleRefusal::NONE;
    /// One script: SAVE_GCODE_STATE, G90, Z to the probe height, LAPS
    /// rectangles, back to the probe point's X/Y, M400, RESTORE_GCODE_STATE.
    /// Empty when refused.
    std::string gcode;
    float speed_mm_s = 0.0f;
    float inset_x_mm = 0.0f;
    float inset_y_mm = 0.0f;
    float seconds = 0.0f;    ///< XY path length over speed; the approach move is not counted
    uint32_t timeout_ms = 0; ///< max(MIN_TIMEOUT_MS, TIMEOUT_FACTOR x seconds)
};

[[nodiscard]] BeltSettlePlan plan_belt_settle(const BeltSettleInputs& in);

} // namespace helix::calibration
