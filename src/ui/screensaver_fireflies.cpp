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

namespace {

const char* mode_name(helix::ui::FirefliesMode mode) {
    using helix::ui::FirefliesMode;
    switch (mode) {
    case FirefliesMode::STOP:
        return "stop";
    case FirefliesMode::LOOP:
        return "loop";
    case FirefliesMode::GLOW:
        return "glow";
    case FirefliesMode::HYPERSPEED:
        return "hyperspeed";
    case FirefliesMode::SPLIT:
        return "split";
    case FirefliesMode::MERGE:
        return "merge";
    case FirefliesMode::WIND_GUST:
        return "wind gust";
    default:
        return "normal";
    }
}

} // namespace

void helix::ui::FirefliesScreensaver::on_frame(uint32_t dt_ms, std::vector<DirtyRect>& dirty) {
    uint32_t begun[FIREFLIES_MODE_COUNT];
    for (size_t m = 0; m < FIREFLIES_MODE_COUNT; m++) {
        begun[m] = sim_.modes_begun(static_cast<FirefliesMode>(m));
    }
    FrameTarget frame = canvas().frame();
    sim_.step(dt_ms, frame, rng(), dirty);
    for (size_t m = 0; m < FIREFLIES_MODE_COUNT; m++) {
        if (sim_.modes_begun(static_cast<FirefliesMode>(m)) != begun[m]) {
            spdlog::debug("[Screensaver] Fireflies mode: {}",
                          mode_name(static_cast<FirefliesMode>(m)));
        }
    }
}

void helix::ui::FirefliesScreensaver::on_level_request(size_t level) {
    apply_level(level);
    sim_.request_level(this->level());
}

void helix::ui::FirefliesScreensaver::on_stop() {
    sim_.clear();
}

#endif // HELIX_ENABLE_SCREENSAVER
