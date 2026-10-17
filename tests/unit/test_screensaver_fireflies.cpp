// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "../lvgl_test_fixture.h"
#include "../test_helpers/screensaver_cost_model.h"
#include "../test_helpers/screensaver_test_access.h"
#include "screensaver_canvas.h"
#include "screensaver_fireflies.h"
#include "screensaver_fireflies_sim.h"
#include "screensaver_pixel_writer.h"
#include "screensaver_registry.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <random>
#include <set>
#include <sstream>
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
using helix::ui::Rgb;

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

namespace {

/// `deg` wrapped into [-180, 180).
float wrap180(float deg) {
    float d = std::fmod(deg + 180.0f, 360.0f);
    if (d < 0.0f) {
        d += 360.0f;
    }
    return d - 180.0f;
}

} // namespace

TEST_CASE("each swarm's hue turns 15 degrees a second", "[screensaver][fireflies_sim]") {
    Swarm s(21);
    std::vector<float> before;
    for (const FirefliesSim::Bait& b : s.sim.baits()) {
        before.push_back(b.hue_deg);
    }
    s.run_for(2000, 10);
    REQUIRE(s.sim.elapsed_ms() == 2000);
    for (size_t i = 0; i < s.sim.baits().size(); i++) {
        const FirefliesSim::Bait& b = s.sim.baits()[i];
        if (!b.alive) {
            continue;
        }
        CAPTURE(i, before[i], b.hue_deg);
        CHECK(b.hue_deg >= 0.0f);
        CHECK(b.hue_deg < 360.0f);
        CHECK(std::fabs(wrap180(b.hue_deg - before[i] - 30.0f)) < 0.01f);
    }
}

TEST_CASE("a fly's hue swings 20 degrees either way with its speed",
          "[screensaver][fireflies_sim]") {
    Swarm s(23);
    s.run_for(3000);
    float lowest = 0.0f;
    float highest = 0.0f;
    for (size_t i = 0; i < s.sim.flies().size(); i++) {
        const FirefliesSim::Fly& f = s.sim.flies()[i];
        const float offset = wrap180(s.sim.fly_hue_deg(i) - s.sim.baits()[f.bait].hue_deg);
        const float expected =
            -20.0f + 40.0f * std::min(1.0f, length(f.vel) / helix::ui::FIREFLIES_FLY_SPEED);
        CAPTURE(i, offset, expected);
        REQUIRE(s.sim.fly_hue_deg(i) >= 0.0f);
        REQUIRE(s.sim.fly_hue_deg(i) < 360.0f);
        CHECK(std::fabs(offset - expected) < 0.01f);
        lowest = std::min(lowest, offset);
        highest = std::max(highest, offset);
    }
    CHECK(lowest < -5.0f);
    CHECK(highest > 5.0f);
}

TEST_CASE("a fly's tail grows to 2.25 s of history and stops there",
          "[screensaver][fireflies_sim]") {
    Swarm s(24);
    const auto all_ages_in = [&](uint32_t lo, uint32_t hi) {
        return s.sim.tail_age_ms() >= lo && s.sim.tail_age_ms() < hi;
    };
    CHECK(all_ages_in(0, 1));
    s.run_for(1000, 10);
    CHECK(all_ages_in(1000, 1001));
    s.run_for(4000, 10);
    REQUIRE(s.sim.elapsed_ms() == 5000);
    CHECK(all_ages_in(2250, 2300));
    s.run_for(40, 10);
    CHECK(all_ages_in(2250, 2300));
    CHECK(s.sim.tail_age_ms() == 2290);
    s.run_for(24960, 10);
    REQUIRE(s.sim.elapsed_ms() == 30000);
    CHECK(all_ages_in(2250, 2300));
}

TEST_CASE("a tail is widest and brightest in its middle and dims toward its old end",
          "[screensaver][fireflies_sim]") {
    using helix::ui::fireflies_tail_brightness;
    using helix::ui::fireflies_tail_width;
    const auto b = fireflies_tail_brightness;
    CHECK(b(0.0f) == 0.0f);
    CHECK(std::fabs(b(1.0f)) < 1e-6f);
    CHECK(b(0.5f) > b(0.1f));
    CHECK(b(0.5f) > b(0.9f));
    CHECK(b(0.9f) < b(0.1f));
    CHECK(b(-0.1f) == 0.0f);
    CHECK(b(1.1f) == 0.0f);
    CHECK(fireflies_tail_width(0.5f, 3.0f) == 3);
    CHECK(fireflies_tail_width(0.0f, 3.0f) == 1);
    CHECK(fireflies_tail_width(0.5f, 1.0f) == 1);
    CHECK(fireflies_tail_width(0.25f, 5.0f) == 4);
}

TEST_CASE("the wind takes a new heading every 4 to 8 tail lengths",
          "[screensaver][fireflies_sim]") {
    Swarm s(22);
    std::vector<uint64_t> changes;
    uint32_t seen = 0;
    float strongest_calm = 0.0f;
    double total = 0.0;
    int samples = 0;
    for (uint32_t t = 0; t < 200000; t += 100) {
        s.step(100);
        if (s.sim.wind_changes() != seen) {
            REQUIRE(s.sim.wind_changes() == seen + 1);
            seen = s.sim.wind_changes();
            changes.push_back(s.sim.last_wind_change_ms());
        }
        if (s.sim.mode() != helix::ui::FirefliesMode::WIND_GUST) {
            strongest_calm = std::max(strongest_calm, length(s.sim.wind()));
        }
        total += length(s.sim.wind());
        samples++;
    }
    REQUIRE(s.sim.wind_changes() >= 10);
    CHECK(changes[0] >= 9000);
    CHECK(changes[0] <= 18000);
    for (size_t i = 1; i < changes.size(); i++) {
        CAPTURE(i);
        CHECK(changes[i] - changes[i - 1] >= 9000);
        CHECK(changes[i] - changes[i - 1] <= 18000);
    }
    CHECK(strongest_calm <= 6.0f + 1e-3f);
    // Blowing at all: the tails bend.
    CHECK(total / samples > 1.5);
}

namespace {

using helix::ui::FirefliesMode;

std::vector<size_t> swarm_sizes(const FirefliesSim& sim) {
    std::vector<size_t> sizes(sim.baits().size(), 0);
    for (const FirefliesSim::Fly& f : sim.flies()) {
        sizes[f.bait]++;
    }
    return sizes;
}

std::vector<uint8_t> fly_baits(const FirefliesSim& sim) {
    std::vector<uint8_t> out;
    for (const FirefliesSim::Fly& f : sim.flies()) {
        out.push_back(f.bait);
    }
    return out;
}

bool every_fly_chases_a_live_bait(const FirefliesSim& sim) {
    return std::all_of(sim.flies().begin(), sim.flies().end(), [&](const FirefliesSim::Fly& f) {
        return f.bait < sim.baits().size() && sim.baits()[f.bait].alive;
    });
}

} // namespace

TEST_CASE("ten minutes of fireflies runs every mode and never orphans a fly",
          "[screensaver][fireflies_sim]") {
    Swarm s(31, 0, PixelFormat::XRGB8888, 160, 96);
    for (uint32_t t = 0; t < 600000; t += 100) {
        s.step(100);
        REQUIRE(s.sim.baits_alive() >= 2);
        REQUIRE(s.sim.baits_alive() <= 5);
        REQUIRE(every_fly_chases_a_live_bait(s.sim));
    }
    for (const FirefliesMode m :
         {FirefliesMode::STOP, FirefliesMode::LOOP, FirefliesMode::GLOW, FirefliesMode::HYPERSPEED,
          FirefliesMode::SPLIT, FirefliesMode::MERGE, FirefliesMode::WIND_GUST}) {
        CAPTURE(static_cast<int>(m));
        CHECK(s.sim.modes_begun(m) >= 1);
    }
    CHECK(s.sim.modes_begun(FirefliesMode::NORMAL) == 0);
}

TEST_CASE("a split at five baits and a merge at two are refused", "[screensaver][fireflies_sim]") {
    Swarm s(32);
    for (int k = 0; k < 5 && s.sim.baits_alive() < FIREFLIES_MAX_BAITS; k++) {
        REQUIRE(s.sim.begin_mode(FirefliesMode::SPLIT, 0, s.rng));
    }
    REQUIRE(s.sim.baits_alive() == FIREFLIES_MAX_BAITS);
    const std::vector<uint8_t> at_five = fly_baits(s.sim);
    const uint32_t splits = s.sim.modes_begun(FirefliesMode::SPLIT);
    CHECK_FALSE(s.sim.begin_mode(FirefliesMode::SPLIT, 0, s.rng));
    CHECK(s.sim.baits_alive() == FIREFLIES_MAX_BAITS);
    CHECK(fly_baits(s.sim) == at_five);
    CHECK(s.sim.modes_begun(FirefliesMode::SPLIT) == splits);

    for (int k = 0; k < 5 && s.sim.baits_alive() > helix::ui::FIREFLIES_MIN_BAITS; k++) {
        REQUIRE(s.sim.begin_mode(FirefliesMode::MERGE, 0, s.rng));
        REQUIRE(every_fly_chases_a_live_bait(s.sim));
    }
    REQUIRE(s.sim.baits_alive() == helix::ui::FIREFLIES_MIN_BAITS);
    const std::vector<uint8_t> at_two = fly_baits(s.sim);
    CHECK_FALSE(s.sim.begin_mode(FirefliesMode::MERGE, 0, s.rng));
    CHECK(s.sim.baits_alive() == helix::ui::FIREFLIES_MIN_BAITS);
    CHECK(fly_baits(s.sim) == at_two);
}

TEST_CASE("a split hands every other fly to a new bait", "[screensaver][fireflies_sim]") {
    Swarm s(33);
    s.run_for(1000);
    if (s.sim.baits_alive() == FIREFLIES_MAX_BAITS) {
        REQUIRE(s.sim.begin_mode(FirefliesMode::MERGE, 0, s.rng));
    }
    REQUIRE(s.sim.baits_alive() < FIREFLIES_MAX_BAITS);
    const std::vector<size_t> before = swarm_sizes(s.sim);
    size_t parent = 0;
    for (size_t i = 0; i < before.size(); i++) {
        if (s.sim.baits()[i].alive && before[i] > before[parent]) {
            parent = i;
        }
    }
    const FirefliesSim::Bait parent_bait = s.sim.baits()[parent];
    std::vector<bool> was_alive;
    for (const FirefliesSim::Bait& b : s.sim.baits()) {
        was_alive.push_back(b.alive);
    }

    REQUIRE(s.sim.begin_mode(FirefliesMode::SPLIT, 0, s.rng));
    CHECK(s.sim.mode() == FirefliesMode::NORMAL);
    size_t born = FIREFLIES_MAX_BAITS;
    for (size_t i = 0; i < was_alive.size(); i++) {
        if (!was_alive[i] && s.sim.baits()[i].alive) {
            born = i;
        }
    }
    REQUIRE(born < FIREFLIES_MAX_BAITS);
    const std::vector<size_t> after = swarm_sizes(s.sim);
    const size_t c = before[parent];
    CHECK(after[parent] == (c + 1) / 2);
    CHECK(after[born] == c / 2);
    const FirefliesSim::Bait& child = s.sim.baits()[born];
    CHECK(std::fabs(wrap180(child.hue_deg - parent_bait.hue_deg - 120.0f)) < 0.01f);
    CHECK(same_vec(child.pos, parent_bait.pos));
    CHECK(child.vel.x == -parent_bait.vel.x);
    CHECK(every_fly_chases_a_live_bait(s.sim));

    // The split swarms go on drawing their hues apart by the same 120 degrees.
    s.run_for(1000, 10);
    CHECK(std::fabs(wrap180(s.sim.baits()[born].hue_deg - s.sim.baits()[parent].hue_deg - 120.0f)) <
          0.01f);
}

TEST_CASE("a merge moves the smallest swarm to the nearest other bait",
          "[screensaver][fireflies_sim]") {
    Swarm s(34);
    s.run_for(1000);
    for (int k = 0; k < 5 && s.sim.baits_alive() < 4; k++) {
        REQUIRE(s.sim.begin_mode(FirefliesMode::SPLIT, 0, s.rng));
    }
    REQUIRE(s.sim.baits_alive() >= 4);
    s.run_for(1000);
    const std::vector<size_t> before = swarm_sizes(s.sim);
    size_t smallest = FIREFLIES_MAX_BAITS;
    for (size_t i = 0; i < before.size(); i++) {
        if (s.sim.baits()[i].alive &&
            (smallest == FIREFLIES_MAX_BAITS || before[i] < before[smallest])) {
            smallest = i;
        }
    }
    size_t nearest = FIREFLIES_MAX_BAITS;
    for (size_t i = 0; i < before.size(); i++) {
        if (i == smallest || !s.sim.baits()[i].alive) {
            continue;
        }
        if (nearest == FIREFLIES_MAX_BAITS ||
            distance(s.sim.baits()[i].pos, s.sim.baits()[smallest].pos) <
                distance(s.sim.baits()[nearest].pos, s.sim.baits()[smallest].pos)) {
            nearest = i;
        }
    }
    REQUIRE(nearest < FIREFLIES_MAX_BAITS);
    const size_t alive = s.sim.baits_alive();

    REQUIRE(s.sim.begin_mode(FirefliesMode::MERGE, 0, s.rng));
    const std::vector<size_t> after = swarm_sizes(s.sim);
    CHECK(s.sim.baits_alive() == alive - 1);
    CHECK_FALSE(s.sim.baits()[smallest].alive);
    CHECK(after[smallest] == 0);
    CHECK(after[nearest] == before[nearest] + before[smallest]);
    CHECK(every_fly_chases_a_live_bait(s.sim));
}

TEST_CASE("stop halts the baits, loop circles them, hyperspeed triples them, a gust quadruples "
          "the wind",
          "[screensaver][fireflies_sim]") {
    using helix::ui::FIREFLIES_FLY_SPEED;
    const auto alive_baits = [](const FirefliesSim& sim) {
        std::vector<size_t> out;
        for (size_t i = 0; i < sim.baits().size(); i++) {
            if (sim.baits()[i].alive) {
                out.push_back(i);
            }
        }
        return out;
    };

    SECTION("stop") {
        Swarm s(35);
        s.run_for(1000);
        REQUIRE(s.sim.begin_mode(FirefliesMode::STOP, 5000, s.rng));
        s.run_for(1000);
        REQUIRE(s.sim.mode() == FirefliesMode::STOP);
        for (const size_t i : alive_baits(s.sim)) {
            CHECK(length(s.sim.baits()[i].vel) < 0.5f);
        }
    }

    SECTION("loop") {
        Swarm s(36);
        s.run_for(1000);
        REQUIRE(s.sim.begin_mode(FirefliesMode::LOOP, 6000, s.rng));
        s.run_for(1000, 10);
        const std::vector<size_t> baits = alive_baits(s.sim);
        std::vector<float> travelled(FIREFLIES_MAX_BAITS, 0.0f);
        for (int i = 0; i < 300; i++) {
            std::vector<FireflyVec> was;
            for (const FirefliesSim::Bait& b : s.sim.baits()) {
                was.push_back(b.pos);
            }
            s.step(10);
            REQUIRE(s.sim.mode() == FirefliesMode::LOOP);
            for (const size_t b : baits) {
                const FirefliesSim::Bait& bait = s.sim.baits()[b];
                REQUIRE(distance(bait.pos, bait.loop_centre) <= 20.0f);
                travelled[b] += distance(bait.pos, was[b]);
            }
        }
        for (const size_t b : baits) {
            CHECK(travelled[b] >= 60.0f);
        }
    }

    SECTION("hyperspeed") {
        Swarm s(37);
        s.run_for(1000);
        REQUIRE(s.sim.begin_mode(FirefliesMode::HYPERSPEED, 3000, s.rng));
        float fastest = 0.0f;
        for (int i = 0; i < 100; i++) {
            s.step(10);
            for (const FirefliesSim::Fly& f : s.sim.flies()) {
                fastest = std::max(fastest, length(f.vel));
            }
        }
        CHECK(fastest > 1.5f * FIREFLIES_FLY_SPEED);
        s.run_for(2000, 10);
        REQUIRE(s.sim.mode() == FirefliesMode::NORMAL);
        s.run_for(2000, 10);
        for (const FirefliesSim::Fly& f : s.sim.flies()) {
            CHECK(length(f.vel) <= FIREFLIES_FLY_SPEED + 1e-3f);
        }
    }

    SECTION("wind gust") {
        Swarm s(38);
        s.run_for(1000);
        const float calm = length(s.sim.wind());
        REQUIRE(calm > 0.0f);
        REQUIRE(s.sim.begin_mode(FirefliesMode::WIND_GUST, 4000, s.rng));
        s.run_for(1000, 10);
        CHECK(length(s.sim.wind()) >= 3.0f * calm);
        s.run_for(3500, 10);
        REQUIRE(s.sim.mode() == FirefliesMode::NORMAL);
        CHECK(length(s.sim.wind()) <= 6.0f + 1e-3f);
    }
}

namespace {

/// Pixels of `frame` with any light in them.
size_t lit_pixels(const SimFrame& frame) {
    const helix::ui::PixelWriter writer(frame.target);
    size_t lit = 0;
    for (int32_t y = 0; y < static_cast<int32_t>(frame.target.h); y++) {
        for (int32_t x = 0; x < static_cast<int32_t>(frame.target.w); x++) {
            const Rgb c = writer.get(x, y);
            lit += (c.r | c.g | c.b) != 0 ? 1 : 0;
        }
    }
    return lit;
}

/// Sum of every channel of every pixel of `frame`.
uint64_t light_sum(const SimFrame& frame) {
    const helix::ui::PixelWriter writer(frame.target);
    uint64_t sum = 0;
    for (int32_t y = 0; y < static_cast<int32_t>(frame.target.h); y++) {
        for (int32_t x = 0; x < static_cast<int32_t>(frame.target.w); x++) {
            const Rgb c = writer.get(x, y);
            sum += static_cast<uint64_t>(c.r) + c.g + c.b;
        }
    }
    return sum;
}

bool in_some_box(const std::vector<DirtyRect>& boxes, int32_t x, int32_t y) {
    return std::any_of(boxes.begin(), boxes.end(), [&](const DirtyRect& r) {
        return x >= r.x1 && x <= r.x2 && y >= r.y1 && y <= r.y2;
    });
}

} // namespace

TEST_CASE("every changed fireflies pixel lies in a dirty box", "[screensaver][fireflies_sim]") {
    for (const PixelFormat format : {PixelFormat::XRGB8888, PixelFormat::RGB565}) {
        for (const size_t level : {size_t{0}, helix::ui::FIREFLIES_LEVEL_COUNT - 1}) {
            CAPTURE(static_cast<int>(format), level);
            Swarm s(41, level, format);
            const uint32_t bpp = bytes_per_pixel(format);
            bool changed_any = false;
            // Split first unless every slot is taken; the other change follows.
            const bool split_first = s.sim.baits_alive() < FIREFLIES_MAX_BAITS;
            bool split = false;
            bool merged = false;
            for (int frame = 0; frame < 300; frame++) {
                if (frame == (split_first ? 100 : 200)) {
                    split = s.sim.begin_mode(FirefliesMode::SPLIT, 0, s.rng);
                }
                if (frame == (split_first ? 200 : 100)) {
                    merged = s.sim.begin_mode(FirefliesMode::MERGE, 0, s.rng);
                }
                // GLOW draws halos; a level change drops flies whose last tiles must clear.
                if (frame == 50) {
                    REQUIRE(s.sim.begin_mode(FirefliesMode::GLOW, 3000, s.rng));
                }
                if (frame == 150) {
                    s.sim.request_level(level == 0 ? helix::ui::FIREFLIES_LEVEL_COUNT - 1 : 0);
                }
                const std::vector<uint8_t> before = s.frame.bytes;
                s.step();
                REQUIRE(s.dirty.size() <= helix::ui::FIREFLIES_MAX_DIRTY_AREAS);
                for (size_t i = 0; i < before.size(); i++) {
                    if (before[i] == s.frame.bytes[i]) {
                        continue;
                    }
                    changed_any = true;
                    const auto y = static_cast<int32_t>(i / s.frame.target.stride);
                    const auto x = static_cast<int32_t>(i % s.frame.target.stride / bpp);
                    CAPTURE(frame, x, y);
                    REQUIRE(in_some_box(s.dirty, x, y));
                }
            }
            CHECK(split);
            CHECK(merged);
            CHECK(changed_any);
        }
    }
}

TEST_CASE("fireflies draw only light on black, and never past a row",
          "[screensaver][fireflies_sim]") {
    constexpr uint8_t UNTOUCHED = 0xAA;
    constexpr uint32_t OW = 321;
    constexpr uint32_t OH = 203;
    for (const PixelFormat format : {PixelFormat::XRGB8888, PixelFormat::RGB565}) {
        CAPTURE(static_cast<int>(format));
        const uint32_t bpp = bytes_per_pixel(format);
        const uint32_t stride = OW * bpp + 7;
        // One sentinel row below the frame.
        std::vector<uint8_t> bytes(static_cast<size_t>(stride) * (OH + 1), UNTOUCHED);
        FrameTarget target{bytes.data(), stride, OW, OH, format};
        std::minstd_rand rng(43);
        FirefliesSim sim;
        std::vector<DirtyRect> dirty;
        sim.init(target, rng, 0);
        for (int i = 0; i < 150; i++) {
            sim.step(FRAME_MS, target, rng, dirty);
        }
        bool padding_kept = true;
        bool opaque = true;
        bool lit = false;
        for (uint32_t y = 0; y <= OH; y++) {
            for (uint32_t b = 0; b < stride; b++) {
                const uint8_t v = bytes[static_cast<size_t>(y) * stride + b];
                if (y == OH || b >= OW * bpp) {
                    padding_kept = padding_kept && v == UNTOUCHED;
                } else if (format == PixelFormat::XRGB8888 && b % 4 == 3) {
                    opaque = opaque && v == 0xFF;
                } else {
                    lit = lit || v != 0;
                }
            }
        }
        CHECK(padding_kept);
        CHECK(opaque);
        CHECK(lit);
    }
}

TEST_CASE("an RGB565 swarm shows at least 60% of the light an XRGB8888 one does",
          "[screensaver][fireflies_sim]") {
    Swarm wide(44, 0, PixelFormat::XRGB8888);
    Swarm narrow(44, 0, PixelFormat::RGB565);
    wide.run_for(3000);
    narrow.run_for(3000);
    const size_t wide_lit = lit_pixels(wide.frame);
    REQUIRE(wide_lit > 500);
    CHECK(static_cast<double>(lit_pixels(narrow.frame)) >= 0.6 * static_cast<double>(wide_lit));
}

TEST_CASE("glow brightens the swarm", "[screensaver][fireflies_sim]") {
    Swarm plain(42);
    Swarm glowing(42);
    REQUIRE(glowing.sim.begin_mode(FirefliesMode::GLOW, 5000, glowing.rng));
    plain.run_for(3000);
    glowing.run_for(3000);
    REQUIRE(glowing.sim.mode() == FirefliesMode::GLOW);
    REQUIRE(same_positions(plain.sim, glowing.sim));
    plain.step(0);
    glowing.step(0);
    const uint64_t plain_sum = light_sum(plain.frame);
    REQUIRE(plain_sum > 0);
    CHECK(static_cast<double>(light_sum(glowing.frame)) >= 1.3 * static_cast<double>(plain_sum));
}

// 60% is the design limit, not a measurement with headroom: level 0 runs close to it, and a
// change that pushes past it should be a deliberate retuning of the tiles or the ladder.
TEST_CASE("a fireflies frame dirties a fraction of the canvas", "[screensaver][fireflies_sim]") {
    Swarm s(45, 0, PixelFormat::XRGB8888, 800, 480);
    double dirty_px = 0;
    int frames = 0;
    for (uint32_t t = 0; t < 10000; t += helix::ui::SAVER_FAST_PERIOD) {
        s.step(helix::ui::SAVER_FAST_PERIOD);
        for (const DirtyRect& r : s.dirty) {
            dirty_px += static_cast<double>(r.area());
        }
        frames++;
    }
    CHECK(dirty_px / frames < 0.6 * 800.0 * 480.0);
}

TEST_CASE("a fireflies level change applies at the next frame and frames allocate nothing",
          "[screensaver][fireflies_sim]") {
    Swarm s(51);
    const size_t at_init = s.sim.heap_bytes();
    REQUIRE(at_init > 0);
    for (size_t level = 0; level <= FIREFLIES_LEVEL_COUNT; level++) {
        CAPTURE(level);
        s.sim.request_level(level);
        s.step();
        CHECK(s.sim.level() == std::min(level, FIREFLIES_LEVEL_COUNT - 1));
        for (int i = 0; i < 20; i++) {
            s.step();
            REQUIRE(s.sim.heap_bytes() == at_init);
        }
    }
}

TEST_CASE("each fireflies rung lights less than the one above", "[screensaver][fireflies_sim]") {
    std::vector<size_t> lit;
    for (size_t level = 0; level < FIREFLIES_LEVEL_COUNT; level++) {
        Swarm s(52, level, PixelFormat::XRGB8888, 800, 480);
        s.run_for(4000);
        lit.push_back(lit_pixels(s.frame));
    }
    for (size_t level = 2; level < FIREFLIES_LEVEL_COUNT; level++) {
        CAPTURE(level, lit[level - 1], lit[level]);
        CHECK(lit[level] < lit[level - 1]);
    }
}

TEST_CASE("a fireflies level draws only its first flies", "[screensaver][fireflies_sim]") {
    // Straight after init every tail is collapsed on its fly, so a frame shows bodies alone.
    const auto bodies_lit = [](size_t level) {
        Swarm s(54, level, PixelFormat::XRGB8888, 800, 480);
        s.step(0);
        return lit_pixels(s.frame);
    };
    const size_t all = bodies_lit(2);
    const size_t few = bodies_lit(FIREFLIES_LEVEL_COUNT - 1);
    REQUIRE(all > 0);
    CAPTURE(all, few);
    CHECK(few > 0);
    CHECK(static_cast<double>(few) < 0.5 * static_cast<double>(all));
}

namespace {

/// What a level's frames cost a board, per the K1C model, scaled to the level's frame period.
double fireflies_cost(size_t level) {
    Swarm s(53, level, PixelFormat::RGB565, 800, 480);
    const uint32_t period = FIREFLIES_LEVELS[level].period_ms;
    helix::test::SaverLoad load;
    constexpr int FRAMES = 240;
    for (int i = 0; i < FRAMES; i++) {
        s.step(period);
        load.add_frame(s.dirty);
    }
    load.average_over(FRAMES);
    return load.cost() * 33.0 / static_cast<double>(period);
}

} // namespace

// The model prices only the redraw LVGL does for the dirty areas, not the sim's own drawing into
// the canvas, so it understates fireflies; the device gate is the real measure.
TEST_CASE("the fireflies bottom rung's redraw fits a 1-core board's 10% budget",
          "[screensaver][fireflies_sim]") {
    const double bottom = fireflies_cost(FIREFLIES_LEVEL_COUNT - 1);
    const double top = fireflies_cost(0);
    CAPTURE(bottom, top);
    CHECK(bottom < 10.0);
    CHECK(top > bottom);
}

TEST_CASE("fireflies frame cost per level", "[.][fireflies_bench]") {
    std::ostringstream report;
    for (size_t level = 0; level < FIREFLIES_LEVEL_COUNT; level++) {
        Swarm s(3, level, PixelFormat::XRGB8888, 800, 480);
        s.run_for(5000);
        constexpr int FRAMES = 150;
        const uint32_t period = FIREFLIES_LEVELS[level].period_ms;
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < FRAMES; i++) {
            s.step(period);
        }
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count() /
            FRAMES;
        report << "L" << level << " " << ms << " ms/frame\n";
    }
    WARN(report.str());
}

TEST_CASE("a new wind heading during a gust never compounds the gust",
          "[screensaver][fireflies_sim]") {
    constexpr float WIND_MAX = 6.0f;
    constexpr float GUST_MAX = 4.0f * WIND_MAX;
    for (uint32_t seed = 1; seed <= 40; seed++) {
        CAPTURE(seed);
        Swarm s(seed, helix::ui::FIREFLIES_LEVEL_COUNT - 1, PixelFormat::XRGB8888, 160, 96);
        // Gust back to back, so every heading change lands inside a gust.
        const uint32_t changes_before = s.sim.wind_changes();
        float strongest = 0.0f;
        for (uint32_t t = 0; t < 60000; t += 100) {
            if (s.sim.mode() == FirefliesMode::NORMAL) {
                REQUIRE(s.sim.begin_mode(FirefliesMode::WIND_GUST, 4000, s.rng));
            }
            s.step(100);
            strongest = std::max(strongest, length(s.sim.wind()));
        }
        REQUIRE(s.sim.wind_changes() >= changes_before + 3);
        CHECK(strongest <= GUST_MAX + 1e-3f);
        // The last gust ends; the calm wind is back within its bounds at once and stays there.
        s.run_for(4000, 100);
        for (uint32_t t = 0; t < 3000; t += 100) {
            s.step(100);
            REQUIRE(s.sim.mode() == FirefliesMode::NORMAL);
            CHECK(length(s.sim.wind()) <= WIND_MAX + 1e-3f);
        }
    }
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
