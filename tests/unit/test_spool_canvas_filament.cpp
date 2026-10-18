// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The spool illustration paints its filament the way every swatch does: the
// multi-colour split across the wound body and the finish's texture on it.
// Pixels are read straight from the canvas draw buffer.

#include "ui_spool_canvas.h"

#include "../lvgl_ui_test_fixture.h"

#include "../catch_amalgamated.hpp"

namespace {

constexpr int32_t SPOOL = 64;

lv_color32_t px(lv_obj_t* canvas, int32_t x, int32_t y) {
    const lv_draw_buf_t* buf = lv_canvas_get_draw_buf(canvas);
    REQUIRE(buf != nullptr);
    return reinterpret_cast<const lv_color32_t*>(buf->data + y * buf->header.stride)[x];
}

int brightness(lv_color32_t c) {
    return c.red + c.green + c.blue;
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "spool_canvas: a multi-colour filament splits across the body",
                 "[spool_canvas][swatch]") {
    lv_obj_t* canvas = ui_spool_canvas_create(test_screen(), SPOOL);
    REQUIRE(canvas != nullptr);

    // A plain render first, so the split render must not come back from the cache.
    helix::ui::spool_canvas_set_filament(canvas, 0xFF0000, "", helix::ui::SwatchFinish::Plain);
    const lv_color32_t upper = px(canvas, 24, 15);
    const lv_color32_t lower = px(canvas, 24, 50);
    CHECK(upper.red > upper.blue);
    CHECK(lower.red > lower.blue);

    helix::ui::spool_canvas_set_filament(canvas, 0xFF0000, "#FF0000,#0000FF",
                                         helix::ui::SwatchFinish::Plain);
    const lv_color32_t split_upper = px(canvas, 24, 15);
    const lv_color32_t split_lower = px(canvas, 24, 50);
    CHECK(split_upper.red > split_upper.blue); // colour 0 keeps the top-left
    CHECK(split_lower.blue > split_lower.red); // colour 1 takes the bottom-right

    // A plain colour clears the split again.
    ui_spool_canvas_set_color(canvas, lv_color_hex(0xFF0000));
    const lv_color32_t again = px(canvas, 24, 50);
    CHECK(again.red > again.blue);
}

TEST_CASE_METHOD(LVGLUITestFixture, "spool_canvas: silk lights the body, matte mutes it",
                 "[spool_canvas][swatch]") {
    lv_obj_t* canvas = ui_spool_canvas_create(test_screen(), SPOOL);
    REQUIRE(canvas != nullptr);

    helix::ui::spool_canvas_set_filament(canvas, 0x2060C0, "", helix::ui::SwatchFinish::Plain);
    const lv_color32_t plain = px(canvas, 26, 33);

    helix::ui::spool_canvas_set_filament(canvas, 0x2060C0, "", helix::ui::SwatchFinish::Silk);
    CHECK(brightness(px(canvas, 26, 33)) > brightness(plain) + 30);

    helix::ui::spool_canvas_set_filament(canvas, 0x2060C0, "", helix::ui::SwatchFinish::Matte);
    const lv_color32_t matte = px(canvas, 26, 33);
    CHECK(matte.blue - matte.red < plain.blue - plain.red);
}
