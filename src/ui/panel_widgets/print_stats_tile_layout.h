// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Pure content and layout decision for the print stats home tile.
//
// Free of LVGL so it is unit-testable without a display. PrintStatsWidget measures the
// minimum box each rung draws in and feeds it here; this header picks the content, the
// rung and the flags the XML binds. The tile offers Strip, Cell, Wide and Full.

#include "src/ui/panel_widgets/size_rung.h"

#include <cstdint>

namespace helix {

/// What the tile has to say. The enum values are the print_stats_content subject ints.
enum class PrintStatsContent : int {
    Loading = 0,   ///< the history cache has not answered yet
    NoHistory = 1, ///< the printer has never printed
    Activity = 2,  ///< this week's hours, with whatever lifetime figures are known
};

/// @p lifetime_prints is the server's lifetime job count, negative while unknown.
[[nodiscard]] constexpr PrintStatsContent print_stats_content(bool history_loaded, int cached_jobs,
                                                              int64_t lifetime_prints) {
    if (!history_loaded)
        return PrintStatsContent::Loading;
    if (cached_jobs == 0 && lifetime_prints <= 0)
        return PrintStatsContent::NoHistory;
    return PrintStatsContent::Activity;
}

/// How much of the lifetime totals the support line carries.
enum class PrintStatsSupport : int {
    None = 0,       ///< no line
    PrintsOnly = 1, ///< the lifetime print count
    Full = 2,       ///< prints, hours and filament
};

struct PrintStatsTileLayoutInput {
    int content_w = 0; ///< the box the content draws in, chrome already subtracted
    int content_h = 0;
    PrintStatsContent content = PrintStatsContent::Loading;
    bool lifetime_known = false; ///< the server's totals have arrived
    RungNeeds needs;             ///< offers strip, cell, wide and full only
};

/// The published verdict. `rung` is SizeRung's int; the flags are 0/1 subjects.
struct PrintStatsTileVerdict {
    SizeRung rung = SizeRung::Strip;
    bool fits = false;
    bool show_bars = false;
    bool show_day_labels = false;
    bool side_by_side = false; ///< hero beside the bars rather than above them
    PrintStatsSupport support = PrintStatsSupport::None;
};

[[nodiscard]] constexpr PrintStatsTileVerdict
decide_print_stats_tile_layout(const PrintStatsTileLayoutInput& in) {
    PrintStatsTileVerdict v;
    v.rung = choose_size_rung(in.content_w, in.content_h, in.needs);
    v.fits = holds_any_rung(in.content_w, in.content_h, in.needs);
    const int r = static_cast<int>(v.rung);
    const bool activity = in.content == PrintStatsContent::Activity;
    v.show_bars = r >= static_cast<int>(SizeRung::Cell) && activity;
    v.show_day_labels = v.show_bars && v.rung == SizeRung::Full;
    v.side_by_side = v.rung == SizeRung::Wide;
    if (v.rung != SizeRung::Strip && in.lifetime_known && activity) {
        v.support = r >= static_cast<int>(SizeRung::Wide) ? PrintStatsSupport::Full
                                                          : PrintStatsSupport::PrintsOnly;
    }
    return v;
}

} // namespace helix
