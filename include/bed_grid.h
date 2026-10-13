// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <vector>

namespace helix {

/// Spacing of the reference grid every bed view draws, in printer mm.
inline constexpr double kBedGridSpacingMm = 50.0;

/// One grid line in printer mm.
struct BedGridSegment {
    double x1, y1, x2, y2;
};

/**
 * @brief Interior grid lines of a bed, at whole multiples of @p spacing_mm
 *
 * A rectangular bed gets a line at every multiple strictly inside
 * [x_min, x_max] x [y_min, y_max], each spanning the box edge to edge. A round
 * bed is the circle inscribed in that box (radius = the smaller half-size), and
 * each line is its chord. Lines on the perimeter are left out on both shapes:
 * every caller draws its own outline.
 *
 * Empty for a degenerate box or a non-positive spacing.
 */
std::vector<BedGridSegment> bed_grid_lines(double x_min, double x_max, double y_min, double y_max,
                                           double spacing_mm, bool circular);

} // namespace helix
