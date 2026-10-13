// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_tool_switcher_layout.cpp
 * @brief Unit tests for decide_tool_switcher_layout() — the pure layout
 *        decision behind the home tool switcher.
 *
 * The content boxes are the ones the tile really measures at 800x480 and
 * 480x272 for each grid span, so the mode boundaries below are the live ones.
 * Mode selection depends only on the box, the touch floor and the gap; the
 * font sizes decide the detail block alone, so every table row is checked at
 * both breakpoints' fonts.
 *
 * Run with: ./build/bin/helix-tests "[tool_switcher]"
 */

#include "src/ui/panel_widgets/tool_switcher_layout.h"

#include <string>

#include "../catch_amalgamated.hpp"

using helix::decide_tool_switcher_layout;
using helix::ToolSwitcherLayout;
using helix::ToolSwitcherMode;

namespace {

constexpr int kMin = 48;
constexpr int kGap = 2;

struct Fonts {
    int body;
    int small;
};
constexpr Fonts kFonts800{18, 16};
constexpr Fonts kFonts480{12, 10};

struct Box {
    const char* name;
    int w;
    int h;
};

constexpr Box k800_1x1{"800x480 1x1", 95, 93};
constexpr Box k800_2x1{"800x480 2x1", 214, 93};
constexpr Box k800_1x2{"800x480 1x2", 95, 211};
constexpr Box k800_2x2{"800x480 2x2", 214, 211};
constexpr Box k480_1x1{"480x272 1x1", 58, 52};
constexpr Box k480_2x1{"480x272 2x1", 130, 52};
constexpr Box k480_1x2{"480x272 1x2", 58, 118};
constexpr Box k480_2x2{"480x272 2x2", 130, 118};

ToolSwitcherLayout layout_for(int heads, Box box, Fonts f = kFonts800) {
    return decide_tool_switcher_layout(heads, box.w, box.h, kMin, kGap, f.body, f.small);
}

const char* mode_name(ToolSwitcherMode m) {
    switch (m) {
    case ToolSwitcherMode::RailWithPark:
        return "RailWithPark";
    case ToolSwitcherMode::Rail:
        return "Rail";
    case ToolSwitcherMode::Pair:
        return "Pair";
    case ToolSwitcherMode::Single:
        return "Single";
    }
    return "?";
}

} // namespace

TEST_CASE("Tool switcher layout: mode per measured box and head count", "[tool_switcher][1784]") {
    using M = ToolSwitcherMode;
    constexpr M RP = M::RailWithPark, R = M::Rail, P = M::Pair, S = M::Single;
    const Box boxes[8] = {k800_1x1, k800_2x1, k800_1x2, k800_2x2,
                          k480_1x1, k480_2x1, k480_1x2, k480_2x2};
    struct Row {
        int heads;
        M modes[8];
    };
    const Row table[] = {
        {2, {S, RP, RP, RP, S, R, R, RP}},
        {4, {S, R, R, RP, S, P, P, R}},
        {5, {S, P, P, RP, S, P, P, P}},
        {8, {S, P, P, RP, S, P, P, P}},
    };

    for (const Fonts f : {kFonts800, kFonts480}) {
        for (const Row& row : table) {
            for (int i = 0; i < 8; i++) {
                const ToolSwitcherLayout l = layout_for(row.heads, boxes[i], f);
                INFO(row.heads << " heads, " << boxes[i].name << ", body " << f.body << ": got "
                               << mode_name(l.mode) << ", want " << mode_name(row.modes[i]));
                CHECK(l.mode == row.modes[i]);
            }
        }
    }
}

TEST_CASE("Tool switcher layout: 2x1 at 800x480 holds four 48 px segments but not five",
          "[tool_switcher][1784]") {
    // Four segments need 4*48 + 3*2 = 198 of the 214 px; five (or four plus
    // Park) need 248.
    REQUIRE(4 * kMin + 3 * kGap <= k800_2x1.w);
    REQUIRE(5 * kMin + 4 * kGap > k800_2x1.w);

    const ToolSwitcherLayout four = layout_for(4, k800_2x1);
    CHECK(four.mode == ToolSwitcherMode::Rail);
    CHECK(four.cols == 4);
    CHECK(four.rows == 1);
    CHECK(four.cell_w == (214 - 3 * kGap) / 4);
    CHECK(four.cell_w >= kMin);

    const ToolSwitcherLayout five = layout_for(5, k800_2x1);
    CHECK(five.mode == ToolSwitcherMode::Pair);
}

TEST_CASE("Tool switcher layout: rail grid wraps to the first row count that fits",
          "[tool_switcher][1784]") {
    // 5 heads + Park on 214x211: one row of six is 34 px wide, two rows of
    // three are 70x104.
    const ToolSwitcherLayout l = layout_for(5, k800_2x2);
    REQUIRE(l.mode == ToolSwitcherMode::RailWithPark);
    CHECK(l.cols == 3);
    CHECK(l.rows == 2);
    CHECK(l.cell_w == (214 - 2 * kGap) / 3);
}

TEST_CASE("Tool switcher layout: one head and sixteen heads on a 2x2 box",
          "[tool_switcher][1784]") {
    const ToolSwitcherLayout one = layout_for(1, k800_2x2);
    CHECK(one.mode == ToolSwitcherMode::RailWithPark);
    CHECK(one.cols == 2);
    CHECK(one.rows == 1);

    // Seventeen cells do not fit; sixteen do as a 4x4 grid.
    const ToolSwitcherLayout sixteen = layout_for(16, k800_2x2);
    CHECK(sixteen.mode == ToolSwitcherMode::Rail);
    CHECK(sixteen.cols == 4);
    CHECK(sixteen.rows == 4);
    CHECK(sixteen.cell_w >= kMin);
    CHECK(sixteen.cell_h >= kMin);
}

TEST_CASE("Tool switcher layout: degenerate inputs fall back to Single", "[tool_switcher][1784]") {
    SECTION("a 0x0 box") {
        const ToolSwitcherLayout l = decide_tool_switcher_layout(5, 0, 0, kMin, kGap, 18, 16);
        CHECK(l.mode == ToolSwitcherMode::Single);
        CHECK(l.cell_w == 0);
        CHECK(l.cell_h == 0);
        CHECK_FALSE(l.detail);
    }
    SECTION("no heads at all") {
        const ToolSwitcherLayout l = layout_for(0, k800_2x2);
        CHECK(l.mode == ToolSwitcherMode::Single);
        CHECK(l.cell_w == k800_2x2.w);
        CHECK(l.cell_h == k800_2x2.h);
    }
}

TEST_CASE("Tool switcher layout: Pair lies along the long axis with a 30% Park cell",
          "[tool_switcher][1784]") {
    SECTION("wide box: face and Park side by side") {
        const ToolSwitcherLayout l = layout_for(5, k800_2x1);
        REQUIRE(l.mode == ToolSwitcherMode::Pair);
        CHECK(l.pair_horizontal);
        CHECK(l.cols == 2);
        CHECK(l.rows == 1);
        // Park takes max(48, 30% of 214) = 64; the face the rest.
        CHECK(l.cell_w == 214 - kGap - 64);
        CHECK(l.cell_h == 93);
        CHECK(l.rail_h == 93);
        CHECK_FALSE(l.detail);
    }
    SECTION("tall box: face over Park") {
        const ToolSwitcherLayout l = layout_for(5, k800_1x2);
        REQUIRE(l.mode == ToolSwitcherMode::Pair);
        CHECK_FALSE(l.pair_horizontal);
        CHECK(l.cols == 1);
        CHECK(l.rows == 2);
        CHECK(l.cell_w == 95);
        CHECK(l.cell_h == 211 - kGap - 63);
    }
    SECTION("a short long axis still gives Park the 48 px floor") {
        // 30% of 130 is 39, so Park takes 48 and the face 80.
        const ToolSwitcherLayout l = layout_for(5, k480_2x1, kFonts480);
        REQUIRE(l.mode == ToolSwitcherMode::Pair);
        CHECK(l.cell_w == 130 - kGap - kMin);
    }
}

TEST_CASE("Tool switcher layout: Single fills the whole box", "[tool_switcher][1784]") {
    const ToolSwitcherLayout l = layout_for(5, k800_1x1);
    REQUIRE(l.mode == ToolSwitcherMode::Single);
    CHECK(l.cols == 1);
    CHECK(l.rows == 1);
    CHECK(l.cell_w == 95);
    CHECK(l.cell_h == 93);
    CHECK(l.rail_h == 93);
    CHECK_FALSE(l.detail);
}

TEST_CASE("Tool switcher layout: no mode yields a cell under the touch floor",
          "[tool_switcher][1784]") {
    for (int heads = 1; heads <= 16; heads++) {
        for (int w = 40; w <= 260; w += 6) {
            for (int h = 40; h <= 260; h += 6) {
                const ToolSwitcherLayout l =
                    decide_tool_switcher_layout(heads, w, h, kMin, kGap, 18, 16);
                INFO(heads << " heads on " << w << "x" << h << ": " << mode_name(l.mode) << " "
                           << l.cell_w << "x" << l.cell_h);
                if (l.mode == ToolSwitcherMode::Single && (w < kMin || h < kMin)) {
                    REQUIRE(l.cell_w == w);
                    REQUIRE(l.cell_h == h);
                    continue;
                }
                REQUIRE(l.cell_w >= kMin);
                REQUIRE(l.cell_h >= kMin);
                REQUIRE(l.rail_h <= h);
            }
        }
    }
}

TEST_CASE("Tool switcher layout: a detail block fits under a tall rail", "[tool_switcher][1784]") {
    SECTION("2x2 at 800x480 with five heads") {
        const ToolSwitcherLayout l = layout_for(5, k800_2x2);
        REQUIRE(l.mode == ToolSwitcherMode::RailWithPark);
        CHECK(l.detail);
        CHECK(l.rail_h < k800_2x2.h);
        CHECK(l.rail_h == l.rows * l.cell_h + (l.rows - 1) * kGap);
        CHECK(l.cell_h >= kMin);
        CHECK(k800_2x2.h - l.rail_h >= 2.5 * kFonts800.small);
    }
    SECTION("2x1 at 800x480 with five heads") {
        const ToolSwitcherLayout l = layout_for(5, k800_2x1);
        CHECK_FALSE(l.detail);
        CHECK(l.rail_h == k800_2x1.h);
    }
    SECTION("a rail with no room left under it fills the box") {
        // 4 heads at 480x272 2x2 need two rows of 58 px: nothing is left.
        const ToolSwitcherLayout l = layout_for(4, k480_2x2, kFonts480);
        REQUIRE(l.mode == ToolSwitcherMode::Rail);
        CHECK_FALSE(l.detail);
        CHECK(l.rail_h == k480_2x2.h);
        CHECK(l.cell_h == (118 - kGap) / 2);
    }
    SECTION("a one-row-tall tile never shows detail, however much the rail could shrink") {
        // 2 heads + Park at 214x93: 48 px rows would leave 45 px, enough for
        // 2.5 lines of 16 px text, but 93 px is under two touch rows.
        const ToolSwitcherLayout l = layout_for(2, k800_2x1);
        REQUIRE(l.mode == ToolSwitcherMode::RailWithPark);
        CHECK_FALSE(l.detail);
        CHECK(l.rail_h == k800_2x1.h);
        CHECK(l.cell_h == k800_2x1.h);
    }
    SECTION("the tall-box gate sits at two touch rows") {
        // One head + Park on a 130 px wide box: a single 48 px rail row.
        const ToolSwitcherLayout at = decide_tool_switcher_layout(1, 130, 2 * kMin, kMin, kGap,
                                                                  kFonts480.body, kFonts480.small);
        REQUIRE(at.mode == ToolSwitcherMode::RailWithPark);
        CHECK(at.detail);
        CHECK(at.rail_h == kMin);
        const ToolSwitcherLayout below = decide_tool_switcher_layout(
            1, 130, 2 * kMin - 1, kMin, kGap, kFonts480.body, kFonts480.small);
        REQUIRE(below.mode == ToolSwitcherMode::RailWithPark);
        CHECK_FALSE(below.detail);
        CHECK(below.rail_h == 2 * kMin - 1);
    }
    SECTION("480x272 2x2: detail exactly when the leftover holds 2.5 small lines") {
        // One head + Park on 130x118 is one 48 px row, leaving 70 px.
        const int leftover = k480_2x2.h - kMin;
        REQUIRE(leftover == 70);
        const ToolSwitcherLayout fits =
            decide_tool_switcher_layout(1, k480_2x2.w, k480_2x2.h, kMin, kGap, 12, 28);
        REQUIRE(fits.mode == ToolSwitcherMode::RailWithPark);
        CHECK(fits.detail); // 2.5 * 28 = 70
        CHECK(fits.rail_h == kMin);
        const ToolSwitcherLayout short_by_one =
            decide_tool_switcher_layout(1, k480_2x2.w, k480_2x2.h, kMin, kGap, 12, 29);
        CHECK_FALSE(short_by_one.detail); // 2.5 * 29 = 72.5
        CHECK(short_by_one.rail_h == k480_2x2.h);
        CHECK(layout_for(1, k480_2x2, kFonts480).detail);
    }
}
