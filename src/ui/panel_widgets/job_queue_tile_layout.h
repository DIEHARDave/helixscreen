// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Pure layout decision for the job queue home tile.
//
// Free of LVGL so it is unit-testable without a display. JobQueueWidget measures the
// minimum box each rung draws in and feeds it here; this header picks the rung and the
// flags the XML binds. The tile offers Strip, Cell, Wide and Full.

#include "job_queue_tile_state.h"
#include "src/ui/panel_widgets/size_rung.h"

#include <algorithm>

namespace helix {

/// Jobs after the first that the Full rung lists.
inline constexpr int kJobQueueTileListRows = 3;

struct JobQueueTileLayoutInput {
    int content_w = 0; ///< the box the content draws in, chrome already subtracted
    int content_h = 0;
    JobQueueTileState state = JobQueueTileState::NotLoaded;
    int job_count = 0;
    RungNeeds needs; ///< offers strip, cell, wide and full only
};

/// The published verdict. `rung` is SizeRung's int; the flags are 0/1 subjects.
struct JobQueueTileVerdict {
    SizeRung rung = SizeRung::Strip;
    bool fits = false;
    bool show_thumbnail = false; ///< the next job's picture, while it waits or loads
    bool show_detail = false;
    bool show_more = false; ///< "+N more"
    int list_rows = 0;      ///< following jobs listed under the next one
    bool button_icon_only = false;
};

[[nodiscard]] constexpr JobQueueTileVerdict
decide_job_queue_tile_layout(const JobQueueTileLayoutInput& in) {
    JobQueueTileVerdict v;
    v.rung = choose_size_rung(in.content_w, in.content_h, in.needs);
    v.fits = holds_any_rung(in.content_w, in.content_h, in.needs);
    const int r = static_cast<int>(v.rung);
    const bool has_room = r >= static_cast<int>(SizeRung::Cell);
    const bool waiting = in.state == JobQueueTileState::WaitingForStart;
    v.show_thumbnail = has_room && (waiting || in.state == JobQueueTileState::Starting);
    // A waiting job's detail is its estimate, which earns the thumbnail's width.
    v.show_detail = r >= static_cast<int>(waiting ? SizeRung::Wide : SizeRung::Cell);
    v.show_more = has_room && in.job_count >= 2;
    const bool has_queue = in.state != JobQueueTileState::Empty &&
                           in.state != JobQueueTileState::NotLoaded &&
                           in.state != JobQueueTileState::Unavailable;
    if (v.rung == SizeRung::Full && has_queue) {
        v.list_rows = std::min(kJobQueueTileListRows, std::max(0, in.job_count - 1));
    }
    v.button_icon_only = v.rung == SizeRung::Strip;
    return v;
}

} // namespace helix
