// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver_aqua.h"

#include <spdlog/spdlog.h>

#include <string>

namespace helix::ui {

namespace {

/// The date or print line: a little dimmer than the digits in the rings.
constexpr Rgb LINE_COLOR{200, 206, 212};

/// "ETA 9:41 PM · 1h 12m left" while a print runs, else "October 21, 2019 · Monday".
std::string line_text(const ClockReading& reading) {
    const auto join = [](const std::string& a, const std::string& b) {
        if (a.empty() || b.empty()) {
            return a.empty() ? b : a;
        }
        return a + " · " + b;
    };
    if (reading.mode != ClockMode::CLOCK) {
        const ClockPrintText print = clock_print_text(reading);
        return join(print.eta, print.left);
    }
    const ClockDateText date = clock_date_text(reading.date);
    return join(date.date, date.weekday);
}

} // namespace

ClockInput AquaScreensaver::read_input() const {
    return source_ ? source_() : saver_clock_now();
}

bool AquaScreensaver::on_start() {
    spdlog::info("[Screensaver] Starting aqua surface");
    clock_.reset();
    clock_.step(0, read_input());
    const FrameTarget frame = canvas().frame();
    sim_.init(frame, rng(), level(), clock_.reading());
    spdlog::debug("[Screensaver] Aqua surface holds {} KB at {}x{}", sim_.heap_bytes() / 1024,
                  frame.w, frame.h);
    line_.reset();
    text_built_ = false;
    under_.reserve(SAVER_MAX_DIRTY_AREAS);
    std::vector<DirtyRect> dirty;
    line_.draw(canvas(), current_text(), sim_.clock_layout().text, LINE_COLOR, LV_OPA_90,
               LV_TEXT_ALIGN_CENTER, dirty, dirty);
    return true;
}

void AquaScreensaver::on_frame(uint32_t dt_ms, std::vector<DirtyRect>& dirty) {
    clock_.step(dt_ms, read_input());
    FrameTarget frame = canvas().frame();
    sim_.step(dt_ms, frame, rng(), clock_.reading(), dirty);
    const std::string& text = current_text();
    const DirtyRect& box = sim_.clock_layout().text;
    if (text != line_.text() || !(box == line_.box())) {
        sim_.restore(frame, line_.box(), dirty);
    }
    under_.assign(dirty.begin(), dirty.end());
    line_.draw(canvas(), text, box, LINE_COLOR, LV_OPA_90, LV_TEXT_ALIGN_CENTER, under_, dirty);
}

const std::string& AquaScreensaver::current_text() {
    if (!text_built_ || !clock_text_same(clock_.reading(), text_for_)) {
        text_ = line_text(clock_.reading());
        text_for_ = clock_.reading();
        text_built_ = true;
    }
    return text_;
}

void AquaScreensaver::on_stop() {
    sim_.clear();
    line_.reset();
    std::string().swap(text_);
    std::vector<DirtyRect>().swap(under_);
    text_built_ = false;
}

void AquaScreensaver::on_level_request(size_t level) {
    apply_level(level);
    sim_.request_level(this->level());
}

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
