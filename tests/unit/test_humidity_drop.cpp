// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_humidity_drop.h"

#include "../test_fixtures.h"
#include "display_settings_manager.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "humidity_drop_geometry.h"
#include "settings_manager.h"

#include <cmath>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::ui::humidity_drop;

namespace {

struct Pixel {
    uint8_t b, g, r, a;
};

struct Canvas {
    int w, h;
    std::vector<uint8_t> px;
    Canvas(int w_, int h_) : w(w_), h(h_), px(static_cast<size_t>(w_ * h_ * 4), 0xAB) {}
    Pixel at(int x, int y) const {
        const uint8_t* p = &px[static_cast<size_t>((y * w + x) * 4)];
        return {p[0], p[1], p[2], p[3]};
    }
};

// Distinct, saturated colours so a pixel's source is readable from its value.
const DropColors kColors{{255, 0, 0}, {0, 0, 255}, {0, 255, 0}};

Canvas render(int w, int h, int humidity_x10, float phase = 0.0f) {
    Canvas c(w, h);
    rasterize(c.px.data(), w, h, w * 4, kColors, humidity_x10, phase);
    return c;
}

// A pixel whose dominant channel is green or blue: painted by a wave, not the outline.
bool is_fill(const Pixel& p) {
    return p.a > 128 && (p.g > p.r + 40 || p.b > p.r + 40);
}

int count_fill(const Canvas& c) {
    int n = 0;
    for (int y = 0; y < c.h; ++y)
        for (int x = 0; x < c.w; ++x)
            n += is_fill(c.at(x, y)) ? 1 : 0;
    return n;
}

} // namespace

// ============================================================================
// Shape
// ============================================================================

TEST_CASE("humidity_drop: the drop fits its box, centred", "[humidity_drop]") {
    for (int size : {16, 24, 48, 128}) {
        const DropShape s = fit_drop(size, size);
        CAPTURE(size);
        CHECK(s.cx == Catch::Approx(size / 2.0f));
        CHECK(s.tip_y > 0.0f);
        CHECK(s.bottom() < size);
        CHECK(s.cx - s.r > 0.0f);
        CHECK(s.cx + s.r < size);
        // Centred vertically: the air above the tip matches the air below the bottom.
        CHECK(s.tip_y == Catch::Approx(size - s.bottom()).margin(0.01));
        // Taller than wide, as the glyph is.
        CHECK(s.bottom() - s.tip_y > 2.0f * s.r);
    }
}

TEST_CASE("humidity_drop: a narrow box limits the drop by its width", "[humidity_drop]") {
    const DropShape tall = fit_drop(10, 100);
    CHECK(2.0f * tall.r < 10.0f);
    CHECK(tall.bottom() - tall.tip_y < 100.0f * 0.5f);
}

TEST_CASE("humidity_drop: half width runs from the tip to the round bottom", "[humidity_drop]") {
    const DropShape s = fit_drop(100, 100);
    CHECK(half_width(s, s.tip_y - 1.0f) == 0.0f);
    CHECK(half_width(s, s.bottom() + 1.0f) == 0.0f);
    CHECK(half_width(s, s.tip_y) == Catch::Approx(0.0f).margin(0.05));
    CHECK(half_width(s, s.cy) == Catch::Approx(s.r));
    CHECK(half_width(s, s.cy + s.r * 0.6f) == Catch::Approx(s.r * 0.8f).margin(0.01));
    // The upper part widens all the way down to the equator.
    float prev = 0.0f;
    for (float y = s.tip_y + 0.5f; y < s.cy; y += 0.5f) {
        const float hw = half_width(s, y);
        CHECK(hw >= prev);
        prev = hw;
    }
}

TEST_CASE("humidity_drop: signed distance is negative inside, positive outside",
          "[humidity_drop]") {
    const DropShape s = fit_drop(100, 100);
    CHECK(signed_distance(s, s.cx, s.cy) == Catch::Approx(-s.r).margin(0.01));
    CHECK(signed_distance(s, s.cx, s.bottom() + 3.0f) == Catch::Approx(3.0f).margin(0.01));
    CHECK(signed_distance(s, s.cx, s.tip_y - 2.0f) == Catch::Approx(2.0f).margin(0.01));
    CHECK(signed_distance(s, s.cx + s.r + 4.0f, s.cy) == Catch::Approx(4.0f).margin(0.01));
    // On the curved upper side, distance tracks the edge, not just the row's half width.
    const float y = (s.tip_y + s.cy) / 2.0f;
    const float edge = s.cx + half_width(s, y);
    CHECK(signed_distance(s, edge, y) == Catch::Approx(0.0f).margin(0.01));
    CHECK(signed_distance(s, edge - 3.0f, y) < 0.0f);
    CHECK(signed_distance(s, edge - 3.0f, y) > -3.0f);
    CHECK(signed_distance(s, edge + 3.0f, y) > 0.0f);
}

TEST_CASE("humidity_drop: stroke and wave scale with the drop, with floors", "[humidity_drop]") {
    const DropShape icon = fit_drop(16, 16);
    const DropShape big = fit_drop(160, 160);
    CHECK(stroke_width(icon) >= 1.0f);
    CHECK(wave_amplitude(icon) >= 0.75f);
    CHECK(stroke_width(big) > stroke_width(icon));
    CHECK(wave_amplitude(big) > wave_amplitude(icon));
    CHECK(stroke_width(big) <= 3.5f);
}

// ============================================================================
// Level
// ============================================================================

TEST_CASE("humidity_drop: fill fraction clamps and flags unknown", "[humidity_drop]") {
    CHECK(fill_fraction(0) == 0.0f);
    CHECK(fill_fraction(500) == Catch::Approx(0.5f));
    CHECK(fill_fraction(150) == Catch::Approx(0.15f));
    CHECK(fill_fraction(1000) == 1.0f);
    CHECK(fill_fraction(1500) == 1.0f);
    CHECK(fill_fraction(kUnknown) < 0.0f);
    CHECK(fill_fraction(-250) < 0.0f);
}

TEST_CASE("humidity_drop: 0% sits every crest below the floor, 100% every trough over the tip",
          "[humidity_drop]") {
    const DropShape s = fit_drop(64, 64);
    const float a = wave_amplitude(s);
    const float sw = stroke_width(s);
    const float empty = surface_level(s, 0.0f);
    const float full = surface_level(s, 1.0f);
    CHECK(empty - a >= s.bottom() - sw - 0.001f);
    CHECK(full + a <= s.tip_y + sw + 0.001f);
    // Linear in between, rising as the fraction grows (y grows downward).
    CHECK(surface_level(s, 0.5f) == Catch::Approx((empty + full) / 2.0f));
    CHECK(surface_level(s, 0.85f) < surface_level(s, 0.15f));
}

TEST_CASE("humidity_drop: the wave stays within its amplitude and repeats each drop width",
          "[humidity_drop]") {
    const DropShape s = fit_drop(64, 64);
    const float level = 30.0f;
    const float a = 2.0f;
    for (float x = 0; x < 64; x += 0.25f) {
        const float y = wave_y(s, level, a, 0.3f, x);
        CHECK(y >= level - a - 1e-4f);
        CHECK(y <= level + a + 1e-4f);
        CHECK(wave_y(s, level, a, 0.3f, x + 2.0f * s.r) == Catch::Approx(y).margin(1e-3));
    }
    // The phase moves it: some column reads differently a quarter turn later.
    CHECK(wave_y(s, level, a, 0.0f, s.cx) != Catch::Approx(wave_y(s, level, a, 1.57f, s.cx)));
    CHECK(back_phase(0.0f) != Catch::Approx(0.0f));
    CHECK(back_phase(1.0f) - back_phase(0.0f) < 0.0f);
}

TEST_CASE("humidity_drop: only a drawn, partial fill animates, and only with animations on",
          "[humidity_drop]") {
    CHECK(should_animate(true, true, 500));
    CHECK(should_animate(true, true, 10));
    CHECK_FALSE(should_animate(false, true, 500));
    CHECK_FALSE(should_animate(true, false, 500));
    CHECK_FALSE(should_animate(true, true, 0));
    CHECK_FALSE(should_animate(true, true, 1000));
    CHECK_FALSE(should_animate(true, true, 1200));
    CHECK_FALSE(should_animate(true, true, kUnknown));
}

// ============================================================================
// Rasterization
// ============================================================================

TEST_CASE("humidity_drop: nothing paints outside the drop, at any size, level or phase",
          "[humidity_drop]") {
    for (int size : {16, 20, 24, 48, 120}) {
        const DropShape s = fit_drop(size, size);
        for (int h : {kUnknown, 0, 150, 500, 850, 1000}) {
            for (float phase : {0.0f, 1.3f, 2.9f, 4.4f}) {
                const Canvas c = render(size, size, h, phase);
                for (int y = 0; y < size; ++y) {
                    for (int x = 0; x < size; ++x) {
                        const float d = signed_distance(s, x + 0.5f, y + 0.5f);
                        if (d > 1.0f) {
                            CAPTURE(size, h, phase, x, y, d);
                            REQUIRE(c.at(x, y).a == 0);
                        }
                        if (is_fill(c.at(x, y))) {
                            // Fill keeps clear of the outline's outer edge.
                            CAPTURE(size, h, phase, x, y, d);
                            REQUIRE(d < 0.0f);
                        }
                    }
                }
            }
        }
    }
}

TEST_CASE("humidity_drop: every pixel is written", "[humidity_drop]") {
    // The buffer starts as 0xAB garbage; a corner the rasterizer skipped would keep it.
    const Canvas c = render(24, 24, 500);
    CHECK(c.at(0, 0).a == 0);
    CHECK(c.at(23, 23).a == 0);
    CHECK(c.at(0, 0).r == 0);
}

TEST_CASE("humidity_drop: 0% and unknown fill nothing; 100% fills the inside", "[humidity_drop]") {
    const int size = 48;
    const DropShape s = fit_drop(size, size);
    CHECK(count_fill(render(size, size, 0)) == 0);
    CHECK(count_fill(render(size, size, kUnknown)) == 0);

    const Canvas full = render(size, size, 1000);
    const Pixel centre = full.at(static_cast<int>(s.cx), static_cast<int>(s.cy));
    CHECK(is_fill(centre));
    const int near_tip_y = static_cast<int>(s.tip_y + stroke_width(s) * 3.0f);
    CHECK(is_fill(full.at(static_cast<int>(s.cx), near_tip_y)));
}

TEST_CASE("humidity_drop: the fill grows with humidity", "[humidity_drop]") {
    const int size = 64;
    const int f15 = count_fill(render(size, size, 150));
    const int f50 = count_fill(render(size, size, 500));
    const int f85 = count_fill(render(size, size, 850));
    const int f100 = count_fill(render(size, size, 1000));
    CHECK(f15 > 0);
    CHECK(f15 < f50);
    CHECK(f50 < f85);
    CHECK(f85 < f100);
}

TEST_CASE("humidity_drop: half full fills below the middle, not above it", "[humidity_drop]") {
    const int size = 64;
    const DropShape s = fit_drop(size, size);
    const Canvas c = render(size, size, 500);
    const float mid = (s.tip_y + s.bottom()) / 2.0f;
    const int cx = static_cast<int>(s.cx);
    CHECK(is_fill(c.at(cx, static_cast<int>(mid + 6))));
    CHECK_FALSE(is_fill(c.at(cx, static_cast<int>(mid - 6))));
}

TEST_CASE("humidity_drop: the front wave is the lighter tone on top, the back peeks above it",
          "[humidity_drop]") {
    const int size = 120;
    const Canvas c = render(size, size, 500);
    int front = 0;
    int back = 0;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const Pixel p = c.at(x, y);
            if (p.a < 200)
                continue;
            front += (p.g > 200 && p.b < 60) ? 1 : 0;
            back += (p.b > 200 && p.g < 60) ? 1 : 0;
        }
    }
    CHECK(front > back);
    CHECK(back > 0);
}

TEST_CASE("humidity_drop: unknown draws a faint outline only", "[humidity_drop]") {
    const int size = 48;
    const DropShape s = fit_drop(size, size);
    const Canvas known = render(size, size, 0);
    const Canvas unknown = render(size, size, kUnknown);
    // The outline at the bottom centre.
    const int x = static_cast<int>(s.cx);
    const int y = static_cast<int>(s.bottom() - stroke_width(s) * 0.5f);
    CHECK(known.at(x, y).a > 200);
    CHECK(unknown.at(x, y).a > 0);
    CHECK(unknown.at(x, y).a < known.at(x, y).a);
    // And no interior tint: the centre stays clear.
    CHECK(unknown.at(x, static_cast<int>(s.cy)).a == 0);
}

// ============================================================================
// Widget
// ============================================================================

namespace {
lv_obj_t* make_drop(lv_obj_t* parent, const char* subject, const char* size = "md") {
    const char* attrs[] = {"bind_value", subject, "size", size, nullptr};
    return static_cast<lv_obj_t*>(lv_xml_create(parent, "humidity_drop", attrs));
}
} // namespace

TEST_CASE_METHOD(XMLTestFixture, "humidity_drop: bind_value tracks the subject",
                 "[humidity_drop][widget]") {
    helix::ui::register_humidity_drop_widget();
    lv_subject_t subj;
    lv_subject_init_int(&subj, 425);
    lv_xml_register_subject(nullptr, "test_drop_humidity", &subj);

    lv_obj_t* drop = make_drop(test_screen(), "test_drop_humidity");
    REQUIRE(drop != nullptr);
    process_lvgl(20);
    CHECK(helix::ui::humidity_drop_value(drop) == 425);

    lv_subject_set_int(&subj, 870);
    process_lvgl(20);
    CHECK(helix::ui::humidity_drop_value(drop) == 870);

    lv_subject_set_int(&subj, kUnknown);
    process_lvgl(20);
    CHECK(helix::ui::humidity_drop_value(drop) == kUnknown);

    lv_obj_delete(drop);
    lv_subject_deinit(&subj);
}

TEST_CASE_METHOD(XMLTestFixture, "humidity_drop: without a binding it shows no reading",
                 "[humidity_drop][widget]") {
    helix::ui::register_humidity_drop_widget();
    const char* attrs[] = {"size", "sm", nullptr};
    lv_obj_t* drop = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "humidity_drop", attrs));
    REQUIRE(drop != nullptr);
    CHECK(helix::ui::humidity_drop_value(drop) == kUnknown);
    helix::ui::humidity_drop_set_value(drop, 330);
    CHECK(helix::ui::humidity_drop_value(drop) == 330);
    lv_obj_delete(drop);
}

TEST_CASE_METHOD(XMLTestFixture, "humidity_drop: size matches the water icon of the same size",
                 "[humidity_drop][widget]") {
    helix::ui::register_humidity_drop_widget();
    for (const char* size : {"xs", "sm", "md", "lg"}) {
        CAPTURE(size);
        const char* icon_attrs[] = {"src", "water", "size", size, nullptr};
        auto* icon = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "icon", icon_attrs));
        const char* drop_attrs[] = {"size", size, nullptr};
        auto* drop =
            static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "humidity_drop", drop_attrs));
        REQUIRE(icon != nullptr);
        REQUIRE(drop != nullptr);
        lv_obj_update_layout(test_screen());
        CHECK(lv_obj_get_width(drop) == lv_obj_get_width(icon));
        CHECK(lv_obj_get_height(drop) == lv_obj_get_height(icon));
        CHECK(lv_obj_get_width(drop) > 0);
        CHECK(helix::ui::is_humidity_drop(drop));
        CHECK_FALSE(helix::ui::is_humidity_drop(icon));
        lv_obj_delete(icon);
        lv_obj_delete(drop);
    }
}

TEST_CASE_METHOD(XMLTestFixture, "humidity_drop: a font change resizes it with the icon",
                 "[humidity_drop][widget]") {
    // bind_tile_rung re-faces an icon by its text font; the drop follows the same font.
    helix::ui::register_humidity_drop_widget();
    const char* attrs[] = {"size", "xs", nullptr};
    auto* drop = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "humidity_drop", attrs));
    REQUIRE(drop != nullptr);
    lv_obj_update_layout(test_screen());
    const int32_t small_h = lv_obj_get_height(drop);
    lv_obj_set_style_text_font(drop, &mdi_icons_48, LV_PART_MAIN);
    lv_obj_update_layout(test_screen());
    CHECK(lv_obj_get_height(drop) > small_h);
    CHECK(lv_obj_get_height(drop) == static_cast<int32_t>(lv_font_get_line_height(&mdi_icons_48)));
    lv_obj_delete(drop);
}

TEST_CASE_METHOD(XMLTestFixture, "humidity_drop: explicit width and height win over the font",
                 "[humidity_drop][widget]") {
    helix::ui::register_humidity_drop_widget();
    const char* attrs[] = {"width", "90", "height", "120", "size", "xs", nullptr};
    auto* drop = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "humidity_drop", attrs));
    REQUIRE(drop != nullptr);
    lv_obj_update_layout(test_screen());
    CHECK(lv_obj_get_width(drop) == 90);
    CHECK(lv_obj_get_height(drop) == 120);
    lv_obj_delete(drop);
}

TEST_CASE_METHOD(XMLTestFixture, "humidity_drop: the wave ticks only while animations are on",
                 "[humidity_drop][widget]") {
    helix::ui::register_humidity_drop_widget();
    SettingsManager::instance().init_subjects();
    auto& dsm = DisplaySettingsManager::instance();
    const bool was = dsm.get_animations_enabled();
    const char* attrs[] = {"size", "lg", nullptr};
    auto* drop = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "humidity_drop", attrs));
    REQUIRE(drop != nullptr);
    helix::ui::humidity_drop_set_value(drop, 500);

    // The test display never runs periodic timers, so step the shared one by hand.
    dsm.set_animations_enabled(false);
    const float frozen = helix::ui::humidity_drop_phase();
    helix::ui::humidity_drop_tick();
    CHECK(helix::ui::humidity_drop_phase() == frozen);

    dsm.set_animations_enabled(true);
    helix::ui::humidity_drop_tick();
    CHECK(helix::ui::humidity_drop_phase() != frozen);

    dsm.set_animations_enabled(was);
    lv_obj_delete(drop);
}
