// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver_highfly.h"

#include <spdlog/spdlog.h>

#include <string>

namespace helix::ui {

namespace {

/// The date block and AM/PM: the numerals' pale blue-white, a little more see-through.
constexpr Rgb TEXT_COLOR{226, 238, 250};

} // namespace

ClockInput HighFlyScreensaver::read_input() const {
    return source_ ? source_() : saver_clock_now();
}

bool HighFlyScreensaver::on_start() {
    spdlog::info("[Screensaver] Starting high fly");
    clock_.reset();
    clock_.step(0, read_input());
    const FrameTarget frame = canvas().frame();
    sim_.init(frame, rng(), level(), clock_.reading());
    spdlog::debug("[Screensaver] High fly holds {} KB at {}x{}; first fly-through in {:.0f} s",
                  sim_.heap_bytes() / 1024, frame.w, frame.h, sim_.next_fly_s());
    date_line_.reset();
    day_line_.reset();
    meridiem_.reset();
    std::vector<DirtyRect> dirty;
    draw_texts(dirty, dirty);
    return true;
}

void HighFlyScreensaver::on_frame(uint32_t dt_ms, std::vector<DirtyRect>& dirty) {
    clock_.step(dt_ms, read_input());
    FrameTarget frame = canvas().frame();
    sim_.step(dt_ms, frame, rng(), clock_.reading(), dirty);
    if (dirty.empty()) {
        // Nothing beneath the text was repainted, so a changed line waits for the next frame
        // that repaints the clock rather than drawing over the old one.
        return;
    }
    const std::vector<DirtyRect> under = dirty;
    draw_texts(under, dirty);
}

void HighFlyScreensaver::draw_texts(const std::vector<DirtyRect>& under,
                                    std::vector<DirtyRect>& dirty) {
    const ClockReading& reading = clock_.reading();
    const HighFlyClockLayout& layout = sim_.clock_layout();
    std::string first;
    std::string second;
    if (reading.mode != ClockMode::CLOCK) {
        const ClockPrintText print = clock_print_text(reading);
        first = print.eta;
        second = print.left;
    } else {
        const ClockDateText date = clock_date_text(reading.date);
        first = date.date;
        second = date.weekday;
    }
    // Every box sits inside the clock bounds, which the sim repaints before the text changes.
    date_line_.draw(canvas(), first, layout.date_line, TEXT_COLOR, LV_OPA_80, LV_TEXT_ALIGN_LEFT,
                    under, dirty);
    day_line_.draw(canvas(), second, layout.day_line, TEXT_COLOR, LV_OPA_80, LV_TEXT_ALIGN_LEFT,
                   under, dirty);
    meridiem_.draw(canvas(), clock_meridiem_text(reading.time.meridiem), layout.meridiem,
                   TEXT_COLOR, LV_OPA_80, LV_TEXT_ALIGN_LEFT, under, dirty);
}

void HighFlyScreensaver::on_stop() {
    sim_.clear();
    date_line_.reset();
    day_line_.reset();
    meridiem_.reset();
}

void HighFlyScreensaver::on_level_request(size_t level) {
    apply_level(level);
    sim_.request_level(this->level());
}

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
