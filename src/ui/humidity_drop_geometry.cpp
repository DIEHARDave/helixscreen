// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "humidity_drop_geometry.h"

#include <algorithm>
#include <cmath>

namespace helix::ui::humidity_drop {

namespace {

// Single precision throughout: the ESP32-S3's FPU has no double unit.
constexpr float kTwoPi = 6.28318530718f;

// The MDI "water" glyph in its 24-unit box: a half circle of radius 6 under a
// cubic from the equator's left end (6,14), control (6,10), to the tip
// (12,3.25) with its second control on the tip. In radii from the circle's
// centre the tip sits 10.75/6 above it.
constexpr float kTipAbove = 10.75f / 6.0f;
constexpr float kCtrlAbove = 4.0f / 6.0f;

// The drop's share of its box's height, the rest left as air.
constexpr float kHeightShare = 0.84f;
constexpr float kWidthShare = 0.92f;

/// The upper curve at parameter t: half width u and height above the centre v,
/// both in radii. t=0 is the equator, t=1 the tip; v falls monotonically.
void upper_curve(float t, float& u, float& v) {
    const float s = 1.0f - t;
    u = s * s * (1.0f + 2.0f * t);
    v = 3.0f * s * s * t * kCtrlAbove + kTipAbove * t * t * (3.0f - 2.0f * t);
}

/// Half width in radii at height @p above (radii above the centre), 0..kTipAbove.
float upper_half_width(float above) {
    float lo = 0.0f;
    float hi = 1.0f;
    float u = 0.0f;
    float v = 0.0f;
    for (int i = 0; i < 18; ++i) {
        const float mid = 0.5f * (lo + hi);
        upper_curve(mid, u, v);
        if (v < above)
            lo = mid;
        else
            hi = mid;
    }
    upper_curve(0.5f * (lo + hi), u, v);
    return u;
}

/// What a row needs to measure distance across it, so a raster pays the curve
/// inversion once per row rather than per pixel.
struct Row {
    enum Kind { AboveTip, Upper, Round } kind;
    float hw;       ///< half width (Upper)
    float inv_norm; ///< 1/sqrt(1 + slope^2) of the edge (Upper)
};

Row row_profile(const DropShape& s, float y) {
    if (y >= s.cy)
        return {Row::Round, 0, 0};
    if (y <= s.tip_y)
        return {Row::AboveTip, 0, 0};
    const float hw = half_width(s, y);
    const float e = 0.5f;
    const float slope = (half_width(s, std::min(y + e, s.cy)) - half_width(s, y - e)) /
                        (std::min(y + e, s.cy) - (y - e));
    return {Row::Upper, hw, 1.0f / std::sqrt(1.0f + slope * slope)};
}

float row_distance(const DropShape& s, const Row& row, float x, float y) {
    const float dx = std::fabs(x - s.cx);
    switch (row.kind) {
    case Row::Round: {
        const float dy = y - s.cy;
        return std::sqrt(dx * dx + dy * dy) - s.r;
    }
    case Row::AboveTip: {
        const float dy = s.tip_y - y;
        return std::sqrt(dx * dx + dy * dy);
    }
    case Row::Upper:
    default:
        return (dx - row.hw) * row.inv_norm;
    }
}

float clamp01(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

/// Premultiplied colour accumulator for one pixel, layered back to front.
struct Acc {
    float r = 0, g = 0, b = 0, a = 0;
    void over(const Rgb& c, float alpha) {
        if (alpha <= 0.0f)
            return;
        const float k = 1.0f - alpha;
        r = c.r * alpha + r * k;
        g = c.g * alpha + g * k;
        b = c.b * alpha + b * k;
        a = alpha + a * k;
    }
};

constexpr Rgb kWhite{255, 255, 255};

// Translucent layers, as alpha.
constexpr float kTintAlpha = 0.14f;
constexpr float kHighlightAlpha = 0.45f;
constexpr float kUnknownOutlineAlpha = 0.45f;

} // namespace

DropShape fit_drop(int w, int h) {
    const float height_units = 1.0f + kTipAbove;
    float r = (h * kHeightShare) / height_units;
    r = std::min(r, w * kWidthShare * 0.5f);
    DropShape s;
    s.r = r;
    s.cx = w * 0.5f;
    s.tip_y = (h - r * height_units) * 0.5f;
    s.cy = s.tip_y + r * kTipAbove;
    return s;
}

float half_width(const DropShape& s, float y) {
    if (s.r <= 0.0f || y < s.tip_y || y > s.bottom())
        return 0.0f;
    if (y >= s.cy) {
        const float dy = y - s.cy;
        return std::sqrt(std::max(0.0f, s.r * s.r - dy * dy));
    }
    return s.r * upper_half_width((s.cy - y) / s.r);
}

float signed_distance(const DropShape& s, float x, float y) {
    return row_distance(s, row_profile(s, y), x, y);
}

float stroke_width(const DropShape& s) {
    return std::clamp(s.r * 0.17f, 1.0f, 3.5f);
}

float wave_amplitude(const DropShape& s) {
    return std::max(0.75f, s.r * 0.09f);
}

float fill_fraction(int humidity_x10) {
    if (humidity_x10 < 0)
        return -1.0f;
    return std::min(1.0f, humidity_x10 / 1000.0f);
}

float surface_level(const DropShape& s, float fraction) {
    const float a = wave_amplitude(s);
    const float sw = stroke_width(s);
    const float empty = s.bottom() - sw + a;
    const float full = s.tip_y + sw - a;
    return empty + (full - empty) * clamp01(fraction);
}

float wave_y(const DropShape& s, float level, float amplitude, float phase, float x) {
    const float wavelength = 2.0f * s.r;
    if (wavelength <= 0.0f)
        return level;
    return level + amplitude * std::sin(kTwoPi * (x - s.cx) / wavelength + phase);
}

float back_phase(float front_phase) {
    return 2.2f - 0.8f * front_phase;
}

bool should_animate(bool animations_enabled, bool visible, int humidity_x10) {
    return animations_enabled && visible && humidity_x10 > 0 && humidity_x10 < 1000;
}

void rasterize(uint8_t* px, int w, int h, int stride, const DropColors& colors, int humidity_x10,
               float phase) {
    if (!px || w <= 0 || h <= 0)
        return;

    const DropShape s = fit_drop(w, h);
    const float sw = stroke_width(s);
    const float fraction = fill_fraction(humidity_x10);
    const bool known = fraction >= 0.0f;
    const bool has_fill = known && fraction > 0.0f;

    const float amp = wave_amplitude(s);
    const float level = surface_level(s, fraction);
    // The back wave rides a little higher so its crests show between the front's.
    const float back_level = level - amp;
    const float back_ph = back_phase(phase);
    // Rows wholly above both waves hold no water; rows wholly below are full.
    const float wave_top = back_level - amp - 1.0f;
    const float wave_bottom = level + amp + 1.0f;

    // A specular spot up and left on the round part sells the bulge; too small to
    // read below a few pixels of radius.
    const bool highlight = known && s.r >= 6.0f;
    const float hl_cx = s.cx - s.r * 0.45f;
    const float hl_cy = s.cy - s.r * 0.30f;
    const float hl_rx = std::max(1.0f, s.r * 0.15f);
    const float hl_ry = std::max(1.5f, s.r * 0.36f);

    const float outline_alpha = known ? 1.0f : kUnknownOutlineAlpha;

    for (int j = 0; j < h; ++j) {
        uint8_t* out = px + j * stride;
        const float y = j + 0.5f;
        const Row row = row_profile(s, y);
        for (int i = 0; i < w; ++i, out += 4) {
            const float x = i + 0.5f;
            const float d = row_distance(s, row, x, y);
            if (d >= 1.0f) {
                out[0] = out[1] = out[2] = out[3] = 0;
                continue;
            }

            Acc acc;
            // The water keeps clear of the outline's outer half.
            const float inner = clamp01(0.5f - (d + sw * 0.5f));
            if (known && inner > 0.0f) {
                acc.over(colors.front, inner * kTintAlpha);
                if (has_fill && y >= wave_top) {
                    float back_cov;
                    float front_cov;
                    if (y > wave_bottom) {
                        back_cov = front_cov = 1.0f;
                    } else {
                        back_cov = clamp01(y - wave_y(s, back_level, amp, back_ph, x) + 0.5f);
                        front_cov = clamp01(y - wave_y(s, level, amp, phase, x) + 0.5f);
                    }
                    acc.over(colors.back, inner * back_cov);
                    acc.over(colors.front, inner * front_cov);
                }
                if (highlight) {
                    const float ex = (x - hl_cx) / hl_rx;
                    const float ey = (y - hl_cy) / hl_ry;
                    // Distance outside the ellipse, roughly in pixels, for a soft edge.
                    const float e = (std::sqrt(ex * ex + ey * ey) - 1.0f) * hl_rx;
                    acc.over(kWhite, inner * kHighlightAlpha * clamp01(0.5f - e));
                }
            }

            const float ring = clamp01(std::min(0.5f - d, 0.5f + d + sw));
            acc.over(colors.outline, ring * outline_alpha);

            if (acc.a <= 0.0f) {
                out[0] = out[1] = out[2] = out[3] = 0;
                continue;
            }
            const float inv = 1.0f / acc.a;
            out[0] = static_cast<uint8_t>(std::min(255.0f, acc.b * inv + 0.5f));
            out[1] = static_cast<uint8_t>(std::min(255.0f, acc.g * inv + 0.5f));
            out[2] = static_cast<uint8_t>(std::min(255.0f, acc.r * inv + 0.5f));
            out[3] = static_cast<uint8_t>(std::min(255.0f, acc.a * 255.0f + 0.5f));
        }
    }
}

} // namespace helix::ui::humidity_drop
