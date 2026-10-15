// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_progress_bar_sweep.cpp
 * @brief The estimate bar's highlight travels the whole track and back without
 *        leaving it, and its animation runs only while the bar can be seen.
 *
 * Run with: ./build/bin/helix-tests "[progress_bar]"
 */

#include "ui_progress_bar.h"

#include "../lvgl_ui_test_fixture.h"
#include "helix-xml/src/xml/lv_xml.h"

#include "../catch_amalgamated.hpp"

using helix::ui::kProgressSweepPeriodMs;
using helix::ui::progress_sweep_band;

TEST_CASE("the sweep band travels end to end and back inside the track", "[progress_bar]") {
    const int32_t x1 = 100;
    const int32_t w = 300;

    const auto start = progress_sweep_band(x1, w, 0);
    CHECK(start.x1 == x1);
    CHECK(start.x2 - start.x1 + 1 == w / 3);

    const auto turn = progress_sweep_band(x1, w, kProgressSweepPeriodMs / 2);
    CHECK(turn.x2 == x1 + w - 1);

    // One period later it starts over.
    CHECK(progress_sweep_band(x1, w, kProgressSweepPeriodMs).x1 == start.x1);

    int32_t last = start.x1;
    for (uint32_t t = 100; t <= kProgressSweepPeriodMs / 2; t += 100) {
        const auto band = progress_sweep_band(x1, w, t);
        CHECK(band.x1 > last);
        CHECK(band.x1 >= x1);
        CHECK(band.x2 <= x1 + w - 1);
        last = band.x1;
    }
    for (uint32_t t = kProgressSweepPeriodMs / 2 + 100; t < kProgressSweepPeriodMs; t += 100) {
        const auto band = progress_sweep_band(x1, w, t);
        CHECK(band.x1 < last);
        CHECK(band.x1 >= x1);
        last = band.x1;
    }
}

TEST_CASE("a narrow track still gets an 8px band, never wider than the track", "[progress_bar]") {
    const auto band = progress_sweep_band(0, 12, 0);
    CHECK(band.x2 - band.x1 + 1 == 8);
    const auto tiny = progress_sweep_band(0, 5, kProgressSweepPeriodMs / 2);
    CHECK(tiny.x1 == 0);
    CHECK(tiny.x2 == 4);
}

TEST_CASE_METHOD(LVGLUITestFixture, "only a visible estimate bar keeps an animation running",
                 "[progress_bar]") {
    const char* attrs[] = {"width", "200", nullptr};
    auto* estimate =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "progress_bar_estimate", attrs));
    auto* plain = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "progress_bar", attrs));
    REQUIRE(estimate != nullptr);
    REQUIRE(plain != nullptr);

    lv_refr_now(nullptr);
    // The sweep is keyed on the bar, so only the estimate bar owns one.
    CHECK(lv_anim_get(estimate, nullptr) != nullptr);
    CHECK(lv_anim_get(plain, nullptr) == nullptr);

    // At a non-zero estimate it still sweeps: the value may sit still.
    lv_bar_set_value(estimate, 40, LV_ANIM_OFF);
    lv_refr_now(nullptr);
    CHECK(lv_anim_get(estimate, nullptr) != nullptr);

    lv_obj_add_flag(estimate, LV_OBJ_FLAG_HIDDEN);
    process_lvgl(100);
    CHECK(lv_anim_get(estimate, nullptr) == nullptr);

    lv_obj_remove_flag(estimate, LV_OBJ_FLAG_HIDDEN);
    lv_refr_now(nullptr);
    CHECK(lv_anim_get(estimate, nullptr) != nullptr);
}
