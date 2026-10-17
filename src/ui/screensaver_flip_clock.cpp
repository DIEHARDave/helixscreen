// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver_flip_clock.h"

#include "lvgl/src/others/translation/lv_translation.h"
#include "screensaver_pixel_writer.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>

namespace helix::ui {

namespace {

/// Caption and marker text: dimmer than the digits, so the cards stay the subject.
constexpr Rgb TEXT_COLOR{150, 150, 150};

} // namespace

ClockInput FlipClockScreensaver::read_input() const {
    return source_ ? source_() : saver_clock_now();
}

bool FlipClockScreensaver::on_start() {
    spdlog::info("[Screensaver] Starting flip clock");
    state_.reset();
    state_.step(0, read_input());
    FrameTarget frame = canvas().frame();
    sim_.init(frame, level(), state_.face());
    caption_.reset();
    meridiem_.reset();
    std::vector<DirtyRect> dirty;
    draw_texts({}, dirty);
    // The canvas was invalidated whole when it was created, so the first refresh shows all of
    // this without the boxes.
    return true;
}

void FlipClockScreensaver::on_frame(uint32_t dt_ms, std::vector<DirtyRect>& dirty) {
    state_.step(dt_ms, read_input());
    FrameTarget frame = canvas().frame();
    sim_.step(dt_ms, frame, state_.face(), dirty);
    const std::vector<DirtyRect> card_dirty = dirty;
    draw_texts(card_dirty, dirty);
}

void FlipClockScreensaver::on_stop() {
    sim_.clear();
    caption_.reset();
    meridiem_.reset();
}

void FlipClockScreensaver::on_level_request(size_t level) {
    sim_.request_level(level);
    apply_level(level);
}

std::string FlipClockScreensaver::caption_text(const FlipFace& face) const {
    switch (face.caption) {
    case FlipCaption::DONE_AT: {
        return fmt::format(fmt::runtime(lv_tr("done at {} · {}%")),
                           clock_minute_text(face.eta_minute_of_day), face.progress_pct);
    }
    case FlipCaption::PAUSED:
        return lv_tr("PAUSED");
    case FlipCaption::DONE:
        return lv_tr("DONE");
    case FlipCaption::NONE:
        break;
    }
    return {};
}

std::string FlipClockScreensaver::meridiem_text(const FlipFace& face) const {
    return clock_meridiem_text(face.meridiem);
}

void FlipClockScreensaver::draw_texts(const std::vector<DirtyRect>& card_dirty,
                                      std::vector<DirtyRect>& dirty) {
    const FlipFace face = state_.face();
    const FlipLayout& layout = sim_.layout();
    draw_text(caption_, caption_text(face), layout.caption, card_dirty, false, dirty);
    draw_text(meridiem_, meridiem_text(face), layout.meridiem, card_dirty, true, dirty);
}

void FlipClockScreensaver::draw_text(SaverLabel& label, const std::string& text,
                                     const DirtyRect& box, const std::vector<DirtyRect>& card_dirty,
                                     bool on_card, std::vector<DirtyRect>& dirty) {
    const bool covered = std::any_of(card_dirty.begin(), card_dirty.end(),
                                     [&box](const DirtyRect& r) { return r.intersects(box); });
    if (text == label.text() && box == label.box() && !covered) {
        return;
    }

    // Old text goes first: the caption sits on the black background, the marker on the first
    // card, which the sim draws again.
    FrameTarget frame = canvas().frame();
    if (!label.text().empty() && !covered) {
        if (on_card) {
            sim_.redraw_card(frame, 0, dirty);
        } else {
            const DirtyRect& old = label.box();
            PixelWriter writer(frame);
            for (int32_t y = old.y1; y <= old.y2; y++) {
                for (int32_t x = old.x1; x <= old.x2; x++) {
                    if (writer.contains(x, y)) {
                        writer.put(x, y, FlipClockSim::BACKGROUND);
                    }
                }
            }
            dirty.push_back(old);
        }
    }
    label.draw(canvas(), text, box, TEXT_COLOR, LV_OPA_COVER,
               on_card ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, card_dirty, dirty);
}

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
