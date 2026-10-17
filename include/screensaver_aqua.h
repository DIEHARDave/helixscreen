// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver.h"
#include "screensaver_aqua_sim.h"
#include "screensaver_base.h"
#include "screensaver_clock.h"

#include <cstdint>
#include <functional>
#include <lvgl.h>
#include <optional>
#include <string>
#include <vector>

namespace helix::ui {

/**
 * @brief Aqua Surface: glassy water with a ring clock in it
 *
 * helix::ui::AquaSim draws the water, the rings and their digits; this class reads the clock
 * and the print, and draws the date or print line with lv_draw_label, so the canvas takes the
 * pipes format (see FLIP_CLOCK_CANVAS_FORMAT).
 */
class AquaScreensaver : public SaverBase {
  public:
    using InputSource = std::function<ClockInput()>;

    ScreensaverType type() const override {
        return ScreensaverType::AQUA;
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
        return PIPES_CANVAS_FORMAT;
    }
    bool on_start() override;
    void on_frame(uint32_t dt_ms, std::vector<DirtyRect>& dirty) override;
    void on_stop() override;
    size_t ladder_size() const override {
        return AQUA_LEVEL_COUNT;
    }
    uint32_t ladder_period_ms(size_t level) const override {
        return AQUA_LEVELS[level].period_ms;
    }
    void on_level_request(size_t level) override;

  private:
    friend class AquaScreensaverTestAccess;

    ClockInput read_input() const;
    /// The date or print line for the current reading.
    const std::string& current_text();

    InputSource source_;
    ClockState clock_;
    AquaSim sim_;
    SaverLabel line_;
    /// The line's text and the reading it was built for, rebuilt only when that changes.
    std::string text_;
    ClockReading text_for_;
    bool text_built_ = false;
    std::vector<DirtyRect> under_;
};

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
