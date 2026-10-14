// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../../include/belt_tension_types.h"

#include <utility>
#include <vector>

/**
 * @file belt_test_curves.h
 * @brief Synthetic resonance curves shared by the belt comparison and tuning tests
 */

namespace helix::calibration::test {

/// Lorentzian peaks (centre Hz, height) on a flat floor, bins every bin_hz from
/// lo up to (not including) hi. Klipper's resonance CSVs space bins 3200/4096 Hz
/// apart. Each peak has a 5 Hz half-width.
inline BeltCurve lorentzian_curve(std::vector<std::pair<float, float>> peaks_hz_height, float lo,
                                  float hi, float bin_hz = 3200.0f / 4096.0f, float floor = 0.01f) {
    BeltCurve c;
    for (float f = lo; f < hi; f += bin_hz) {
        float v = floor;
        for (const auto& [peak_hz, height] : peaks_hz_height) {
            const float d = (f - peak_hz) / 5.0f;
            v += height / (1.0f + d * d);
        }
        c.emplace_back(f, v);
    }
    return c;
}

} // namespace helix::calibration::test
