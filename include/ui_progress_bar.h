// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

/// Register progress_bar XML event callbacks.
/// Must be called before XML components that use <progress_bar> are created.
void ui_progress_bar_init();

namespace helix::ui {

/// One round trip of the estimate bar's highlight, left end to right end and back.
inline constexpr uint32_t kProgressSweepPeriodMs = 2400;

/// Where the estimate bar's highlight sits, in px.
struct SweepBand {
    int32_t x1;
    int32_t x2;
};

/// The highlight band over a track starting at @p track_x1 and @p track_w wide,
/// at @p elapsed_ms into its period. The band is a third of the track (never
/// narrower than 8px, never wider than the track) and travels end to end and
/// back, always wholly inside the track, so no clip ever cuts it square against
/// the track's rounded ends.
constexpr SweepBand progress_sweep_band(int32_t track_x1, int32_t track_w, uint32_t elapsed_ms) {
    int32_t band_w = track_w / 3 > 8 ? track_w / 3 : 8;
    if (band_w > track_w) {
        band_w = track_w;
    }
    const int32_t travel = track_w - band_w;
    const uint32_t half = kProgressSweepPeriodMs / 2;
    const uint32_t t = elapsed_ms % kProgressSweepPeriodMs;
    const uint32_t leg = t < half ? t : kProgressSweepPeriodMs - t;
    const int32_t at = static_cast<int32_t>(static_cast<int64_t>(leg) * travel / half);
    return {track_x1 + at, track_x1 + at + band_w - 1};
}

} // namespace helix::ui
