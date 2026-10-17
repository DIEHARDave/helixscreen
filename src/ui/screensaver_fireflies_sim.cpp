// SPDX-License-Identifier: GPL-3.0-or-later

#include "screensaver_fireflies_sim.h"

#include <algorithm>
#include <cmath>

namespace helix::ui {

namespace {

using screensaver::random_below;
using screensaver::unit_random;

// Baits wander a box inset by this share of each half-extent.
constexpr float WAYPOINT_INSET = 0.9f;
constexpr float FLY_PACE_MIN = 0.75f;
constexpr float SWARM_HUE_JITTER_DEG = 30.0f;

// Tail points are stored in fixed point, this many steps to a world unit.
constexpr float TAIL_FIXED_SCALE = 64.0f;

int16_t to_fixed(float v) {
    return static_cast<int16_t>(
        std::clamp(std::lround(v * TAIL_FIXED_SCALE), long{INT16_MIN}, long{INT16_MAX}));
}

float uniform(std::minstd_rand& rng, float lo, float hi) {
    return lo + (hi - lo) * unit_random(rng);
}

template <typename T> size_t bytes_of(const std::vector<T>& v) {
    return v.capacity() * sizeof(T);
}

/// Empties `v` and returns its storage, which clear() alone keeps.
template <typename T> void free_buffer(std::vector<T>& v) {
    std::vector<T>().swap(v);
}

} // namespace

void FirefliesSim::init(const FrameTarget& frame, std::minstd_rand& rng, size_t level) {
    clear();
    w_ = static_cast<int32_t>(frame.w);
    h_ = static_cast<int32_t>(frame.h);
    world_w_ = FIREFLIES_WORLD_H * static_cast<float>(w_) / static_cast<float>(std::max(h_, 1));
    level_ = std::min(level, FIREFLIES_LEVEL_COUNT - 1);
    pending_level_ = level_;

    const auto inside_box = [&]() {
        return FireflyVec{uniform(rng, -0.5f, 0.5f) * world_w_ * WAYPOINT_INSET,
                          uniform(rng, -0.5f, 0.5f) * FIREFLIES_WORLD_H * WAYPOINT_INSET,
                          uniform(rng, -0.5f, 0.5f) * FIREFLIES_WORLD_D * WAYPOINT_INSET};
    };

    baits_.assign(FIREFLIES_MAX_BAITS, Bait{});
    const size_t n =
        FIREFLIES_MIN_BAITS +
        static_cast<size_t>(random_below(rng, FIREFLIES_MAX_BAITS - FIREFLIES_MIN_BAITS + 1));
    for (size_t i = 0; i < n; i++) {
        Bait& b = baits_[i];
        b.alive = true;
        b.pos = inside_box();
        b.target = inside_box();
        b.hue_deg = 360.0f * static_cast<float>(i) / static_cast<float>(n) +
                    uniform(rng, 0.0f, SWARM_HUE_JITTER_DEG);
    }

    const size_t flies =
        FIREFLIES_MIN_FLIES +
        static_cast<size_t>(random_below(rng, FIREFLIES_MAX_FLIES - FIREFLIES_MIN_FLIES + 1));
    flies_.resize(flies);
    for (Fly& f : flies_) {
        f.pos = {uniform(rng, -0.5f, 0.5f) * world_w_,
                 uniform(rng, -0.5f, 0.5f) * FIREFLIES_WORLD_H,
                 uniform(rng, -0.5f, 0.5f) * FIREFLIES_WORLD_D};
        f.pace = uniform(rng, FLY_PACE_MIN, 1.0f);
        f.bait = static_cast<uint8_t>(random_below(rng, static_cast<int>(n)));
    }

    tail_.resize(flies * FIREFLIES_TAIL_POINTS * 3);
    for (size_t i = 0; i < flies; i++) {
        const int16_t fx = to_fixed(flies_[i].pos.x);
        const int16_t fy = to_fixed(flies_[i].pos.y);
        const int16_t fz = to_fixed(flies_[i].pos.z);
        int16_t* t = &tail_[i * FIREFLIES_TAIL_POINTS * 3];
        for (size_t p = 0; p < FIREFLIES_TAIL_POINTS; p++) {
            t[p * 3] = fx;
            t[p * 3 + 1] = fy;
            t[p * 3 + 2] = fz;
        }
    }
    tail_head_ = 0;
    tail_count_ = 1;
}

void FirefliesSim::clear() {
    free_buffer(baits_);
    free_buffer(flies_);
    free_buffer(tail_);
    tail_head_ = 0;
    tail_count_ = 0;
    steps_.reset();
    elapsed_ms_ = 0;
    for (DirtyRect& r : drawn_) {
        r = DirtyRect{};
    }
}

size_t FirefliesSim::heap_bytes() const {
    return bytes_of(baits_) + bytes_of(flies_) + bytes_of(tail_);
}

void FirefliesSim::request_level(size_t level) {
    pending_level_ = std::min(level, FIREFLIES_LEVEL_COUNT - 1);
}

size_t FirefliesSim::baits_alive() const {
    return static_cast<size_t>(
        std::count_if(baits_.begin(), baits_.end(), [](const Bait& b) { return b.alive; }));
}

void FirefliesSim::step(uint32_t dt_ms, FrameTarget& frame, std::minstd_rand& rng,
                        std::vector<DirtyRect>& dirty) {
    (void)frame;
    (void)rng;
    level_ = pending_level_;
    elapsed_ms_ += steps_.steps_due(dt_ms, FIREFLIES_STEP_MS, 50) * FIREFLIES_STEP_MS;
    dirty.clear();
}

} // namespace helix::ui
