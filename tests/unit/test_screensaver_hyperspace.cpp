// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "../lvgl_test_fixture.h"
#include "../test_helpers/screensaver_test_access.h"
#include "screensaver_canvas.h"
#include "screensaver_hyperspace.h"
#include "screensaver_hyperspace_sim.h"
#include "screensaver_pixel_writer.h"
#include "screensaver_registry.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <random>
#include <sstream>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::DirtyRect;
using helix::ui::FrameTarget;
using helix::ui::HYPERSPACE_LEVEL_COUNT;
using helix::ui::HYPERSPACE_LEVELS;
using helix::ui::HYPERSPACE_MAX_ROLL;
using helix::ui::HyperspaceScreensaver;
using helix::ui::HyperspaceSim;
using helix::ui::PixelFormat;
using helix::ui::PixelWriter;
using helix::ui::Rgb;

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

/// A sim and its frame, stepped at FRAME_MS.
struct Flight {
    std::minstd_rand rng;
    HyperspaceSim sim;
    SimFrame frame;
    std::vector<DirtyRect> dirty;

    Flight(uint32_t seed, size_t level, PixelFormat format = PixelFormat::XRGB8888)
        : rng(seed), frame(format) {
        sim.init(frame.target, rng, level);
    }

    void step(uint32_t dt_ms = FRAME_MS) {
        sim.step(dt_ms, frame.target, rng, dirty);
    }

    /// Steps until `done` holds or `limit_ms` of flight passes; true when `done` held.
    template <typename Done> bool run_until(Done&& done, uint32_t limit_ms) {
        for (uint32_t t = 0; t < limit_ms; t += FRAME_MS) {
            step();
            if (done()) {
                return true;
            }
        }
        return false;
    }

    bool run_to_tunnel(uint32_t limit_ms = 90000) {
        return run_until([this] { return sim.tunnel_amount() == 255; }, limit_ms);
    }
    bool run_to_open_space(uint32_t limit_ms = 90000) {
        return run_until(
            [this] { return sim.tunnel_amount() == 0 && sim.burst_flares_alive() == 0; }, limit_ms);
    }
};

int max_channel(Rgb c) {
    return std::max({c.r, c.g, c.b});
}

/// Share of pixels whose brightest channel reaches `threshold`.
double share_at_least(const SimFrame& frame, int threshold) {
    const PixelWriter writer(frame.target);
    size_t count = 0;
    for (int32_t y = 0; y < static_cast<int32_t>(frame.target.h); y++) {
        for (int32_t x = 0; x < static_cast<int32_t>(frame.target.w); x++) {
            count += max_channel(writer.get(x, y)) >= threshold ? 1 : 0;
        }
    }
    return static_cast<double>(count) / (frame.target.w * frame.target.h);
}

/// Share of the aligned `block` x `block` squares whose pixels are all one colour.
double uniform_block_share(const SimFrame& frame, int32_t block) {
    const PixelWriter writer(frame.target);
    size_t blocks = 0;
    size_t uniform = 0;
    for (int32_t by = 0; by + block <= static_cast<int32_t>(frame.target.h); by += block) {
        for (int32_t bx = 0; bx + block <= static_cast<int32_t>(frame.target.w); bx += block) {
            const Rgb first = writer.get(bx, by);
            bool same = true;
            for (int32_t y = by; y < by + block && same; y++) {
                for (int32_t x = bx; x < bx + block && same; x++) {
                    same = writer.get(x, y) == first;
                }
            }
            blocks++;
            uniform += same ? 1 : 0;
        }
    }
    return static_cast<double>(uniform) / static_cast<double>(blocks);
}

constexpr int HUE_BINS = 12;

/// How many of the pixels at least `threshold` bright fall in each 30-degree slice of the wheel,
/// red first and turning through yellow toward green.
std::array<double, HUE_BINS> hue_histogram(const SimFrame& frame, int threshold) {
    const PixelWriter writer(frame.target);
    std::array<double, HUE_BINS> bins{};
    for (int32_t y = 0; y < static_cast<int32_t>(frame.target.h); y++) {
        for (int32_t x = 0; x < static_cast<int32_t>(frame.target.w); x++) {
            const Rgb c = writer.get(x, y);
            if (max_channel(c) < threshold) {
                continue;
            }
            // The angle of the colour in the chroma plane.
            const double alpha = c.r - 0.5 * (c.g + c.b);
            const double beta = std::sqrt(3.0) / 2.0 * (c.g - c.b);
            double degrees = std::atan2(beta, alpha) * 180.0 / M_PI;
            degrees = degrees < 0 ? degrees + 360.0 : degrees;
            bins[static_cast<size_t>(degrees / (360.0 / HUE_BINS)) % HUE_BINS] += 1.0;
        }
    }
    return bins;
}

/// Slices round the wheel that `before` must turn to best match `after`.
int best_hue_turn(const std::array<double, HUE_BINS>& before,
                  const std::array<double, HUE_BINS>& after) {
    int best = 0;
    double best_score = -1.0;
    for (int turn = 0; turn < HUE_BINS; turn++) {
        double score = 0.0;
        for (int i = 0; i < HUE_BINS; i++) {
            score +=
                before[static_cast<size_t>(i)] * after[static_cast<size_t>((i + turn) % HUE_BINS)];
        }
        if (score > best_score) {
            best_score = score;
            best = turn;
        }
    }
    return best;
}

void fire(lv_timer_t* timer) {
    REQUIRE(timer != nullptr);
    REQUIRE(timer->timer_cb != nullptr);
    timer->timer_cb(timer);
}

} // namespace

// ============================================================================
// HyperspaceSim
// ============================================================================

TEST_CASE("a hyperspace flight replays exactly from a seed, and another seed differs",
          "[screensaver][hyperspace_sim]") {
    const PixelFormat format = GENERATE(PixelFormat::XRGB8888, PixelFormat::RGB565);
    CAPTURE(static_cast<int>(format));
    const auto run = [format](uint32_t seed) {
        Flight flight(seed, 0, format);
        constexpr uint32_t STEPS_MS[] = {16, 33, 50, 7, 100};
        for (int i = 0; i < 600; i++) {
            flight.step(STEPS_MS[i % 5]);
        }
        return flight.frame.bytes;
    };

    const auto first = run(9);
    const auto repeat = run(9);
    const auto other = run(10);

    CHECK(repeat == first);
    CHECK_FALSE(other == first);
}

TEST_CASE("every hyperspace pixel is opaque and every frame dirties the whole canvas",
          "[screensaver][hyperspace_sim]") {
    const size_t level = GENERATE(range(size_t{0}, HYPERSPACE_LEVEL_COUNT));
    CAPTURE(level);
    Flight flight(5, level);
    size_t frames = 0;
    size_t opaque_failures = 0;
    for (uint32_t t = 0; t < 40000; t += FRAME_MS) {
        flight.step();
        frames++;
        REQUIRE(flight.dirty.size() == 1);
        CHECK(flight.dirty[0] ==
              DirtyRect{0, 0, static_cast<int32_t>(W) - 1, static_cast<int32_t>(H) - 1});
        if (frames % 30 != 0) {
            continue;
        }
        for (uint32_t y = 0; y < H; y++) {
            for (uint32_t x = 0; x < W; x++) {
                opaque_failures +=
                    flight.frame.bytes[static_cast<size_t>(y) * flight.frame.target.stride + x * 4 +
                                       3] != 0xFF
                        ? 1
                        : 0;
            }
        }
    }
    CHECK(opaque_failures == 0);
}

TEST_CASE("the flight passes through tunnel phases and open space, in either format",
          "[screensaver][hyperspace_sim]") {
    const PixelFormat format = GENERATE(PixelFormat::XRGB8888, PixelFormat::RGB565);
    CAPTURE(static_cast<int>(format));
    Flight flight(11, 0, format);

    REQUIRE(flight.run_to_tunnel());
    const double tunnel_lit = share_at_least(flight.frame, 64);
    CAPTURE(tunnel_lit);
    CHECK(tunnel_lit > 0.35);

    REQUIRE(flight.run_to_open_space());
    const double open_lit = share_at_least(flight.frame, 64);
    CAPTURE(open_lit);
    CHECK(open_lit < 0.08);
    // ...but not empty: the star flares still stream past.
    CHECK(share_at_least(flight.frame, 96) > 0.0);

    REQUIRE(flight.run_to_tunnel());
}

TEST_CASE("goo glows behind open space down to the level that drops it",
          "[screensaver][hyperspace_sim]") {
    const auto unlit_share_in_open_space = [](size_t level) {
        Flight flight(17, level);
        REQUIRE(flight.run_to_open_space());
        return 1.0 - share_at_least(flight.frame, 1);
    };
    CHECK(HYPERSPACE_LEVELS[2].goo);
    CHECK_FALSE(HYPERSPACE_LEVELS[3].goo);
    const double with_goo = unlit_share_in_open_space(2);
    const double without_goo = unlit_share_in_open_space(3);
    CAPTURE(with_goo, without_goo);
    CHECK(with_goo < 0.75);
    CHECK(without_goo > 0.85);
}

TEST_CASE("the tunnel draws at each level's block size, and the last level draws none",
          "[screensaver][hyperspace_sim]") {
    const auto tunnel_frame_stats = [](size_t level) {
        Flight flight(23, level);
        // A level without a tunnel still keeps the phase clock, so a tunnel phase comes round.
        REQUIRE(flight.run_to_tunnel());
        return std::make_pair(share_at_least(flight.frame, 64),
                              std::make_pair(uniform_block_share(flight.frame, 2),
                                             uniform_block_share(flight.frame, 4)));
    };

    const auto full = tunnel_frame_stats(0);
    const auto half = tunnel_frame_stats(2);
    const auto quarter = tunnel_frame_stats(4);
    const auto none = tunnel_frame_stats(HYPERSPACE_LEVEL_COUNT - 1);
    CAPTURE(full.second.first, half.second.first, half.second.second, quarter.second.second);
    CAPTURE(full.first, quarter.first, none.first);

    CHECK(HYPERSPACE_LEVELS[0].block == 1);
    CHECK(HYPERSPACE_LEVELS[2].block == 2);
    CHECK(HYPERSPACE_LEVELS[4].block == 4);
    CHECK(HYPERSPACE_LEVELS[HYPERSPACE_LEVEL_COUNT - 1].block == 0);

    CHECK(full.second.first < 0.7);
    CHECK(half.second.first > 0.9);
    CHECK(half.second.second < 0.7);
    CHECK(quarter.second.second > 0.85);

    CHECK(quarter.first > 0.35);
    CHECK(none.first < 0.08);
}

TEST_CASE("the last level flies fewer stars than the first", "[screensaver][hyperspace_sim]") {
    CHECK(HYPERSPACE_LEVELS[HYPERSPACE_LEVEL_COUNT - 1].stars < HYPERSPACE_LEVELS[0].stars);
    for (const auto& level : HYPERSPACE_LEVELS) {
        CHECK(level.stars <= HyperspaceSim::MAX_STARS);
    }
}

TEST_CASE("the hyperspace colours cycle around the wheel over a few seconds",
          "[screensaver][hyperspace_sim]") {
    Flight flight(29, 0);
    REQUIRE(flight.run_to_tunnel());
    const uint8_t hue_then = flight.sim.hue();
    const auto seen_then = hue_histogram(flight.frame, 64);
    flight.run_until([] { return false; }, 6000);
    REQUIRE(flight.sim.tunnel_amount() == 255);
    const auto seen_now = hue_histogram(flight.frame, 64);
    const int turn = best_hue_turn(seen_then, seen_now);
    CAPTURE(turn, static_cast<int>(hue_then), static_cast<int>(flight.sim.hue()));
    // Six seconds turn the colours a quarter of the way round, give or take a slice.
    CHECK(turn >= 2);
    CHECK(turn <= 4);
    CHECK(flight.sim.hue() != hue_then);
}

TEST_CASE("the camera always flies forward, rolls a little and drifts gently",
          "[screensaver][hyperspace_sim]") {
    Flight flight(31, 0);
    float slowest = 1e9f;
    float most_roll = 0.0f;
    float min_drift = 1e9f;
    float max_drift = -1e9f;
    constexpr uint32_t STEPS_MS[] = {16, 33, 100, 250, 5};
    for (int i = 0; i < 2000; i++) {
        flight.step(STEPS_MS[i % 5]);
        const auto& cam = flight.sim.camera();
        slowest = std::min(slowest, cam.speed);
        most_roll = std::max(most_roll, std::fabs(cam.roll));
        min_drift = std::min(min_drift, cam.drift_x);
        max_drift = std::max(max_drift, cam.drift_x);
    }
    CAPTURE(slowest, most_roll, min_drift, max_drift);
    CHECK(slowest > 0.0f);
    CHECK(most_roll <= HYPERSPACE_MAX_ROLL);
    CHECK(most_roll > 0.05f);
    CHECK(max_drift - min_drift > 4.0f);
    CHECK(std::fabs(min_drift) < static_cast<float>(W) / 4);
    CHECK(std::fabs(max_drift) < static_cast<float>(W) / 4);
}

TEST_CASE("a starburst of flares bursts ahead within the first few seconds",
          "[screensaver][hyperspace_sim]") {
    Flight flight(37, 0);
    REQUIRE(flight.sim.bursts_started() == 0);
    REQUIRE(flight.run_until([&] { return flight.sim.bursts_started() > 0; }, 15000));
    flight.step();
    CHECK(flight.sim.burst_flares_alive() > HyperspaceSim::BURST_FLARES / 2);
    // Its flares burn out rather than piling up.
    REQUIRE(flight.run_until([&] { return flight.sim.burst_flares_alive() == 0; }, 15000));
}

TEST_CASE("a hyperspace level change applies at the next frame and allocates nothing",
          "[screensaver][hyperspace_sim]") {
    Flight flight(41, 0);
    flight.step();
    const size_t bytes = flight.sim.heap_bytes();
    REQUIRE(bytes > 0);
    for (size_t level = 0; level < HYPERSPACE_LEVEL_COUNT + 1; level++) {
        flight.sim.request_level(level);
        flight.step();
        CHECK(flight.sim.level() == std::min(level, HYPERSPACE_LEVEL_COUNT - 1));
        for (int i = 0; i < 20; i++) {
            flight.step();
        }
        CHECK(flight.sim.heap_bytes() == bytes);
    }
    flight.sim.clear();
    CHECK(flight.sim.heap_bytes() == 0);
}

TEST_CASE("hyperspace frame cost per level", "[.][hyperspace_bench]") {
    std::ostringstream report;
    for (size_t level = 0; level < HYPERSPACE_LEVEL_COUNT; level++) {
        for (const bool tunnel : {true, false}) {
            std::minstd_rand rng(3);
            HyperspaceSim sim;
            SimFrame frame(PixelFormat::XRGB8888, 800, 480);
            std::vector<DirtyRect> dirty;
            sim.init(frame.target, rng, level);
            for (int i = 0; i < 5000; i++) {
                sim.step(FRAME_MS, frame.target, rng, dirty);
                const bool there = tunnel ? sim.tunnel_amount() == 255 : sim.tunnel_amount() == 0;
                if (there && i > 30) {
                    break;
                }
            }
            constexpr int FRAMES = 150;
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < FRAMES; i++) {
                sim.step(16, frame.target, rng, dirty);
            }
            const double ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                    .count() /
                FRAMES;
            report << "L" << level << (tunnel ? " tunnel " : " open   ") << ms << " ms/frame\n";
        }
    }
    WARN(report.str());
}

// ============================================================================
// HyperspaceScreensaver
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture,
                 "the hyperspace saver draws on a canvas in the build's format with its ladder",
                 "[screensaver][screensaver_hyperspace]") {
    HyperspaceScreensaver ss;
    ScreensaverStopOnExit<HyperspaceScreensaver> stop_on_exit{ss};
    ss.start();
    REQUIRE(ss.is_active());
    const lv_draw_buf_t* buf = lv_canvas_get_draw_buf(SaverTestAccess::canvas(ss));
    REQUIRE(buf != nullptr);
    CHECK(buf->header.cf == helix::ui::SAVER_BUILD_CANVAS_FORMAT);
    CHECK(ss.level_count() == HYPERSPACE_LEVEL_COUNT);
    CHECK(HYPERSPACE_LEVELS[0].period_ms == helix::ui::SAVER_FAST_PERIOD);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a hyperspace level request applies at once, frame period included",
                 "[screensaver][screensaver_hyperspace]") {
    HyperspaceScreensaver ss;
    ScreensaverStopOnExit<HyperspaceScreensaver> stop_on_exit{ss};
    SaverTestAccess::set_fixed_seed(ss, 4);
    ss.start();
    REQUIRE(ss.is_active());

    const size_t last = HYPERSPACE_LEVEL_COUNT - 1;
    ss.request_level(last);
    CHECK(ss.level() == last);
    CHECK(SaverTestAccess::timer(ss)->period == HYPERSPACE_LEVELS[last].period_ms);
    lv_tick_inc(33);
    fire(SaverTestAccess::timer(ss));
    CHECK(ss.is_active());
}

TEST_CASE("hyperspace is registered by name at both depths",
          "[screensaver][screensaver_hyperspace]") {
    const helix::ui::ScreensaverInfo* info = helix::ui::find_screensaver_by_name("hyperspace");
    REQUIRE(info != nullptr);
    CHECK(info->type == ScreensaverType::HYPERSPACE);
    CHECK(std::string(info->label_key) == "Hyperspace");
    CHECK((info->depths & helix::ui::SAVER_DEPTH_16) != 0);
    CHECK((info->depths & helix::ui::SAVER_DEPTH_32) != 0);
}

#endif // HELIX_ENABLE_SCREENSAVER
