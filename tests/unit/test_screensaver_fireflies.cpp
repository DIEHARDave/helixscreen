// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "../lvgl_test_fixture.h"
#include "../test_helpers/screensaver_test_access.h"
#include "screensaver_canvas.h"
#include "screensaver_fireflies.h"
#include "screensaver_fireflies_sim.h"
#include "screensaver_pixel_writer.h"
#include "screensaver_registry.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::DirtyRect;
using helix::ui::FIREFLIES_LEVEL_COUNT;
using helix::ui::FIREFLIES_LEVELS;
using helix::ui::FIREFLIES_MAX_BAITS;
using helix::ui::FirefliesScreensaver;
using helix::ui::FirefliesSim;
using helix::ui::FireflyVec;
using helix::ui::FrameTarget;
using helix::ui::PixelFormat;

namespace helix::ui {

/// Reads the sim a FirefliesScreensaver owns.
class FirefliesScreensaverTestAccess {
  public:
    static const FirefliesSim& sim(const FirefliesScreensaver& saver) {
        return saver.sim_;
    }
};

} // namespace helix::ui

using helix::ui::FirefliesScreensaverTestAccess;

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

void fire(lv_timer_t* timer) {
    REQUIRE(timer != nullptr);
    REQUIRE(timer->timer_cb != nullptr);
    timer->timer_cb(timer);
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

TEST_CASE("fireflies move identically at 16 ms and 48 ms frames", "[screensaver][fireflies_sim]") {
    Swarm a(11);
    Swarm b(11);
    for (int i = 0; i < 300; i++) {
        a.step(16);
    }
    for (int i = 0; i < 100; i++) {
        b.step(48);
    }
    REQUIRE(a.sim.elapsed_ms() == 4800);
    REQUIRE(b.sim.elapsed_ms() == 4800);
    CHECK(same_positions(a.sim, b.sim));
    // The flies did move, so matching positions mean matching motion.
    Swarm still(11);
    CHECK_FALSE(same_positions(a.sim, still.sim));
}

TEST_CASE("a fireflies level change leaves the motion untouched", "[screensaver][fireflies_sim]") {
    Swarm a(12);
    Swarm b(12);
    a.run_for(2000);
    b.run_for(2000);
    b.sim.request_level(helix::ui::FIREFLIES_LEVEL_COUNT - 1);
    a.run_for(4000);
    b.run_for(4000);
    REQUIRE(b.sim.level() == helix::ui::FIREFLIES_LEVEL_COUNT - 1);
    CHECK(a.sim.elapsed_ms() == b.sim.elapsed_ms());
    CHECK(same_positions(a.sim, b.sim));
}

namespace {

float distance(const FireflyVec& a, const FireflyVec& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float length(const FireflyVec& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

float mean_distance_to_bait(const FirefliesSim& sim) {
    double sum = 0;
    for (const FirefliesSim::Fly& f : sim.flies()) {
        sum += distance(f.pos, sim.baits()[f.bait].pos);
    }
    return static_cast<float>(sum / static_cast<double>(sim.flies().size()));
}

} // namespace

TEST_CASE("flies close in on a stopped bait", "[screensaver][fireflies_sim]") {
    Swarm s(13);
    REQUIRE(s.sim.begin_mode(helix::ui::FirefliesMode::STOP, 10000, s.rng));
    const float start_mean = mean_distance_to_bait(s.sim);
    REQUIRE(start_mean > 30.0f);
    s.run_for(8000);
    REQUIRE(s.sim.mode() == helix::ui::FirefliesMode::STOP);
    const float end_mean = mean_distance_to_bait(s.sim);
    CHECK(end_mean <= 25.0f);
    CHECK(end_mean <= 0.5f * start_mean);
}

TEST_CASE("baits and flies keep to their top speeds", "[screensaver][fireflies_sim]") {
    using helix::ui::FIREFLIES_BAIT_SPEED;
    using helix::ui::FIREFLIES_FLY_SPEED;
    Swarm s(14);
    const float half_w = 40.0f * static_cast<float>(W) / static_cast<float>(H);
    float fastest_fly = 0.0f;
    for (int i = 0; i < 6000; i++) {
        s.step(helix::ui::FIREFLIES_STEP_MS);
        const float mult = s.sim.mode() == helix::ui::FirefliesMode::HYPERSPEED ? 3.0f : 1.0f;
        for (const FirefliesSim::Bait& b : s.sim.baits()) {
            if (!b.alive) {
                continue;
            }
            REQUIRE(length(b.vel) <= FIREFLIES_BAIT_SPEED * mult + 1e-3f);
            REQUIRE(std::fabs(b.pos.x) <= half_w + 1.0f);
            REQUIRE(std::fabs(b.pos.y) <= 40.0f + 1.0f);
            REQUIRE(std::fabs(b.pos.z) <= 25.0f + 1.0f);
        }
        for (const FirefliesSim::Fly& f : s.sim.flies()) {
            const float speed = length(f.vel);
            REQUIRE(speed <= FIREFLIES_FLY_SPEED * f.pace * mult + 1e-3f);
            fastest_fly = std::max(fastest_fly, speed);
        }
    }
    // The flies reached their top speeds, so the cap is what held them.
    CHECK(fastest_fly > 0.7f * FIREFLIES_FLY_SPEED);
}

TEST_CASE("a stall advances the swarm a capped step", "[screensaver][fireflies_sim]") {
    Swarm s(15);
    s.step(5000);
    CHECK(s.sim.elapsed_ms() == 500);
}

// ============================================================================
// FirefliesScreensaver
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture,
                 "the fireflies saver draws on a canvas in the build's format with its ladder",
                 "[screensaver][screensaver_fireflies]") {
    FirefliesScreensaver ss;
    ScreensaverStopOnExit<FirefliesScreensaver> stop_on_exit{ss};
    ss.start();
    REQUIRE(ss.is_active());
    const lv_draw_buf_t* buf = lv_canvas_get_draw_buf(SaverTestAccess::canvas(ss));
    REQUIRE(buf != nullptr);
    CHECK(buf->header.cf == helix::ui::SAVER_BUILD_CANVAS_FORMAT);
    CHECK(ss.level_count() == FIREFLIES_LEVEL_COUNT);
    CHECK(FIREFLIES_LEVELS[0].period_ms == helix::ui::SAVER_FAST_PERIOD);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a fireflies level request applies at once, frame period included",
                 "[screensaver][screensaver_fireflies]") {
    FirefliesScreensaver ss;
    ScreensaverStopOnExit<FirefliesScreensaver> stop_on_exit{ss};
    SaverTestAccess::set_fixed_seed(ss, 4);
    ss.start();
    REQUIRE(ss.is_active());

    const size_t last = FIREFLIES_LEVEL_COUNT - 1;
    ss.request_level(last);
    CHECK(ss.level() == last);
    CHECK(SaverTestAccess::timer(ss)->period == FIREFLIES_LEVELS[last].period_ms);
    lv_tick_inc(100);
    fire(SaverTestAccess::timer(ss));
    CHECK(ss.is_active());
    CHECK(FirefliesScreensaverTestAccess::sim(ss).level() == last);
}

TEST_CASE_METHOD(LVGLTestFixture, "a stopped fireflies saver holds no buffers",
                 "[screensaver][screensaver_fireflies]") {
    FirefliesScreensaver ss;
    CHECK(FirefliesScreensaverTestAccess::sim(ss).heap_bytes() == 0);
    {
        ScreensaverStopOnExit<FirefliesScreensaver> stop_on_exit{ss};
        ss.start();
        REQUIRE(ss.is_active());
        lv_tick_inc(16);
        fire(SaverTestAccess::timer(ss));
        REQUIRE(FirefliesScreensaverTestAccess::sim(ss).heap_bytes() > 0);
    }
    CHECK_FALSE(ss.is_active());
    CHECK(FirefliesScreensaverTestAccess::sim(ss).heap_bytes() == 0);
}

TEST_CASE("fireflies is registered by name at both depths",
          "[screensaver][screensaver_fireflies]") {
    const helix::ui::ScreensaverInfo* info = helix::ui::find_screensaver_by_name("fireflies");
    REQUIRE(info != nullptr);
    CHECK(info->type == ScreensaverType::FIREFLIES);
    CHECK(std::string(info->label_key) == "Fireflies");
    CHECK((info->depths & helix::ui::SAVER_DEPTH_16) != 0);
    CHECK((info->depths & helix::ui::SAVER_DEPTH_32) != 0);
}

#endif // HELIX_ENABLE_SCREENSAVER
