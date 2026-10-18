// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
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
