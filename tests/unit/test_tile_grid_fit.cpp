// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// fit_tile_grid: the grid shape for N equal tiles is the one whose tile clears
// the icon-plus-label minimum by the most, so the same tile count lays out
// differently in a wide, a tall and a large box.

#include "tile_grid_fit.h"

#include "../catch_amalgamated.hpp"

using helix::ui::fit_tile_grid;
using helix::ui::TileGridFit;

namespace {

constexpr int kMinW = 76;
constexpr int kMinH = 56;
constexpr int kGap = 4;

TileGridFit fit(int count, int w, int h, int gap = kGap) {
    return fit_tile_grid(count, w, h, gap, kMinW, kMinH);
}

} // namespace

TEST_CASE("fit_tile_grid in a landscape card keeps every tile count to two rows",
          "[tile-grid-fit][calib-grid]") {
    // The 800x480 Controls card's grid.
    const int w = 333, h = 153;
    struct Case {
        int count, cols, rows;
    };
    for (const Case c : {Case{1, 1, 1}, Case{2, 2, 1}, Case{3, 3, 1}, Case{4, 2, 2}, Case{5, 3, 2},
                         Case{6, 3, 2}, Case{7, 4, 2}}) {
        const TileGridFit f = fit(c.count, w, h);
        CAPTURE(c.count);
        CHECK(f.cols == c.cols);
        CHECK(f.rows == c.rows);
        CHECK(f.fits);
    }
}

TEST_CASE("fit_tile_grid stacks rows in a tall card", "[tile-grid-fit][calib-grid]") {
    const TileGridFit six = fit(6, 200, 300);
    CHECK(six.cols == 2);
    CHECK(six.rows == 3);
    CHECK(six.fits);

    const TileGridFit seven = fit(7, 200, 400);
    CHECK(seven.cols == 2);
    CHECK(seven.rows == 4);
}

TEST_CASE("fit_tile_grid widens in a large wide card", "[tile-grid-fit][calib-grid]") {
    const TileGridFit four = fit(4, 700, 200);
    CHECK(four.cols == 4);
    CHECK(four.rows == 1);

    // 4x2 and 3x2 give the same height-bound tile; the full grid wins.
    const TileGridFit six = fit(6, 700, 200);
    CHECK(six.cols == 3);
    CHECK(six.rows == 2);
}

TEST_CASE("fit_tile_grid breaks a tie in score and waste toward more columns",
          "[tile-grid-fit][calib-grid]") {
    // 1x4 and 2x2 both leave a 1.29x tile with no empty cell.
    const TileGridFit f = fit(4, 200, 300);
    CHECK(f.cols == 2);
    CHECK(f.rows == 2);
}

TEST_CASE("fit_tile_grid reports when no shape leaves room for labels",
          "[tile-grid-fit][calib-grid]") {
    const TileGridFit f = fit(6, 100, 60);
    CHECK_FALSE(f.fits);
    CHECK(f.cols == 3);
    CHECK(f.rows == 2);
}

TEST_CASE("fit_tile_grid takes the gaps out of the tile", "[tile-grid-fit][calib-grid]") {
    CHECK(fit(3, 240, 60, 0).fits);
    const TileGridFit gapped = fit(3, 240, 60, 12);
    CHECK_FALSE(gapped.fits);
    CHECK(gapped.cols == 3);
    CHECK(gapped.rows == 1);
}

TEST_CASE("fit_tile_grid degenerate inputs", "[tile-grid-fit][calib-grid]") {
    const TileGridFit none = fit(0, 333, 153);
    CHECK(none.cols == 1);
    CHECK(none.rows == 1);
    CHECK_FALSE(none.fits);

    const TileGridFit unmeasured = fit(5, 0, 0);
    CHECK_FALSE(unmeasured.fits);

    // More tiles than 4x4 holds: the widest grid, flagged as not fitting.
    const TileGridFit crowded = fit(17, 2000, 2000);
    CHECK(crowded.cols == 4);
    CHECK(crowded.rows == 5);
    CHECK_FALSE(crowded.fits);
}
