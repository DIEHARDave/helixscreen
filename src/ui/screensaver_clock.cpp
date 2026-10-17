// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver_clock.h"

#include "display_settings_manager.h"
#include "theme_manager.h"

#include <algorithm>
#include <ctime>

namespace helix::ui {

namespace {

/// Breakpoint fonts a clock line may use, largest first.
constexpr const char* CLOCK_FONTS[] = {"font_heading", "font_body", "font_small", "font_xs"};

} // namespace

ClockInput saver_clock_now() {
    const time_t now = time(nullptr);
    struct tm tm_info {};
    localtime_r(&now, &tm_info);
    ClockInput in;
    in.seconds_of_day = tm_info.tm_hour * 3600 + tm_info.tm_min * 60 + tm_info.tm_sec;
    in.use_24h = DisplaySettingsManager::instance().get_time_format() == TimeFormat::HOUR_24;
    in.date.year = tm_info.tm_year + 1900;
    in.date.month = static_cast<uint8_t>(tm_info.tm_mon + 1);
    in.date.day = static_cast<uint8_t>(tm_info.tm_mday);
    in.date.weekday = static_cast<uint8_t>(tm_info.tm_wday);
    return in;
}

ClockPrintPhase clock_print_phase(PrintState state) {
    switch (state) {
    case PrintState::Preparing:
        return ClockPrintPhase::PREPARING;
    case PrintState::Printing:
        return ClockPrintPhase::PRINTING;
    case PrintState::Paused:
        return ClockPrintPhase::PAUSED;
    case PrintState::Complete:
        return ClockPrintPhase::COMPLETE;
    case PrintState::Idle:
    case PrintState::Cancelled:
    case PrintState::Error:
        break;
    }
    return ClockPrintPhase::NONE;
}

const lv_font_t* clock_font_fitting(int32_t height) {
    const lv_font_t* chosen = nullptr;
    for (const char* token : CLOCK_FONTS) {
        const lv_font_t* font = theme_manager_get_font(token);
        if (font == nullptr) {
            continue;
        }
        chosen = font;
        if (theme_manager_get_font_height(font) <= height) {
            break;
        }
    }
    return chosen;
}

bool SaverLabel::draw(SaverCanvas& canvas, const std::string& text, const DirtyRect& box, Rgb color,
                      lv_opa_t opa, lv_text_align_t align, const std::vector<DirtyRect>& under,
                      std::vector<DirtyRect>& dirty) {
    const bool covered = std::any_of(under.begin(), under.end(),
                                     [&box](const DirtyRect& r) { return r.intersects(box); });
    if (text == text_ && box == box_ && !covered) {
        return false;
    }
    text_ = text;
    box_ = box;
    if (text.empty() || box.empty()) {
        return false;
    }

    const lv_font_t* font = clock_font_fitting(box.y2 - box.y1 + 1);
    if (font == nullptr) {
        return false;
    }
    lv_layer_t layer;
    canvas.begin_layer(&layer);
    lv_draw_label_dsc_t label;
    lv_draw_label_dsc_init(&label);
    label.font = font;
    label.color = lv_color_make(color.r, color.g, color.b);
    label.opa = opa;
    label.align = align;
    label.text = text_.c_str();
    label.text_local = 1;
    const int32_t font_h = theme_manager_get_font_height(font);
    const int32_t top = box.y1 + std::max(0, (box.y2 - box.y1 + 1 - font_h) / 2);
    lv_area_t area{box.x1, top, box.x2, box.y2};
    canvas.mark_dirty(box.x1, box.y1, box.x2, box.y2);
    lv_draw_label(&layer, &label, &area);
    canvas.finish_layer(&layer);
    dirty.push_back(box);
    return true;
}

void SaverLabel::reset() {
    text_.clear();
    box_ = {};
}

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
