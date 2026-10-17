// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver_base.h"
#include "screensaver_canvas.h"
#include "screensaver_pipes_sim.h"

#include <cstdint>
#include <lvgl.h>
#include <optional>
#include <vector>

/**
 * @brief Windows NT-style 3D Pipes screensaver
 *
 * PipesSim grows shaded pipes through a node grid and rasterizes them with a z-buffer straight
 * into the canvas at the display's own depth, then dissolves the full screen and starts the next
 * one a few degrees round.
 */
class PipesScreensaver : public helix::ui::SaverBase {
  public:
    ScreensaverType type() const override {
        return ScreensaverType::PIPES_3D;
    }

  protected:
    std::optional<lv_color_format_t> canvas_format() const override {
        return helix::ui::SAVER_BUILD_CANVAS_FORMAT;
    }
    bool on_start() override;
    void on_frame(uint32_t dt_ms, std::vector<helix::ui::DirtyRect>& dirty) override;
    void on_stop() override;
    size_t ladder_size() const override {
        return helix::ui::PIPES_LEVEL_COUNT;
    }
    uint32_t ladder_period_ms(size_t level) const override {
        return helix::ui::PIPES_LEVELS[level].period_ms;
    }
    /// The period changes at once; the scale waits for the next screen.
    void on_level_request(size_t level) override;

  private:
    // Test-only seam: reads the simulation. See tests/test_helpers/screensaver_test_access.h.
    friend class PipesScreensaverTestAccess;

    helix::ui::PipesSim sim_;
};

#endif // HELIX_ENABLE_SCREENSAVER
