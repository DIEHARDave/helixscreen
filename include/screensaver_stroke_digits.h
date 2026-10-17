// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <vector>

/**
 * @file screensaver_stroke_digits.h
 * @brief Digits drawn as strokes along lines and arcs, at any size and stroke weight
 *
 * A digit's centre line lives in a box one glyph unit wide and STROKE_DIGIT_PATH_H tall (y
 * down). Coverage comes from the distance to that line, so one shape serves the flip clock's
 * bold cards and High Fly's thin numerals. No LVGL.
 */

namespace helix::ui {

/// Height of a digit's centre-line box, in glyph units; its width is 1.
inline constexpr float STROKE_DIGIT_PATH_H = 1.6f;

/// Distance, in glyph units, from (px, py) to the nearest point of `digit`'s centre line
/// (0 to 9). Box-relative coordinates: (0, 0) is the path box's top left.
float stroke_digit_distance(int digit, float px, float py);

/// Coverage of the ten digits at one size and stroke, each `w` x `h`, 0 to 255, back to back.
struct StrokeDigitSet {
    int32_t w = 0; ///< lround((1 + stroke) * unit)
    int32_t h = 0; ///< lround((STROKE_DIGIT_PATH_H + stroke) * unit)
    std::vector<uint8_t> coverage;

    /// Coverage of `digit` at (gx, gy) inside its box, 0 outside it or for a digit past 0-9.
    uint8_t at(int digit, int32_t gx, int32_t gy) const;
    /// Bytes held, capacity included.
    size_t heap_bytes() const;
    /// Frees the coverage, capacity included.
    void clear();
};

/**
 * @brief Rasterises the ten digits at `unit` pixels per glyph unit with strokes `stroke` glyph
 * units wide
 *
 * Pixel (x, y) samples the distance at ((x + 0.5) / unit - stroke / 2, (y + 0.5) / unit -
 * stroke / 2) and covers (stroke / 2 - distance) * unit + 0.5, clamped to 0-1. The flip clock
 * calls it with stroke 0.2.
 */
void build_stroke_digits(int32_t unit, float stroke, StrokeDigitSet& out);

} // namespace helix::ui
