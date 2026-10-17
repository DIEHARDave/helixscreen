// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver_fireflies.h"

#include <spdlog/spdlog.h>

using helix::ui::DirtyRect;
using helix::ui::FrameTarget;

bool helix::ui::FirefliesScreensaver::on_start() {
    spdlog::info("[Screensaver] Starting fireflies");
    const FrameTarget frame = canvas().frame();
    sim_.init(frame, rng(), level());
    spdlog::debug("[Screensaver] Fireflies holds {} KB at {}x{}", sim_.heap_bytes() / 1024, frame.w,
                  frame.h);
    return true;
}

void helix::ui::FirefliesScreensaver::on_frame(uint32_t dt_ms, std::vector<DirtyRect>& dirty) {
    FrameTarget frame = canvas().frame();
    sim_.step(dt_ms, frame, rng(), dirty);
}

void helix::ui::FirefliesScreensaver::on_level_request(size_t level) {
    apply_level(level);
    sim_.request_level(this->level());
}

void helix::ui::FirefliesScreensaver::on_stop() {
    sim_.clear();
}

#endif // HELIX_ENABLE_SCREENSAVER
