// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

/// Pure geometry and rasterization behind <humidity_drop>: a water drop filled
/// from the bottom to a level set by a humidity reading, its surface a wave.
/// No LVGL here, so every rule is testable without a display.
namespace helix::ui::humidity_drop {

/// A humidity_x10 below zero: no reading. The drop draws as an outline only.
constexpr int kUnknown = -1;

/// The drop's outline: the MDI "water" glyph's shape, a round bottom under a
/// point. y grows downward.
struct DropShape {
    float cx = 0;    ///< centre line
    float tip_y = 0; ///< the point at the top
    float cy = 0;    ///< centre (and equator) of the round bottom
    float r = 0;     ///< radius of the round bottom; the drop is 2r wide

    float bottom() const {
        return cy + r;
    }
};

/// The largest drop that fits a w x h box with a little air around it, centred.
DropShape fit_drop(int w, int h);

/// Half the drop's width at row @p y; 0 above the tip and below the bottom.
float half_width(const DropShape& s, float y);

/// Signed distance from (x, y) to the drop's edge, in pixels: negative inside.
float signed_distance(const DropShape& s, float x, float y);

/// Outline stroke width for a drop, in pixels.
float stroke_width(const DropShape& s);

/// Wave amplitude for a drop, in pixels: visible at icon size, gentle when large.
float wave_amplitude(const DropShape& s);

/// The fill fraction 0..1 a reading shows; below zero when there is no reading.
/// Readings past 100% clamp to full.
float fill_fraction(int humidity_x10);

/// The still water line for @p fraction: at 0 every crest sits under the inside
/// of the bottom, at 1 every trough sits over the inside of the tip, so 0% and
/// 100% read as empty and full instead of a sliver or a gap.
float surface_level(const DropShape& s, float fraction);

/// The wave's y at column @p x: @p level displaced by a sine of one drop width.
float wave_y(const DropShape& s, float level, float amplitude, float phase, float x);

/// Phase the back wave runs at for a given front phase: offset and drifting the
/// other way, so the two tones cross instead of moving as one.
float back_phase(float front_phase);

/// Whether a drop needs animation ticks: only a partial fill shows a moving
/// surface, and only a drawn drop is worth redrawing.
bool should_animate(bool animations_enabled, bool visible, int humidity_x10);

struct Rgb {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
};

struct DropColors {
    Rgb outline;
    Rgb back;  ///< the darker wave behind
    Rgb front; ///< the lighter wave in front
};

/// Paint the drop into @p px, a w x h buffer of LVGL ARGB8888 pixels (bytes
/// B, G, R, A, straight alpha) with @p stride bytes per row. Every pixel is
/// written; outside the drop is transparent.
void rasterize(uint8_t* px, int w, int h, int stride, const DropColors& colors, int humidity_x10,
               float phase);

} // namespace helix::ui::humidity_drop
