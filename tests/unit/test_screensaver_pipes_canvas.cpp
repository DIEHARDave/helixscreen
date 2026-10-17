// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "../lvgl_test_fixture.h"
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

void run_timer(lv_timer_t* timer) {
    REQUIRE(timer != nullptr);
    REQUIRE(timer->timer_cb != nullptr);
    timer->timer_cb(timer);
}

const lv_draw_buf_t* pipes_canvas_buf(const PipesScreensaver& ss) {
    lv_obj_t* canvas = SaverTestAccess::canvas(ss);
    REQUIRE(canvas != nullptr);
    const lv_draw_buf_t* buf = lv_canvas_get_draw_buf(canvas);
    REQUIRE(buf != nullptr);
    REQUIRE(lv_color_format_get_size(static_cast<lv_color_format_t>(buf->header.cf)) == 4);
    return buf;
}

struct PixelCounts {
    size_t lit = 0;        ///< pixels with any colour
    size_t not_opaque = 0; ///< pixels whose 4th byte is not 0xFF
};

PixelCounts count_pixels(const lv_draw_buf_t* buf) {
    PixelCounts counts;
    for (uint32_t y = 0; y < buf->header.h; y++) {
        const uint8_t* row = buf->data + y * buf->header.stride;
        for (uint32_t x = 0; x < buf->header.w; x++) {
            const uint8_t* px = row + x * 4;
            counts.lit += (px[0] | px[1] | px[2]) != 0 ? 1 : 0;
            counts.not_opaque += px[3] != 0xFF ? 1 : 0;
        }
    }
    return counts;
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
    const lv_draw_buf_t* buf = pipes_canvas_buf(ss);
    PixelCounts counts = count_pixels(buf);
    REQUIRE(counts.lit > 0);
    CHECK(counts.not_opaque == 0);

    for (int step = 0; step < 30; step++) {
        lv_tick_inc(100);
        run_timer(SaverTestAccess::timer(ss));
    }
    counts = count_pixels(buf);
    REQUIRE(counts.lit > 0);
    CHECK(counts.not_opaque == 0);
}

#endif // HELIX_ENABLE_SCREENSAVER
