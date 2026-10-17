// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver_base.h"
#include "screensaver_canvas.h"
#include "screensaver_fireflies_sim.h"

#include <cstdint>
#include <lvgl.h>
#include <optional>
#include <vector>

namespace helix::ui {

/**
 * @brief Coloured swarms of fireflies chasing invisible baits, after Fireflies
 *
 * helix::ui::FirefliesSim draws into the canvas at the display's own depth, RGB565 or
 * XRGB8888, and reports the box it cleared and drew. A level request applies at once.
 */
class FirefliesScreensaver : public SaverBase {
  public:
    ScreensaverType type() const override {
        return ScreensaverType::FIREFLIES;
    }

  protected:
    std::optional<lv_color_format_t> canvas_format() const override {
        return SAVER_BUILD_CANVAS_FORMAT;
    }
    bool on_start() override;
    void on_frame(uint32_t dt_ms, std::vector<DirtyRect>& dirty) override;
    void on_stop() override;
    size_t ladder_size() const override {
        return FIREFLIES_LEVEL_COUNT;
    }
    uint32_t ladder_period_ms(size_t level) const override {
        return FIREFLIES_LEVELS[level].period_ms;
    }
    void on_level_request(size_t level) override;

  private:
    friend class FirefliesScreensaverTestAccess;

    FirefliesSim sim_;
};

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
