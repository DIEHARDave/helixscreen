// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_widget_size_tool_switcher.cpp
 * @brief tool_switcher picks compact vs. pill layout — and, within pills, the
 * single-column-vs-row shape — from physical pixels, not colspan/rowspan.
 *
 * Unlike widgets whose on_size_changed() reads spans once and returns, three
 * separate sites read the granted size: on_size_changed() itself picks
 * compact vs. pills; rebuild_pills() (called again later from two different
 * observers) picks the vertical-column shape from the same size; and
 * on_active_tool_changed() re-derives which rebuild function to call, again
 * from the same cached size. The last two never receive a size argument —
 * they only see whatever on_size_changed() cached last time it ran. Each
 * test below drives one of the three consumers and pairs its target pixels
 * with a colspan/rowspan the *old* span-based predicate would resolve
 * differently, so an implementation that still reads spans fails here
 * instead of passing by coincidence.
 *
 * Tool data is seeded directly on the ToolState singleton via
 * ToolTopology/set_ams_topology() (mirrors nozzle_temps's use of
 * PrinterDiscovery+init_tools) — the mock `--test` printer backend never
 * exposes more than one tool, so multi-pill scenarios are unreachable by
 * driving the real app.
 */

#include "ui_tile_rung.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "../test_helpers/tool_switcher_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "grid_layout.h"
#include "panel_widget_size.h"
#include "printer_discovery.h"
#include "src/ui/panel_widgets/tool_switcher_widget.h"
#include "tool_state.h"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::widget_size;

namespace {

/// ToolState is a singleton outside LVGLUITestFixture's own init/deinit
/// chain, so a test that seeds it must clear it on the way out or later
/// test files in the same binary inherit stale tools (see
/// test_widget_size_nozzle_temps.cpp's NozzleTempsFixture, same trap).
struct ToolSwitcherFixture : public LVGLUITestFixture {
    ~ToolSwitcherFixture() override {
        ToolState::instance().deinit_subjects();
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }
};

/// Seeds ToolState with `count` generated tools ("T0".."Tn-1") and the given
/// active index via the AMS-topology path — the simplest way to get an
/// arbitrary tool count without going through PrinterDiscovery/JSON.
void configure_tools(int count, int active_index = 0) {
    ToolState& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);

    ToolTopology topo;
    topo.tool_count = count;
    topo.active_tool = active_index;
    ts.set_ams_topology(topo);
}

/// Re-applies the topology with a new tool_count/active_tool so a later
/// call only flips the ONE subject the test cares about (tool_count_ or
/// active_tool_), isolating which observer fires.
void update_tools(int count, int active_index) {
    ToolTopology topo;
    topo.tool_count = count;
    topo.active_tool = active_index;
    ToolState::instance().set_ams_topology(topo);
}

/// Mirrors tool_switcher_widget.cpp's (anonymous-namespace, unexported)
/// resolve_space_token() so the row-count thresholds below are derived from
/// whatever breakpoint tier is actually active in this test run, not a
/// hardcoded guess at "button_height_sm"/"space_xs"'s resolved pixel value.
int resolve_space_token(const char* name, int fallback) {
    const char* s = lv_xml_get_const(nullptr, name);
    return s ? std::atoi(s) : fallback;
}

} // namespace

/// Columns and rows the pill grid was built with.
static std::pair<int, int> pill_tracks(lv_obj_t* container) {
    return {
        helix::grid_count_tracks(lv_obj_get_style_grid_column_dsc_array(container, LV_PART_MAIN)),
        helix::grid_count_tracks(lv_obj_get_style_grid_row_dsc_array(container, LV_PART_MAIN))};
}

/// Whether @p pill wears a different face than the active pill: equal buttons,
/// one highlighted.
static bool looks_inactive(lv_obj_t* pill, lv_obj_t* active) {
    return !lv_color_eq(lv_obj_get_style_bg_color(pill, LV_PART_MAIN),
                        lv_obj_get_style_bg_color(active, LV_PART_MAIN));
}

TEST_CASE_METHOD(ToolSwitcherFixture,
                 "tool_switcher: pills show only where every pill fits legibly, in the "
                 "squarest grid",
                 "[widget_size][tool_switcher]") {
    configure_tools(3);

    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    lv_obj_t* container = h.child("tool_switcher_container");
    lv_obj_t* compact = h.child("tool_switcher_compact");
    REQUIRE(container != nullptr);
    REQUIRE(compact != nullptr);

    // attach()'s observers each queue one rebuild on subscribe; drain them
    // before the first resize so they cannot re-derive a layout later and
    // mask a broken on_size_changed().
    process_lvgl(30);

    // Too small for three legible pills in any arrangement: compact. The span
    // contradicts the pixels, so a span-reading rule fails here.
    h.resize(8, 8, 60, 60);
    process_lvgl(30);
    CHECK_FALSE(lv_obj_has_flag(compact, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(container, LV_OBJ_FLAG_HIDDEN));

    // Wide and short: one row of three.
    h.resize(1, 1, 400, 60);
    process_lvgl(30);
    CHECK_FALSE(lv_obj_has_flag(container, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(compact, LV_OBJ_FLAG_HIDDEN));
    REQUIRE(lv_obj_get_child_count(container) == 3);
    CHECK(pill_tracks(container) == std::make_pair(3, 1));

    // Narrow and tall: one column.
    h.resize(1, 1, 90, 300);
    process_lvgl(30);
    REQUIRE(lv_obj_get_child_count(container) == 3);
    CHECK(pill_tracks(container) == std::make_pair(1, 3));

    // Square: a balanced grid of equal cells, not one long row. Big enough
    // that a 2x2 cell holds the full "Tool 1 (T0)" label, which the pills
    // prefer over a squarer grid of shorter ones.
    h.resize(1, 1, 300, 300);
    process_lvgl(30);
    REQUIRE(lv_obj_get_child_count(container) == 3);
    CHECK(pill_tracks(container) == std::make_pair(2, 2));
    lv_obj_update_layout(container);
    // Equal cells, to the pixel the grid's rounding allows.
    CHECK(std::abs(lv_obj_get_width(lv_obj_get_child(container, 0)) -
                   lv_obj_get_width(lv_obj_get_child(container, 1))) <= 1);

    // The pills fill their cells up to a large button's height, and the grid
    // they make sits centred in the tile.
    lv_area_t tile;
    lv_obj_get_coords(h.root(), &tile);
    lv_area_t box{INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
    for (uint32_t i = 0; i < lv_obj_get_child_count(container); ++i) {
        lv_area_t a;
        lv_obj_get_coords(lv_obj_get_child(container, i), &a);
        box = {std::min(box.x1, a.x1), std::min(box.y1, a.y1), std::max(box.x2, a.x2),
               std::max(box.y2, a.y2)};
    }
    CHECK(std::abs((box.x1 + box.x2) - (tile.x1 + tile.x2)) <= 4);
    CHECK(std::abs((box.y1 + box.y2) - (tile.y1 + tile.y2)) <= 4);
    CHECK(lv_obj_get_height(lv_obj_get_child(container, 0)) ==
          resolve_space_token("button_height_lg", 0));

    // fits_at follows the same measurement: a box with no legible pill
    // arrangement is sized by the compact form alone.
    CHECK(h.widget().fits_at(300, 300));
    CHECK_FALSE(h.widget().fits_at(4, 4));
}

TEST_CASE_METHOD(ToolSwitcherFixture,
                 "tool_switcher: on_active_tool_changed rebuilds in pill form using the "
                 "cached size, not a fresh span",
                 "[widget_size][tool_switcher]") {
    configure_tools(3, /*active_index=*/0);

    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    lv_obj_t* container = h.child("tool_switcher_container");
    REQUIRE(container != nullptr);

    // Drain attach()'s immediate-on-subscribe observer notifications (see
    // the longer comment in the compact/pill-row/pill-column test above)
    // before the first resize(), so it can't fire on a later process_lvgl()
    // and mask a broken on_size_changed().
    process_lvgl(30);

    // Grant wide pixels (pills, either row or grid sub-shape — that choice
    // is a separate real-geometry heuristic this test doesn't target).
    // on_active_tool_changed() never receives a size — it only ever sees
    // whatever this resize cached.
    h.resize(1, 1, w_wide(), h_tall() - 1);
    process_lvgl(30);
    REQUIRE(lv_obj_get_child_count(container) == 3);

    // T0 active: pill 0 highlighted, pill 1 not.
    CHECK(looks_inactive(lv_obj_get_child(container, 1), lv_obj_get_child(container, 0)));

    // Change the active tool WITHOUT any further on_size_changed() call —
    // exactly what a real touchscreen does (screens don't resize at
    // runtime). Only active_tool_ changes; tool_count_ stays at 3.
    update_tools(3, /*active_index=*/1);
    process_lvgl(30);

    // Still pill form (3 buttons) — on_active_tool_changed read the cached
    // wide/short size and picked rebuild_pills(), not rebuild_compact(). A
    // stale-span implementation (colspan/rowspan defaulting to 1x1) would
    // instead collapse this to the 2-child compact form.
    REQUIRE(lv_obj_get_child_count(container) == 3);
    CHECK(looks_inactive(lv_obj_get_child(container, 0), lv_obj_get_child(container, 1)));
}

TEST_CASE_METHOD(ToolSwitcherFixture,
                 "tool_switcher: tool_count observer rebuilds in pill form using the "
                 "cached size, not a fresh span",
                 "[widget_size][tool_switcher]") {
    configure_tools(1);

    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    lv_obj_t* container = h.child("tool_switcher_container");
    REQUIRE(container != nullptr);

    // Drain attach()'s immediate-on-subscribe observer notifications (see
    // the longer comment in the compact/pill-row/pill-column test above)
    // before the first resize().
    process_lvgl(30);

    // Grant pill-row pixels with a single tool present.
    h.resize(1, 1, w_wide(), h_tall() - 1);
    process_lvgl(30);
    REQUIRE(lv_obj_get_child_count(container) == 1);

    // Tool count jumps 1 -> 3 with no further on_size_changed() call. The
    // tool_count_ observer (tool_switcher_widget.cpp:66-76) fires and must
    // read the cached wide/short size to pick rebuild_pills(). A
    // stale-span implementation defaults to rebuild_compact(), which would
    // produce 2 children (icon + label) regardless of tool count — a
    // different, distinguishable number from the 3 pills expected here.
    update_tools(3, /*active_index=*/0);
    process_lvgl(30);

    REQUIRE(lv_obj_get_child_count(container) == 3);
    CHECK(looks_inactive(lv_obj_get_child(container, 1), lv_obj_get_child(container, 0)));
    CHECK(looks_inactive(lv_obj_get_child(container, 2), lv_obj_get_child(container, 0)));
}

TEST_CASE_METHOD(ToolSwitcherFixture,
                 "tool_switcher: the pill grid follows the granted size, not the container's "
                 "pre-grid box",
                 "[widget_size][tool_switcher]") {
    // PanelWidgetManager calls on_size_changed() before it activates the grid
    // (#983), so the container still reports the panel's whole content box. The
    // arrangement must come from the size the widget was granted.
    configure_tools(3);
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    lv_obj_t* container = h.child("tool_switcher_container");
    REQUIRE(container != nullptr);
    process_lvgl(30);

    lv_obj_set_size(h.root(), 400, 1000);
    lv_obj_update_layout(h.root());
    h.widget().on_size_changed(1, 1, 400, 60);
    process_lvgl(30);

    REQUIRE(lv_obj_get_child_count(container) == 3);
    CHECK(pill_tracks(container) == std::make_pair(3, 1));
}

TEST_CASE_METHOD(ToolSwitcherFixture, "tool_switcher: compact mode marks an unknown active tool",
                 "[widget_size][tool_switcher]") {
    // Klipper's toolchanger.tool_number is taken as reported, so a printer that
    // names a tool the discovery never listed leaves active_tool_index() past
    // the end of the tool list.
    ToolState& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);
    helix::PrinterDiscovery disc;
    nlohmann::json objects = {"toolchanger", "tool T0",   "tool T1",  "tool T2",
                              "extruder",    "extruder1", "extruder2"};
    disc.parse_objects(objects);
    ts.init_tools(disc);
    REQUIRE(ts.tool_count() == 3);

    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    REQUIRE(h.child("tool_switcher_container") != nullptr);
    process_lvgl(30);

    // Too small for any pill arrangement: the compact icon-plus-label form.
    h.resize(2, 2, 60, 60);
    process_lvgl(30);

    lv_obj_t* label = ToolSwitcherTestAccess::compact_label(h.widget());
    REQUIRE(label != nullptr);
    REQUIRE(std::string(lv_label_get_text(label)) == "T0");

    ts.update_from_status(nlohmann::json{{"toolchanger", {{"tool_number", 7}}}});
    process_lvgl(30);
    REQUIRE(ts.active_tool_index() == 7);

    label = ToolSwitcherTestAccess::compact_label(h.widget());
    REQUIRE(label != nullptr);
    CHECK(std::string(lv_label_get_text(label)) == "?");
}

TEST_CASE_METHOD(ToolSwitcherFixture,
                 "tool_switcher: the compact form draws in the faces its box earns",
                 "[widget_size][tool_switcher][tile]") {
    // The compact glyph and tool label follow the tile's rung like every other
    // sized tile, so a bigger compact box draws a bigger glyph and label.
    configure_tools(3);
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);
    lv_obj_t* icon = h.child("tool_switcher_compact_icon");
    lv_obj_t* label = h.child("tool_switcher_compact_label");
    REQUIRE(icon != nullptr);
    REQUIRE(label != nullptr);
    lv_subject_t* rung = lv_xml_get_subject(nullptr, "tool_switcher_tile_icon");
    REQUIRE(rung != nullptr);

    h.resize(2, 2, 60, 60);
    process_lvgl(30);
    const int small = lv_subject_get_int(rung);
    const lv_font_t* small_face = lv_obj_get_style_text_font(icon, LV_PART_MAIN);
    h.resize(2, 2, w_normal() - 1, h_tall() - 1);
    process_lvgl(30);
    const int large = lv_subject_get_int(rung);

    INFO("rung " << small << " at 60x60, " << large << " at the compact ceiling");
    REQUIRE(large > small);
    CHECK(lv_obj_get_style_text_font(icon, LV_PART_MAIN) != small_face);
    CHECK(lv_obj_get_style_text_font(label, LV_PART_MAIN) ==
          ui::tile_rung_face(ui::TileLadder::Value, large).font);
    CHECK(std::string(lv_label_get_text(label)) == "T0");
}

TEST_CASE_METHOD(ToolSwitcherFixture,
                 "tool_switcher: tools arriving after the last size change re-measure the tile",
                 "[widget_size][tool_switcher][tile]") {
    // The compact label is budgeted at the widest tool label. Tools discovered
    // after the tile was sized must re-budget it, or the tile keeps a rung sized
    // for labels it no longer draws.
    // Narrow and tall, so the label's width is what limits the rung.
    auto rung_for = [&](int tools, int px) {
        configure_tools(tools);
        PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
        process_lvgl(30);
        h.resize(1, 1, px, 60);
        process_lvgl(30);
        return lv_subject_get_int(lv_xml_get_subject(nullptr, "tool_switcher_tile_icon"));
    };
    // A box where one short label and a dozen tools' widest label pick
    // different rungs; its existence is the premise, the size is this tier's.
    int px = -1;
    for (int p = 20; p <= 160 && px < 0; ++p) {
        if (rung_for(1, p) != rung_for(12, p)) {
            px = p;
        }
    }
    INFO("no box where one tool and twelve pick different compact rungs");
    REQUIRE(px > 0);
    const int expected = rung_for(12, px);

    // Sized with one tool, then eleven more arrive with no size change.
    configure_tools(1);
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);
    h.resize(1, 1, px, 60);
    process_lvgl(30);
    update_tools(12, 0);
    process_lvgl(30);
    CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "tool_switcher_tile_icon")) == expected);
}

// ============================================================================
// #1769: a generated tool name reads "Tool 1 (T0)" where there is room, "T0"
// where there is not
// ============================================================================

namespace {

/// The text a pill draws: ui_button's label child.
std::string pill_text(lv_obj_t* pill) {
    for (uint32_t i = 0; i < lv_obj_get_child_count(pill); ++i) {
        lv_obj_t* child = lv_obj_get_child(pill, static_cast<int32_t>(i));
        if (lv_obj_check_type(child, &lv_label_class)) {
            return lv_label_get_text(child);
        }
    }
    return {};
}

/// The label a picker row shows: whichever of its two forms is not hidden.
std::string picker_row_text(lv_obj_t* row) {
    for (const char* name : {"tool_btn_label", "tool_btn_code_label"}) {
        lv_obj_t* label = lv_obj_find_by_name(row, name);
        if (label && !lv_obj_has_flag(label, LV_OBJ_FLAG_HIDDEN)) {
            return lv_label_get_text(label);
        }
    }
    return {};
}

} // namespace

TEST_CASE_METHOD(ToolSwitcherFixture,
                 "tool_switcher: pills carry the G-code name beside the number when they fit",
                 "[widget_size][tool_switcher][1769]") {
    configure_tools(3);
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    lv_obj_t* container = h.child("tool_switcher_container");
    REQUIRE(container != nullptr);
    process_lvgl(30);

    // A wide row: every pill holds "Tool N (T<N-1>)".
    h.resize(1, 1, 800, 60);
    process_lvgl(30);
    REQUIRE(lv_obj_get_child_count(container) == 3);
    CHECK(pill_text(lv_obj_get_child(container, 0)) == "Tool 1 (T0)");
    CHECK(pill_text(lv_obj_get_child(container, 2)) == "Tool 3 (T2)");

    // Narrow and tall: a column of pills too narrow for the full form reads
    // in the G-code name alone, every pill in the same form.
    h.resize(1, 1, 90, 300);
    process_lvgl(30);
    REQUIRE(lv_obj_get_child_count(container) == 3);
    CHECK(pill_text(lv_obj_get_child(container, 0)) == "T0");
    CHECK(pill_text(lv_obj_get_child(container, 1)) == "T1");
    CHECK(pill_text(lv_obj_get_child(container, 2)) == "T2");
}

TEST_CASE_METHOD(ToolSwitcherFixture, "tool_switcher: the compact readout names the G-code tool",
                 "[widget_size][tool_switcher][1769]") {
    // The compact form exists because there is no room, so it reads in the
    // shortest form: the T-number Klipper and the slicer use.
    configure_tools(3, /*active_index=*/1);
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);
    h.resize(2, 2, 60, 60);
    process_lvgl(30);

    lv_obj_t* label = ToolSwitcherTestAccess::compact_label(h.widget());
    REQUIRE(label != nullptr);
    CHECK(std::string(lv_label_get_text(label)) == "T1");
}

TEST_CASE_METHOD(ToolSwitcherFixture,
                 "tool_switcher: a configured tool name keeps its physical label",
                 "[widget_size][tool_switcher][1769]") {
    ToolState& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(false);
    helix::PrinterDiscovery disc;
    nlohmann::json objects = {"toolchanger", "tool Left", "tool Right", "extruder", "extruder1"};
    disc.parse_objects(objects);
    ts.init_tools(disc);
    REQUIRE(ts.tool_count() == 2);
    REQUIRE(ts.tools()[0].name == "Left");

    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    lv_obj_t* container = h.child("tool_switcher_container");
    REQUIRE(container != nullptr);
    process_lvgl(30);

    h.resize(1, 1, 800, 60);
    process_lvgl(30);
    REQUIRE(lv_obj_get_child_count(container) == 2);
    CHECK(pill_text(lv_obj_get_child(container, 0)) == "Tool 1");

    h.resize(2, 2, 60, 60);
    process_lvgl(30);
    lv_obj_t* label = ToolSwitcherTestAccess::compact_label(h.widget());
    REQUIRE(label != nullptr);
    CHECK(std::string(lv_label_get_text(label)) == "Tool 1");
}

TEST_CASE_METHOD(ToolSwitcherFixture,
                 "tool_switcher: the picker rows read in the form the card has room for",
                 "[widget_size][tool_switcher][1769]") {
    configure_tools(3);
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);

    auto rows_after_opening_at = [&](int w, int hpx) {
        h.resize(1, 1, w, hpx);
        process_lvgl(30);
        REQUIRE(h.child("tool_switcher_compact") != nullptr);
        REQUIRE_FALSE(lv_obj_has_flag(h.child("tool_switcher_compact"), LV_OBJ_FLAG_HIDDEN));
        ToolSwitcherTestAccess::show_picker(h.widget());
        lv_obj_t* list = ToolSwitcherTestAccess::picker_list(h.widget());
        REQUIRE(list != nullptr);
        REQUIRE(lv_obj_get_child_count(list) == 3);
        std::vector<std::string> texts;
        for (uint32_t i = 0; i < 3; ++i) {
            texts.push_back(picker_row_text(lv_obj_get_child(list, static_cast<int32_t>(i))));
        }
        ToolSwitcherTestAccess::hide_picker(h.widget());
        process_lvgl(30);
        return texts;
    };

    // Too short for any pill, but wide: the card hangs off this tile at its
    // width, so the rows have room for the full form.
    CHECK(rows_after_opening_at(300, 30) ==
          std::vector<std::string>{"Tool 1 (T0)", "Tool 2 (T1)", "Tool 3 (T2)"});
    // A one-cell tile: the card is too narrow, so every row reads the G-code name.
    CHECK(rows_after_opening_at(60, 60) == std::vector<std::string>{"T0", "T1", "T2"});
}

// ============================================================================
// #1784: Park sits beside the tools only where the backend can dock one
// ============================================================================

namespace {

/// A tool-changer mock as the connected backend, for as long as it lives.
struct ParkingBackend {
    AmsBackendMock* backend = nullptr;
    explicit ParkingBackend(bool tool_changer) {
        auto owned = std::make_unique<AmsBackendMock>(3);
        owned->set_tool_changer_mode(tool_changer);
        owned->set_operation_delay(0);
        REQUIRE(owned->start());
        backend = owned.get();
        AmsState::instance().deinit_subjects();
        AmsState::instance().init_subjects(true);
        AmsState::instance().set_backend(std::move(owned));
    }
    ~ParkingBackend() {
        backend->wait_for_operation_thread();
        backend->stop();
        AmsState::instance().deinit_subjects();
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }
};

} // namespace

TEST_CASE_METHOD(ToolSwitcherFixture,
                 "tool_switcher: a backend that can park adds a Park pill after the tools",
                 "[widget_size][tool_switcher][1784]") {
    ParkingBackend parking(/*tool_changer=*/true);
    configure_tools(3);
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    lv_obj_t* container = h.child("tool_switcher_container");
    REQUIRE(container != nullptr);
    process_lvgl(30);

    h.resize(1, 1, 800, 60);
    process_lvgl(30);
    REQUIRE(lv_obj_get_child_count(container) == 4);
    lv_obj_t* park = lv_obj_get_child(container, 3);
    CHECK(std::string(lv_obj_get_name(park)) == "tool_park_pill");
    CHECK(pill_text(park) == "Park");
    // The Park cell is part of the grid the pills are measured into.
    CHECK(pill_tracks(container) == std::make_pair(4, 1));
}

TEST_CASE_METHOD(ToolSwitcherFixture, "tool_switcher: no Park pill where the backend cannot park",
                 "[widget_size][tool_switcher][1784]") {
    ParkingBackend parking(/*tool_changer=*/false);
    configure_tools(3);
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    lv_obj_t* container = h.child("tool_switcher_container");
    REQUIRE(container != nullptr);
    process_lvgl(30);

    h.resize(1, 1, 800, 60);
    process_lvgl(30);
    CHECK(lv_obj_get_child_count(container) == 3);
    CHECK(lv_obj_find_by_name(container, "tool_park_pill") == nullptr);
}

TEST_CASE_METHOD(ToolSwitcherFixture,
                 "tool_switcher: the picker shows Park only where the backend can park",
                 "[widget_size][tool_switcher][1784]") {
    const bool tool_changer = GENERATE(true, false);
    ParkingBackend parking(tool_changer);
    configure_tools(3);
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    process_lvgl(30);
    h.resize(2, 2, 60, 60);
    process_lvgl(30);

    ToolSwitcherTestAccess::show_picker(h.widget());
    lv_obj_t* list = ToolSwitcherTestAccess::picker_list(h.widget());
    REQUIRE(list != nullptr);
    lv_obj_t* park = lv_obj_find_by_name(lv_obj_get_parent(list), "tool_park_btn");
    REQUIRE(park != nullptr);
    CHECK(lv_obj_has_flag(park, LV_OBJ_FLAG_HIDDEN) == !tool_changer);
    ToolSwitcherTestAccess::hide_picker(h.widget());
    process_lvgl(30);
}

TEST_CASE_METHOD(ToolSwitcherFixture, "tool_switcher: Park is disabled with no tool mounted",
                 "[widget_size][tool_switcher][1784]") {
    ParkingBackend parking(/*tool_changer=*/true);
    // Registered in XML, as production registers it, so the pill's binding
    // to active_tool resolves.
    ToolState& ts = ToolState::instance();
    ts.deinit_subjects();
    ts.init_subjects(true);
    ToolTopology topo;
    topo.tool_count = 3;
    topo.active_tool = 0;
    topo.allows_empty_carriage = true;
    ts.set_ams_topology(topo);

    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    lv_obj_t* container = h.child("tool_switcher_container");
    REQUIRE(container != nullptr);
    process_lvgl(30);
    h.resize(1, 1, 800, 60);
    process_lvgl(30);

    lv_obj_t* park = lv_obj_find_by_name(container, "tool_park_pill");
    REQUIRE(park != nullptr);
    CHECK_FALSE(lv_obj_has_state(park, LV_STATE_DISABLED));

    topo.active_tool = -1;
    ts.set_ams_topology(topo);
    process_lvgl(30);
    park = lv_obj_find_by_name(container, "tool_park_pill");
    REQUIRE(park != nullptr);
    CHECK(lv_obj_has_state(park, LV_STATE_DISABLED));
}

TEST_CASE_METHOD(ToolSwitcherFixture,
                 "tool_switcher: Park never turns the pills into the compact readout",
                 "[widget_size][tool_switcher][1784]") {
    // A Park cell joins the pills only where there is room for it; the line
    // between pills and the compact form is the tools' alone.
    auto compact_map = [&](bool tool_changer) {
        ParkingBackend parking(tool_changer);
        configure_tools(4);
        std::vector<bool> compact;
        for (int w = 40; w <= 400; w += 10) {
            for (int hpx = 40; hpx <= 240; hpx += 10) {
                compact.push_back(ToolSwitcherTestAccess::is_compact_at(w, hpx));
            }
        }
        return compact;
    };
    const std::vector<bool> without_park = compact_map(false);
    const std::vector<bool> with_park = compact_map(true);
    REQUIRE(std::count(without_park.begin(), without_park.end(), false) > 0);
    CHECK(with_park == without_park);
}

TEST_CASE_METHOD(ToolSwitcherFixture,
                 "tool_switcher: every pill, Park included, gets a cell of its own at any size",
                 "[widget_size][tool_switcher][1784]") {
    // Where the tools fit but one more cell does not, Park is left out rather
    // than stacked into a cell another pill already holds.
    ParkingBackend parking(/*tool_changer=*/true);
    configure_tools(4);
    PanelWidgetHarness<ToolSwitcherWidget> h(test_screen(), state());
    lv_obj_t* container = h.child("tool_switcher_container");
    REQUIRE(container != nullptr);
    process_lvgl(30);

    int pill_sizes = 0;
    int without_park = 0;
    for (int w = 40; w <= 300; w += 20) {
        for (int hpx = 40; hpx <= 240; hpx += 20) {
            h.resize(1, 1, w, hpx);
            if (lv_obj_has_flag(container, LV_OBJ_FLAG_HIDDEN)) {
                continue;
            }
            ++pill_sizes;
            const auto [cols, rows] = pill_tracks(container);
            INFO(w << "x" << hpx << ": " << cols << "x" << rows << " for "
                   << lv_obj_get_child_count(container) << " pills");
            CHECK(cols * rows >= static_cast<int>(lv_obj_get_child_count(container)));
            if (lv_obj_find_by_name(container, "tool_park_pill") == nullptr) {
                ++without_park;
            }
        }
    }
    INFO(pill_sizes << " pill sizes, " << without_park << " without Park");
    REQUIRE(pill_sizes > 0);
}
