// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver_hyperspace.h"

#include <spdlog/spdlog.h>

using helix::ui::DirtyRect;
using helix::ui::FrameTarget;

bool helix::ui::HyperspaceScreensaver::on_start() {
    spdlog::info("[Screensaver] Starting hyperspace");
    const FrameTarget frame = canvas().frame();
    sim_.init(frame, rng(), level());
    spdlog::debug("[Screensaver] Hyperspace holds {} KB at {}x{}", sim_.heap_bytes() / 1024,
                  frame.w, frame.h);
    return true;
}

void helix::ui::HyperspaceScreensaver::on_frame(uint32_t dt_ms, std::vector<DirtyRect>& dirty) {
    FrameTarget frame = canvas().frame();
    sim_.step(dt_ms, frame, rng(), dirty);
}

void helix::ui::HyperspaceScreensaver::on_level_request(size_t level) {
    apply_level(level);
    sim_.request_level(this->level());
}

void helix::ui::HyperspaceScreensaver::on_stop() {
    sim_.clear();
}

#endif // HELIX_ENABLE_SCREENSAVER
