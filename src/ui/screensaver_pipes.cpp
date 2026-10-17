// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver_pipes.h"

#include <spdlog/spdlog.h>

bool PipesScreensaver::on_start() {
    spdlog::info("[Screensaver] Starting 3D pipes");
    sim_.init(canvas().frame(), rng(), level());
    spdlog::debug("[Screensaver] Pipes started ({}x{}, level {}, scale {})", screen_w(), screen_h(),
                  level(), sim_.renderer().scale());
    return true;
}

void PipesScreensaver::on_frame(uint32_t dt_ms, std::vector<helix::ui::DirtyRect>& dirty) {
    sim_.step(dt_ms, canvas().frame(), rng(), dirty);
}

void PipesScreensaver::on_level_request(size_t level) {
    apply_level(level);
    sim_.set_level(this->level());
}

void PipesScreensaver::on_stop() {
    sim_.clear();
}

#endif // HELIX_ENABLE_SCREENSAVER
