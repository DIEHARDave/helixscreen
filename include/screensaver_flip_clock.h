// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef HELIX_ENABLE_SCREENSAVER

#include "print_lifecycle_state.h"
#include "screensaver.h"
#include "screensaver_base.h"
#include "screensaver_clock.h"
#include "screensaver_flip_clock_sim.h"

#include <cstdint>
#include <functional>
#include <lvgl.h>
#include <optional>
#include <string>
#include <vector>

namespace helix::ui {

/**
 * @brief Color format of the flip clock canvas
 *
 * The cards are written pixel by pixel, but the caption and the AM/PM marker are drawn with
 * lv_draw_label, so the canvas takes LV_DRAW_CANVAS_FORMAT for the reason it gives: a 32 bpp
 * blend must land on real alpha. PixelWriter's XRGB8888 path writes an alpha
 * byte of 0xFF, which is an opaque ARGB8888 pixel.
 */
inline constexpr lv_color_format_t FLIP_CLOCK_CANVAS_FORMAT = LV_DRAW_CANVAS_FORMAT;

/**
 * @brief Split-flap clock that counts a running print down
 *
 * helix::ui::FlipClockState decides what to show and helix::ui::FlipClockSim draws the cards;
 * this class reads the clock and the print, and draws the caption and AM/PM marker in the
 * user's language and time format.
 */
class FlipClockScreensaver : public SaverBase {
  public:
    using InputSource = std::function<ClockInput()>;

    ScreensaverType type() const override {
        return ScreensaverType::FLIP_CLOCK;
    }

    /// Where each frame reads the time and the print; unset reads saver_clock_now().
    void set_input_source(InputSource source) {
        source_ = std::move(source);
    }

    const FlipClockState& state() const {
        return state_;
    }

  protected:
    std::optional<lv_color_format_t> canvas_format() const override {
        return FLIP_CLOCK_CANVAS_FORMAT;
    }
    bool on_start() override;
    void on_frame(uint32_t dt_ms, std::vector<DirtyRect>& dirty) override;
    void on_stop() override;
    size_t ladder_size() const override {
        return FLIP_CLOCK_LEVEL_COUNT;
    }
    uint32_t ladder_period_ms(size_t level) const override {
        return FLIP_CLOCK_LEVELS[level].period_ms;
    }
    void on_level_request(size_t level) override;

  private:
    ClockInput read_input() const;
    /// Redraws the caption and marker when their text changes or the cards drew over them.
    void draw_texts(const std::vector<DirtyRect>& card_dirty, std::vector<DirtyRect>& dirty);
    void draw_text(SaverLabel& label, const std::string& text, const DirtyRect& box,
                   const std::vector<DirtyRect>& card_dirty, bool on_card,
                   std::vector<DirtyRect>& dirty);
    std::string caption_text(const FlipFace& face) const;
    std::string meridiem_text(const FlipFace& face) const;

    InputSource source_;
    FlipClockState state_;
    FlipClockSim sim_;
    SaverLabel caption_;
    SaverLabel meridiem_;
};

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
