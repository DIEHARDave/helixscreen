// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver_base.h"
#include "screensaver_canvas.h"
#include "screensaver_hyperspace_sim.h"

#include <cstdint>
#include <lvgl.h>
#include <optional>
#include <vector>

namespace helix::ui {

/**
 * @brief A flight through glowing rainbow space, after Hyperspace
 *
 * helix::ui::HyperspaceSim redraws the whole canvas every frame at the display's own depth,
 * RGB565 or XRGB8888, and reports one box over all of it. A level request applies at once.
 */
class HyperspaceScreensaver : public SaverBase {
  public:
    ScreensaverType type() const override {
        return ScreensaverType::HYPERSPACE;
    }

  protected:
    std::optional<lv_color_format_t> canvas_format() const override {
        return SAVER_BUILD_CANVAS_FORMAT;
    }
    bool on_start() override;
    void on_frame(uint32_t dt_ms, std::vector<DirtyRect>& dirty) override;
    void on_stop() override;
    size_t ladder_size() const override {
        return HYPERSPACE_LEVEL_COUNT;
    }
    uint32_t ladder_period_ms(size_t level) const override {
        return HYPERSPACE_LEVELS[level].period_ms;
    }
    void on_level_request(size_t level) override;

  private:
    HyperspaceSim sim_;
};

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
