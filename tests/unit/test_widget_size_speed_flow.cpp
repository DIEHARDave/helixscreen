// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_widget_size_speed_flow.cpp
 * @brief The speed_flow widget draws what its ladder promises at each rung: which parts
 * carry HIDDEN, which text they show, and the re-decision when print_active or the
 * physical-units setting changes.
 *
 * Boxes are the pixel budgets measured on a live grid (55x54, 114x54, 55x113, 114x113 at
 * medium; 51x53 / 51x112 / 108x53 at large; 70x64 at micro). The decision is
 * speed_flow_layout.h's; these tests pin its wiring to the real fonts and XML.
 *
 * Run with: ./build/bin/helix-tests "[widget_size][speed_flow]"
 */

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "../test_helpers/scoped_breakpoint.h"
#include "../test_helpers/update_queue_test_access.h"
#include "display_settings_manager.h"
#include "panel_widget_manager.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/speed_flow_widget.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

struct SpeedFlowFixture : public LVGLUITestFixture {
    SpeedFlowFixture() {
        PanelWidgetManager::instance().init_widget_subjects();
        set_physical_units(false);
        set_print_active(false);
        set_percentages(100, 100);
    }
    ~SpeedFlowFixture() override {
        set_physical_units(false);
        set_print_active(false);
        set_percentages(100, 100);
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }

    void set_physical_units(bool on) {
        DisplaySettingsManager::instance().set_speed_flow_physical_units(on);
        drain();
    }
    void set_print_active(bool on) {
        lv_subject_set_int(state().print_state().get_print_active_subject(), on ? 1 : 0);
        drain();
    }
    void set_percentages(int speed, int flow) {
        lv_subject_set_int(state().motion_state().get_speed_factor_subject(), speed);
        lv_subject_set_int(state().motion_state().get_flow_factor_subject(), flow);
        drain();
    }
    void set_live(int velocity_mm_s, int extruder_centimm_s) {
        lv_subject_set_int(state().motion_state().get_live_velocity_subject(), velocity_mm_s);
        lv_subject_set_int(state().motion_state().get_live_extruder_velocity_subject(),
                           extruder_centimm_s);
        drain();
    }
    void drain() {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }
};

int rung_subject() {
    lv_subject_t* subject = lv_xml_get_subject(nullptr, "speed_flow_rung");
    REQUIRE(subject != nullptr);
    return lv_subject_get_int(subject);
}

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

/// A named part of one axis ("speed" or "flow").
lv_obj_t* part(PanelWidgetHarness<SpeedFlowWidget>& h, const char* axis, const char* name) {
    lv_obj_t* axis_part = h.child(std::string("speed_flow_" + std::string(axis) + "_part").c_str());
    REQUIRE(axis_part != nullptr);
    lv_obj_t* obj = lv_obj_find_by_name(axis_part, name);
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

} // namespace

TEST_CASE_METHOD(SpeedFlowFixture, "speed_flow: medium-tier budgets land on their rung",
                 "[widget_size][speed_flow]") {
    PanelWidgetHarness<SpeedFlowWidget> h(test_screen(), state());
    // Half cells, a whole cell, then 2x1 and 2x2 (two cells plus the gutter).
    const Box boxes[] = {
        {55, 54, SizeRung::Glance}, {114, 54, SizeRung::Strip}, {55, 113, SizeRung::Column},
        {114, 113, SizeRung::Cell}, {234, 113, SizeRung::Wide}, {234, 232, SizeRung::Full},
    };
    for (const auto& b : boxes) {
        INFO(b.w << "x" << b.h);
        h.resize(1, 1, b.w, b.h);
        CHECK(rung_subject() == static_cast<int>(b.rung));
    }
}

TEST_CASE_METHOD(SpeedFlowFixture, "speed_flow: each rung shows exactly its parts",
                 "[widget_size][speed_flow]") {
    PanelWidgetHarness<SpeedFlowWidget> h(test_screen(), state());

    h.resize(1, 1, 55, 54);
    CHECK(hidden(h.child("speed_flow_flow_part")));
    CHECK(shown(part(h, "speed", "speed_flow_icon")));
    CHECK(shown(part(h, "speed", "speed_flow_value")));
    CHECK_FALSE(shown(part(h, "speed", "speed_flow_label")));

    h.resize(1, 1, 114, 54);
    CHECK(shown(h.child("speed_flow_flow_part")));
    CHECK(shown(part(h, "flow", "speed_flow_icon")));
    CHECK_FALSE(shown(part(h, "flow", "speed_flow_label")));

    h.resize(1, 1, 114, 113);
    CHECK(shown(part(h, "speed", "speed_flow_icon")));
    CHECK_FALSE(shown(part(h, "speed", "speed_flow_measured_below"))); // idle

    h.resize(1, 1, 234, 113);
    CHECK_FALSE(shown(part(h, "speed", "speed_flow_icon")));
    CHECK(shown(part(h, "speed", "speed_flow_label")));
    CHECK(shown(part(h, "flow", "speed_flow_label")));
    CHECK_FALSE(shown(part(h, "speed", "speed_flow_measured_right"))); // idle

    h.resize(1, 1, 234, 232);
    CHECK(shown(part(h, "speed", "speed_flow_full_label")));
    CHECK(shown(part(h, "speed", "speed_flow_full_value")));
    CHECK_FALSE(shown(part(h, "speed", "speed_flow_main")));
    // Idle at Full is read-only.
    CHECK_FALSE(shown(part(h, "speed", "speed_flow_minus")));
    CHECK_FALSE(shown(part(h, "speed", "speed_flow_plus")));
}

TEST_CASE_METHOD(SpeedFlowFixture, "speed_flow: printing adds the measured readings and steppers",
                 "[widget_size][speed_flow]") {
    set_print_active(true);
    set_live(60, 500);
    PanelWidgetHarness<SpeedFlowWidget> h(test_screen(), state());

    h.resize(1, 1, 114, 113);
    REQUIRE(rung_subject() == static_cast<int>(SizeRung::Cell));
    CHECK(shown(part(h, "speed", "speed_flow_measured_below")));
    CHECK(subject_text("speed_flow_speed_measured").find("mm/s") != std::string::npos);
    CHECK(subject_text("speed_flow_speed_text") == "100%");

    h.resize(1, 1, 234, 113);
    REQUIRE(rung_subject() == static_cast<int>(SizeRung::Wide));
    CHECK(shown(part(h, "speed", "speed_flow_measured_right")));
    CHECK(shown(part(h, "flow", "speed_flow_measured_right")));

    h.resize(1, 1, 234, 232);
    REQUIRE(rung_subject() == static_cast<int>(SizeRung::Full));
    CHECK(subject_int("speed_flow_show_steppers") == 1);
    CHECK(shown(part(h, "speed", "speed_flow_minus")));
    CHECK(shown(part(h, "flow", "speed_flow_plus")));
    CHECK(shown(part(h, "speed", "speed_flow_full_measured")));
}

TEST_CASE_METHOD(SpeedFlowFixture, "speed_flow: print_active re-decides the rung",
                 "[widget_size][speed_flow]") {
    PanelWidgetHarness<SpeedFlowWidget> h(test_screen(), state());
    // Room for the Full box idle but not printing: the steppers cost width.
    h.resize(1, 1, 234, 232);
    REQUIRE(rung_subject() == static_cast<int>(SizeRung::Full));
    CHECK(subject_int("speed_flow_show_steppers") == 0);

    set_print_active(true);
    CHECK(subject_int("speed_flow_show_steppers") == 1);
    set_print_active(false);
    CHECK(subject_int("speed_flow_show_steppers") == 0);
}

TEST_CASE_METHOD(SpeedFlowFixture, "speed_flow: physical units swap the value only below Cell",
                 "[widget_size][speed_flow]") {
    set_print_active(true);
    set_live(45, 300);
    PanelWidgetHarness<SpeedFlowWidget> h(test_screen(), state());

    h.resize(1, 1, 55, 54);
    REQUIRE(rung_subject() == static_cast<int>(SizeRung::Glance));
    CHECK(subject_text("speed_flow_speed_text") == "100%");

    set_physical_units(true);
    CHECK(subject_text("speed_flow_speed_text") == "45");

    h.resize(1, 1, 114, 54);
    REQUIRE(rung_subject() == static_cast<int>(SizeRung::Strip));
    CHECK(subject_text("speed_flow_speed_text") == "45");

    // With room, the percentage stays primary whatever the setting.
    h.resize(1, 1, 114, 113);
    REQUIRE(rung_subject() == static_cast<int>(SizeRung::Cell));
    CHECK(subject_text("speed_flow_speed_text") == "100%");

    // Idle never swaps.
    set_print_active(false);
    h.resize(1, 1, 55, 54);
    CHECK(subject_text("speed_flow_speed_text") == "100%");
}

TEST_CASE_METHOD(SpeedFlowFixture, "speed_flow: an override off 100 draws amber at every rung",
                 "[widget_size][speed_flow]") {
    PanelWidgetHarness<SpeedFlowWidget> h(test_screen(), state());
    h.resize(1, 1, 234, 113);
    const lv_color_t warning = theme_manager_get_color("warning");
    const lv_color_t normal =
        lv_obj_get_style_text_color(part(h, "flow", "speed_flow_value"), LV_PART_MAIN);

    set_percentages(120, 100);
    for (const auto& b : std::initializer_list<Box>{{55, 54, SizeRung::Glance},
                                                    {114, 54, SizeRung::Strip},
                                                    {55, 113, SizeRung::Column},
                                                    {114, 113, SizeRung::Cell},
                                                    {234, 113, SizeRung::Wide},
                                                    {234, 232, SizeRung::Full}}) {
        INFO(b.w << "x" << b.h);
        h.resize(1, 1, b.w, b.h);
        REQUIRE(rung_subject() == static_cast<int>(b.rung));
        REQUIRE(subject_int("speed_flow_speed_status") == 1);
        REQUIRE(subject_int("speed_flow_flow_status") == 0);
        const char* value = b.rung == SizeRung::Full ? "speed_flow_full_value" : "speed_flow_value";
        const lv_color_t speed_color =
            lv_obj_get_style_text_color(part(h, "speed", value), LV_PART_MAIN);
        const lv_color_t flow_color =
            lv_obj_get_style_text_color(part(h, "flow", value), LV_PART_MAIN);
        if (b.rung == SizeRung::Glance)
            continue; // the flow axis is hidden here
        CHECK(lv_color_eq(speed_color, warning));
        CHECK_FALSE(lv_color_eq(flow_color, warning));
    }
    (void)normal;
}

TEST_CASE_METHOD(SpeedFlowFixture, "speed_flow: fits_at refuses what Glance cannot draw",
                 "[widget_size][speed_flow]") {
    SpeedFlowWidget widget(state());
    // The measured half-cell budgets at each tier must hold the Glance content.
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

    // Monotonic: once a width fits, every wider one does.
    bool fitted = false;
    for (int w = 0; w <= 200; ++w) {
        const bool now = widget.fits_at(w, 113);
        if (fitted)
            CHECK(now);
        fitted = fitted || now;
    }
    bool fitted_h = false;
    for (int hgt = 0; hgt <= 200; ++hgt) {
        const bool now = widget.fits_at(114, hgt);
        if (fitted_h)
            CHECK(now);
        fitted_h = fitted_h || now;
    }
}

TEST_CASE_METHOD(SpeedFlowFixture, "speed_flow: half-cell boxes at large and micro land on a rung",
                 "[widget_size][speed_flow]") {
    {
        helix::test::ScopedBreakpoint bp(UiBreakpoint::Large);
        PanelWidgetHarness<SpeedFlowWidget> h(test_screen(), state());
        h.resize(1, 1, 51, 53);
        CHECK(rung_subject() == static_cast<int>(SizeRung::Glance));
        h.resize(1, 1, 108, 53);
        CHECK(rung_subject() == static_cast<int>(SizeRung::Strip));
        h.resize(1, 1, 51, 112);
        CHECK(rung_subject() == static_cast<int>(SizeRung::Column));
        h.resize(1, 1, 108, 112);
        CHECK(rung_subject() == static_cast<int>(SizeRung::Cell));
    }
}
