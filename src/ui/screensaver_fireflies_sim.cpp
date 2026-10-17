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

constexpr float STEP_S = static_cast<float>(FIREFLIES_STEP_MS) / 1000.0f;

// A bait takes a new waypoint once this near it, or when its timer runs out.
constexpr float WAYPOINT_REACHED = 5.0f;
constexpr int32_t RETARGET_MIN_MS = 2000;
constexpr int32_t RETARGET_MAX_MS = 5000;

FireflyVec operator+(FireflyVec a, FireflyVec b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
FireflyVec operator-(FireflyVec a, FireflyVec b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
FireflyVec operator*(FireflyVec a, float k) {
    return {a.x * k, a.y * k, a.z * k};
}
float length_sq(FireflyVec v) {
    return v.x * v.x + v.y * v.y + v.z * v.z;
}

/// `v` shortened to at most `max_len`.
FireflyVec clamp_length(FireflyVec v, float max_len) {
    const float sq = length_sq(v);
    if (sq <= max_len * max_len || sq == 0.0f) {
        return v;
    }
    return v * (max_len / std::sqrt(sq));
}

/**
 * @brief Steers `vel` toward `desired` for one step
 *
 * The change is capped at `accel` per second and the result at `speed`. There is no slow-down
 * on arrival, so a chaser swings through its target instead of settling on it.
 */
void steer(FireflyVec& vel, FireflyVec desired, float speed, float accel) {
    vel = clamp_length(vel + clamp_length(desired - vel, accel) * STEP_S, speed);
}

/// Velocity of top speed `speed` from `from` toward `to`; zero when they coincide.
FireflyVec seek(FireflyVec from, FireflyVec to, float speed) {
    const FireflyVec d = to - from;
    const float sq = length_sq(d);
    return sq == 0.0f ? FireflyVec{} : d * (speed / std::sqrt(sq));
}

int32_t random_ms(std::minstd_rand& rng, int32_t lo, int32_t hi) {
    return lo + random_below(rng, hi - lo + 1);
}

} // namespace

FireflyVec FirefliesSim::random_waypoint(std::minstd_rand& rng) const {
    return {uniform(rng, -0.5f, 0.5f) * world_w_ * WAYPOINT_INSET,
            uniform(rng, -0.5f, 0.5f) * FIREFLIES_WORLD_H * WAYPOINT_INSET,
            uniform(rng, -0.5f, 0.5f) * FIREFLIES_WORLD_D * WAYPOINT_INSET};
}

float FirefliesSim::speed_mult() const {
    return mode_ == FirefliesMode::HYPERSPEED ? 3.0f : 1.0f;
}

void FirefliesSim::init(const FrameTarget& frame, std::minstd_rand& rng, size_t level) {
    clear();
    w_ = static_cast<int32_t>(frame.w);
    h_ = static_cast<int32_t>(frame.h);
    world_w_ = FIREFLIES_WORLD_H * static_cast<float>(w_) / static_cast<float>(std::max(h_, 1));
    level_ = std::min(level, FIREFLIES_LEVEL_COUNT - 1);
    pending_level_ = level_;

    baits_.assign(FIREFLIES_MAX_BAITS, Bait{});
    const size_t n =
        FIREFLIES_MIN_BAITS +
        static_cast<size_t>(random_below(rng, FIREFLIES_MAX_BAITS - FIREFLIES_MIN_BAITS + 1));
    for (size_t i = 0; i < n; i++) {
        Bait& b = baits_[i];
        b.alive = true;
        b.pos = random_waypoint(rng);
        b.target = random_waypoint(rng);
        b.retarget_ms = random_ms(rng, RETARGET_MIN_MS, RETARGET_MAX_MS);
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
    mode_ = FirefliesMode::NORMAL;
    mode_left_ms_ = 0;
    mode_total_ms_ = 0;
    std::fill(std::begin(modes_begun_), std::end(modes_begun_), 0u);
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

uint32_t FirefliesSim::modes_begun(FirefliesMode mode) const {
    return modes_begun_[static_cast<size_t>(mode)];
}

bool FirefliesSim::begin_mode(FirefliesMode mode, uint32_t duration_ms, std::minstd_rand& rng) {
    (void)rng;
    switch (mode) {
    case FirefliesMode::STOP:
        for (Bait& b : baits_) {
            b.target = b.pos;
        }
        break;
    default:
        break;
    }
    mode_ = mode;
    mode_total_ms_ = static_cast<int32_t>(duration_ms);
    mode_left_ms_ = mode_total_ms_;
    modes_begun_[static_cast<size_t>(mode)]++;
    return true;
}

void FirefliesSim::advance_one(std::minstd_rand& rng) {
    if (mode_ != FirefliesMode::NORMAL) {
        mode_left_ms_ -= static_cast<int32_t>(FIREFLIES_STEP_MS);
        if (mode_left_ms_ <= 0) {
            mode_ = FirefliesMode::NORMAL;
        }
    }

    const float mult = speed_mult();
    const float bait_speed = FIREFLIES_BAIT_SPEED * mult;
    const float bait_accel = FIREFLIES_BAIT_ACCEL * mult;
    const FireflyVec half{world_w_ / 2.0f, FIREFLIES_WORLD_H / 2.0f, FIREFLIES_WORLD_D / 2.0f};
    for (Bait& b : baits_) {
        if (!b.alive) {
            continue;
        }
        FireflyVec desired{};
        if (mode_ != FirefliesMode::STOP) {
            b.retarget_ms -= static_cast<int32_t>(FIREFLIES_STEP_MS);
            if (b.retarget_ms <= 0 ||
                length_sq(b.target - b.pos) < WAYPOINT_REACHED * WAYPOINT_REACHED) {
                b.target = random_waypoint(rng);
                b.retarget_ms = random_ms(rng, RETARGET_MIN_MS, RETARGET_MAX_MS);
            }
            desired = seek(b.pos, b.target, bait_speed);
        }
        steer(b.vel, desired, bait_speed, bait_accel);
        b.pos = b.pos + b.vel * STEP_S;
        // The walls of the box stop a bait that overshoots its waypoint.
        float* p[3] = {&b.pos.x, &b.pos.y, &b.pos.z};
        float* v[3] = {&b.vel.x, &b.vel.y, &b.vel.z};
        const float lim[3] = {half.x, half.y, half.z};
        for (int a = 0; a < 3; a++) {
            if (std::fabs(*p[a]) > lim[a]) {
                *p[a] = std::copysign(lim[a], *p[a]);
                *v[a] = 0.0f;
            }
        }
    }

    for (Fly& f : flies_) {
        const float speed = FIREFLIES_FLY_SPEED * f.pace * mult;
        steer(f.vel, seek(f.pos, baits_[f.bait].pos, speed), speed,
              FIREFLIES_FLY_ACCEL * f.pace * mult);
        f.pos = f.pos + f.vel * STEP_S;
    }

    elapsed_ms_ += FIREFLIES_STEP_MS;
}

void FirefliesSim::step(uint32_t dt_ms, FrameTarget& frame, std::minstd_rand& rng,
                        std::vector<DirtyRect>& dirty) {
    (void)frame;
    level_ = pending_level_;
    const uint32_t steps = steps_.steps_due(dt_ms, FIREFLIES_STEP_MS, MAX_STEPS_PER_FRAME);
    for (uint32_t i = 0; i < steps; i++) {
        advance_one(rng);
    }
    dirty.clear();
}

} // namespace helix::ui
