// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_tool_switcher_widget.cpp
 * @brief Tests for ToolSwitcherWidget registration and metadata
 *
 * Verifies that the tool_switcher widget is registered correctly in the
 * panel widget registry with expected metadata and hardware gating, and that
 * the rail, the face and the sheet it draws keep every target finger-sized and
 * route each tap where it belongs.
 */

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/tool_switcher_rig.h"
#include "../test_helpers/tool_switcher_test_access.h"
#include "panel_widget_registry.h"
#include "theme_manager.h"
#include "tool_state.h"

#include <cstring>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

TEST_CASE("ToolSwitcherWidget: widget def exists in registry", "[tool_switcher][panel_widget]") {
    const auto* def = find_widget_def("tool_switcher");
    REQUIRE(def != nullptr);

    SECTION("has correct id") {
        REQUIRE(std::strcmp(def->id, "tool_switcher") == 0);
    }

    SECTION("has display name") {
        REQUIRE(def->display_name != nullptr);
        REQUIRE(std::strlen(def->display_name) > 0);
    }

    SECTION("has icon") {
        REQUIRE(def->icon != nullptr);
        REQUIRE(std::strlen(def->icon) > 0);
    }

    SECTION("has description") {
        REQUIRE(def->description != nullptr);
        REQUIRE(std::strlen(def->description) > 0);
    }
}

TEST_CASE("ToolSwitcherWidget: no hardware gate (visible to all printers)",
          "[tool_switcher][panel_widget]") {
    const auto* def = find_widget_def("tool_switcher");
    REQUIRE(def != nullptr);
    REQUIRE(def->hardware_gate_subject == nullptr);
}

TEST_CASE("ToolSwitcherWidget: supports scaling from one cell to 2x2 cells",
          "[tool_switcher][panel_widget]") {
    const auto* def = find_widget_def("tool_switcher");
    REQUIRE(def != nullptr);

    // Tracks, not cells — a track is half a cell (GridLayout::TRACKS_PER_CELL).
    // Default is one whole cell (compact).
    REQUIRE(def->colspan == 2);
    REQUIRE(def->rowspan == 2);

    // Can scale up to 2x2 cells
    REQUIRE(def->effective_max_colspan() == 4);
    REQUIRE(def->effective_max_rowspan() == 4);

    // Minimum is one track: every box has a mode, so fits_at refuses none
    REQUIRE(def->effective_min_colspan() == 1);
    REQUIRE(def->effective_min_rowspan() == 1);

    REQUIRE(def->is_scalable());
}

TEST_CASE("ToolSwitcherWidget: not enabled by default (requires multi-tool)",
          "[tool_switcher][panel_widget]") {
    const auto* def = find_widget_def("tool_switcher");
    REQUIRE(def != nullptr);
    REQUIRE_FALSE(def->default_enabled);
}

// ============================================================================
// The rail, the face and the sheet as drawn (prestonbrown/helixscreen#1784)
// ============================================================================

namespace {

int mode() {
    return lv_subject_get_int(lv_xml_get_subject(nullptr, "tool_switcher_mode"));
}

bool shown(lv_obj_t* obj) {
    return obj && !lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t* named(PanelWidgetHarness<ToolSwitcherWidget>& h, const std::string& name) {
    return h.child(name.c_str());
}

void tap(lv_obj_t* obj) {
    REQUIRE(obj != nullptr);
    lv_obj_send_event(obj, LV_EVENT_CLICKED, nullptr);
}

/// The sheet's head rows' buttons and its Park row, while it is open.
struct Sheet {
    std::vector<lv_obj_t*> rows;
    lv_obj_t* park = nullptr;
};

Sheet open_sheet(ToolSwitcherWidget& widget) {
    Sheet sheet;
    lv_obj_t* list = ToolSwitcherTestAccess::picker_list(widget);
    REQUIRE(list != nullptr);
    lv_obj_update_layout(lv_obj_get_parent(list));
    for (uint32_t i = 0; i < lv_obj_get_child_count(list); ++i) {
        sheet.rows.push_back(
            lv_obj_find_by_name(lv_obj_get_child(list, static_cast<int32_t>(i)), "tool_btn"));
    }
    sheet.park = lv_obj_find_by_name(lv_obj_get_parent(list), "tool_park_btn");
    return sheet;
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "tool_switcher: the rail draws a finger-sized segment per head, then Park",
                 "[tool_switcher][1784]") {
    ToolChangerRig rig(4);
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);
    const int floor = resolve_px("touch_target_min", 48);

    // 214x93: four segments fit at the floor, a fifth cell for Park does not.
    resize_content(h, 214, 93);
    process_lvgl(30);
    REQUIRE(mode() == static_cast<int>(ToolSwitcherMode::Rail));
    lv_obj_update_layout(h.root());
    CHECK(shown(named(h, "tool_switcher_rail_view")));
    CHECK_FALSE(shown(named(h, "tool_switcher_face_view")));
    int last_x = -1;
    for (int i = 0; i < 4; ++i) {
        CAPTURE(i);
        lv_obj_t* seg = named(h, "tool_switcher_seg_" + std::to_string(i));
        REQUIRE(shown(seg));
        CHECK(lv_obj_get_width(seg) >= floor);
        CHECK(lv_obj_get_height(seg) >= floor);
        // Side by side in head order, not stacked.
        CHECK(lv_obj_get_x(seg) > last_x);
        last_x = lv_obj_get_x(seg);
        CHECK_FALSE(shown(named(h, "tool_switcher_rail_park_" + std::to_string(i))));
    }
    CHECK(named(h, "tool_switcher_seg_4") == nullptr);

    // A wide tile has a cell for Park too: after the last head, finger-sized.
    resize_content(h, 600, 60);
    process_lvgl(30);
    REQUIRE(mode() == static_cast<int>(ToolSwitcherMode::RailWithPark));
    lv_obj_update_layout(h.root());
    lv_obj_t* park = named(h, "tool_switcher_rail_park_3");
    REQUIRE(shown(park));
    CHECK(lv_obj_get_width(park) >= floor);
    CHECK(lv_obj_get_height(park) >= floor);
    CHECK(lv_obj_get_x(park) > lv_obj_get_x(named(h, "tool_switcher_seg_3")));
    for (int i = 0; i < 3; ++i) {
        CHECK_FALSE(shown(named(h, "tool_switcher_rail_park_" + std::to_string(i))));
    }
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "tool_switcher: a segment tap mounts its head; the mounted one opens the sheet",
                 "[tool_switcher][1784]") {
    ToolChangerRig rig(4);
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);
    resize_content(h, 214, 93);
    process_lvgl(30);
    REQUIRE(mode() == static_cast<int>(ToolSwitcherMode::Rail));
    REQUIRE(ToolState::instance().active_tool_index() == 0);

    tap(named(h, "tool_switcher_seg_1"));
    CHECK(rig.backend->change_tool_calls == 1);
    CHECK(rig.backend->last_tool == 1);
    CHECK_FALSE(h.widget().has_overlay_open());

    tap(named(h, "tool_switcher_seg_0"));
    CHECK(h.widget().has_overlay_open());
    CHECK(rig.backend->change_tool_calls == 1);
    ToolSwitcherTestAccess::hide_picker(h.widget());
    process_lvgl(30);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "tool_switcher: the Pair face opens the sheet, and Park beside it needs a head",
                 "[tool_switcher][1784]") {
    ToolChangerRig rig(5);
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);
    const int floor = resolve_px("touch_target_min", 48);

    // 214x93: five heads do not fit a rail, so the mounted head beside Park.
    resize_content(h, 214, 93);
    process_lvgl(30);
    REQUIRE(mode() == static_cast<int>(ToolSwitcherMode::Pair));
    lv_obj_update_layout(h.root());
    CHECK_FALSE(shown(named(h, "tool_switcher_rail_view")));
    REQUIRE(shown(named(h, "tool_switcher_face_view")));
    lv_obj_t* face = named(h, "tool_switcher_face");
    lv_obj_t* park_cell = named(h, "tool_switcher_face_park");
    lv_obj_t* park = named(h, "tool_switcher_face_park_btn");
    REQUIRE(shown(face));
    REQUIRE(shown(park_cell));
    CHECK(lv_obj_get_width(face) >= floor);
    CHECK(lv_obj_get_height(face) >= floor);
    CHECK(lv_obj_get_width(park) >= floor);
    CHECK(lv_obj_get_x(park_cell) > lv_obj_get_x(face));

    // A head is mounted, so Park is live; an empty carriage disables it.
    CHECK_FALSE(lv_obj_has_state(park, LV_STATE_DISABLED));
    ToolChangerRig::unmount();
    process_lvgl(30);
    REQUIRE(ToolState::instance().active_tool_index() == -1);
    CHECK(lv_obj_has_state(park, LV_STATE_DISABLED));
    const std::string face_label =
        lv_subject_get_string(lv_xml_get_subject(nullptr, "tool_switcher_face_label"));
    CHECK((face_label == lv_tr("Choose head") || face_label == lv_tr("None")));

    tap(face);
    CHECK(h.widget().has_overlay_open());
    ToolSwitcherTestAccess::hide_picker(h.widget());
    process_lvgl(30);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "tool_switcher: the sheet lists every head and Park, each a finger tall",
                 "[tool_switcher][1784]") {
    ToolChangerRig rig(3);
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);
    const int floor = resolve_px("touch_target_min", 48);
    h.resize(2, 2, 160, 60);
    process_lvgl(30);

    ToolSwitcherTestAccess::show_picker(h.widget());
    Sheet sheet = open_sheet(h.widget());
    REQUIRE(sheet.rows.size() == 3);
    for (size_t i = 0; i < sheet.rows.size(); ++i) {
        CAPTURE(i);
        REQUIRE(sheet.rows[i] != nullptr);
        CHECK(lv_obj_get_height(sheet.rows[i]) >= floor);
    }
    REQUIRE(shown(sheet.park));
    CHECK(lv_obj_get_height(sheet.park) >= floor);
    CHECK_FALSE(lv_obj_has_state(sheet.park, LV_STATE_DISABLED));

    // Nothing mounted: nothing to park.
    ToolChangerRig::unmount();
    process_lvgl(30);
    REQUIRE(ToolState::instance().active_tool_index() == -1);
    CHECK(lv_obj_has_state(sheet.park, LV_STATE_DISABLED));
    ToolSwitcherTestAccess::hide_picker(h.widget());
    process_lvgl(30);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "tool_switcher: a segment shows its second line only where the cell holds it",
                 "[tool_switcher][1784]") {
    ToolChangerRig rig(2);
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);
    const lv_font_t* body = theme_manager_get_font("font_body");
    const lv_font_t* small = theme_manager_get_font("font_small");
    const int lines = lv_font_get_line_height(body) + lv_font_get_line_height(small);
    const int strip = resolve_px("space_xs", 4);
    lv_subject_t* two_line = lv_xml_get_subject(nullptr, "tool_switcher_two_line");
    REQUIRE(two_line != nullptr);
    auto cell_h = [] {
        return lv_subject_get_int(lv_xml_get_subject(nullptr, "tool_switcher_cell_h"));
    };

    // Tall cells: the label and its sub both show.
    resize_content(h, 214, 93);
    process_lvgl(30);
    REQUIRE(cell_h() - strip >= lines);
    CHECK(lv_subject_get_int(two_line) == 1);
    lv_obj_t* sub = lv_obj_find_by_name(h.child("tool_switcher_seg_0"), "tool_switcher_seg_sub");
    REQUIRE(sub != nullptr);
    CHECK_FALSE(lv_obj_has_flag(sub, LV_OBJ_FLAG_HIDDEN));

    // Cells on the touch floor are too short for two lines over the strip.
    const int floor = resolve_px("touch_target_min", 48);
    resize_content(h, 3 * floor + 2 * 2, floor);
    process_lvgl(30);
    REQUIRE(mode() == static_cast<int>(ToolSwitcherMode::RailWithPark));
    REQUIRE(cell_h() - strip < lines);
    CHECK(lv_subject_get_int(two_line) == 0);
    CHECK(lv_obj_has_flag(sub, LV_OBJ_FLAG_HIDDEN));
}
