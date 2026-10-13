// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "bed_grid.h"

#include <cmath>
#include <vector>

#include "../catch_amalgamated.hpp"

using Catch::Approx;
using helix::bed_grid_lines;
using helix::BedGridSegment;

namespace {

std::vector<double> vertical_xs(const std::vector<BedGridSegment>& lines) {
    std::vector<double> xs;
    for (const auto& s : lines) {
        if (s.x1 == s.x2) {
            xs.push_back(s.x1);
        }
    }
    return xs;
}

std::vector<double> horizontal_ys(const std::vector<BedGridSegment>& lines) {
    std::vector<double> ys;
    for (const auto& s : lines) {
        if (s.y1 == s.y2) {
            ys.push_back(s.y1);
        }
    }
    return ys;
}

} // namespace

TEST_CASE("rectangular bed grid: interior multiples, edge to edge", "[bed_grid]") {
    const auto lines = bed_grid_lines(0, 235, 0, 220, 50, false);

    // 0 is the perimeter; 250 is past the bed.
    CHECK(vertical_xs(lines) == std::vector<double>{50, 100, 150, 200});
    CHECK(horizontal_ys(lines) == std::vector<double>{50, 100, 150, 200});
    REQUIRE(lines.size() == 8);
    for (const auto& s : lines) {
        if (s.x1 == s.x2) {
            CHECK(s.y1 == 0.0);
            CHECK(s.y2 == 220.0);
        } else {
            CHECK(s.x1 == 0.0);
            CHECK(s.x2 == 235.0);
        }
    }
}

TEST_CASE("rectangular bed grid leaves out lines on an aligned perimeter", "[bed_grid]") {
    const auto lines = bed_grid_lines(0, 300, 0, 300, 50, false);
    CHECK(vertical_xs(lines) == std::vector<double>{50, 100, 150, 200, 250});
}

TEST_CASE("rectangular bed grid on negative coordinates", "[bed_grid]") {
    const auto lines = bed_grid_lines(-120, 120, -60, 60, 50, false);
    CHECK(vertical_xs(lines) == std::vector<double>{-100, -50, 0, 50, 100});
    CHECK(horizontal_ys(lines) == std::vector<double>{-50, 0, 50});
}

TEST_CASE("round bed grid: chords of the inscribed circle", "[bed_grid]") {
    // A centred delta: lines at -100..100, the +-125 perimeter excluded.
    const auto lines = bed_grid_lines(-125, 125, -125, 125, 50, true);
    CHECK(vertical_xs(lines) == std::vector<double>{-100, -50, 0, 50, 100});
    CHECK(horizontal_ys(lines) == std::vector<double>{-100, -50, 0, 50, 100});

    for (const auto& s : lines) {
        INFO("segment " << s.x1 << "," << s.y1 << " -> " << s.x2 << "," << s.y2);
        // Both ends sit on the circle.
        CHECK(std::hypot(s.x1, s.y1) == Approx(125.0));
        CHECK(std::hypot(s.x2, s.y2) == Approx(125.0));
    }
    // The diameter spans the whole bed; the chord at 100 is 2*sqrt(125^2-100^2).
    for (const auto& s : lines) {
        if (s.x1 == 0.0 && s.x2 == 0.0) {
            CHECK(s.y2 - s.y1 == Approx(250.0));
        }
        if (s.x1 == 100.0 && s.x2 == 100.0) {
            CHECK(s.y2 - s.y1 == Approx(150.0));
        }
    }
}

TEST_CASE("round bed grid on an offset box uses the box centre", "[bed_grid]") {
    // A 0-based 250 box: centre (125,125), radius 125.
    const auto lines = bed_grid_lines(0, 250, 0, 250, 50, true);
    CHECK(vertical_xs(lines) == std::vector<double>{50, 100, 150, 200});
    for (const auto& s : lines) {
        CHECK(std::hypot(s.x1 - 125, s.y1 - 125) == Approx(125.0));
        CHECK(std::hypot(s.x2 - 125, s.y2 - 125) == Approx(125.0));
    }
}

TEST_CASE("round bed grid inscribes the circle in a non-square box", "[bed_grid]") {
    // Radius is the smaller half-size (100): x = +-150 lies outside the circle.
    const auto lines = bed_grid_lines(-200, 200, -100, 100, 50, true);
    CHECK(vertical_xs(lines) == std::vector<double>{-50, 0, 50});
    CHECK(horizontal_ys(lines) == std::vector<double>{-50, 0, 50});
    for (const auto& s : lines) {
        CHECK(std::hypot(s.x1, s.y1) == Approx(100.0));
    }
}

TEST_CASE("bed grid is empty for a degenerate box or spacing", "[bed_grid]") {
    CHECK(bed_grid_lines(0, 0, 0, 200, 50, false).empty());
    CHECK(bed_grid_lines(100, 0, 0, 200, 50, true).empty());
    CHECK(bed_grid_lines(0, 200, 0, 200, 0, false).empty());
    CHECK(bed_grid_lines(0, 200, 0, 200, -50, true).empty());
    // A bed smaller than one spacing has no interior line.
    CHECK(bed_grid_lines(10, 40, 10, 40, 50, false).empty());
}
