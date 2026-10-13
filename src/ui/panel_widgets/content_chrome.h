// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "lvgl/lvgl.h"

#include <algorithm>

namespace helix {

/// What sits between a widget's outer box and the box its content draws in.
struct ContentChrome {
    int w = 0;
    int h = 0;
};

/// The chrome of the single-child chain from `root` down to the first container
/// that holds more than one child: the padding of every container on the way, and
/// the flex gap of that branching container. A container with one child draws no
/// gap, so its gap is not chrome.
///
/// Measured from the tree because container padding is a theme value that varies by
/// widget and by breakpoint; a constant guess is either too small, and the content
/// spills out of its container, or too large, and the widget draws smaller than it
/// needs to. `reserve_px` is held back on each side on top of whatever is measured,
/// since a measured-exact fit renders as an overlap by a pixel or two.
/// `row_inset_px` is a side inset charged to row candidates alone, so it is not
/// chrome of a container whose flow is already a row.
inline ContentChrome measure_content_chrome(lv_obj_t* root, int reserve_px, int row_inset_px = 0) {
    ContentChrome chrome{2 * reserve_px, 2 * reserve_px};
    for (lv_obj_t* o = root; o != nullptr; o = lv_obj_get_child(o, 0)) {
        int side_pads = static_cast<int>(lv_obj_get_style_pad_left(o, LV_PART_MAIN)) +
                        static_cast<int>(lv_obj_get_style_pad_right(o, LV_PART_MAIN));
        if (lv_obj_get_style_flex_flow(o, LV_PART_MAIN) == LV_FLEX_FLOW_ROW) {
            side_pads -= std::min(side_pads, row_inset_px);
        }
        chrome.w += side_pads;
        chrome.h += static_cast<int>(lv_obj_get_style_pad_top(o, LV_PART_MAIN)) +
                    static_cast<int>(lv_obj_get_style_pad_bottom(o, LV_PART_MAIN));
        if (lv_obj_get_child_count(o) != 1) {
            chrome.w += static_cast<int>(lv_obj_get_style_pad_column(o, LV_PART_MAIN));
            chrome.h += static_cast<int>(lv_obj_get_style_pad_row(o, LV_PART_MAIN));
            break;
        }
    }
    return chrome;
}

} // namespace helix
