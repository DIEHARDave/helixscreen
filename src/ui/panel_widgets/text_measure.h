// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "lvgl/lvgl.h"
#include "lvgl/src/misc/lv_text_private.h" // lv_text_get_width, lv_text_attributes_t

#include <algorithm>
#include <string>

namespace helix::ui {

/// Pixel width of a UTF-8 string in `font`. lv_text_get_width dereferences its
/// attributes argument, so it gets a zeroed block (no recolor, zero letter
/// space, unbounded width); NULL crashes.
inline int measure_text_px(const char* txt, const lv_font_t* font) {
    if (!txt || !font)
        return 0;
    lv_text_attributes_t attrs;
    lv_text_attributes_init(&attrs);
    attrs.letter_space = 0;
    attrs.max_width = LV_COORD_MAX;
    return lv_text_get_width(txt, LV_TEXT_LEN_MAX, font, &attrs);
}

inline int measure_text_px(const std::string& txt, const lv_font_t* font) {
    return measure_text_px(txt.c_str(), font);
}

/// The widest single word of @p text in `font`: the least width a label
/// wrapping at spaces needs.
inline int widest_word_px(const std::string& text, const lv_font_t* font) {
    int widest = 0;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = std::min(text.find(' ', start), text.size());
        if (end > start) {
            widest = std::max(widest, measure_text_px(text.substr(start, end - start), font));
        }
        start = end + 1;
    }
    return widest;
}

} // namespace helix::ui
