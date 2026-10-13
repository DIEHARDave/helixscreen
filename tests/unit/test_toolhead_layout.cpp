// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_toolhead_layout.cpp
 * @brief decide_toolhead_layout() at each rung boundary and for each state, and the
 * axis text and position source it feeds.
 *
 * Run with: ./build/bin/helix-tests "[toolhead][layout]"
 */

#include "src/ui/panel_widgets/toolhead_layout.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

constexpr RungNeeds kNeeds{rung_box(30, 20), rung_box(60, 20),  rung_box(44, 70),
                           rung_box(80, 70), rung_box(190, 80), rung_box(190, 170),
                           RungBox{}};

ToolheadVerdict layout_for(int w, int h, bool printing = false, bool homed = true) {
    ToolheadInput in;
    in.content_w = w;
    in.content_h = h;
    in.printing = printing;
    in.homed = homed;
    in.needs = kNeeds;
    return decide_toolhead_layout(in);
}

} // namespace

TEST_CASE("toolhead layout: each rung takes the largest box it holds", "[toolhead][layout]") {
    CHECK(layout_for(30, 20).rung == SizeRung::Glance);
    CHECK(layout_for(60, 20).rung == SizeRung::Strip);
    CHECK(layout_for(44, 70).rung == SizeRung::Column);
    CHECK(layout_for(80, 70).rung == SizeRung::Cell);
    CHECK(layout_for(190, 80).rung == SizeRung::Wide);
    CHECK(layout_for(190, 170).rung == SizeRung::Full);
    // One pixel short of a rung keeps the smaller one.
    CHECK(layout_for(189, 170).rung == SizeRung::Cell);
    CHECK(layout_for(190, 169).rung == SizeRung::Wide);
    CHECK(layout_for(79, 70).rung != SizeRung::Cell);
}

TEST_CASE("toolhead layout: a box that holds nothing does not fit", "[toolhead][layout]") {
    CHECK_FALSE(layout_for(29, 20).fits);
    CHECK_FALSE(layout_for(30, 19).fits);
    CHECK(layout_for(30, 20).fits);
}

TEST_CASE("toolhead layout: Glance and Strip show letters idle and the Z height printing",
          "[toolhead][layout]") {
    for (const auto& box : {std::pair{30, 20}, std::pair{60, 20}}) {
        INFO(box.first << "x" << box.second);
        const auto idle = layout_for(box.first, box.second, false);
        CHECK(idle.show_letters);
        CHECK_FALSE(idle.show_z_only);
        const auto printing = layout_for(box.first, box.second, true);
        CHECK_FALSE(printing.show_letters);
        CHECK(printing.show_z_only);
    }
    // Bigger rungs draw the same content in both states.
    for (const auto& box : {std::pair{44, 70}, std::pair{80, 70}, std::pair{190, 80}}) {
        INFO(box.first << "x" << box.second);
        CHECK_FALSE(layout_for(box.first, box.second, true).show_z_only);
        CHECK_FALSE(layout_for(box.first, box.second, false).show_letters);
    }
}

TEST_CASE("toolhead layout: only Column compacts its values", "[toolhead][layout]") {
    CHECK(layout_for(44, 70).compact_values);
    CHECK_FALSE(layout_for(80, 70).compact_values);
    CHECK_FALSE(layout_for(30, 20).compact_values);
    CHECK_FALSE(layout_for(190, 170).compact_values);
}

TEST_CASE("toolhead layout: rows at Column and Cell, the plate from Wide", "[toolhead][layout]") {
    CHECK(layout_for(44, 70).show_rows);
    CHECK(layout_for(80, 70).show_rows);
    CHECK_FALSE(layout_for(190, 80).show_rows);
    CHECK_FALSE(layout_for(60, 20).show_rows);
    CHECK_FALSE(layout_for(80, 70).show_plate);
    CHECK(layout_for(190, 80).show_plate);
    CHECK(layout_for(190, 170).show_plate);
}

TEST_CASE("toolhead layout: Home All shows at Full, idle, with an axis unhomed",
          "[toolhead][layout]") {
    CHECK(layout_for(190, 170, false, false).show_home_button);
    CHECK_FALSE(layout_for(190, 170, false, true).show_home_button);
    CHECK_FALSE(layout_for(190, 170, true, false).show_home_button);
    CHECK_FALSE(layout_for(190, 170, true, true).show_home_button);
    // Below Full there is no room for the button, unhomed or not.
    CHECK_FALSE(layout_for(190, 80, false, false).show_home_button);
    CHECK_FALSE(layout_for(80, 70, false, false).show_home_button);
}

TEST_CASE("toolhead layout: the tool and speed row is Full while printing", "[toolhead][layout]") {
    CHECK(layout_for(190, 170, true).show_status_row);
    CHECK_FALSE(layout_for(190, 170, false).show_status_row);
    CHECK_FALSE(layout_for(190, 80, true).show_status_row);
}

TEST_CASE("toolhead layout: axis text is a dash until the axis is homed", "[toolhead][layout]") {
    CHECK(format_toolhead_axis(Axis::X, 12750, false) == "--");
    CHECK(format_toolhead_axis(Axis::Z, 0, false) == "--");
    CHECK(format_toolhead_axis(Axis::X, 12750, true) == "127.5");
    CHECK(format_toolhead_axis(Axis::Y, -1000, true) == "-10.0");
    CHECK(format_toolhead_axis(Axis::Z, 2000, true) == "20.00");
    CHECK(format_toolhead_axis(Axis::Z, 25, true) == "0.25");
    // The half-cell-wide column drops decimals to fit.
    CHECK(format_toolhead_axis(Axis::X, 12750, true, true) == "128");
    CHECK(format_toolhead_axis(Axis::Y, -1000, true, true) == "-10");
    CHECK(format_toolhead_axis(Axis::Z, 2025, true, true) == "20.2");
    CHECK(format_toolhead_axis(Axis::X, 12750, false, true) == "--");
}

TEST_CASE("toolhead layout: the live position leads only while the toolhead moves",
          "[toolhead][layout]") {
    CHECK(toolhead_position_centimm(true, 111, 222) == 111);
    CHECK(toolhead_position_centimm(false, 111, 222) == 222);
}
