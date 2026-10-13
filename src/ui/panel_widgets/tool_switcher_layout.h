// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Pure layout-decision logic for the home tool switcher.
//
// Free of LVGL so it can be unit-tested without a display. The widget measures
// its content box and fonts and feeds them here; this header decides whether
// the tile draws a rail of head segments (with or without a Park segment), a
// mounted-head face beside Park, or the face alone, and how the rail's cells
// are arranged. Every cell it hands back is at least min_target on both axes,
// so no mode ever draws a touch target smaller than the floor.

#include <vector>

namespace helix {

/// The values are the widget's published mode subject ints, so XML ref_values
/// and this enum stay in lockstep by construction.
enum class ToolSwitcherMode : int { RailWithPark = 0, Rail = 1, Pair = 2, Single = 3 };

struct ToolSwitcherLayout {
    ToolSwitcherMode mode = ToolSwitcherMode::Single;
    int cols = 1; ///< rail modes: grid columns; Pair: 2 along the long axis, else 1
    int rows = 1;
    int cell_w = 0; ///< px; >= min_target in every mode
    int cell_h = 0;
    int rail_h = 0;              ///< px of the content box the rail or face occupies
    bool detail = false;         ///< a detail block fits under the rail
    bool pair_horizontal = true; ///< Pair: face and Park side by side (else stacked)
};

/// A grid of equal cells laid into a box: the first row count whose cells
/// clear the touch floor on both axes.
struct ToolSwitcherGrid {
    bool fits = false;
    int cols = 0;
    int rows = 0;
    int cell_w = 0;
    int cell_h = 0;
};

/// Lay `cells` equal cells into a w x h box. Rows are tried from one upward,
/// each with as many columns as it needs; more rows only ever shorten the
/// cells, so the search stops at the first row count whose cells are too
/// short.
[[nodiscard]] inline ToolSwitcherGrid fit_tool_switcher_cells(int cells, int w, int h,
                                                              int min_target, int gap) {
    for (int rows = 1; rows <= cells; rows++) {
        const int cols = (cells + rows - 1) / rows;
        const int cell_w = (w - gap * (cols - 1)) / cols;
        const int cell_h = (h - gap * (rows - 1)) / rows;
        if (cell_h < min_target)
            break;
        if (cell_w >= min_target)
            return {true, cols, rows, cell_w, cell_h};
    }
    return {};
}

/// Decide the tool switcher's mode and geometry from its content box.
///
/// @param tool_count  heads on the carriage's tool changer
/// @param content_w   usable inner width of the tile
/// @param content_h   usable inner height of the tile
/// @param min_target  touch floor: no cell is drawn smaller on either axis
/// @param gap         px between adjacent cells
/// @param body_px     line height of the breakpoint's body font
/// @param small_px    line height of the breakpoint's small font
/// @param park_available  the backend can dock the mounted head; without it
///                    there is no Park cell to draw, so no mode reserves one
///
/// The first mode that fits wins: a segment per head plus Park, a segment per
/// head, the mounted head's face beside Park along the long axis, and the face
/// alone, which takes whatever box there is. The two Park modes need
/// @p park_available.
[[nodiscard]] inline ToolSwitcherLayout
decide_tool_switcher_layout(int tool_count, int content_w, int content_h, int min_target, int gap,
                            int body_px, int small_px, bool park_available) {
    ToolSwitcherLayout out;
    if (content_w <= 0 || content_h <= 0)
        return out;
    out.cell_w = content_w;
    out.cell_h = content_h;
    out.rail_h = content_h;
    if (tool_count < 1)
        return out;

    const ToolSwitcherMode rail_modes[2] = {ToolSwitcherMode::RailWithPark, ToolSwitcherMode::Rail};
    const int rail_cells[2] = {tool_count + 1, tool_count};
    for (int i = park_available ? 0 : 1; i < 2; i++) {
        const ToolSwitcherGrid g =
            fit_tool_switcher_cells(rail_cells[i], content_w, content_h, min_target, gap);
        if (!g.fits)
            continue;
        out.mode = rail_modes[i];
        out.cols = g.cols;
        out.rows = g.rows;
        out.cell_w = g.cell_w;
        out.cell_h = g.cell_h;

        // A tall box (two touch rows or more) sizes the rail's rows to their
        // content, up to half the box, and gives what is left to a detail
        // block when that holds two and a half lines of small text. A
        // one-row-tall tile keeps its full-height segments.
        const int gaps_h = gap * (g.rows - 1);
        int row_h = (content_h / 2 - gaps_h) / g.rows;
        if (row_h > body_px * 34 / 10)
            row_h = body_px * 34 / 10;
        if (row_h < min_target)
            row_h = min_target;
        const int rail_h = row_h * g.rows + gaps_h;
        if (content_h >= 2 * min_target && (content_h - rail_h) * 2 >= small_px * 5) {
            out.detail = true;
            out.cell_h = row_h;
            out.rail_h = rail_h;
        }
        return out;
    }

    // Pair: the face and a Park cell of 30% of the long axis, never under the
    // floor, with the face taking the rest.
    const bool horizontal = content_w >= content_h;
    const int long_px = horizontal ? content_w : content_h;
    const int short_px = horizontal ? content_h : content_w;
    int park_px = long_px * 3 / 10;
    if (park_px < min_target)
        park_px = min_target;
    const int face_px = long_px - gap - park_px;
    if (park_available && short_px >= min_target && face_px >= min_target) {
        out.mode = ToolSwitcherMode::Pair;
        out.pair_horizontal = horizontal;
        out.cols = horizontal ? 2 : 1;
        out.rows = horizontal ? 1 : 2;
        out.cell_w = horizontal ? face_px : content_w;
        out.cell_h = horizontal ? content_h : face_px;
    }
    return out;
}

/// The filament-colour strip along the foot of a cell @p cell_h tall: about 9%
/// of it, rounded, and never under 3 px so it reads on the smallest cells.
[[nodiscard]] inline int tool_switcher_strip_px(int cell_h) {
    const int px = (cell_h * 9 + 50) / 100;
    return px < 3 ? 3 : px;
}

/// A detail row that is the empty-carriage line rather than a head.
inline constexpr int kToolSwitcherEmptyCarriageRow = -1;

/// The detail block's rows in display order, as tool indices: the mounted head
/// first, then the others by index. With the carriage empty (@p active_tool
/// -1) the first row is kToolSwitcherEmptyCarriageRow instead. An active index
/// the tool list does not hold names no head, so every head keeps its order.
[[nodiscard]] inline std::vector<int> tool_switcher_detail_order(int tool_count, int active_tool) {
    std::vector<int> order;
    if (tool_count < 1)
        return order;
    const bool mounted = active_tool >= 0 && active_tool < tool_count;
    if (mounted)
        order.push_back(active_tool);
    else if (active_tool == -1)
        order.push_back(kToolSwitcherEmptyCarriageRow);
    for (int i = 0; i < tool_count; i++) {
        if (i != active_tool)
            order.push_back(i);
    }
    return order;
}

/// How many of @p rows detail rows fit in @p avail_h px: the first is a body
/// line, each after it a small line. Rows that do not fit are dropped whole.
[[nodiscard]] inline int tool_switcher_detail_rows_that_fit(int rows, int avail_h, int body_px,
                                                            int small_px) {
    int used = 0;
    for (int i = 0; i < rows; i++) {
        used += i == 0 ? body_px : small_px;
        if (used > avail_h)
            return i;
    }
    return rows;
}

} // namespace helix
