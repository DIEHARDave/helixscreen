// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_job_queue_tile_layout.cpp
 * @brief decide_job_queue_tile_layout() at each rung boundary, per tile state.
 *
 * Run with: ./build/bin/helix-tests "[job_queue_tile][layout]"
 */

#include "src/ui/panel_widgets/job_queue_tile_layout.h"

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

JobQueueTileVerdict verdict_for(int w, int h,
                                JobQueueTileState state = JobQueueTileState::WaitingForStart,
                                int jobs = 4) {
    JobQueueTileLayoutInput in;
    in.content_w = w;
    in.content_h = h;
    in.state = state;
    in.job_count = jobs;
    in.needs = synthetic_needs();
    return decide_job_queue_tile_layout(in);
}

} // namespace

TEST_CASE("job queue tile: rung boundaries are exact", "[job_queue_tile][layout]") {
    CHECK_FALSE(verdict_for(99, 40).fits);
    CHECK(verdict_for(100, 40).rung == SizeRung::Strip);
    CHECK(verdict_for(100, 99).rung == SizeRung::Strip);
    CHECK(verdict_for(100, 100).rung == SizeRung::Cell);
    CHECK(verdict_for(219, 100).rung == SizeRung::Cell);
    CHECK(verdict_for(220, 100).rung == SizeRung::Wide);
    CHECK(verdict_for(220, 219).rung == SizeRung::Wide);
    CHECK(verdict_for(220, 220).rung == SizeRung::Full);
}

TEST_CASE("job queue tile: a waiting job's flags per rung", "[job_queue_tile][layout]") {
    const auto strip = verdict_for(100, 40);
    CHECK(strip.button_icon_only);
    CHECK_FALSE(strip.show_thumbnail);
    CHECK_FALSE(strip.show_detail);
    CHECK_FALSE(strip.show_more);
    CHECK(strip.list_rows == 0);

    const auto cell = verdict_for(100, 100);
    CHECK_FALSE(cell.button_icon_only);
    CHECK(cell.show_thumbnail);
    CHECK(cell.show_more);
    CHECK_FALSE(cell.show_detail);
    CHECK(cell.list_rows == 0);

    const auto wide = verdict_for(220, 100);
    CHECK(wide.show_thumbnail);
    CHECK(wide.show_detail);
    CHECK(wide.show_more);
    CHECK(wide.list_rows == 0);

    const auto full = verdict_for(220, 220);
    CHECK(full.show_thumbnail);
    CHECK(full.show_detail);
    CHECK(full.show_more);
    CHECK(full.list_rows == 3);
}

TEST_CASE("job queue tile: the following-jobs list follows the queue", "[job_queue_tile][layout]") {
    CHECK(verdict_for(220, 220, JobQueueTileState::WaitingForStart, 2).list_rows == 1);
    CHECK(verdict_for(220, 220, JobQueueTileState::WaitingForStart, 3).list_rows == 2);
    CHECK(verdict_for(220, 220, JobQueueTileState::WaitingForStart, 9).list_rows ==
          kJobQueueTileListRows);
    CHECK(verdict_for(220, 220, JobQueueTileState::WaitingForStart, 1).list_rows == 0);
    CHECK(verdict_for(220, 100, JobQueueTileState::WaitingForStart, 9).list_rows == 0);
    CHECK(verdict_for(220, 220, JobQueueTileState::Empty, 0).list_rows == 0);
    CHECK(verdict_for(220, 220, JobQueueTileState::NotLoaded, 5).list_rows == 0);
    CHECK(verdict_for(220, 220, JobQueueTileState::Unavailable, 5).list_rows == 0);
}

TEST_CASE("job queue tile: thumbnail only while a job waits or loads", "[job_queue_tile][layout]") {
    const auto printing = verdict_for(100, 100, JobQueueTileState::Printing);
    CHECK_FALSE(printing.show_thumbnail);
    CHECK(printing.show_detail);
    CHECK(verdict_for(100, 100, JobQueueTileState::Starting).show_thumbnail);
    CHECK_FALSE(verdict_for(100, 100, JobQueueTileState::AutoNext).show_thumbnail);
}

TEST_CASE("job queue tile: fits is false below the strip and monotonic",
          "[job_queue_tile][layout]") {
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
