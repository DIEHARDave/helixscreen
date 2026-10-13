// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Pure layout decision for the Dryer home widget.
//
// Free of LVGL so it is unit-testable without a display. DryerWidget measures the
// minimum box each rung draws in for the state being decided and feeds it here; this
// header picks the rung and the flags the XML binds.

#include "src/ui/panel_widgets/size_rung.h"

#include <algorithm>

namespace helix {

struct DryerInput {
    int content_w = 0; ///< the box the content draws in, chrome already subtracted
    int content_h = 0;
    bool drying = false;         ///< any source is drying
    bool multi = false;          ///< two or more sources are drying
    bool progress_known = false; ///< the source a single-number rung shows has a progress
    int source_count = 0;        ///< sources a Full tile lists, idle ones included
    /// Minimum box per rung for THIS state. Idle, one source drying and several drying
    /// draw different content, so the caller measures whichever it is deciding for.
    RungNeeds needs;
    int row_h = 0;    ///< one Full row
    int row_gap = 0;  ///< between Full rows
    int max_rows = 0; ///< rows the XML can draw
};

/// The published verdict. `rung` is SizeRung's int; the flags are 0/1 subjects.
struct DryerVerdict {
    SizeRung rung = SizeRung::Glance;
    bool fits = false;
    /// A progress ring around the time left. Never a ring at 0 for an unknown progress.
    bool show_ring = false;
    int row_count = 0; ///< Full: source rows that fit
};

[[nodiscard]] constexpr DryerVerdict decide_dryer_layout(const DryerInput& in) {
    DryerVerdict v;
    v.rung = choose_size_rung(in.content_w, in.content_h, in.needs);
    v.fits = holds_any_rung(in.content_w, in.content_h, in.needs);
    const bool ring_rung = v.rung == SizeRung::Glance || v.rung == SizeRung::Cell ||
                           (v.rung == SizeRung::Wide && !in.multi);
    v.show_ring = in.drying && in.progress_known && ring_rung;
    if (v.rung == SizeRung::Full) {
        const int per_row = std::max(in.row_h + in.row_gap, 1);
        const int fit = (in.content_h + in.row_gap) / per_row;
        v.row_count = std::clamp(fit, 1, std::max(1, std::min(in.max_rows, in.source_count)));
    }
    return v;
}

} // namespace helix
