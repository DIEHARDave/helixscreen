// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "belt_test_signals.h"
#include "sweep_response_estimator.h"

#include <cmath>
#include <limits>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::calibration;
using Catch::Approx;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

std::vector<float> sine(size_t n, float amplitude, float freq_hz, float rate_hz,
                        float offset = 0.0f) {
    std::vector<float> out(n);
    for (size_t i = 0; i < n; ++i) {
        out[i] = offset + amplitude * static_cast<float>(std::sin(
                                          2.0 * kPi * freq_hz * static_cast<double>(i) / rate_hz));
    }
    return out;
}

/// The PSD a Hann-windowed sine of amplitude A reads at its own frequency:
/// A^2 (sum w)^2 / (2 rate sum w^2).
double hann_sine_psd(size_t n, double amplitude, double rate_hz) {
    double sum_w = 0.0;
    double sum_w2 = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double w =
            0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(n - 1));
        sum_w += w;
        sum_w2 += w * w;
    }
    return amplitude * amplitude * sum_w * sum_w / (2.0 * rate_hz * sum_w2);
}

} // namespace

TEST_CASE("SweepPlan counts one point per Hz of a valid ramp", "[sweep_preview]") {
    const SweepPlan plan{5.0f, 135.0f, 1.0f};
    CHECK(plan.valid());
    CHECK(plan.point_count() == 131);
    CHECK(plan.point_hz(0) == 5.0f);
    CHECK(plan.point_hz(130) == 135.0f);

    const SweepPlan klipper{5.0f, 133.33f, 1.0f};
    CHECK(klipper.valid());
    CHECK(klipper.point_count() == 129);
    CHECK(klipper.point_hz(128) == 133.0f);
}

TEST_CASE("SweepPlan rejects degenerate ramps", "[sweep_preview]") {
    const SweepPlan bad[] = {
        {0.0f, 135.0f, 1.0f},
        {5.0f, 5.0f, 1.0f},
        {5.0f, 135.0f, 0.0f},
        {kNaN, 135.0f, 1.0f},
        {5.0f, std::numeric_limits<float>::infinity(), 1.0f},
    };
    for (const auto& plan : bad) {
        INFO(plan.freq_start_hz << " " << plan.freq_end_hz << " " << plan.hz_per_sec);
        CHECK_FALSE(plan.valid());
        CHECK(plan.point_count() == 0);
    }
}

TEST_CASE("SweepPoint columns match the CSV's", "[sweep_preview]") {
    const SweepPoint point{50.0f, 1.0f, 2.0f, 3.0f, true};
    CHECK(point.psd_xyz() == 6.0f);
    CHECK(sweep_column_value(point, SweepColumn::XYZ) == 6.0f);
    CHECK(sweep_column_value(point, SweepColumn::X) == 1.0f);
    CHECK(sweep_column_value(point, SweepColumn::Y) == 2.0f);

    const SweepPoint unclean{50.0f, 1.0f, 2.0f, 3.0f, false};
    CHECK(std::isnan(sweep_column_value(unclean, SweepColumn::XYZ)));
    CHECK(std::isnan(sweep_column_value(unclean, SweepColumn::X)));
    CHECK(std::isnan(sweep_column_value(unclean, SweepColumn::Y)));
}

TEST_CASE("goertzel_psd reads a sine's PSD at its frequency", "[sweep_preview]") {
    const auto x = sine(3200, 1000.0f, 50.0f, 3200.0f);
    const double expected = hann_sine_psd(3200, 1000.0, 3200.0);
    CHECK(expected == Approx(333229.17).epsilon(1e-4));

    const float at = goertzel_psd(x.data(), x.size(), 50.0f, 3200.0f);
    CHECK(at == Approx(expected).epsilon(0.05));

    // 3.3 Hz away is far outside a 1 s Hann window's main lobe.
    const float below = goertzel_psd(x.data(), x.size(), 46.7f, 3200.0f);
    const float above = goertzel_psd(x.data(), x.size(), 53.3f, 3200.0f);
    CHECK(10.0 * std::log10(below / at) < -20.0);
    CHECK(10.0 * std::log10(above / at) < -20.0);

    const auto y = sine(1527, 500.0f, 40.0f, 3053.0f);
    const double expected_y = hann_sine_psd(1527, 500.0, 3053.0);
    CHECK(expected_y == Approx(41653.0).epsilon(1e-3));
    CHECK(goertzel_psd(y.data(), y.size(), 40.0f, 3053.0f) == Approx(expected_y).epsilon(0.05));
}

TEST_CASE("goertzel_psd removes the mean", "[sweep_preview]") {
    const auto plain = sine(3200, 1000.0f, 50.0f, 3200.0f);
    const auto gravity = sine(3200, 1000.0f, 50.0f, 3200.0f, 9810.0f);
    const float a = goertzel_psd(plain.data(), plain.size(), 50.0f, 3200.0f);
    const float b = goertzel_psd(gravity.data(), gravity.size(), 50.0f, 3200.0f);
    CHECK(b == Approx(a).epsilon(0.01));

    // A lock window is 0.5 s, so a low drive sits a few bins from DC, where
    // gravity's leakage through the window would swamp it.
    const auto low = sine(1527, 1000.0f, 5.0f, 3053.0f);
    const auto low_gravity = sine(1527, 1000.0f, 5.0f, 3053.0f, 9810.0f);
    const float c = goertzel_psd(low.data(), low.size(), 5.0f, 3053.0f);
    const float d = goertzel_psd(low_gravity.data(), low_gravity.size(), 5.0f, 3053.0f);
    CHECK(d == Approx(c).epsilon(0.01));
}

TEST_CASE("goertzel_psd is 0 for empty or meaningless input", "[sweep_preview]") {
    const std::vector<float> zeros(3200, 0.0f);
    CHECK(goertzel_psd(zeros.data(), zeros.size(), 50.0f, 3200.0f) == 0.0f);

    const auto x = sine(3200, 1000.0f, 50.0f, 3200.0f);
    CHECK(goertzel_psd(x.data(), 0, 50.0f, 3200.0f) == 0.0f);
    CHECK(goertzel_psd(x.data(), 1, 50.0f, 3200.0f) == 0.0f);
    CHECK(goertzel_psd(x.data(), x.size(), 50.0f, 0.0f) == 0.0f);
    CHECK(goertzel_psd(x.data(), x.size(), 0.0f, 3200.0f) == 0.0f);
    CHECK(goertzel_psd(x.data(), x.size(), -5.0f, 3200.0f) == 0.0f);
}

namespace {

/// A 0.5 s lock window at 3053 Hz: x carries gravity plus the tone, y the tone.
std::vector<AccelSample> tone_window(float freq_hz, float amplitude) {
    constexpr size_t n = 1527;
    constexpr float rate = 3053.0f;
    std::vector<AccelSample> out(n);
    for (size_t i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / rate;
        const auto s = static_cast<float>(amplitude * std::sin(2.0 * kPi * freq_hz * i / rate));
        out[i] = AccelSample{t, 9810.0f + s, s, 0.0f};
    }
    return out;
}

std::optional<float> drive_in(const std::vector<AccelSample>& w, float hint,
                              float hz_per_sec = 1.0f) {
    return find_drive_hz(w.data(), w.size(), 3053.0f, hint, hz_per_sec);
}

/// Feed one reading whose t0 estimate is `estimate`, at stream time `at`.
void observe_estimate(DriveLock& lock, double at, double estimate, const SweepPlan& plan) {
    lock.observe(at, static_cast<float>(plan.freq_start_hz + plan.hz_per_sec * (at - estimate)));
}

} // namespace

TEST_CASE("find_drive_hz finds the drive in a lock window", "[sweep_preview][lock]") {
    const auto w40 = tone_window(40.0f, 200.0f);
    auto f = drive_in(w40, 41.5f);
    REQUIRE(f.has_value());
    CHECK(*f == Approx(40.0f).margin(0.1));

    // Off the 0.25 Hz grid: the parabola refines it.
    f = drive_in(tone_window(40.3f, 200.0f), 41.5f);
    REQUIRE(f.has_value());
    CHECK(*f == Approx(40.3f).margin(0.1));
    f = drive_in(tone_window(40.12f, 200.0f), 41.5f);
    REQUIRE(f.has_value());
    CHECK(*f == Approx(40.12f).margin(0.05));

    // Grid 37..44 at 2 Hz/s.
    f = drive_in(tone_window(38.0f, 200.0f), 43.0f, 2.0f);
    REQUIRE(f.has_value());
    CHECK(*f == Approx(38.0f).margin(0.1));
}

TEST_CASE("find_drive_hz refuses what is not a drive inside the grid", "[sweep_preview][lock]") {
    const auto w40 = tone_window(40.0f, 200.0f);
    // Grid 42..45.5: the argmax sits on its low end.
    CHECK_FALSE(drive_in(w40, 45.0f).has_value());
    // Fewer than 3 meaningful grid points near DC.
    CHECK_FALSE(drive_in(w40, 0.3f).has_value());
    CHECK_FALSE(find_drive_hz(w40.data(), 0, 3053.0f, 41.5f, 1.0f).has_value());

    helix::calibration::test::Hiss rng;
    const auto hiss = helix::calibration::test::hiss_bed(1527, 30.0f, 3053.0f, rng);
    CHECK_FALSE(drive_in(hiss, 41.5f).has_value());
}

TEST_CASE("DriveLock locks once five estimates of t0 agree", "[sweep_preview][lock]") {
    const SweepPlan plan{5.0f, 135.0f, 1.0f};
    DriveLock lock(plan);
    CHECK(std::isnan(lock.drive_start_s()));
    CHECK(std::isnan(lock.model_hz(5.0)));

    lock.observe(2.00, 5.30f);
    lock.observe(2.25, 5.55f);
    lock.observe(2.50, 5.80f);
    lock.observe(2.75, 6.05f);
    CHECK_FALSE(lock.locked());
    lock.observe(3.00, 6.30f);
    REQUIRE(lock.locked());
    CHECK(lock.drive_start_s() == Approx(1.70).margin(1e-6));
    CHECK(lock.model_hz(10.0) == Approx(13.3f).margin(1e-4));
}

TEST_CASE("DriveLock ignores an outlier and spread estimates", "[sweep_preview][lock]") {
    const SweepPlan plan{5.0f, 135.0f, 1.0f};
    {
        DriveLock lock(plan);
        observe_estimate(lock, 2.00, 1.70, plan);
        observe_estimate(lock, 2.25, 1.70, plan);
        lock.observe(2.50, 4.30f); // estimate 3.20
        observe_estimate(lock, 2.75, 1.70, plan);
        observe_estimate(lock, 3.00, 1.70, plan);
        CHECK_FALSE(lock.locked());
        observe_estimate(lock, 3.25, 1.70, plan);
        REQUIRE(lock.locked());
        CHECK(lock.drive_start_s() == Approx(1.70).margin(1e-5));
    }
    {
        DriveLock lock(plan);
        for (int k = 0; k < 8; ++k) {
            observe_estimate(lock, 3.0 + 0.25 * k, 1.0 + 0.2 * k, plan);
        }
        CHECK_FALSE(lock.locked());
    }
}

TEST_CASE("DriveLock takes the median of the densest cluster", "[sweep_preview][lock]") {
    const SweepPlan plan{5.0f, 135.0f, 1.0f};
    DriveLock lock(plan);
    const double estimates[] = {1.60, 1.65, 1.70, 1.75, 1.80};
    for (int k = 0; k < 5; ++k) {
        observe_estimate(lock, 3.0 + 0.25 * k, estimates[k], plan);
    }
    REQUIRE(lock.locked());
    CHECK(lock.drive_start_s() == Approx(1.70).margin(1e-5));
}

TEST_CASE("DriveLock forgets estimates past its history", "[sweep_preview][lock]") {
    const SweepPlan plan{5.0f, 135.0f, 1.0f};
    DriveLock lock(plan);
    double at = 2.0;
    observe_estimate(lock, at, 1.70, plan);
    for (int k = 0; k < 16; ++k) {
        at += 0.25;
        observe_estimate(lock, at, 5.0 + 0.2 * k, plan);
    }
    for (int k = 0; k < 4; ++k) {
        at += 0.25;
        observe_estimate(lock, at, 1.70, plan);
    }
    CHECK_FALSE(lock.locked());
    at += 0.25;
    observe_estimate(lock, at, 1.70, plan);
    CHECK(lock.locked());
}

TEST_CASE("DriveLock drops a lock the readings disagree with", "[sweep_preview][lock]") {
    const SweepPlan plan{5.0f, 135.0f, 1.0f};
    auto locked_at_1_70 = [&plan]() {
        DriveLock lock(plan);
        for (int k = 0; k < 5; ++k) {
            observe_estimate(lock, 2.0 + 0.25 * k, 1.70, plan);
        }
        REQUIRE(lock.locked());
        return lock;
    };

    SECTION("three seconds of disagreement loses it") {
        auto lock = locked_at_1_70();
        double at = 10.0;
        for (; at <= 12.75 + 1e-9; at += 0.25) {
            lock.observe(at, lock.model_hz(at) + 2.0f);
        }
        CHECK(lock.locked());
        lock.observe(13.0, lock.model_hz(13.0) + 2.0f);
        CHECK_FALSE(lock.locked());
        CHECK(lock.drive_start_s() == Approx(1.70).margin(1e-5));
        CHECK(std::isnan(lock.model_hz(14.0)));

        // A fresh cluster relocks.
        for (int k = 0; k < 5; ++k) {
            observe_estimate(lock, 14.0 + 0.25 * k, 6.70, plan);
        }
        REQUIRE(lock.locked());
        CHECK(lock.drive_start_s() == Approx(6.70).margin(1e-5));
    }

    SECTION("one agreeing reading resets the timer") {
        auto lock = locked_at_1_70();
        for (double at = 10.0; at <= 12.75 + 1e-9; at += 0.25) {
            lock.observe(at, lock.model_hz(at) + 2.0f);
        }
        lock.observe(13.0, lock.model_hz(13.0));
        for (double at = 13.25; at <= 16.0 + 1e-9; at += 0.25) {
            lock.observe(at, lock.model_hz(at) + 2.0f);
        }
        CHECK(lock.locked());
    }
}

TEST_CASE("DriveLock counts a reading exactly LOCK_LOST_HZ off as agreeing",
          "[sweep_preview][lock]") {
    const SweepPlan plan{5.0f, 135.0f, 1.0f};
    DriveLock lock(plan);
    // Exact binary values: estimates of exactly 1.5.
    for (int k = 0; k < 5; ++k) {
        const double at = 2.0 + 0.25 * k;
        lock.observe(at, static_cast<float>(5.5 + 0.25 * k));
    }
    REQUIRE(lock.locked());
    REQUIRE(lock.drive_start_s() == 1.5);
    for (double at = 10.0; at <= 20.0; at += 0.25) {
        const float model = lock.model_hz(at);
        REQUIRE(model == static_cast<float>(5.0 + (at - 1.5)));
        lock.observe(at, model + sweep_preview::LOCK_LOST_HZ);
    }
    CHECK(lock.locked());
}

TEST_CASE("DriveLock scales estimates by the sweep rate", "[sweep_preview][lock]") {
    const SweepPlan plan{5.0f, 135.0f, 2.0f};
    DriveLock lock(plan);
    for (int k = 0; k < 5; ++k) {
        lock.observe(3.0 + 0.25 * k, 7.6f + 0.5f * static_cast<float>(k));
    }
    REQUIRE(lock.locked());
    CHECK(lock.drive_start_s() == Approx(1.70).margin(1e-5));
}
