// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "bed_grid.h"

#include <algorithm>
#include <cmath>

namespace helix {

namespace {

/// Multiples of @p spacing strictly inside (lo, hi).
std::vector<double> interior_multiples(double lo, double hi, double spacing) {
    constexpr double EDGE_EPSILON = 1e-6;
    std::vector<double> out;
    for (double v = std::ceil((lo + EDGE_EPSILON) / spacing) * spacing; v < hi - EDGE_EPSILON;
         v += spacing) {
        out.push_back(v);
    }
    return out;
}

} // namespace

std::vector<BedGridSegment> bed_grid_lines(double x_min, double x_max, double y_min, double y_max,
                                           double spacing_mm, bool circular) {
    std::vector<BedGridSegment> lines;
    if (!(x_max > x_min) || !(y_max > y_min) || !(spacing_mm > 0.0)) {
        return lines;
    }

    if (!circular) {
        for (double x : interior_multiples(x_min, x_max, spacing_mm)) {
            lines.push_back({x, y_min, x, y_max});
        }
        for (double y : interior_multiples(y_min, y_max, spacing_mm)) {
            lines.push_back({x_min, y, x_max, y});
        }
        return lines;
    }

    const double cx = (x_min + x_max) / 2.0;
    const double cy = (y_min + y_max) / 2.0;
    const double r = std::min(x_max - x_min, y_max - y_min) / 2.0;
    for (double x : interior_multiples(cx - r, cx + r, spacing_mm)) {
        const double half = std::sqrt(r * r - (x - cx) * (x - cx));
        lines.push_back({x, cy - half, x, cy + half});
    }
    for (double y : interior_multiples(cy - r, cy + r, spacing_mm)) {
        const double half = std::sqrt(r * r - (y - cy) * (y - cy));
        lines.push_back({cx - half, y, cx + half, y});
    }
    return lines;
}

} // namespace helix
