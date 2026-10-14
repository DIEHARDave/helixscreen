// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/// The belt gap gauge: pure mapping and severity rules, plus the widget's
/// set() contract (no children, safe before layout).

#include "ui_belt_gap_gauge.h"

#include "../lvgl_test_fixture.h"
#include "belt_tune_session.h"
#include "helix-xml/src/xml/lv_xml.h"

#include "../catch_amalgamated.hpp"

using namespace helix::ui;
using helix::calibration::belt_tune::GAUGE_WARN_HZ;

TEST_CASE("belt gap gauge x mapping", "[belt][gauge]") {
    constexpr int TRACK = 201;
    constexpr float RANGE = 10.0f;

    SECTION("zero is centred") {
        CHECK(belt_gap_gauge_x(0.0f, RANGE, TRACK) == 100);
    }
    SECTION("plus and minus range reach the ends") {
        CHECK(belt_gap_gauge_x(RANGE, RANGE, TRACK) == TRACK - 1);
        CHECK(belt_gap_gauge_x(-RANGE, RANGE, TRACK) == 0);
    }
    SECTION("beyond the range clamps to the track") {
        CHECK(belt_gap_gauge_x(50.0f, RANGE, TRACK) == TRACK - 1);
        CHECK(belt_gap_gauge_x(-50.0f, RANGE, TRACK) == 0);
    }
    SECTION("half range sits half way to the end") {
        CHECK(belt_gap_gauge_x(5.0f, RANGE, TRACK) == 150);
        CHECK(belt_gap_gauge_x(-5.0f, RANGE, TRACK) == 50);
    }
    SECTION("degenerate inputs stay inside the track") {
        CHECK(belt_gap_gauge_x(3.0f, 0.0f, TRACK) == 100);
        CHECK(belt_gap_gauge_x(3.0f, RANGE, 0) == 0);
    }
}

TEST_CASE("belt gap severity bands", "[belt][gauge]") {
    constexpr float TOL = 1.0f;
    CHECK(belt_gap_severity(0.0f, TOL) == GapSeverity::MATCHED);
    CHECK(belt_gap_severity(TOL, TOL) == GapSeverity::MATCHED);
    CHECK(belt_gap_severity(-TOL, TOL) == GapSeverity::MATCHED);
    CHECK(belt_gap_severity(TOL + 0.01f, TOL) == GapSeverity::NEAR);
    CHECK(belt_gap_severity(GAUGE_WARN_HZ, TOL) == GapSeverity::NEAR);
    CHECK(belt_gap_severity(-GAUGE_WARN_HZ, TOL) == GapSeverity::NEAR);
    CHECK(belt_gap_severity(GAUGE_WARN_HZ + 0.01f, TOL) == GapSeverity::FAR);
    CHECK(belt_gap_severity(-8.0f, TOL) == GapSeverity::FAR);
    // A tolerance wider than the warning band still reads matched inside it.
    CHECK(belt_gap_severity(3.5f, 4.0f) == GapSeverity::MATCHED);
}

TEST_CASE_METHOD(LVGLTestFixture, "belt gap gauge set() is child-free and layout-safe",
                 "[belt][gauge]") {
    register_belt_gap_gauge_widget();
    lv_obj_t* gauge =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "belt_gap_gauge", nullptr));
    REQUIRE(gauge != nullptr);
    CHECK(lv_obj_get_child_count(gauge) == 0);

    ui_belt_gap_gauge_set(gauge, 2.5f, std::nullopt, 1.0f, 10.0f);
    CHECK(lv_obj_get_child_count(gauge) == 0);
    ui_belt_gap_gauge_set(gauge, -2.5f, 4.0f, 1.0f, 10.0f);
    CHECK(lv_obj_get_child_count(gauge) == 0);

    // Before layout the gauge has zero size; drawing must be a no-op.
    lv_obj_set_size(gauge, 0, 0);
    ui_belt_gap_gauge_set(gauge, 99.0f, -99.0f, 1.0f, 10.0f);
    lv_obj_invalidate(gauge);
    lv_refr_now(nullptr);

    lv_obj_set_size(gauge, 200, 40);
    lv_refr_now(nullptr);
    ui_belt_gap_gauge_set(nullptr, 1.0f, std::nullopt, 1.0f, 10.0f);
    CHECK(lv_obj_get_child_count(gauge) == 0);
}
