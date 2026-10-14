// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_stats_tile_layout.cpp
 * @brief print_stats_content() and decide_print_stats_tile_layout() at each rung boundary.
 *
 * Run with: ./build/bin/helix-tests "[print_stats_tile][layout]"
 */

#include "src/ui/panel_widgets/print_stats_tile_layout.h"

#include <utility>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

RungNeeds synthetic_needs() {
    RungNeeds n;
    n.strip = rung_box(100, 40);
    n.cell = rung_box(100, 100);
    n.wide = rung_box(220, 100);
    n.full = rung_box(220, 220);
    return n;
}

PrintStatsTileLayoutInput input_for(int w, int h,
                                    PrintStatsContent content = PrintStatsContent::Activity,
                                    bool lifetime_known = true) {
    PrintStatsTileLayoutInput in;
    in.content_w = w;
    in.content_h = h;
    in.content = content;
    in.lifetime_known = lifetime_known;
    in.needs = synthetic_needs();
    in.count_line_w = 60;
    in.full_line_w = 200;
    in.support_line_h = 20;
    return in;
}

PrintStatsTileVerdict verdict_for(int w, int h,
                                  PrintStatsContent content = PrintStatsContent::Activity,
                                  bool lifetime_known = true) {
    return decide_print_stats_tile_layout(input_for(w, h, content, lifetime_known));
}

} // namespace

TEST_CASE("print stats tile: content follows what is known", "[print_stats_tile][layout]") {
    CHECK(print_stats_content(false, 5, 10) == PrintStatsContent::Loading);
    CHECK(print_stats_content(true, 0, -1) == PrintStatsContent::NoHistory);
    CHECK(print_stats_content(true, 0, 0) == PrintStatsContent::NoHistory);
    CHECK(print_stats_content(true, 0, 12) == PrintStatsContent::Activity);
    CHECK(print_stats_content(true, 3, -1) == PrintStatsContent::Activity);
}

TEST_CASE("print stats tile: rung boundaries are exact", "[print_stats_tile][layout]") {
    CHECK_FALSE(verdict_for(99, 40).fits);
    CHECK(verdict_for(100, 40).rung == SizeRung::Strip);
    CHECK(verdict_for(100, 99).rung == SizeRung::Strip);
    CHECK(verdict_for(100, 100).rung == SizeRung::Cell);
    CHECK(verdict_for(219, 100).rung == SizeRung::Cell);
    CHECK(verdict_for(220, 100).rung == SizeRung::Wide);
    CHECK(verdict_for(220, 219).rung == SizeRung::Wide);
    CHECK(verdict_for(220, 220).rung == SizeRung::Full);
}

TEST_CASE("print stats tile: parts per rung with the lifetime totals known",
          "[print_stats_tile][layout]") {
    const auto strip = verdict_for(100, 40);
    CHECK_FALSE(strip.show_bars);
    CHECK_FALSE(strip.show_day_labels);
    CHECK_FALSE(strip.side_by_side);
    CHECK(strip.support == PrintStatsSupport::None);

    const auto cell = verdict_for(100, 120);
    CHECK_FALSE(cell.show_bars);
    CHECK_FALSE(cell.show_day_labels);
    CHECK_FALSE(cell.side_by_side);
    CHECK(cell.support == PrintStatsSupport::PrintsOnly);

    const auto wide = verdict_for(220, 100);
    CHECK(wide.show_bars);
    CHECK_FALSE(wide.show_day_labels);
    CHECK(wide.side_by_side);
    CHECK(wide.support == PrintStatsSupport::Full);

    const auto full = verdict_for(220, 220);
    CHECK(full.show_bars);
    CHECK(full.show_day_labels);
    CHECK_FALSE(full.side_by_side);
    CHECK(full.support == PrintStatsSupport::Full);
}

TEST_CASE("print stats tile: the support line steps down rather than cut",
          "[print_stats_tile][layout]") {
    // Cell: the count needs its own line's height under the stacked hero.
    CHECK(verdict_for(100, 119).support == PrintStatsSupport::None);
    CHECK(verdict_for(100, 120).support == PrintStatsSupport::PrintsOnly);

    auto in = input_for(220, 100);
    in.full_line_w = 220;
    CHECK(decide_print_stats_tile_layout(in).support == PrintStatsSupport::Full);
    in.full_line_w = 221;
    CHECK(decide_print_stats_tile_layout(in).support == PrintStatsSupport::PrintsOnly);
    in.count_line_w = 221;
    CHECK(decide_print_stats_tile_layout(in).support == PrintStatsSupport::None);

    auto cell = input_for(100, 200);
    cell.count_line_w = 101;
    CHECK(decide_print_stats_tile_layout(cell).support == PrintStatsSupport::None);
}

TEST_CASE("print stats tile: no support line until the lifetime totals are known",
          "[print_stats_tile][layout]") {
    for (const auto& box : {std::pair{100, 40}, {100, 120}, {220, 100}, {220, 220}}) {
        INFO(box.first << "x" << box.second);
        CHECK(verdict_for(box.first, box.second, PrintStatsContent::Activity, false).support ==
              PrintStatsSupport::None);
    }
}

TEST_CASE("print stats tile: loading and empty history draw no bars or support",
          "[print_stats_tile][layout]") {
    for (const auto content : {PrintStatsContent::Loading, PrintStatsContent::NoHistory}) {
        const auto v = verdict_for(220, 220, content);
        CHECK_FALSE(v.show_bars);
        CHECK_FALSE(v.show_day_labels);
        CHECK(v.support == PrintStatsSupport::None);
        CHECK(v.rung == SizeRung::Full);
    }
}

TEST_CASE("print stats tile: fits is false below the strip and monotonic",
          "[print_stats_tile][layout]") {
    CHECK_FALSE(verdict_for(60, 30).fits);
    for (int w = 90; w <= 240; w += 10) {
        for (int h = 30; h <= 240; h += 10) {
            INFO(w << "x" << h);
            if (verdict_for(w, h).fits) {
                CHECK(verdict_for(w + 10, h).fits);
                CHECK(verdict_for(w, h + 10).fits);
            }
        }
    }
}
