// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_widget_size_toolhead.cpp
 * @brief The toolhead widget draws what its ladder promises at each rung: which parts
 * carry HIDDEN, which text they show, where the plate puts the crosshair, and what Home
 * All and a tile tap do.
 *
 * Boxes are the pixel budgets measured on a live grid (55x54, 114x54, 55x113, 114x113 at
 * medium; 51x53 / 51x112 / 108x53 at large). The decision is toolhead_layout.h's; these
 * tests pin its wiring to the real fonts and XML.
 *
 * Run with: ./build/bin/helix-tests "[widget_size][toolhead]"
 */

#include "ui_nav_manager.h"
#include "ui_panel_motion.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "../test_helpers/scoped_breakpoint.h"
#include "../test_helpers/update_queue_test_access.h"
#include "app_globals.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "panel_widget_manager.h"
#include "printer_capabilities_state.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/toolhead_widget.h"
#include "static_panel_registry.h"
#include "tool_state.h"

#include <array>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using nlohmann::json;

namespace {

struct ToolheadFixture : public LVGLUITestFixture {
    ToolheadFixture() : mock(MoonrakerClientMock::PrinterType::VORON_24), mock_api(mock, state()) {
        StaticPanelRegistry::instance().destroy_all();
        state().set_klippy_state_sync(KlippyState::READY);
        mock.connect("ws://mock/websocket", []() {}, []() {});
        previous_api = get_moonraker_api();
        set_moonraker_api(&mock_api);

        ToolState::instance().deinit_subjects();
        ToolState::instance().init_subjects(true);
        PrinterDiscovery hw;
        hw.parse_objects(json::array({"gcode_move"}));
        ToolState::instance().init_tools(hw);

        PanelWidgetManager::instance().init_widget_subjects();
        reset_state();
        sentinel();
    }
    ~ToolheadFixture() override {
        StaticPanelRegistry::instance().destroy_all();
        reset_state();
        set_moonraker_api(previous_api);
        mock.stop_temperature_simulation();
        mock.disconnect();
        ToolState::instance().deinit_subjects();
        drain();
    }

    PrinterMotionState& motion() {
        return state().motion_state();
    }
    void reset_state() {
        set_print_active(false);
        set_homed("");
        set_velocity(0);
        set_position(0, 0, 0);
        set_live_position(0, 0, 0);
        drain();
    }
    void drain() {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }
    void set_print_active(bool on) {
        lv_subject_set_int(state().print_state().get_print_active_subject(), on ? 1 : 0);
        drain();
    }
    void set_homed(const char* axes) {
        lv_subject_copy_string(motion().get_homed_axes_subject(), axes);
        drain();
    }
    void set_velocity(int mm_s) {
        lv_subject_set_int(motion().get_live_velocity_subject(), mm_s);
        drain();
    }
    /// Centimillimetres, as the printer state stores them.
    void set_position(int x, int y, int z) {
        lv_subject_set_int(motion().get_position_x_subject(), x);
        lv_subject_set_int(motion().get_position_y_subject(), y);
        lv_subject_set_int(motion().get_position_z_subject(), z);
        drain();
    }
    void set_live_position(int x, int y, int z) {
        lv_subject_set_int(motion().get_live_position_x_subject(), x);
        lv_subject_set_int(motion().get_live_position_y_subject(), y);
        lv_subject_set_int(motion().get_live_position_z_subject(), z);
        drain();
    }
    /// Let the 4 Hz refresh come due.
    void settle() {
        process_lvgl(300);
        drain();
    }

    /// A gcode no action sends, so "nothing was sent" is distinguishable.
    void sentinel() {
        mock_api.execute_gcode("M117 sentinel", nullptr, nullptr);
        REQUIRE(last_sent() == "M117 sentinel");
    }
    const std::string& last_sent() const {
        return mock.last_send_script();
    }
    void tap(lv_obj_t* obj) {
        lv_obj_send_event(obj, LV_EVENT_CLICKED, nullptr);
    }

    MoonrakerClientMock mock;
    MoonrakerAPI mock_api;
    IMoonrakerAPI* previous_api = nullptr;
};

int subject_int(const char* name) {
    lv_subject_t* subject = lv_xml_get_subject(nullptr, name);
    REQUIRE(subject != nullptr);
    return lv_subject_get_int(subject);
}

std::string subject_text(const char* name) {
    lv_subject_t* subject = lv_xml_get_subject(nullptr, name);
    REQUIRE(subject != nullptr);
    return lv_subject_get_string(subject);
}

int rung_subject() {
    return subject_int("toolhead_rung");
}

lv_obj_t* part(PanelWidgetHarness<ToolheadWidget>& h, const char* name) {
    lv_obj_t* obj = h.child(name);
    REQUIRE(obj != nullptr);
    return obj;
}

bool hidden(lv_obj_t* obj) {
    return lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

/// Whether an object draws: it and every ancestor up to the widget root is unhidden.
bool shown(lv_obj_t* obj) {
    for (lv_obj_t* o = obj; o != nullptr; o = lv_obj_get_parent(o)) {
        if (hidden(o))
            return false;
    }
    return true;
}

struct Box {
    int w;
    int h;
    SizeRung rung;
};

constexpr Box kWide{234, 113, SizeRung::Wide};
constexpr Box kFull{234, 232, SizeRung::Full};

} // namespace

TEST_CASE_METHOD(ToolheadFixture, "toolhead: medium-tier budgets land on their rung",
                 "[widget_size][toolhead]") {
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    const Box boxes[] = {
        {55, 54, SizeRung::Glance},
        {114, 54, SizeRung::Strip},
        {55, 113, SizeRung::Column},
        {114, 113, SizeRung::Cell},
        kWide,
        kFull,
    };
    for (const auto& b : boxes) {
        INFO(b.w << "x" << b.h);
        h.resize(1, 1, b.w, b.h);
        CHECK(rung_subject() == static_cast<int>(b.rung));
    }
}

TEST_CASE_METHOD(ToolheadFixture, "toolhead: large-tier half cells land on their rung",
                 "[widget_size][toolhead]") {
    helix::test::ScopedBreakpoint bp(UiBreakpoint::Large);
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    const Box boxes[] = {
        {51, 53, SizeRung::Glance},
        {108, 53, SizeRung::Strip},
        {51, 112, SizeRung::Column},
    };
    for (const auto& b : boxes) {
        INFO(b.w << "x" << b.h);
        h.resize(1, 1, b.w, b.h);
        CHECK(rung_subject() == static_cast<int>(b.rung));
    }
}

TEST_CASE_METHOD(ToolheadFixture, "toolhead: each rung shows exactly its parts",
                 "[widget_size][toolhead]") {
    set_homed("xyz");
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());

    h.resize(1, 1, 55, 54);
    CHECK(shown(part(h, "toolhead_letters")));
    CHECK_FALSE(shown(part(h, "toolhead_zonly")));
    CHECK_FALSE(shown(part(h, "toolhead_rows")));
    CHECK_FALSE(shown(part(h, "toolhead_map")));

    h.resize(1, 1, 55, 113);
    CHECK_FALSE(shown(part(h, "toolhead_letters")));
    CHECK(shown(part(h, "toolhead_rows")));
    CHECK_FALSE(shown(part(h, "toolhead_row_x_dot")));
    CHECK_FALSE(shown(part(h, "toolhead_map")));

    h.resize(1, 1, 114, 113);
    CHECK(shown(part(h, "toolhead_rows")));
    CHECK(shown(part(h, "toolhead_row_x_dot")));
    CHECK(shown(part(h, "toolhead_row_z_dot")));
    CHECK_FALSE(shown(part(h, "toolhead_map")));

    h.resize(1, 1, kWide.w, kWide.h);
    CHECK_FALSE(shown(part(h, "toolhead_rows")));
    CHECK(shown(part(h, "toolhead_plate")));
    CHECK(shown(part(h, "toolhead_values")));
    CHECK_FALSE(shown(part(h, "toolhead_footer_home")));
    CHECK_FALSE(shown(part(h, "toolhead_footer_status")));

    h.resize(1, 1, kFull.w, kFull.h);
    CHECK(shown(part(h, "toolhead_plate")));
    CHECK(shown(part(h, "toolhead_values")));
    // Homed and idle: nothing to add under the values.
    CHECK_FALSE(shown(part(h, "toolhead_footer_home")));
    CHECK_FALSE(shown(part(h, "toolhead_footer_status")));
}

TEST_CASE_METHOD(ToolheadFixture,
                 "toolhead: Glance and Strip swap the letters for Z while printing",
                 "[widget_size][toolhead]") {
    set_homed("xyz");
    set_position(10000, 10000, 1234);
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());

    for (const auto& b : {Box{55, 54, SizeRung::Glance}, Box{114, 54, SizeRung::Strip}}) {
        INFO(b.w << "x" << b.h);
        h.resize(1, 1, b.w, b.h);
        CHECK(shown(part(h, "toolhead_letters")));
        CHECK_FALSE(shown(part(h, "toolhead_zonly")));

        set_print_active(true);
        CHECK_FALSE(shown(part(h, "toolhead_letters")));
        CHECK(shown(part(h, "toolhead_zonly")));
        settle();
        CHECK(subject_text("toolhead_z_text") == "12.34");

        set_print_active(false);
    }
}

TEST_CASE_METHOD(ToolheadFixture,
                 "toolhead: an unhomed axis reads a dash, a homed one its position",
                 "[widget_size][toolhead]") {
    set_position(12750, -1000, 2000);
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    h.resize(1, 1, 114, 113);
    settle();
    CHECK(subject_text("toolhead_x_text") == "--");
    CHECK(subject_text("toolhead_y_text") == "--");
    CHECK(subject_text("toolhead_z_text") == "--");
    CHECK(subject_int("toolhead_homed_x") == 0);

    set_homed("xy");
    settle();
    CHECK(subject_text("toolhead_x_text") == "127.5");
    CHECK(subject_text("toolhead_y_text") == "-10.0");
    CHECK(subject_text("toolhead_z_text") == "--");
    CHECK(subject_int("toolhead_homed_x") == 1);
    CHECK(subject_int("toolhead_homed_y") == 1);
    CHECK(subject_int("toolhead_homed_z") == 0);

    set_homed("xyz");
    settle();
    CHECK(subject_text("toolhead_z_text") == "20.00");
}

TEST_CASE_METHOD(ToolheadFixture, "toolhead: the live position leads only while the toolhead moves",
                 "[widget_size][toolhead]") {
    set_homed("xyz");
    set_position(10000, 10000, 1000);
    set_live_position(10500, 10500, 1500);
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    h.resize(1, 1, 114, 113);
    settle();
    CHECK(subject_text("toolhead_x_text") == "100.0");

    set_velocity(80);
    settle();
    CHECK(subject_text("toolhead_x_text") == "105.0");
    CHECK(subject_text("toolhead_z_text") == "15.00");

    set_velocity(0);
    settle();
    CHECK(subject_text("toolhead_x_text") == "100.0");
}

TEST_CASE_METHOD(ToolheadFixture, "toolhead: position updates are coalesced to the refresh period",
                 "[widget_size][toolhead]") {
    set_homed("xyz");
    set_position(10000, 10000, 1000);
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    h.resize(1, 1, 114, 113);
    settle();
    REQUIRE(subject_text("toolhead_x_text") == "100.0");

    for (int x = 10100; x <= 10500; x += 100) {
        set_position(x, 10000, 1000);
        process_lvgl(10);
        drain();
    }
    // Still inside the period the first update armed: nothing republished yet.
    CHECK(subject_text("toolhead_x_text") == "100.0");
    settle();
    CHECK(subject_text("toolhead_x_text") == "105.0");
}

TEST_CASE_METHOD(ToolheadFixture, "toolhead: a rung that shows no positions does no work per tick",
                 "[widget_size][toolhead]") {
    set_homed("xyz");
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    h.resize(1, 1, 55, 54);
    settle();
    const std::string before = subject_text("toolhead_x_text");
    set_position(12345, 12345, 1234);
    settle();
    CHECK(subject_text("toolhead_x_text") == before);
    CHECK_FALSE(h.widget().plate_geometry().has_value());
}

TEST_CASE_METHOD(ToolheadFixture, "toolhead: homed letters go green, unhomed red",
                 "[widget_size][toolhead]") {
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    h.resize(1, 1, 55, 54);
    const lv_color_t ok = theme_manager_get_color("success");
    const lv_color_t bad = theme_manager_get_color("danger");
    const auto color = [&](const char* name) {
        return lv_obj_get_style_text_color(part(h, name), LV_PART_MAIN);
    };

    CHECK(lv_color_eq(color("toolhead_letter_x"), bad));
    set_homed("xy");
    CHECK(lv_color_eq(color("toolhead_letter_x"), ok));
    CHECK(lv_color_eq(color("toolhead_letter_y"), ok));
    CHECK(lv_color_eq(color("toolhead_letter_z"), bad));
}

TEST_CASE_METHOD(ToolheadFixture, "toolhead: content sits vertically centred at the small rungs",
                 "[widget_size][toolhead]") {
    set_homed("xyz");
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    struct Case {
        Box box;
        const char* section;
    };
    const Case cases[] = {{{55, 54, SizeRung::Glance}, "toolhead_letters"},
                          {{114, 54, SizeRung::Strip}, "toolhead_letters"},
                          {{55, 113, SizeRung::Column}, "toolhead_rows"},
                          {{114, 113, SizeRung::Cell}, "toolhead_rows"}};
    for (const auto& c : cases) {
        INFO(c.box.w << "x" << c.box.h << " " << c.section);
        h.resize(1, 1, c.box.w, c.box.h);
        lv_obj_t* section = part(h, c.section);
        lv_area_t box;
        lv_area_t content;
        lv_obj_get_coords(h.root(), &box);
        lv_obj_get_coords(section, &content);
        const int above = content.y1 - box.y1;
        const int below = box.y2 - content.y2;
        CHECK(std::abs(above - below) <= 2);
    }
}

TEST_CASE_METHOD(ToolheadFixture, "toolhead: Home All shows at Full, idle, unhomed, and sends G28",
                 "[widget_size][toolhead][actions]") {
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    h.resize(1, 1, kFull.w, kFull.h);
    CHECK(shown(part(h, "toolhead_footer_home")));
    CHECK(shown(part(h, "toolhead_home_all")));
    CHECK(subject_int("toolhead_show_home") == 1);

    tap(part(h, "toolhead_home_all"));
    CHECK(last_sent() == "G28");
}

TEST_CASE_METHOD(ToolheadFixture, "toolhead: Home All is gone once homed and while printing",
                 "[widget_size][toolhead][actions]") {
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    h.resize(1, 1, kFull.w, kFull.h);
    REQUIRE(shown(part(h, "toolhead_home_all")));

    set_homed("xyz");
    CHECK_FALSE(shown(part(h, "toolhead_footer_home")));
    h.widget().home_all();
    CHECK(last_sent() == "M117 sentinel");

    set_homed("xy");
    REQUIRE(shown(part(h, "toolhead_home_all")));
    set_print_active(true);
    CHECK_FALSE(shown(part(h, "toolhead_footer_home")));
    h.widget().home_all();
    CHECK(last_sent() == "M117 sentinel");
}

TEST_CASE_METHOD(ToolheadFixture, "toolhead: below Full there is no Home All to tap",
                 "[widget_size][toolhead][actions]") {
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    h.resize(1, 1, kWide.w, kWide.h);
    CHECK_FALSE(shown(part(h, "toolhead_footer_home")));
    h.widget().home_all();
    CHECK(last_sent() == "M117 sentinel");
}

TEST_CASE_METHOD(ToolheadFixture, "toolhead: printing at Full shows the tool and the live speed",
                 "[widget_size][toolhead]") {
    set_homed("xyz");
    set_print_active(true);
    set_velocity(120);
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    h.resize(1, 1, kFull.w, kFull.h);
    settle();
    CHECK(shown(part(h, "toolhead_footer_status")));
    CHECK_FALSE(shown(part(h, "toolhead_footer_home")));
    CHECK(subject_text("toolhead_speed_text") == "120 mm/s");
    // A single-tool printer has no tool to name.
    CHECK(subject_text("toolhead_tool_text").empty());

    set_velocity(95);
    settle();
    CHECK(subject_text("toolhead_speed_text") == "95 mm/s");
}

TEST_CASE_METHOD(ToolheadFixture, "toolhead: a tool changer names its active tool at Full",
                 "[widget_size][toolhead]") {
    PrinterDiscovery hw;
    hw.parse_objects(json::array(
        {"gcode_move", "toolhead", "extruder", "extruder1", "toolchanger", "tool T0", "tool T1"}));
    state().set_hardware(hw);
    ToolState::instance().init_tools(hw);
    drain();
    set_homed("xyz");
    set_print_active(true);
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    h.resize(1, 1, kFull.w, kFull.h);
    settle();
    CHECK_FALSE(subject_text("toolhead_tool_text").empty());
}

TEST_CASE_METHOD(ToolheadFixture, "toolhead: the crosshair follows the position on the plate",
                 "[widget_size][toolhead]") {
    set_homed("xyz");
    state().update_from_status(
        {{"toolhead", {{"axis_minimum", {0, 0, 0, 0}}, {"axis_maximum", {250, 250, 200, 0}}}}});
    drain();
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    h.resize(1, 1, kWide.w, kWide.h);
    settle();

    set_position(0, 0, 0);
    settle();
    auto g = h.widget().plate_geometry();
    REQUIRE(g.has_value());
    REQUIRE(g->show_crosshair);
    const float x0 = g->cross_x;
    const float y0 = g->cross_y;

    set_position(10000, 10000, 5000);
    settle();
    g = h.widget().plate_geometry();
    REQUIRE(g.has_value());
    CHECK(g->cross_x > x0);
    CHECK(g->cross_y < y0);
    CHECK(g->z_fraction > 0.0f);

    set_homed("");
    settle();
    g = h.widget().plate_geometry();
    REQUIRE(g.has_value());
    CHECK_FALSE(g->show_crosshair);
}

TEST_CASE_METHOD(ToolheadFixture, "toolhead: a bed that moves in Z hangs the bar from the top",
                 "[widget_size][toolhead]") {
    set_homed("xyz");
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    h.resize(1, 1, kWide.w, kWide.h);
    settle();
    REQUIRE(h.widget().plate_geometry().has_value());
    CHECK_FALSE(h.widget().plate_geometry()->fill_from_top);

    lv_subject_set_int(state().capabilities_state().subject(Capability::BedMoves), 1);
    settle();
    CHECK(h.widget().plate_geometry()->fill_from_top);
    lv_subject_set_int(state().capabilities_state().subject(Capability::BedMoves), 0);
    drain();
}

TEST_CASE_METHOD(ToolheadFixture, "toolhead: Home All does not open Motion, a tile tap does",
                 "[widget_size][toolhead][actions]") {
    std::array<lv_obj_t*, 7> panels{};
    for (auto& p : panels)
        p = lv_obj_create(lv_screen_active());
    NavigationManager::instance().set_panels(panels.data());

    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    h.resize(1, 1, kFull.w, kFull.h);

    tap(part(h, "toolhead_home_all"));
    process_lvgl(50);
    CHECK_FALSE(get_global_motion_panel().is_visible());

    tap(h.root());
    process_lvgl(500);
    CHECK(get_global_motion_panel().is_visible());
}

TEST_CASE_METHOD(ToolheadFixture,
                 "toolhead: a transient instance does not publish over the live tile",
                 "[widget_size][toolhead]") {
    PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
    h.resize(1, 1, kFull.w, kFull.h);
    REQUIRE(rung_subject() == static_cast<int>(SizeRung::Full));

    ToolheadWidget transient(state());
    transient.notify_size_changed(1, 1, 55, 54);
    CHECK(rung_subject() == static_cast<int>(SizeRung::Full));
}

TEST_CASE_METHOD(ToolheadFixture, "toolhead: a transient instance leaving hands the tile back",
                 "[widget_size][toolhead][actions]") {
    PanelWidgetHarness<ToolheadWidget> live(test_screen(), state());
    live.resize(1, 1, kFull.w, kFull.h);
    {
        PanelWidgetHarness<ToolheadWidget> transient(test_screen(), state());
        transient.resize(1, 1, 55, 54);
        REQUIRE(rung_subject() == static_cast<int>(SizeRung::Glance));
        // The displaced tile does not act while the transient owns the subjects.
        live.widget().home_all();
        CHECK(last_sent() == "M117 sentinel");
    }

    CHECK(rung_subject() == static_cast<int>(SizeRung::Full));
    live.widget().home_all();
    CHECK(last_sent() == "G28");
}

TEST_CASE_METHOD(ToolheadFixture, "toolhead: fits_at refuses what Glance cannot draw",
                 "[widget_size][toolhead]") {
    ToolheadWidget widget(state());
    CHECK(widget.fits_at(55, 54));
    CHECK(widget.fits_at(55, 113));
    CHECK(widget.fits_at(114, 54));
    CHECK_FALSE(widget.fits_at(20, 20));
    {
        helix::test::ScopedBreakpoint bp(UiBreakpoint::Large);
        CHECK(widget.fits_at(51, 53));
        CHECK(widget.fits_at(51, 112));
        CHECK(widget.fits_at(108, 53));
    }
    {
        helix::test::ScopedBreakpoint bp(UiBreakpoint::Micro);
        CHECK(widget.fits_at(70, 64));
    }
}

TEST_CASE_METHOD(ToolheadFixture, "toolhead: the default 2x2 span lands on its rung at each tier",
                 "[widget_size][toolhead]") {
    // The plate and its values need more room than the small tiers' 2x2 holds, so they
    // draw the smaller rung their pixels allow rather than clip.
    struct Tier {
        UiBreakpoint bp;
        int w;
        int h;
        SizeRung rung;
    };
    const Tier tiers[] = {
        {UiBreakpoint::Micro, 142, 131, SizeRung::Cell},
        {UiBreakpoint::Tiny, 166, 155, SizeRung::Wide},
        {UiBreakpoint::Small, 163, 152, SizeRung::Wide},
        {UiBreakpoint::Medium, 233, 230, SizeRung::Full},
        {UiBreakpoint::Large, 221, 230, SizeRung::Full},
        {UiBreakpoint::XLarge, 276, 275, SizeRung::Full},
    };
    for (const auto& t : tiers) {
        INFO(static_cast<int>(t.bp) << " " << t.w << "x" << t.h);
        helix::test::ScopedBreakpoint bp(t.bp);
        PanelWidgetHarness<ToolheadWidget> h(test_screen(), state());
        h.resize(1, 1, t.w, t.h);
        CHECK(rung_subject() == static_cast<int>(t.rung));
    }
}
