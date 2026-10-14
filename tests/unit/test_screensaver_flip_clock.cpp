// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "../lvgl_test_fixture.h"
#include "../test_helpers/screensaver_test_access.h"
#include "screensaver_canvas.h"
#include "screensaver_flip_clock.h"
#include "screensaver_flip_clock_sim.h"
#include "screensaver_pixel_writer.h"

#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::clock_face;
using helix::ui::countdown_face;
using helix::ui::DirtyRect;
using helix::ui::FLIP_CLOCK_LEVEL_COUNT;
using helix::ui::FLIP_CLOCK_LEVELS;
using helix::ui::FLIP_COUNTDOWN_RESYNC_S;
using helix::ui::FLIP_DONE_HOLD_MS;
using helix::ui::FLIP_DRIFT_STEP_MS;
using helix::ui::FLIP_DURATION_MS;
using helix::ui::FLIP_MAX_COUNTDOWN_S;
using helix::ui::FlipCaption;
using helix::ui::FlipClockInput;
using helix::ui::FlipClockScreensaver;
using helix::ui::FlipClockSim;
using helix::ui::FlipClockState;
using helix::ui::FlipFace;
using helix::ui::FlipLayout;
using helix::ui::FlipMeridiem;
using helix::ui::FlipMode;
using helix::ui::FlipPrintPhase;
using helix::ui::FlipShape;
using helix::ui::FrameTarget;
using helix::ui::PixelFormat;
using helix::ui::PixelWriter;
using helix::ui::Rgb;
using helix::ui::SaverTestAccess;

namespace {

/// One character per card, a blank card as a space, groups joined with ':'.
std::string cards_text(const FlipFace& face) {
    std::string out;
    size_t i = 0;
    for (size_t g = 0; g < face.shape.group_count; g++) {
        if (g > 0) {
            out += ':';
        }
        for (size_t c = 0; c < face.shape.groups[g]; c++, i++) {
            out += face.cards[i].digit == '\0' ? ' ' : face.cards[i].digit;
        }
    }
    return out;
}

FlipShape shape_of(std::initializer_list<uint8_t> groups) {
    FlipShape shape;
    for (const uint8_t g : groups) {
        shape.groups[shape.group_count++] = g;
    }
    return shape;
}

constexpr int32_t hms(int32_t h, int32_t m, int32_t s = 0) {
    return h * 3600 + m * 60 + s;
}

FlipClockInput printing(int32_t remaining_s, int32_t progress = 40) {
    FlipClockInput in;
    in.seconds_of_day = hms(14, 0);
    in.print.phase = FlipPrintPhase::PRINTING;
    in.print.remaining_s = remaining_s;
    in.print.progress_pct = progress;
    return in;
}

uint32_t bytes_per_pixel(PixelFormat format) {
    return format == PixelFormat::RGB565 ? 2 : 4;
}

/// A heap frame whose rows are wider than w * bytes per pixel, as LVGL's aligned strides can be.
struct SimFrame {
    std::vector<uint8_t> bytes;
    FrameTarget target;

    SimFrame(uint32_t w, uint32_t h, PixelFormat format) {
        const uint32_t bpp = bytes_per_pixel(format);
        const uint32_t stride = w * bpp + 3 * bpp;
        bytes.assign(static_cast<size_t>(stride) * h, 0);
        target = {bytes.data(), stride, w, h, format};
    }
    SimFrame(const SimFrame&) = delete;
    SimFrame& operator=(const SimFrame&) = delete;
};

bool box_covers(const std::vector<DirtyRect>& boxes, int32_t x, int32_t y) {
    return std::any_of(boxes.begin(), boxes.end(), [&](const DirtyRect& b) {
        return x >= b.x1 && x <= b.x2 && y >= b.y1 && y <= b.y2;
    });
}

struct Diff {
    size_t changed = 0;
    size_t uncovered = 0;
    DirtyRect changed_box;
};

Diff diff_frame(const std::vector<uint8_t>& before, const SimFrame& frame,
                const std::vector<DirtyRect>& boxes) {
    Diff diff;
    const uint32_t bpp = bytes_per_pixel(frame.target.format);
    for (uint32_t y = 0; y < frame.target.h; y++) {
        for (uint32_t x = 0; x < frame.target.w; x++) {
            const size_t i = static_cast<size_t>(y) * frame.target.stride + x * bpp;
            if (std::memcmp(&before[i], &frame.bytes[i], bpp) == 0) {
                continue;
            }
            diff.changed++;
            const auto px = static_cast<int32_t>(x);
            const auto py = static_cast<int32_t>(y);
            diff.changed_box.add(px, py, px, py);
            diff.uncovered += box_covers(boxes, px, py) ? 0 : 1;
        }
    }
    return diff;
}

/// Pixels inside `box` at least as bright as half the digit colour.
size_t lit_in(const SimFrame& frame, const DirtyRect& box) {
    const PixelWriter writer(frame.target);
    size_t lit = 0;
    for (int32_t y = box.y1; y <= box.y2; y++) {
        for (int32_t x = box.x1; x <= box.x2; x++) {
            lit += writer.get(x, y).g > FlipClockSim::DIGIT.g / 2 ? 1 : 0;
        }
    }
    return lit;
}

std::vector<uint8_t> card_bytes(const SimFrame& frame, const DirtyRect& box) {
    std::vector<uint8_t> out;
    const uint32_t bpp = bytes_per_pixel(frame.target.format);
    for (int32_t y = box.y1; y <= box.y2; y++) {
        const size_t row = static_cast<size_t>(y) * frame.target.stride;
        out.insert(out.end(), frame.bytes.begin() + static_cast<ptrdiff_t>(row + box.x1 * bpp),
                   frame.bytes.begin() + static_cast<ptrdiff_t>(row + (box.x2 + 1) * bpp));
    }
    return out;
}

bool inside(const DirtyRect& inner, const DirtyRect& outer) {
    return inner.x1 >= outer.x1 && inner.y1 >= outer.y1 && inner.x2 <= outer.x2 &&
           inner.y2 <= outer.y2;
}

void fire(lv_timer_t* timer) {
    REQUIRE(timer != nullptr);
    REQUIRE(timer->timer_cb != nullptr);
    timer->timer_cb(timer);
}

} // namespace

// ============================================================================
// Faces
// ============================================================================

TEST_CASE("the clock face shows 24-hour time with a leading zero and no marker",
          "[screensaver][screensaver_flip_clock]") {
    CHECK(cards_text(clock_face(hms(9, 5), true)) == "09:05");
    CHECK(clock_face(hms(9, 5), true).shape == shape_of({2, 2}));
    CHECK(cards_text(clock_face(hms(0, 0), true)) == "00:00");
    CHECK(cards_text(clock_face(hms(23, 59, 59), true)) == "23:59");
    CHECK(clock_face(hms(13, 0), true).meridiem == FlipMeridiem::NONE);
    CHECK(clock_face(hms(13, 0), true).caption == FlipCaption::NONE);
}

TEST_CASE("the clock face shows 12-hour time with a blank leading card, marked AM or PM",
          "[screensaver][screensaver_flip_clock]") {
    CHECK(cards_text(clock_face(hms(0, 30), false)) == "12:30");
    CHECK(clock_face(hms(0, 30), false).meridiem == FlipMeridiem::AM);
    CHECK(cards_text(clock_face(hms(9, 42), false)) == " 9:42");
    CHECK(clock_face(hms(9, 42), false).shape == shape_of({2, 2}));
    CHECK(clock_face(hms(11, 59), false).meridiem == FlipMeridiem::AM);
    CHECK(cards_text(clock_face(hms(12, 0), false)) == "12:00");
    CHECK(clock_face(hms(12, 0), false).meridiem == FlipMeridiem::PM);
    CHECK(cards_text(clock_face(hms(13, 7), false)) == " 1:07");
    CHECK(clock_face(hms(23, 59), false).meridiem == FlipMeridiem::PM);
}

TEST_CASE("the countdown face shows H:MM:SS under an hour, past ten hours and at the limits",
          "[screensaver][screensaver_flip_clock]") {
    CHECK(countdown_face(hms(1, 2, 5)).shape == shape_of({1, 2, 2}));
    CHECK(countdown_face(hms(10, 2, 5)).shape == shape_of({2, 2, 2}));
    CHECK(countdown_face(hms(100, 2, 5)).shape == shape_of({3, 2, 2}));
    CHECK(countdown_face(hms(100, 2, 5)).shape.card_count() == 7);
    CHECK(cards_text(countdown_face(hms(1, 2, 5))) == "1:02:05");
    CHECK(cards_text(countdown_face(59)) == "0:00:59");
    CHECK(cards_text(countdown_face(hms(0, 45, 0))) == "0:45:00");
    CHECK(cards_text(countdown_face(hms(10, 20, 30))) == "10:20:30");
    CHECK(cards_text(countdown_face(hms(37, 0, 1))) == "37:00:01");
    CHECK(cards_text(countdown_face(0)) == "0:00:00");
    CHECK(cards_text(countdown_face(-5)) == "0:00:00");
    CHECK(cards_text(countdown_face(hms(123, 4, 5))) == "123:04:05");
    CHECK(cards_text(countdown_face(FLIP_MAX_COUNTDOWN_S)) == "999:59:59");
    CHECK(cards_text(countdown_face(FLIP_MAX_COUNTDOWN_S + 3600)) == "999:59:59");
}

// ============================================================================
// FlipClockState
// ============================================================================

TEST_CASE("the clock shows the time unless a print has a remaining-time estimate",
          "[screensaver][screensaver_flip_clock]") {
    FlipClockState state;
    FlipClockInput in;
    in.seconds_of_day = hms(21, 5);
    in.use_24h = true;

    SECTION("idle") {
        state.step(16, in);
        CHECK(state.mode() == FlipMode::CLOCK);
        CHECK(cards_text(state.face()) == "21:05");
    }
    SECTION("preparing without an estimate") {
        in.print.phase = FlipPrintPhase::PREPARING;
        state.step(16, in);
        CHECK(state.mode() == FlipMode::CLOCK);
    }
    SECTION("preparing once an estimate exists") {
        in.print.phase = FlipPrintPhase::PREPARING;
        in.print.remaining_s = 600;
        state.step(16, in);
        CHECK(state.mode() == FlipMode::COUNTDOWN);
        CHECK(cards_text(state.face()) == "0:10:00");
    }
    SECTION("printing without an estimate") {
        in.print.phase = FlipPrintPhase::PRINTING;
        in.print.remaining_s = -1;
        state.step(16, in);
        CHECK(state.mode() == FlipMode::CLOCK);
    }
    SECTION("a finished print the clock never saw running") {
        in.print.phase = FlipPrintPhase::COMPLETE;
        in.print.remaining_s = 0;
        state.step(16, in);
        CHECK(state.mode() == FlipMode::CLOCK);
    }
}

TEST_CASE("a running print counts down with the end time and progress under the cards",
          "[screensaver][screensaver_flip_clock]") {
    FlipClockState state;
    FlipClockInput in = printing(hms(1, 2, 3), 63);
    in.seconds_of_day = hms(20, 40, 0);
    state.step(16, in);
    REQUIRE(state.mode() == FlipMode::COUNTDOWN);
    const FlipFace face = state.face();
    CHECK(cards_text(face) == "1:02:03");
    CHECK(face.meridiem == FlipMeridiem::NONE);
    CHECK(face.caption == FlipCaption::DONE_AT);
    CHECK(face.progress_pct == 63);
    // 20:40:00 + 1:02:03 = 21:42:03
    CHECK(face.eta_minute_of_day == 21 * 60 + 42);
}

TEST_CASE("the end time wraps past midnight", "[screensaver][screensaver_flip_clock]") {
    FlipClockState state;
    FlipClockInput in = printing(hms(0, 30));
    in.seconds_of_day = hms(23, 50);
    state.step(16, in);
    CHECK(state.face().eta_minute_of_day == 20);
}

TEST_CASE("the countdown ticks at wall-clock rate and ignores small estimate jitter",
          "[screensaver][screensaver_flip_clock]") {
    FlipClockState state;
    FlipClockInput in = printing(1000);
    state.step(16, in);
    REQUIRE(state.countdown_s() == 1000);

    // The estimate holds still while a second passes: the cards still tick.
    state.step(1000, in);
    CHECK(state.countdown_s() == 999);
    state.step(2000, in);
    CHECK(state.countdown_s() == 997);

    // An estimate a few seconds above the running count never flips it backwards.
    in.print.remaining_s = 997 + FLIP_COUNTDOWN_RESYNC_S;
    state.step(0, in);
    CHECK(state.countdown_s() == 997);

    // A real re-estimate resets it.
    in.print.remaining_s = 1500;
    state.step(0, in);
    CHECK(state.countdown_s() == 1500);
    in.print.remaining_s = 400;
    state.step(0, in);
    CHECK(state.countdown_s() == 400);

    // It never counts below zero.
    in.print.remaining_s = 2;
    state.step(0, in);
    state.step(5000, in);
    CHECK(state.countdown_s() == 0);
}

TEST_CASE("a paused print freezes the countdown under PAUSED until it resumes",
          "[screensaver][screensaver_flip_clock]") {
    FlipClockState state;
    FlipClockInput in = printing(hms(0, 30));
    state.step(16, in);
    state.step(5000, in);
    REQUIRE(state.countdown_s() == hms(0, 29, 55));

    in.print.phase = FlipPrintPhase::PAUSED;
    state.step(16, in);
    CHECK(state.mode() == FlipMode::PAUSED);
    CHECK(state.face().caption == FlipCaption::PAUSED);
    for (int i = 0; i < 300; i++) {
        state.step(1000, in);
    }
    CHECK(state.countdown_s() == hms(0, 29, 55));
    CHECK(cards_text(state.face()) == "0:29:55");

    in.print.phase = FlipPrintPhase::PRINTING;
    state.step(0, in);
    CHECK(state.mode() == FlipMode::COUNTDOWN);
    state.step(1000, in);
    CHECK(state.countdown_s() == hms(0, 29, 54));
}

TEST_CASE("a finished print holds 0:00:00 and DONE for a minute, then shows the clock",
          "[screensaver][screensaver_flip_clock]") {
    FlipClockState state;
    FlipClockInput in = printing(30, 99);
    in.use_24h = true;
    state.step(16, in);
    REQUIRE(state.mode() == FlipMode::COUNTDOWN);

    in.print.phase = FlipPrintPhase::COMPLETE;
    in.print.remaining_s = 0;
    state.step(16, in);
    CHECK(state.mode() == FlipMode::DONE);
    CHECK(cards_text(state.face()) == "0:00:00");
    CHECK(state.face().caption == FlipCaption::DONE);

    state.step(FLIP_DONE_HOLD_MS - 100, in);
    CHECK(state.mode() == FlipMode::DONE);
    state.step(100, in);
    CHECK(state.mode() == FlipMode::CLOCK);
    CHECK(cards_text(state.face()) == "14:00");

    // Still complete a while later: the hold does not come back.
    state.step(FLIP_DONE_HOLD_MS, in);
    CHECK(state.mode() == FlipMode::CLOCK);
}

TEST_CASE("a cancelled print goes straight back to the clock",
          "[screensaver][screensaver_flip_clock]") {
    FlipClockState state;
    FlipClockInput in = printing(300);
    state.step(16, in);
    in.print.phase = FlipPrintPhase::NONE;
    in.print.remaining_s = -1;
    state.step(16, in);
    CHECK(state.mode() == FlipMode::CLOCK);
}

TEST_CASE("the flip clock reads the print lifecycle, with ended prints as no print",
          "[screensaver][screensaver_flip_clock]") {
    using helix::ui::flip_print_phase;
    CHECK(flip_print_phase(PrintState::Idle) == FlipPrintPhase::NONE);
    CHECK(flip_print_phase(PrintState::Preparing) == FlipPrintPhase::PREPARING);
    CHECK(flip_print_phase(PrintState::Printing) == FlipPrintPhase::PRINTING);
    CHECK(flip_print_phase(PrintState::Paused) == FlipPrintPhase::PAUSED);
    CHECK(flip_print_phase(PrintState::Complete) == FlipPrintPhase::COMPLETE);
    CHECK(flip_print_phase(PrintState::Cancelled) == FlipPrintPhase::NONE);
    CHECK(flip_print_phase(PrintState::Error) == FlipPrintPhase::NONE);
}

// ============================================================================
// Layout
// ============================================================================

TEST_CASE("one card per digit fits every screen shape at the widest drift, in rows or one row",
          "[screensaver][screensaver_flip_clock]") {
    struct Screen {
        int32_t w;
        int32_t h;
    };
    const Screen screen = GENERATE(Screen{800, 480}, Screen{480, 272}, Screen{1024, 600},
                                   Screen{480, 800}, Screen{272, 480}, Screen{1280, 720});
    // The clock, then countdowns with one, two and three hour digits.
    const FlipShape shape =
        GENERATE(shape_of({2, 2}), shape_of({1, 2, 2}), shape_of({2, 2, 2}), shape_of({3, 2, 2}));
    const uint8_t count = shape.card_count();
    CAPTURE(screen.w, screen.h, count, shape.groups[0]);

    int32_t max_dx = 0;
    int32_t max_dy = 0;
    helix::ui::flip_drift_range(screen.w, screen.h, shape, max_dx, max_dy);
    CHECK(max_dx > 0);
    CHECK(max_dy > 0);
    const DirtyRect whole{0, 0, screen.w - 1, screen.h - 1};
    for (int sx : {-1, 0, 1}) {
        for (int sy : {-1, 0, 1}) {
            const FlipLayout layout =
                helix::ui::layout_flip_cards(screen.w, screen.h, shape, sx * max_dx, sy * max_dy);
            REQUIRE(layout.card_count == count);
            CHECK(layout.shape == shape);
            CHECK(inside(layout.bounds(), whole));
            CHECK(inside(layout.caption, whole));
            CHECK(inside(layout.meridiem, layout.cards[0]));
            for (size_t i = 0; i < count; i++) {
                CHECK(inside(layout.cards[i], whole));
                CHECK(layout.cards[i].x2 - layout.cards[i].x1 ==
                      layout.cards[0].x2 - layout.cards[0].x1);
                CHECK(layout.cards[i].y2 - layout.cards[i].y1 ==
                      layout.cards[0].y2 - layout.cards[0].y1);
                for (size_t j = i + 1; j < count; j++) {
                    CHECK_FALSE(layout.cards[i].intersects(layout.cards[j]));
                }
                CHECK_FALSE(layout.cards[i].intersects(layout.caption));
                for (size_t c = 0; c < layout.colon_count; c++) {
                    CHECK_FALSE(layout.cards[i].intersects(layout.colons[c]));
                }
            }
            for (size_t c = 0; c < layout.colon_count; c++) {
                CHECK(inside(layout.colons[c], layout.bounds()));
            }
            CHECK(layout.split_top > 0);
            CHECK(layout.split_bottom > layout.split_top);
        }
    }

    const FlipLayout layout = helix::ui::layout_flip_cards(screen.w, screen.h, shape, 0, 0);
    // The cards take a large share of the screen.
    CHECK(layout.bounds().area() * 100 > static_cast<int64_t>(screen.w) * screen.h * 25);
    // Only the countdown carries a line under its cards; the clock centres without one.
    CHECK(layout.caption.empty() == !shape.has_caption());
    if (!shape.has_caption()) {
        const DirtyRect b = layout.bounds();
        CHECK(std::abs((b.y1 + b.y2) - (screen.h - 1)) <= 1);
    }
    // A portrait screen puts each group on its own row with no colons; a landscape one lines
    // every card up with a colon between groups.
    const bool rows = layout.cards[count - 1].y1 > layout.cards[0].y2;
    CHECK(rows == (screen.h > screen.w));
    CHECK(layout.colon_count == (rows ? 0 : shape.group_count - 1));
    // The cards of a group always share a row.
    size_t i = 0;
    for (size_t g = 0; g < shape.group_count; g++) {
        for (size_t c = 1; c < shape.groups[g]; c++) {
            CHECK(layout.cards[i + c].y1 == layout.cards[i].y1);
            CHECK(layout.cards[i + c].x1 > layout.cards[i + c - 1].x2);
        }
        i += shape.groups[g];
    }
}

TEST_CASE("the drift moves the cards once per step and stays within its range",
          "[screensaver][screensaver_flip_clock]") {
    int32_t dx = 0;
    int32_t dy = 0;
    helix::ui::flip_drift_at(0, 20, 10, dx, dy);
    CHECK(dx == 0);
    CHECK(dy == 0);
    helix::ui::flip_drift_at(FLIP_DRIFT_STEP_MS - 1, 20, 10, dx, dy);
    CHECK(dx == 0);
    CHECK(dy == 0);
    bool moved = false;
    for (uint32_t step = 1; step < 200; step++) {
        helix::ui::flip_drift_at(step * FLIP_DRIFT_STEP_MS, 20, 10, dx, dy);
        CHECK(std::abs(dx) <= 20);
        CHECK(std::abs(dy) <= 10);
        moved = moved || dx != 0 || dy != 0;
    }
    CHECK(moved);
}

// ============================================================================
// FlipClockSim
// ============================================================================

TEST_CASE("the flip clock draws light digits on dark cards over black, in either format",
          "[screensaver][screensaver_flip_clock]") {
    const PixelFormat format = GENERATE(PixelFormat::XRGB8888, PixelFormat::RGB565);
    CAPTURE(static_cast<int>(format));
    SimFrame frame(400, 240, format);
    FlipClockSim sim;
    sim.init(frame.target, 0, clock_face(hms(10, 8), true));
    const FlipLayout& layout = sim.layout();
    const PixelWriter writer(frame.target);

    CHECK(writer.get(0, 0) == FlipClockSim::BACKGROUND);
    for (size_t i = 0; i < layout.card_count; i++) {
        CAPTURE(i);
        const DirtyRect& card = layout.cards[i];
        const int32_t cx = card.x1 + layout.corner + 1;
        // Card face just inside the corner, and the split line across the middle.
        CHECK(writer.get(cx, card.y1 + layout.corner + 1).g < 60);
        CHECK(writer.get(cx, card.y1 + layout.corner + 1).g > 10);
        CHECK(writer.get(cx, card.y1 + layout.split_top) == FlipClockSim::BACKGROUND);
        CHECK(lit_in(frame, card) > 100);
    }
    CHECK(writer.get(layout.cards[0].x1, layout.cards[0].y1) == FlipClockSim::BACKGROUND);
    // The colon between hours and minutes.
    REQUIRE(layout.colon_count == 1);
    const DirtyRect& colon = layout.colons[0];
    size_t colon_px = 0;
    for (int32_t y = colon.y1; y <= colon.y2; y++) {
        for (int32_t x = colon.x1; x <= colon.x2; x++) {
            colon_px += writer.get(x, y) == FlipClockSim::BACKGROUND ? 0 : 1;
        }
    }
    CHECK(colon_px > 4);
}

TEST_CASE("a blank card draws no digit", "[screensaver][screensaver_flip_clock]") {
    SimFrame frame(400, 240, PixelFormat::XRGB8888);
    FlipClockSim sim;
    sim.init(frame.target, 0, clock_face(hms(9, 41), false));
    CHECK(lit_in(frame, sim.layout().cards[0]) == 0);
    CHECK(lit_in(frame, sim.layout().cards[1]) > 50);
}

TEST_CASE("different digits draw different cards", "[screensaver][screensaver_flip_clock]") {
    SimFrame frame(400, 240, PixelFormat::XRGB8888);
    FlipClockSim sim;
    std::vector<std::vector<uint8_t>> seen;
    for (int d = 0; d < 10; d++) {
        FlipFace face = clock_face(hms(d, d), true);
        sim.init(frame.target, 2, face);
        const std::vector<uint8_t> card = card_bytes(frame, sim.layout().cards[3]);
        for (const auto& other : seen) {
            CHECK(card != other);
        }
        seen.push_back(card);
    }
}

TEST_CASE("a changed card folds over the flip duration while the others stay still",
          "[screensaver][screensaver_flip_clock]") {
    SimFrame frame(400, 240, PixelFormat::XRGB8888);
    FlipClockSim sim;
    sim.init(frame.target, 0, clock_face(hms(10, 8), true));
    const DirtyRect minutes = sim.layout().cards[3];
    const std::vector<uint8_t> start = card_bytes(frame, minutes);

    std::vector<DirtyRect> dirty;
    const FlipFace next = clock_face(hms(10, 9), true);
    sim.step(0, frame.target, next, dirty);
    CHECK(sim.flipping());
    CHECK(sim.flip_progress(0) == 1.0f);
    CHECK(sim.flip_progress(3) == 0.0f);

    sim.step(FLIP_DURATION_MS / 4, frame.target, next, dirty);
    CHECK(sim.flip_progress(3) == Catch::Approx(0.25f));
    REQUIRE(dirty.size() == 1);
    CHECK(dirty[0] == minutes);
    const std::vector<uint8_t> quarter = card_bytes(frame, minutes);
    CHECK(quarter != start);

    sim.step(FLIP_DURATION_MS / 2, frame.target, next, dirty);
    CHECK(sim.flip_progress(3) == Catch::Approx(0.75f));
    const std::vector<uint8_t> three_quarters = card_bytes(frame, minutes);
    CHECK(three_quarters != quarter);

    sim.step(FLIP_DURATION_MS / 4, frame.target, next, dirty);
    CHECK_FALSE(sim.flipping());
    const std::vector<uint8_t> end = card_bytes(frame, minutes);
    CHECK(end != three_quarters);

    // The finished flip draws exactly what a still card of the new digits draws.
    SimFrame still(400, 240, PixelFormat::XRGB8888);
    FlipClockSim fresh;
    fresh.init(still.target, 0, next);
    CHECK(end == card_bytes(still, minutes));
}

TEST_CASE("only the digits that change flip, each on its own card and all at once",
          "[screensaver][screensaver_flip_clock]") {
    struct Change {
        const char* name;
        FlipFace from;
        FlipFace to;
        std::vector<size_t> flips;
    };
    const Change change = GENERATE(
        Change{"10:08 -> 10:09", clock_face(hms(10, 8), true), clock_face(hms(10, 9), true), {3}},
        Change{"10:59 -> 11:00",
               clock_face(hms(10, 59), true),
               clock_face(hms(11, 0), true),
               {1, 2, 3}},
        Change{"09:59 -> 10:00",
               clock_face(hms(9, 59), true),
               clock_face(hms(10, 0), true),
               {0, 1, 2, 3}},
        Change{"12h 1:09 -> 1:10",
               clock_face(hms(1, 9), false),
               clock_face(hms(1, 10), false),
               {2, 3}},
        Change{"12h 9:59 -> 10:00",
               clock_face(hms(9, 59), false),
               clock_face(hms(10, 0), false),
               {0, 1, 2, 3}},
        Change{"0:00:10 -> 0:00:09", countdown_face(10), countdown_face(9), {3, 4}},
        Change{"0:10:00 -> 0:09:59",
               countdown_face(hms(0, 10, 0)),
               countdown_face(hms(0, 9, 59)),
               {1, 2, 3, 4}},
        Change{"1:00:00 -> 0:59:59",
               countdown_face(hms(1, 0, 0)),
               countdown_face(hms(0, 59, 59)),
               {0, 1, 2, 3, 4}},
        Change{"12:30:00 -> 12:29:59",
               countdown_face(hms(12, 30, 0)),
               countdown_face(hms(12, 29, 59)),
               {2, 3, 4, 5}});
    CAPTURE(change.name);
    const auto flips = [&change](size_t i) {
        return std::find(change.flips.begin(), change.flips.end(), i) != change.flips.end();
    };

    SimFrame frame(800, 480, PixelFormat::XRGB8888);
    FlipClockSim sim;
    sim.init(frame.target, 0, change.from);
    const FlipLayout layout = sim.layout();
    std::vector<std::vector<uint8_t>> before;
    for (size_t i = 0; i < layout.card_count; i++) {
        before.push_back(card_bytes(frame, layout.cards[i]));
    }

    std::vector<DirtyRect> dirty;
    sim.step(0, frame.target, change.to, dirty);
    for (const uint32_t dt : {FLIP_DURATION_MS / 3, FLIP_DURATION_MS / 3}) {
        sim.step(dt, frame.target, change.to, dirty);
        // One box per flipping card, and nothing for the cards that stay.
        std::vector<DirtyRect> expected;
        for (const size_t i : change.flips) {
            expected.push_back(layout.cards[i]);
        }
        CHECK(dirty == expected);
        for (size_t i = 0; i < layout.card_count; i++) {
            CAPTURE(i);
            if (flips(i)) {
                // Every changed card is at the same point of its own fold.
                CHECK(sim.flip_progress(i) == Catch::Approx(sim.flip_progress(change.flips[0])));
                CHECK(sim.flip_progress(i) < 1.0f);
                CHECK(card_bytes(frame, layout.cards[i]) != before[i]);
            } else {
                CHECK(sim.flip_progress(i) == 1.0f);
                CHECK(card_bytes(frame, layout.cards[i]) == before[i]);
            }
        }
    }
}

TEST_CASE("between changes a frame writes nothing", "[screensaver][screensaver_flip_clock]") {
    SimFrame frame(400, 240, PixelFormat::RGB565);
    FlipClockSim sim;
    const FlipFace face = clock_face(hms(7, 30), false);
    sim.init(frame.target, 0, face);
    std::vector<DirtyRect> dirty;
    const std::vector<uint8_t> before = frame.bytes;
    for (int i = 0; i < 100; i++) {
        sim.step(16, frame.target, face, dirty);
        CHECK(dirty.empty());
    }
    CHECK(frame.bytes == before);
}

TEST_CASE("a level that does not animate swaps the digits at once",
          "[screensaver][screensaver_flip_clock]") {
    REQUIRE_FALSE(FLIP_CLOCK_LEVELS[FLIP_CLOCK_LEVEL_COUNT - 1].animate);
    SimFrame frame(400, 240, PixelFormat::XRGB8888);
    FlipClockSim sim;
    sim.init(frame.target, FLIP_CLOCK_LEVEL_COUNT - 1, clock_face(hms(10, 8), true));
    std::vector<DirtyRect> dirty;
    sim.step(0, frame.target, clock_face(hms(10, 9), true), dirty);
    CHECK_FALSE(sim.flipping());
    CHECK(dirty.size() == 1);

    SECTION("and a level request mid-flip finishes the flip") {
        sim.request_level(0);
        sim.step(0, frame.target, clock_face(hms(10, 10), true), dirty);
        REQUIRE(sim.flipping());
        sim.request_level(FLIP_CLOCK_LEVEL_COUNT - 1);
        sim.step(0, frame.target, clock_face(hms(10, 10), true), dirty);
        CHECK_FALSE(sim.flipping());
        CHECK(sim.level() == FLIP_CLOCK_LEVEL_COUNT - 1);
    }
}

TEST_CASE("every pixel a flip clock frame changes lies in one of its dirty boxes",
          "[screensaver][screensaver_flip_clock]") {
    const PixelFormat format = GENERATE(PixelFormat::XRGB8888, PixelFormat::RGB565);
    CAPTURE(static_cast<int>(format));
    SimFrame frame(480, 272, format);
    FlipClockSim sim;
    FlipClockState state;
    FlipClockInput in;
    in.seconds_of_day = hms(9, 59, 58);
    state.step(0, in);
    sim.init(frame.target, 0, state.face());

    std::vector<DirtyRect> dirty;
    size_t changed = 0;
    size_t uncovered = 0;
    // Clock flips, a print starting (three cards), seconds ticking, a pause, the end, and
    // drift steps along the way.
    for (int i = 0; i < 2000; i++) {
        if (i == 400) {
            in.print.phase = FlipPrintPhase::PRINTING;
            in.print.remaining_s = hms(0, 1, 5);
        }
        if (i == 900) {
            in.print.phase = FlipPrintPhase::PAUSED;
        }
        if (i == 1000) {
            in.print.phase = FlipPrintPhase::PRINTING;
        }
        if (i == 1500) {
            in.print.phase = FlipPrintPhase::COMPLETE;
            in.print.remaining_s = 0;
        }
        in.seconds_of_day = (in.seconds_of_day + (i % 3 == 0 ? 1 : 0)) % 86400;
        const uint32_t dt = i % 50 == 0 ? FLIP_DRIFT_STEP_MS / 4 : 33;
        state.step(dt, in);
        const std::vector<uint8_t> before = frame.bytes;
        sim.step(dt, frame.target, state.face(), dirty);
        const Diff diff = diff_frame(before, frame, dirty);
        changed += diff.changed;
        uncovered += diff.uncovered;
    }
    REQUIRE(changed > 0);
    CHECK(uncovered == 0);
}

TEST_CASE("a flip clock run replays exactly", "[screensaver][screensaver_flip_clock]") {
    auto run = [] {
        SimFrame frame(320, 200, PixelFormat::RGB565);
        FlipClockSim sim;
        FlipClockState state;
        FlipClockInput in = printing(hms(2, 0, 0));
        state.step(0, in);
        sim.init(frame.target, 0, state.face());
        std::vector<DirtyRect> dirty;
        for (int i = 0; i < 600; i++) {
            const uint32_t dt = i % 97 == 0 ? FLIP_DRIFT_STEP_MS / 2 : 33;
            state.step(dt, in);
            sim.step(dt, frame.target, state.face(), dirty);
        }
        return frame.bytes;
    };
    CHECK(run() == run());
}

// ============================================================================
// FlipClockScreensaver
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture,
                 "the flip clock saver draws on an opaque canvas with the flip clock ladder",
                 "[screensaver][screensaver_flip_clock]") {
    FlipClockScreensaver ss;
    ScreensaverStopOnExit<FlipClockScreensaver> stop_on_exit{ss};
    ss.start();
    REQUIRE(ss.is_active());
    const lv_draw_buf_t* buf = lv_canvas_get_draw_buf(SaverTestAccess::canvas(ss));
    REQUIRE(buf != nullptr);
    CHECK(buf->header.cf == helix::ui::FLIP_CLOCK_CANVAS_FORMAT);
    CHECK(ss.level_count() == FLIP_CLOCK_LEVEL_COUNT);
    CHECK(FLIP_CLOCK_LEVELS[0].period_ms == helix::ui::SAVER_FAST_PERIOD);

    lv_tick_inc(33);
    fire(SaverTestAccess::timer(ss));
    CHECK(ss.is_active());
}

TEST_CASE_METHOD(LVGLTestFixture, "the flip clock saver counts down the print its source reports",
                 "[screensaver][screensaver_flip_clock]") {
    FlipClockScreensaver ss;
    ScreensaverStopOnExit<FlipClockScreensaver> stop_on_exit{ss};
    FlipClockInput reported = printing(hms(1, 0, 0), 50);
    ss.set_input_source([&reported] { return reported; });
    ss.start();
    REQUIRE(ss.is_active());
    CHECK(ss.state().mode() == FlipMode::COUNTDOWN);
    CHECK(cards_text(ss.state().face()) == "1:00:00");

    reported.print.phase = FlipPrintPhase::PAUSED;
    lv_tick_inc(33);
    fire(SaverTestAccess::timer(ss));
    CHECK(ss.state().mode() == FlipMode::PAUSED);
}

#endif // HELIX_ENABLE_SCREENSAVER
