// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver_base.h"
#include "screensaver_canvas.h"
#include "screensaver_maze_sim.h"

#include <cstdint>
#include <lvgl.h>
#include <optional>
#include <vector>

namespace helix::ui {

/**
 * @brief Windows 95-style 3D Maze screensaver
 *
 * MazeSim walks a random maze and raycasts it Wolfenstein-style straight into the
 * canvas at the display's own depth; every frame repaints the whole canvas. The Helix icon
 * stands where the original placed OpenGL logos, and picture walls show the printer's image.
 */
class MazeScreensaver : public SaverBase {
  public:
    ScreensaverType type() const override {
        return ScreensaverType::MAZE_3D;
    }

  protected:
    std::optional<lv_color_format_t> canvas_format() const override {
        return SAVER_BUILD_CANVAS_FORMAT;
    }
    bool on_start() override;
    void on_frame(uint32_t dt_ms, std::vector<DirtyRect>& dirty) override;
    void on_stop() override;
    size_t ladder_size() const override {
        return MAZE_LEVEL_COUNT;
    }
    uint32_t ladder_period_ms(size_t level) const override {
        return MAZE_LEVELS[level].period_ms;
    }
    void on_level_request(size_t level) override;

  private:
    MazeSim sim_;
};

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
