// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "../lvgl_test_fixture.h"
#include "../test_helpers/screensaver_sim_kit.h"
#include "../test_helpers/screensaver_test_access.h"
#include "screensaver_pipes.h"

#include "../catch_amalgamated.hpp"

// ============================================================================
// Pipes canvas pixel format
//
// Pipes writes every pixel itself through PixelWriter, so its canvas takes the build's own
// depth (SAVER_BUILD_CANVAS_FORMAT). At 32 bpp that is XRGB8888, copied byte for byte into
// the ARGB8888 display, so every pixel's 4th byte must stay 0xFF or it draws transparent.
// ============================================================================

namespace {

helix::ui::FrameTarget pipes_canvas_frame(const PipesScreensaver& ss) {
    lv_obj_t* canvas = SaverTestAccess::canvas(ss);
    REQUIRE(canvas != nullptr);
    return helix::test::frame_of(lv_canvas_get_draw_buf(canvas));
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "the pipes canvas is the build's own depth",
                 "[screensaver][screensaver_canvas][pipes_canvas]") {
    PipesScreensaver ss;
    ScreensaverStopOnExit<PipesScreensaver> stop_on_exit{ss};
    ss.start();
    REQUIRE(ss.is_active());
    lv_obj_t* canvas = SaverTestAccess::canvas(ss);
    REQUIRE(canvas != nullptr);
    const lv_draw_buf_t* buf = lv_canvas_get_draw_buf(canvas);
    REQUIRE(buf != nullptr);
    CHECK(buf->header.cf == helix::ui::SAVER_BUILD_CANVAS_FORMAT);
}

TEST_CASE_METHOD(LVGLTestFixture, "every pipes canvas X byte stays 0xFF as pipes grow",
                 "[screensaver][screensaver_canvas][pipes_canvas]") {
    if (helix::ui::SAVER_BUILD_CANVAS_FORMAT != LV_COLOR_FORMAT_XRGB8888) {
        SKIP("16 bpp build: the canvas has no X byte");
    }
    PipesScreensaver ss;
    ScreensaverStopOnExit<PipesScreensaver> stop_on_exit{ss};
    SaverTestAccess::set_fixed_seed(ss, 21);
    ss.start();
    REQUIRE(ss.is_active());
    const helix::ui::FrameTarget frame = pipes_canvas_frame(ss);
    REQUIRE(helix::test::lit_pixels(frame) > 0);
    CHECK(helix::test::non_opaque_pixels(frame) == 0);

    for (int step = 0; step < 30; step++) {
        advance_saver(ss, 100);
    }
    REQUIRE(helix::test::lit_pixels(frame) > 0);
    CHECK(helix::test::non_opaque_pixels(frame) == 0);
}

#endif // HELIX_ENABLE_SCREENSAVER
