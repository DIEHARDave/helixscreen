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

#include "ui_modal.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/tool_switcher_rig.h"
#include "../test_helpers/tool_switcher_test_access.h"
#include "panel_widget_registry.h"
#include "printer_state.h"
#include "spool_latch_gate.h"
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
    lv_subject_t* two_line = lv_xml_get_subject(nullptr, "tool_switcher_two_line");
    REQUIRE(two_line != nullptr);
    auto cell_h = [] {
        return lv_subject_get_int(lv_xml_get_subject(nullptr, "tool_switcher_cell_h"));
    };

    // Tall cells: the label and its sub both show.
    resize_content(h, 214, 93);
    process_lvgl(30);
    REQUIRE(cell_h() - tool_switcher_strip_px(cell_h()) >= lines);
    CHECK(lv_subject_get_int(two_line) == 1);
    lv_obj_t* sub = lv_obj_find_by_name(h.child("tool_switcher_seg_0"), "tool_switcher_seg_sub");
    REQUIRE(sub != nullptr);
    CHECK_FALSE(lv_obj_has_flag(sub, LV_OBJ_FLAG_HIDDEN));

    // Cells on the touch floor are too short for two lines over the strip.
    const int floor = resolve_px("touch_target_min", 48);
    resize_content(h, 3 * floor + 2 * 2, floor);
    process_lvgl(30);
    REQUIRE(mode() == static_cast<int>(ToolSwitcherMode::RailWithPark));
    REQUIRE(cell_h() - tool_switcher_strip_px(cell_h()) < lines);
    CHECK(lv_subject_get_int(two_line) == 0);
    CHECK(lv_obj_has_flag(sub, LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "tool_switcher: a backend that cannot park gets no Park cell anywhere",
                 "[tool_switcher][1784]") {
    SECTION("a rail with room for Park is a rail of heads") {
        ToolChangerRig rig(4);
        rig.backend->can_park = false;
        PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
        process_lvgl(30);
        resize_content(h, 600, 60);
        process_lvgl(30);
        CHECK(mode() == static_cast<int>(ToolSwitcherMode::Rail));
        lv_obj_update_layout(h.root());
        for (int i = 0; i < 4; ++i) {
            CAPTURE(i);
            CHECK(shown(named(h, "tool_switcher_seg_" + std::to_string(i))));
            CHECK_FALSE(shown(named(h, "tool_switcher_rail_park_" + std::to_string(i))));
        }
        // The heads take the whole width: no cell is held back for Park.
        const int floor = resolve_px("touch_target_min", 48);
        lv_obj_t* last = named(h, "tool_switcher_seg_3");
        CHECK(lv_obj_get_x(last) + lv_obj_get_width(last) + floor > 600);
    }
    SECTION("a box for a face beside Park is the face alone") {
        ToolChangerRig rig(5);
        rig.backend->can_park = false;
        PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
        process_lvgl(30);
        resize_content(h, 214, 93);
        process_lvgl(30);
        CHECK(mode() == static_cast<int>(ToolSwitcherMode::Single));
        lv_obj_update_layout(h.root());
        CHECK_FALSE(shown(named(h, "tool_switcher_face_park")));
        CHECK(lv_obj_get_width(named(h, "tool_switcher_face")) == 214);
    }
    SECTION("a backend that learns to park re-decides the tile it already sized") {
        ToolChangerRig rig(4);
        rig.backend->can_park = false;
        PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
        process_lvgl(30);
        resize_content(h, 600, 60);
        process_lvgl(30);
        REQUIRE(mode() == static_cast<int>(ToolSwitcherMode::Rail));

        rig.backend->can_park = true;
        AmsState::instance().bump_slots_version();
        process_lvgl(30);
        CHECK(mode() == static_cast<int>(ToolSwitcherMode::RailWithPark));
        CHECK(shown(named(h, "tool_switcher_rail_park_3")));
    }
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "tool_switcher: a sheet row shows its head's temperature and colour strip",
                 "[tool_switcher][1784]") {
    ToolChangerRig rig(3);
    // Each head sources its own lane, which is where its colour comes from.
    ToolTopology topo;
    topo.tool_count = 3;
    topo.active_tool = 0;
    topo.tool_to_slot = {0, 1, 2};
    ToolState::instance().set_ams_topology(topo);
    state().temperature_state().init_extruders({"extruder", "extruder1", "extruder2"});
    process_lvgl(30);
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);
    state().update_from_status(nlohmann::json{{"extruder1", {{"temperature", 215.0}}}});
    process_lvgl(30);
    h.resize(2, 2, 160, 60);
    process_lvgl(30);

    ToolSwitcherTestAccess::show_picker(h.widget());
    lv_obj_t* list = ToolSwitcherTestAccess::picker_list(h.widget());
    REQUIRE(list != nullptr);
    REQUIRE(lv_obj_get_child_count(list) == 3);
    lv_obj_t* row1 = lv_obj_get_child(list, 1);
    lv_obj_t* temp = lv_obj_find_by_name(row1, "tool_btn_temp");
    REQUIRE(temp != nullptr);
    CHECK(std::string(lv_label_get_text(temp)) == "215°C");

    // The strip follows the head's own colour subject, shown exactly when the
    // head's filament colour is known.
    int known = 0;
    for (int i = 0; i < 3; ++i) {
        CAPTURE(i);
        const std::string base = "tool_switcher_seg_" + std::to_string(i);
        const int has_color =
            lv_subject_get_int(lv_xml_get_subject(nullptr, (base + "_has_color").c_str()));
        lv_obj_t* strip = lv_obj_find_by_name(lv_obj_get_child(list, i), "tool_btn_strip");
        REQUIRE(strip != nullptr);
        CHECK(lv_obj_has_flag(strip, LV_OBJ_FLAG_HIDDEN) == (has_color == 0));
        if (has_color) {
            ++known;
            CHECK((lv_color_to_u32(lv_obj_get_style_bg_color(strip, LV_PART_MAIN)) & 0xFFFFFF) ==
                  (lv_color_to_u32(lv_subject_get_color(
                       lv_xml_get_subject(nullptr, (base + "_color").c_str()))) &
                   0xFFFFFF));
        }
    }
    CHECK(known > 0);

    lv_obj_update_layout(list);
    for (uint32_t i = 0; i < 3; ++i) {
        CHECK(lv_obj_get_height(lv_obj_find_by_name(lv_obj_get_child(list, i), "tool_btn")) >=
              resolve_px("touch_target_min", 48));
    }
    ToolSwitcherTestAccess::hide_picker(h.widget());
    process_lvgl(30);
}

TEST_CASE_METHOD(LVGLUITestFixture, "tool_switcher: spools on the bed block every head and Park",
                 "[tool_switcher][1784]") {
    ToolChangerRig rig(4);
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);
    resize_content(h, 600, 60);
    process_lvgl(30);
    REQUIRE(mode() == static_cast<int>(ToolSwitcherMode::RailWithPark));
    lv_obj_t* seg = named(h, "tool_switcher_seg_1");
    lv_obj_t* park = named(h, "tool_switcher_rail_park_btn_3");
    REQUIRE(shown(named(h, "tool_switcher_rail_park_3")));
    REQUIRE_FALSE(lv_obj_has_state(seg, LV_STATE_DISABLED));
    REQUIRE_FALSE(lv_obj_has_state(park, LV_STATE_DISABLED));

    state().print_state().set_spool_latch(true);
    process_lvgl(30);
    CHECK(lv_obj_has_state(seg, LV_STATE_DISABLED));
    CHECK(lv_obj_has_state(park, LV_STATE_DISABLED));
    CHECK(lv_obj_has_state(named(h, "tool_switcher_face"), LV_STATE_DISABLED));

    // A tap that reaches them anyway is refused, and says why.
    tap(seg);
    tap(park);
    CHECK(rig.backend->change_tool_calls == 0);
    CHECK(rig.backend->park_calls == 0);
    REQUIRE(rig.warnings.size() == 2);
    for (const std::string& w : rig.warnings) {
        CHECK(w.find(lv_tr(spool_latch_message())) != std::string::npos);
    }

    // The sheet's head rows too.
    ToolSwitcherTestAccess::show_picker(h.widget());
    Sheet sheet = open_sheet(h.widget());
    REQUIRE_FALSE(sheet.rows.empty());
    for (lv_obj_t* row : sheet.rows) {
        CHECK(lv_obj_has_state(row, LV_STATE_DISABLED));
    }
    ToolSwitcherTestAccess::hide_picker(h.widget());
    process_lvgl(30);

    state().print_state().set_spool_latch(false);
    process_lvgl(30);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "tool_switcher: a paused print still lets a head change through its confirmation",
                 "[tool_switcher][1784]") {
    ToolChangerRig rig(4);
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);
    resize_content(h, 600, 60);
    process_lvgl(30);
    REQUIRE(mode() == static_cast<int>(ToolSwitcherMode::RailWithPark));
    lv_obj_t* seg = named(h, "tool_switcher_seg_1");
    lv_obj_t* park = named(h, "tool_switcher_rail_park_btn_3");
    helix::test::set_wire_state(state(), PrintJobState::PAUSED);
    process_lvgl(30);
    REQUIRE(ModalStack::instance().empty());

    SECTION("a head is offered, behind the paused-change confirmation") {
        CHECK_FALSE(lv_obj_has_state(seg, LV_STATE_DISABLED));
        CHECK_FALSE(lv_obj_has_state(named(h, "tool_switcher_face"), LV_STATE_DISABLED));
        tap(seg);
        process_lvgl(30);
        CHECK_FALSE(ModalStack::instance().empty());
        CHECK(rig.backend->change_tool_calls == 0);
        CHECK(rig.warnings.empty());
    }
    SECTION("Park is not: resuming with the head docked prints nothing") {
        CHECK(lv_obj_has_state(park, LV_STATE_DISABLED));
        tap(park);
        CHECK(rig.backend->park_calls == 0);
        REQUIRE(rig.warnings.size() == 1);
        CHECK(rig.warnings[0].find("Cannot park while a print is active") != std::string::npos);
    }
    SECTION("spools on the bed refuse the head change as well") {
        state().print_state().set_spool_latch(true);
        process_lvgl(30);
        CHECK(lv_obj_has_state(seg, LV_STATE_DISABLED));
        tap(seg);
        process_lvgl(30);
        CHECK(ModalStack::instance().empty());
        CHECK(rig.backend->change_tool_calls == 0);
        REQUIRE(rig.warnings.size() == 1);
        CHECK(rig.warnings[0].find(lv_tr(spool_latch_message())) != std::string::npos);
        state().print_state().set_spool_latch(false);
        process_lvgl(30);
    }
    helix::test::set_wire_state(state(), PrintJobState::STANDBY);
    process_lvgl(30);
}

namespace {

/// Each head sources its own lane, which is where its colour comes from.
void map_heads_to_lanes(int heads) {
    ToolTopology topo;
    topo.tool_count = heads;
    topo.active_tool = 0;
    for (int i = 0; i < heads; ++i) {
        topo.tool_to_slot.push_back(i);
    }
    ToolState::instance().set_ams_topology(topo);
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "tool_switcher: sheet rows past the per-head pools show no other head's data",
                 "[tool_switcher][1784]") {
    constexpr int kHeads = ToolSwitcherWidget::kPoolSize + 2;
    ToolChangerRig rig(kHeads);
    map_heads_to_lanes(kHeads);
    state().temperature_state().init_extruders({"extruder"});
    process_lvgl(30);
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);
    state().update_from_status(nlohmann::json{{"extruder", {{"temperature", 215.0}}}});
    process_lvgl(30);
    h.resize(2, 2, 160, 60);
    process_lvgl(30);

    ToolSwitcherTestAccess::show_picker(h.widget());
    lv_obj_t* list = ToolSwitcherTestAccess::picker_list(h.widget());
    REQUIRE(list != nullptr);
    REQUIRE(lv_obj_get_child_count(list) == static_cast<uint32_t>(kHeads));
    auto temp_of = [&](int row) {
        return std::string(
            lv_label_get_text(lv_obj_find_by_name(lv_obj_get_child(list, row), "tool_btn_temp")));
    };
    auto strip_of = [&](int row) {
        return lv_obj_find_by_name(lv_obj_get_child(list, row), "tool_btn_strip");
    };

    // Head 0 has a temperature and a colour, so a row that borrowed its
    // subjects would show them.
    REQUIRE(temp_of(0) == "215°C");
    REQUIRE(strip_of(0) != nullptr);
    REQUIRE_FALSE(lv_obj_has_flag(strip_of(0), LV_OBJ_FLAG_HIDDEN));
    for (int row = ToolSwitcherWidget::kPoolSize; row < kHeads; ++row) {
        CAPTURE(row);
        CHECK(temp_of(row).empty());
        REQUIRE(strip_of(row) != nullptr);
        CHECK(lv_obj_has_flag(strip_of(row), LV_OBJ_FLAG_HIDDEN));
    }
    ToolSwitcherTestAccess::hide_picker(h.widget());
    process_lvgl(30);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "tool_switcher: colour strips are about 9% of their cell and at least 3 px",
                 "[tool_switcher][1784]") {
    lv_display_t* disp = lv_display_get_default();
    for (const auto& [w, hpx] : {std::pair{480, 272}, std::pair{800, 480}}) {
        CAPTURE(w, hpx);
        ScopedResolution res(disp, w, hpx);
        theme_manager_refresh_layout_constants(disp);
        // The small panel's spacing tokens run below the 3 px floor, so no
        // spacing token sizes a strip.
        if (w == 480) {
            REQUIRE(resolve_px("space_xs", 4) < 3);
        }
        ToolChangerRig rig(4);
        map_heads_to_lanes(4);
        PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
        process_lvgl(30);
        auto check_strip = [](lv_obj_t* strip, int cell) {
            REQUIRE(strip != nullptr);
            REQUIRE_FALSE(lv_obj_has_flag(strip, LV_OBJ_FLAG_HIDDEN));
            const int px = lv_obj_get_height(strip);
            CAPTURE(cell, px);
            CHECK(px >= 3);
            CHECK(px == tool_switcher_strip_px(cell));
        };

        // A rail of tall segments, then one on the touch floor.
        for (const auto& [cw, ch] : {std::pair{214, 93}, std::pair{4 * 48 + 3 * 2, 48}}) {
            resize_content(h, cw, ch);
            process_lvgl(30);
            REQUIRE(mode() == static_cast<int>(ToolSwitcherMode::Rail));
            lv_obj_update_layout(h.root());
            lv_obj_t* seg = named(h, "tool_switcher_seg_1");
            check_strip(lv_obj_find_by_name(seg, "tool_switcher_seg_strip"),
                        lv_obj_get_height(seg));
        }

        // The face, alone in its box.
        resize_content(h, 90, 90);
        process_lvgl(30);
        REQUIRE(mode() == static_cast<int>(ToolSwitcherMode::Single));
        lv_obj_update_layout(h.root());
        lv_obj_t* face = named(h, "tool_switcher_face");
        check_strip(lv_obj_find_by_name(face, "tool_switcher_face_strip"), lv_obj_get_height(face));

        // A sheet row.
        ToolSwitcherTestAccess::show_picker(h.widget());
        lv_obj_t* list = ToolSwitcherTestAccess::picker_list(h.widget());
        REQUIRE(list != nullptr);
        lv_obj_update_layout(lv_obj_get_parent(list));
        lv_obj_t* row = lv_obj_find_by_name(lv_obj_get_child(list, 1), "tool_btn");
        check_strip(lv_obj_find_by_name(row, "tool_btn_strip"), lv_obj_get_height(row));
        ToolSwitcherTestAccess::hide_picker(h.widget());
        process_lvgl(30);
    }
    theme_manager_refresh_layout_constants(disp);
}
