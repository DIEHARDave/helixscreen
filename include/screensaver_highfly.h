// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver.h"
#include "screensaver_base.h"
#include "screensaver_clock.h"
#include "screensaver_highfly_sim.h"

#include <cstdint>
#include <functional>
#include <lvgl.h>
#include <optional>
#include <string>
#include <vector>

namespace helix::ui {

/**
 * @brief High Fly: over a sunlit cloud deck with a thin clock at the cloud line
 *
 * helix::ui::HighFlySim draws the sky, clouds and numerals; this class reads the clock
 * and the print, and draws the date block and AM/PM with lv_draw_label, so the canvas takes the
 * pipes format (see FLIP_CLOCK_CANVAS_FORMAT).
 */
class HighFlyScreensaver : public SaverBase {
  public:
    using InputSource = std::function<ClockInput()>;

    ScreensaverType type() const override {
        return ScreensaverType::HIGHFLY;
    }
    /// Where each frame reads the time and the print; unset reads saver_clock_now().
    void set_input_source(InputSource source) {
        source_ = std::move(source);
    }
    const ClockState& clock() const {
        return clock_;
    }

  protected:
    std::optional<lv_color_format_t> canvas_format() const override {
        return LV_DRAW_CANVAS_FORMAT;
    }
    bool on_start() override;
    void on_frame(uint32_t dt_ms, std::vector<DirtyRect>& dirty) override;
    void on_stop() override;
    size_t ladder_size() const override {
        return HIGHFLY_LEVEL_COUNT;
    }
    uint32_t ladder_period_ms(size_t level) const override {
        return HIGHFLY_LEVELS[level].period_ms;
    }
    void on_level_request(size_t level) override;

  private:
    friend class HighFlyScreensaverTestAccess;

    ClockInput read_input() const;
    /// The date block and AM/PM, put back wherever `under` repainted beneath them.
    void draw_texts(const std::vector<DirtyRect>& under, std::vector<DirtyRect>& dirty);

    InputSource source_;
    ClockState clock_;
    HighFlySim sim_;
    SaverLabel date_line_;
    SaverLabel day_line_;
    SaverLabel meridiem_;
    /// The texts and the reading they were built for, rebuilt only when that changes.
    std::string first_;
    std::string second_;
    std::string meridiem_text_;
    ClockReading texts_for_;
    bool texts_built_ = false;
    std::vector<DirtyRect> under_;
};

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
