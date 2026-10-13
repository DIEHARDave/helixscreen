// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_z_offset_layout.cpp
 * @brief decide_z_offset_layout() at each rung boundary, and the value text it feeds.
 *
 * The needs below are the boxes ZOffsetWidget measured at the medium tier, in the faces
 * the labels draw in, and the budgets are the content boxes of the tile sizes in the
 * design's pixel-budget table (the widget's chrome at medium is 9px on each axis).
 *
 * Run with: ./build/bin/helix-tests "[z_offset][layout]"
 */

#include "src/ui/panel_widgets/z_offset_layout.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

constexpr RungNeeds kNeeds{rung_box(30, 37), rung_box(58, 19),  rung_box(37, 66),
                           rung_box(89, 51), rung_box(199, 51), rung_box(166, 142)};
/// A tool changer's Full box carries the tool's name on a line of its own.
constexpr RungNeeds kToolNeeds{rung_box(30, 37), rung_box(58, 19),  rung_box(37, 66),
                               rung_box(89, 51), rung_box(199, 51), rung_box(166, 167)};

/// The widget's chrome at medium: 6px across, 9px down.
constexpr int kChromeW = 6;
constexpr int kChromeH = 9;

ZOffsetVerdict verdict_for(int w, int h, bool tool_changer = false) {
    return decide_z_offset_layout({w, h, tool_changer, tool_changer ? kToolNeeds : kNeeds});
}

ZOffsetVerdict verdict_for_tile(int tile_w, int tile_h) {
    return verdict_for(tile_w - kChromeW, tile_h - kChromeH);
}

} // namespace

TEST_CASE("z_offset layout: rung boundaries are exact", "[z_offset][layout]") {
    CHECK(verdict_for(30, 37).rung == SizeRung::Glance);
    CHECK(verdict_for(30, 37).fits);
    CHECK_FALSE(verdict_for(29, 37).fits);
    CHECK_FALSE(verdict_for(30, 36).fits);
    CHECK(verdict_for(58, 19).rung == SizeRung::Strip);
    CHECK(verdict_for(57, 37).rung == SizeRung::Glance);
    CHECK(verdict_for(58, 18).rung == SizeRung::Glance);
    CHECK_FALSE(verdict_for(58, 18).fits);
    CHECK(verdict_for(37, 66).rung == SizeRung::Column);
    CHECK(verdict_for(37, 65).rung == SizeRung::Glance);
    CHECK(verdict_for(36, 66).rung == SizeRung::Glance);
    CHECK(verdict_for(89, 51).rung == SizeRung::Cell);
    CHECK(verdict_for(88, 51).rung == SizeRung::Strip);
    CHECK(verdict_for(89, 50).rung == SizeRung::Strip);
    CHECK(verdict_for(199, 51).rung == SizeRung::Wide);
    CHECK(verdict_for(198, 51).rung == SizeRung::Cell);
    CHECK(verdict_for(166, 142).rung == SizeRung::Full);
    CHECK(verdict_for(165, 142).rung == SizeRung::Cell);
    CHECK(verdict_for(166, 141).rung == SizeRung::Cell);
    CHECK(verdict_for(199, 141).rung == SizeRung::Wide);
}

TEST_CASE("z_offset layout: the tool label costs Full height", "[z_offset][layout]") {
    CHECK(verdict_for(300, 142, false).rung == SizeRung::Full);
    CHECK(verdict_for(300, 166, true).rung == SizeRung::Wide);
    CHECK(verdict_for(300, 167, true).rung == SizeRung::Full);
}

TEST_CASE("z_offset layout: the design's pixel budgets", "[z_offset][layout]") {
    struct Row {
        const char* name;
        int w, h;
        SizeRung rung;
    };
    const Row rows[] = {
        {"medium 0.5x0.5", 55, 54, SizeRung::Glance}, {"medium 1x0.5", 114, 54, SizeRung::Strip},
        {"medium 0.5x1", 55, 113, SizeRung::Column},  {"medium 1x1", 114, 113, SizeRung::Cell},
        {"large 0.5x0.5", 51, 53, SizeRung::Glance},  {"large 0.5x1", 51, 112, SizeRung::Column},
        {"large 1x0.5", 108, 53, SizeRung::Strip},
    };
    for (const auto& r : rows) {
        INFO(r.name);
        const auto v = verdict_for_tile(r.w, r.h);
        CHECK(v.fits);
        CHECK(v.rung == r.rung);
    }
}

TEST_CASE("z_offset layout: flags follow the rung", "[z_offset][layout]") {
    struct Row {
        int w, h;
        bool tool;
        SizeRung rung;
        int decimals;
        bool save_row, steppers, tool_label;
    };
    const Row rows[] = {
        {30, 37, false, SizeRung::Glance, 2, false, false, false},
        {58, 19, false, SizeRung::Strip, 3, false, false, false},
        {37, 66, false, SizeRung::Column, 3, false, false, false},
        {89, 51, false, SizeRung::Cell, 3, false, false, false},
        {199, 51, false, SizeRung::Wide, 3, true, false, false},
        {166, 142, false, SizeRung::Full, 3, true, true, false},
        {400, 400, false, SizeRung::Full, 3, true, true, false},
        // a tool changer names its tool from Full only
        {199, 51, true, SizeRung::Wide, 3, true, false, false},
        {166, 167, true, SizeRung::Full, 3, true, true, true},
    };
    for (const auto& r : rows) {
        INFO(r.w << "x" << r.h << " tool=" << r.tool);
        const auto v = verdict_for(r.w, r.h, r.tool);
        CHECK(v.rung == r.rung);
        CHECK(v.decimals == r.decimals);
        CHECK(v.show_save_row == r.save_row);
        CHECK(v.show_steppers == r.steppers);
        CHECK(v.show_tool_label == r.tool_label);
    }
}

TEST_CASE("z_offset layout: fits is false below Glance and monotonic above", "[z_offset][layout]") {
    CHECK_FALSE(verdict_for(29, 37).fits);
    CHECK_FALSE(verdict_for(30, 36).fits);
    CHECK(verdict_for(30, 37).fits);

    for (const bool tool : {false, true}) {
        bool fitted_w = false;
        for (int w = 0; w <= 300; ++w) {
            const bool now = verdict_for(w, 200, tool).fits;
            if (fitted_w)
                CHECK(now);
            fitted_w = fitted_w || now;
        }
        bool fitted_h = false;
        for (int h = 0; h <= 300; ++h) {
            const bool now = verdict_for(300, h, tool).fits;
            if (fitted_h)
                CHECK(now);
            fitted_h = fitted_h || now;
        }
    }
}

TEST_CASE("z_offset value text: decimals, sign and the unknown value", "[z_offset][layout]") {
    struct Row {
        int microns;
        int decimals;
        const char* text;
    };
    const Row rows[] = {
        {0, 2, "0.00"},     {0, 3, "0.000"},      {50, 3, "0.050"}, {-125, 3, "-0.125"},
        {-126, 2, "-0.13"}, {-4, 2, "0.00"},      {4, 2, "0.00"},   {-6, 2, "-0.01"},
        {1250, 3, "1.250"}, {-2000, 3, "-2.000"},
    };
    for (const auto& r : rows) {
        INFO(r.microns << "um at " << r.decimals);
        CHECK(format_z_offset_value(r.microns, r.decimals) == r.text);
    }
    CHECK(format_z_offset_value(std::nullopt, 3) == "--");
}
