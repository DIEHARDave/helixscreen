// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "../test_helpers/screensaver_test_access.h"
#include "screensaver_fireflies_sim.h"
#include "screensaver_pixel_writer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>
#include <set>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::DirtyRect;
using helix::ui::FIREFLIES_MAX_BAITS;
using helix::ui::FirefliesSim;
using helix::ui::FireflyVec;
using helix::ui::FrameTarget;
using helix::ui::PixelFormat;

namespace {

constexpr uint32_t W = 320;
constexpr uint32_t H = 200;
constexpr uint32_t FRAME_MS = 33;

uint32_t bytes_per_pixel(PixelFormat format) {
    return format == PixelFormat::RGB565 ? 2 : 4;
}

/// A heap frame whose rows are wider than w * bytes per pixel, as LVGL's aligned strides can be.
struct SimFrame {
    std::vector<uint8_t> bytes;
    FrameTarget target;

    SimFrame(PixelFormat format, uint32_t w = W, uint32_t h = H) {
        const uint32_t bpp = bytes_per_pixel(format);
        const uint32_t stride = w * bpp + 3 * bpp;
        bytes.assign(static_cast<size_t>(stride) * h, 0);
        target = {bytes.data(), stride, w, h, format};
    }
    SimFrame(const SimFrame&) = delete;
    SimFrame& operator=(const SimFrame&) = delete;
};

/// A sim and its frame.
struct Swarm {
    std::minstd_rand rng;
    FirefliesSim sim;
    SimFrame frame;
    std::vector<DirtyRect> dirty;

    Swarm(uint32_t seed, size_t level = 0, PixelFormat format = PixelFormat::XRGB8888,
          uint32_t w = W, uint32_t h = H)
        : rng(seed), frame(format, w, h) {
        sim.init(frame.target, rng, level);
    }

    void step(uint32_t dt_ms = FRAME_MS) {
        sim.step(dt_ms, frame.target, rng, dirty);
    }

    /// Steps `frame_ms` frames until `ms` have passed.
    void run_for(uint32_t ms, uint32_t frame_ms = FRAME_MS) {
        for (uint32_t t = 0; t < ms; t += frame_ms) {
            step(frame_ms);
        }
    }
};

bool same_vec(const FireflyVec& a, const FireflyVec& b) {
    return screensaver_same_bits(a.x, b.x) && screensaver_same_bits(a.y, b.y) &&
           screensaver_same_bits(a.z, b.z);
}

/// True when every bait and fly of the two sims sits at the same bits.
bool same_positions(const FirefliesSim& a, const FirefliesSim& b) {
    if (a.flies().size() != b.flies().size() || a.baits().size() != b.baits().size()) {
        return false;
    }
    for (size_t i = 0; i < a.flies().size(); i++) {
        if (!same_vec(a.flies()[i].pos, b.flies()[i].pos)) {
            return false;
        }
    }
    for (size_t i = 0; i < a.baits().size(); i++) {
        if (a.baits()[i].alive != b.baits()[i].alive ||
            !same_vec(a.baits()[i].pos, b.baits()[i].pos)) {
            return false;
        }
    }
    return true;
}

} // namespace

// ============================================================================
// FirefliesSim
// ============================================================================

TEST_CASE("a fireflies population has 2 to 5 baits and 100 to 175 flies, each chasing a live bait",
          "[screensaver][fireflies_sim]") {
    std::set<size_t> bait_counts;
    std::set<size_t> fly_counts;
    const float half_w = 40.0f * static_cast<float>(W) / static_cast<float>(H);
    for (uint32_t seed = 1; seed <= 200; seed++) {
        Swarm s(seed);
        const FirefliesSim& sim = s.sim;
        REQUIRE(sim.baits_alive() >= 2);
        REQUIRE(sim.baits_alive() <= 5);
        REQUIRE(sim.flies().size() >= 100);
        REQUIRE(sim.flies().size() <= 175);
        bait_counts.insert(sim.baits_alive());
        fly_counts.insert(sim.flies().size());
        for (const FirefliesSim::Fly& f : sim.flies()) {
            REQUIRE(f.bait < sim.baits().size());
            REQUIRE(sim.baits()[f.bait].alive);
            REQUIRE(f.pace >= 0.75f);
            REQUIRE(f.pace <= 1.0f);
        }
        for (const FirefliesSim::Bait& b : sim.baits()) {
            if (!b.alive) {
                continue;
            }
            REQUIRE(std::fabs(b.pos.x) <= half_w);
            REQUIRE(std::fabs(b.pos.y) <= 40.0f);
            REQUIRE(std::fabs(b.pos.z) <= 25.0f);
        }
    }
    CHECK(bait_counts.count(2) == 1);
    CHECK(bait_counts.count(5) == 1);
    CHECK(fly_counts.size() >= 60);
}

TEST_CASE("a fireflies swarm replays exactly from a seed, and another seed differs",
          "[screensaver][fireflies_sim]") {
    Swarm a(7);
    Swarm b(7);
    Swarm c(8);
    a.run_for(3000);
    b.run_for(3000);
    c.run_for(3000);
    CHECK(same_positions(a.sim, b.sim));
    CHECK_FALSE(same_positions(a.sim, c.sim));
}

TEST_CASE("a cleared fireflies sim holds nothing", "[screensaver][fireflies_sim]") {
    Swarm s(3);
    CHECK(s.sim.heap_bytes() > 0);
    CHECK(s.sim.baits().size() == FIREFLIES_MAX_BAITS);
    s.sim.clear();
    CHECK(s.sim.heap_bytes() == 0);
}

#endif // HELIX_ENABLE_SCREENSAVER
