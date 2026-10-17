// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver_clock_reading.h"
#include "screensaver_stroke_digits.h"

#include <cstdint>

#include "../catch_amalgamated.hpp"

using helix::ui::CLOCK_DONE_HOLD_MS;
using helix::ui::clock_time;
using helix::ui::ClockDate;
using helix::ui::ClockInput;
using helix::ui::ClockMeridiem;
using helix::ui::ClockMode;
using helix::ui::ClockPrintPhase;
using helix::ui::ClockState;
using helix::ui::ClockTime;
using helix::ui::StrokeDigitSet;

namespace {

constexpr int32_t hms(int32_t h, int32_t m, int32_t s = 0) {
    return h * 3600 + m * 60 + s;
}

uint64_t total_coverage(const StrokeDigitSet& set, int digit) {
    uint64_t sum = 0;
    for (int32_t y = 0; y < set.h; y++) {
        for (int32_t x = 0; x < set.w; x++) {
            sum += set.at(digit, x, y);
        }
    }
    return sum;
}

} // namespace

// ============================================================================
// Time and readings
// ============================================================================

TEST_CASE("clock_time shows 12-hour and 24-hour time with seconds and the marker",
          "[screensaver][clock_reading]") {
    const ClockTime evening = clock_time(hms(21, 28, 7), false);
    CHECK(evening.hour == 9);
    CHECK(evening.hour_of_day == 21);
    CHECK(evening.minute == 28);
    CHECK(evening.second == 7);
    CHECK(evening.meridiem == ClockMeridiem::PM);

    const ClockTime midnight = clock_time(0, false);
    CHECK(midnight.hour == 12);
    CHECK(midnight.meridiem == ClockMeridiem::AM);

    const ClockTime noon = clock_time(hms(12, 0), false);
    CHECK(noon.hour == 12);
    CHECK(noon.meridiem == ClockMeridiem::PM);

    const ClockTime morning = clock_time(hms(9, 0), true);
    CHECK(morning.hour == 9);
    CHECK(morning.meridiem == ClockMeridiem::NONE);

    const ClockTime wrapped = clock_time(-1, true);
    CHECK(wrapped.hour == 23);
    CHECK(wrapped.minute == 59);
    CHECK(wrapped.second == 59);
}

TEST_CASE("a reading carries the date it was given", "[screensaver][clock_reading]") {
    ClockInput in;
    in.date = ClockDate{2019, 10, 21, 1};
    ClockState state;
    state.step(0, in);
    const ClockDate& date = state.reading().date;
    CHECK(date.year == 2019);
    CHECK(date.month == 10);
    CHECK(date.day == 21);
    CHECK(date.weekday == 1);
}

TEST_CASE("a print with an estimate reads a countdown with its end time and progress",
          "[screensaver][clock_reading]") {
    ClockInput in;
    in.seconds_of_day = hms(21, 28, 0);
    in.print.phase = ClockPrintPhase::PRINTING;
    in.print.remaining_s = 4320;
    in.print.progress_pct = 37;
    ClockState state;
    state.step(0, in);
    CHECK(state.mode() == ClockMode::COUNTDOWN);
    CHECK(state.reading().countdown_s == 4320);
    CHECK(state.reading().eta_minute_of_day == 1360);
    CHECK(state.reading().progress_pct == 37);
    CHECK(state.reading().time.hour == 9);
    CHECK(state.reading().time.minute == 28);
    CHECK(state.reading().time.meridiem == ClockMeridiem::PM);

    state.step(1000, in);
    CHECK(state.countdown_s() == 4319);

    // Estimator jitter within the resync window is ignored.
    in.print.remaining_s = 4319 + 5;
    state.step(0, in);
    CHECK(state.countdown_s() == 4319);

    in.print.remaining_s = 4000;
    state.step(0, in);
    CHECK(state.countdown_s() == 4000);
    CHECK(state.reading().countdown_s == 4000);
}

TEST_CASE("paused freezes, done holds then returns to the clock, cancelled clears",
          "[screensaver][clock_reading]") {
    ClockInput in;
    in.seconds_of_day = hms(10, 0);
    in.print.phase = ClockPrintPhase::PRINTING;
    in.print.remaining_s = 600;
    in.print.progress_pct = 42;
    ClockState state;
    state.step(0, in);
    REQUIRE(state.mode() == ClockMode::COUNTDOWN);

    in.print.phase = ClockPrintPhase::PAUSED;
    state.step(0, in);
    state.step(5000, in);
    CHECK(state.mode() == ClockMode::PAUSED);
    CHECK(state.countdown_s() == 600);
    CHECK(state.reading().progress_pct == 42);

    in.print.phase = ClockPrintPhase::COMPLETE;
    in.print.remaining_s = 0;
    state.step(16, in);
    CHECK(state.mode() == ClockMode::DONE);
    CHECK(state.reading().progress_pct == 100);
    state.step(CLOCK_DONE_HOLD_MS - 100, in);
    CHECK(state.mode() == ClockMode::DONE);
    state.step(200, in);
    CHECK(state.mode() == ClockMode::CLOCK);

    in.print.phase = ClockPrintPhase::PRINTING;
    in.print.remaining_s = 300;
    state.step(16, in);
    REQUIRE(state.mode() == ClockMode::COUNTDOWN);
    in.print.phase = ClockPrintPhase::NONE;
    in.print.remaining_s = -1;
    state.step(16, in);
    CHECK(state.mode() == ClockMode::CLOCK);
    CHECK(state.reading().countdown_s == 0);
    CHECK(state.reading().progress_pct == 0);
}

// ============================================================================
// Stroke digits
// ============================================================================

TEST_CASE("stroke digits size their box from the stroke", "[screensaver][clock_reading]") {
    StrokeDigitSet bold;
    helix::ui::build_stroke_digits(50, 0.2f, bold);
    CHECK(bold.w == 60);
    CHECK(bold.h == 90);
    StrokeDigitSet thin;
    helix::ui::build_stroke_digits(50, 0.06f, thin);
    CHECK(thin.w == 53);
    CHECK(thin.h == 83);
}

TEST_CASE("a thin stroke covers fewer pixels than a bold one", "[screensaver][clock_reading]") {
    StrokeDigitSet bold;
    helix::ui::build_stroke_digits(50, 0.2f, bold);
    StrokeDigitSet thin;
    helix::ui::build_stroke_digits(50, 0.06f, thin);
    for (int digit = 0; digit < 10; digit++) {
        CAPTURE(digit);
        const uint64_t t = total_coverage(thin, digit);
        CHECK(t > 0);
        CHECK(t * 2 < total_coverage(bold, digit));
    }
}

TEST_CASE("a stroke digit's centre line is fully covered", "[screensaver][clock_reading]") {
    StrokeDigitSet thin;
    helix::ui::build_stroke_digits(50, 0.06f, thin);
    // Digit one's upright runs at x = 0.55 path units, shifted by half a stroke into the box.
    const auto x = static_cast<int32_t>((0.55f + 0.03f) * 50.0f);
    CHECK(thin.at(1, x, thin.h / 2) >= 250);
    CHECK(thin.at(1, 2, thin.h / 2) == 0);
}

TEST_CASE("nine is six turned half a turn", "[screensaver][clock_reading]") {
    for (int i = 0; i <= 10; i++) {
        for (int j = 0; j <= 16; j++) {
            const float x = static_cast<float>(i) / 10.0f;
            const float y = static_cast<float>(j) / 10.0f;
            CHECK(helix::ui::stroke_digit_distance(9, x, y) ==
                  helix::ui::stroke_digit_distance(6, 1.0f - x, 1.6f - y));
        }
    }
    CHECK(helix::ui::stroke_digit_distance(9, 0.5f, 0.5f) !=
          helix::ui::stroke_digit_distance(6, 0.5f, 0.5f));
}

TEST_CASE("clear frees the coverage", "[screensaver][clock_reading]") {
    StrokeDigitSet set;
    helix::ui::build_stroke_digits(40, 0.1f, set);
    REQUIRE(set.heap_bytes() > 0);
    set.clear();
    CHECK(set.heap_bytes() == 0);
    CHECK(set.at(3, 0, 0) == 0);
}

#endif // HELIX_ENABLE_SCREENSAVER
