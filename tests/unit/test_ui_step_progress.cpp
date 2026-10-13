// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ui_step_progress.cpp
 * @brief Rendering tests for the step-progress widget's connector lines.
 *
 * The vertical connector lines between step circles are absolutely-positioned
 * IGNORE_LAYOUT rects sized from a layout pass. When the widget is created while
 * an ancestor is hidden, the layout is degenerate (0-size) and the connectors
 * used to be skipped permanently. These tests pin the two contracts:
 *   1. connectors exist with a positive extent when created while visible, and
 *   2. connectors are (re)created + sized once the widget becomes visible, even
 *      if it was built while hidden — driven by an LV_EVENT_SIZE_CHANGED reflow.
 */

#include "ui_step_progress.h"

#include "../lvgl_test_fixture.h"
#include "lvgl/lvgl.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <string>

#include "../catch_amalgamated.hpp"

namespace {

// A 4-step vertical model → 3 connectors between the circles.
std::array<ui_step_t, 4> make_vertical_steps() {
    return {{
        {"Heat nozzle", helix::StepState::Pending},
        {"Feed filament", helix::StepState::Pending},
        {"Purge", helix::StepState::Pending},
        {"Clean", helix::StepState::Pending},
    }};
}

lv_obj_t* make_sized_parent(lv_obj_t* screen) {
    lv_obj_t* parent = lv_obj_create(screen);
    lv_obj_set_size(parent, 300, 400);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_border_width(parent, 0, 0);
    return parent;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "step progress creates vertical connectors when visible",
                 "[ui][step]") {
    lv_obj_t* parent = make_sized_parent(test_screen());

    auto steps = make_vertical_steps();
    lv_obj_t* widget = ui_step_progress_create(parent, steps.data(), static_cast<int>(steps.size()),
                                               /*horizontal=*/false, nullptr);
    REQUIRE(widget != nullptr);

    lv_obj_update_layout(parent);

    for (int i = 0; i < static_cast<int>(steps.size()) - 1; i++) {
        lv_obj_t* conn = ui_step_progress_test_get_connector(widget, i);
        INFO("connector " << i);
        REQUIRE(conn != nullptr);
        CHECK(lv_obj_get_height(conn) > 0);
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "step progress recomputes connectors on reflow (SIZE_CHANGED)",
                 "[ui][step]") {
    // Connectors are absolutely-positioned rects sized from the layout. When the
    // widget reflows — a becoming-visible container or a growing live-temp label
    // shifting the circles — the connectors must be recomputed, not frozen at
    // their create-time geometry. A horizontal bar makes this deterministic: the
    // step items flex-grow to fill the width, so circle spacing (and therefore
    // connector width) tracks the container width directly.
    lv_obj_t* parent = make_sized_parent(test_screen());
    lv_obj_set_width(parent, 600);

    auto steps = make_vertical_steps(); // labels/states reused; horizontal layout
    lv_obj_t* widget = ui_step_progress_create(parent, steps.data(), static_cast<int>(steps.size()),
                                               /*horizontal=*/true, nullptr);
    REQUIRE(widget != nullptr);
    lv_obj_update_layout(parent);

    lv_obj_t* conn0_wide = ui_step_progress_test_get_connector(widget, 0);
    REQUIRE(conn0_wide != nullptr);
    const int32_t wide_w = lv_obj_get_width(conn0_wide);
    CHECK(wide_w > 0);

    // Halve the available width and drive a reflow, as an unhide/resize would.
    lv_obj_set_width(parent, 300);
    lv_obj_update_layout(parent);
    lv_obj_send_event(widget, LV_EVENT_SIZE_CHANGED, nullptr);
    lv_obj_update_layout(parent);

    lv_obj_t* conn0_narrow = ui_step_progress_test_get_connector(widget, 0);
    REQUIRE(conn0_narrow != nullptr);
    const int32_t narrow_w = lv_obj_get_width(conn0_narrow);

    INFO("wide_w=" << wide_w << " narrow_w=" << narrow_w);
    // A recomputed connector must shrink with the container; a frozen one keeps
    // its create-time width and overshoots the narrower layout.
    CHECK(narrow_w < wide_w);
    CHECK(narrow_w > 0);
}

TEST_CASE("step progress caption: the active step, else the last done, else the first",
          "[ui][step][step_caption]") {
    using helix::StepState;
    const StepState active[] = {StepState::Completed, StepState::Active, StepState::Pending};
    CHECK(ui_step_progress_caption_step(active, 3) == 1);
    const StepState done[] = {StepState::Completed, StepState::Completed, StepState::Completed};
    CHECK(ui_step_progress_caption_step(done, 3) == 2);
    const StepState fresh[] = {StepState::Pending, StepState::Pending, StepState::Pending};
    CHECK(ui_step_progress_caption_step(fresh, 3) == 0);
    // Steps completed out of order, nothing active: the last one done.
    const StepState partial[] = {StepState::Completed, StepState::Pending, StepState::Completed};
    CHECK(ui_step_progress_caption_step(partial, 3) == 2);
    CHECK(ui_step_progress_caption_step(nullptr, 0) == -1);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "step progress current-label-only mode: one centred caption, no per-step labels",
                 "[ui][step][step_caption]") {
    lv_obj_t* parent = make_sized_parent(test_screen());
    lv_obj_set_width(parent, 264);
    auto steps = make_vertical_steps();
    lv_obj_t* widget = ui_step_progress_create(parent, steps.data(), static_cast<int>(steps.size()),
                                               /*horizontal=*/true, nullptr);
    REQUIRE(widget != nullptr);
    ui_step_progress_set_current_label_only(widget, true);
    ui_step_progress_set_current(widget, 2);
    lv_obj_update_layout(parent);

    // Every step's own label is hidden; the caption names the current step.
    for (uint32_t i = 0; i < lv_obj_get_child_count(widget); ++i) {
        lv_obj_t* item = lv_obj_get_child(widget, (int32_t)i);
        if (lv_obj_get_child_count(item) < 2)
            continue; // a connector
        CHECK(lv_obj_has_flag(lv_obj_get_child(item, 1), LV_OBJ_FLAG_HIDDEN));
    }
    lv_obj_t* caption = ui_step_progress_get_caption(widget);
    REQUIRE(caption != nullptr);
    CHECK(std::string(lv_label_get_text(caption)) == "Purge");
    lv_area_t ca, wa;
    lv_obj_get_coords(caption, &ca);
    lv_obj_get_coords(widget, &wa);
    CHECK(ca.y1 >= wa.y1);
    CHECK(ca.y2 <= wa.y2);
    CHECK(std::abs((ca.x1 + ca.x2) / 2 - (wa.x1 + wa.x2) / 2) <= 1);
    // The caption stands under the row of dots, not over it.
    for (uint32_t i = 0; i < lv_obj_get_child_count(widget); ++i) {
        lv_obj_t* item = lv_obj_get_child(widget, (int32_t)i);
        if (item == caption || lv_obj_get_child_count(item) < 2)
            continue;
        lv_area_t ia;
        lv_obj_get_coords(lv_obj_get_child(item, 0), &ia);
        CHECK(ca.y1 > ia.y2);
    }

    // A relabel of the current step reaches the caption; another step's does not.
    ui_step_progress_set_label(widget, 2, "Purge 40 mm");
    CHECK(std::string(lv_label_get_text(caption)) == "Purge 40 mm");
    ui_step_progress_set_label(widget, 0, "Heat nozzle 220");
    CHECK(std::string(lv_label_get_text(caption)) == "Purge 40 mm");

    ui_step_progress_set_current(widget, 3);
    CHECK(std::string(lv_label_get_text(caption)) == "Clean");
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "step progress hug mode: a vertical list as wide as its widest step",
                 "[ui][step][step_hug]") {
    lv_obj_t* parent = make_sized_parent(test_screen());
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    auto steps = make_vertical_steps();
    lv_obj_t* widget = ui_step_progress_create(parent, steps.data(), static_cast<int>(steps.size()),
                                               /*horizontal=*/false, nullptr);
    REQUIRE(widget != nullptr);
    ui_step_progress_set_hug_content(widget, true);
    lv_obj_update_layout(parent);

    lv_area_t pa, wa;
    lv_obj_get_coords(parent, &pa);
    lv_obj_get_coords(widget, &wa);
    CAPTURE(pa.x1, pa.x2, wa.x1, wa.x2);
    // The block is narrower than its parent, which centres it.
    REQUIRE(lv_area_get_width(&wa) < lv_area_get_width(&pa) / 2);
    CHECK(std::abs((wa.x1 - pa.x1) - (pa.x2 - wa.x2)) <= 1);
    // Its widest step sets its width, and every label starts at the same x.
    int32_t widest = 0;
    int32_t label_x = -1;
    for (uint32_t i = 0; i < lv_obj_get_child_count(widget); ++i) {
        lv_obj_t* item = lv_obj_get_child(widget, (int32_t)i);
        if (lv_obj_get_child_count(item) < 2)
            continue; // a connector
        widest = std::max(widest, lv_obj_get_width(item));
        lv_area_t la;
        lv_obj_get_coords(lv_obj_get_child(item, 1), &la);
        if (label_x < 0)
            label_x = la.x1;
        CHECK(la.x1 == label_x);
    }
    CHECK(widest == lv_area_get_width(&wa));

    // A label longer than the parent wraps inside it rather than running past.
    ui_step_progress_set_label(widget, 1,
                               "Feed filament through the buffer and the bowden tube to the "
                               "extruder gears, then on to the nozzle");
    lv_obj_update_layout(parent);
    lv_obj_get_coords(widget, &wa);
    CHECK(wa.x1 >= pa.x1);
    CHECK(wa.x2 <= pa.x2);
    lv_obj_t* long_label = nullptr;
    for (uint32_t i = 0, seen = 0; i < lv_obj_get_child_count(widget); ++i) {
        lv_obj_t* item = lv_obj_get_child(widget, (int32_t)i);
        if (lv_obj_get_child_count(item) < 2)
            continue;
        if (seen++ == 1)
            long_label = lv_obj_get_child(item, 1);
    }
    REQUIRE(long_label != nullptr);
    lv_area_t ll;
    lv_obj_get_coords(long_label, &ll);
    CHECK(ll.x2 <= pa.x2);
    CHECK(lv_obj_get_height(long_label) >
          2 * lv_font_get_line_height(lv_obj_get_style_text_font(long_label, LV_PART_MAIN)) - 1);
}
