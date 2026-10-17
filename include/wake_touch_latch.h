// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

namespace helix {

/**
 * @brief Hides the touch that wakes the display from LVGL until the finger lifts
 *
 * The touch that wakes a sleeping, dimmed or screensaver-covered display is only a
 * request to see the screen. Once a press wakes it, every read of that touch is
 * reported as released, at the point the pointer last rested, so LVGL never sees a
 * press, a drag or a long press it could act on. The latch clears on the first raw
 * release; the next touch is an ordinary one.
 *
 * One latch per pointer device: it tracks a single finger's down/up sequence. Pure, and
 * fed from that device's read callback on the main thread.
 */
class WakeTouchLatch {
  public:
    struct Sample {
        bool pressed = false;
        int32_t x = 0;
        int32_t y = 0;
    };

    /**
     * @brief Rewrite one raw read
     *
     * @param press_wakes Called as `bool(x, y)` for a press that is not already being
     *        swallowed; true means this press is waking the display.
     * @return The sample to hand LVGL.
     */
    template <typename PressWakes> Sample filter(Sample raw, PressWakes&& press_wakes) {
        if (m_swallowing) {
            if (raw.pressed) {
                return {false, m_rest.x, m_rest.y};
            }
            m_swallowing = false;
        } else if (raw.pressed && press_wakes(raw.x, raw.y)) {
            m_swallowing = true;
            return {false, m_rest.x, m_rest.y};
        }
        if (!raw.pressed) {
            m_rest = raw;
        }
        return raw;
    }

    bool swallowing() const {
        return m_swallowing;
    }

  private:
    bool m_swallowing = false;
    Sample m_rest{};
};

} // namespace helix
