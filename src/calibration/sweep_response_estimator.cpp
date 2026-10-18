// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "sweep_response_estimator.h"

#include <cmath>

namespace helix::calibration {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr float kNaNf = std::numeric_limits<float>::quiet_NaN();

} // namespace

bool SweepPlan::valid() const {
    return std::isfinite(freq_start_hz) && std::isfinite(freq_end_hz) &&
           std::isfinite(hz_per_sec) && freq_start_hz > 0.0f && freq_end_hz > freq_start_hz &&
           hz_per_sec > 0.0f;
}

size_t SweepPlan::point_count() const {
    if (!valid()) {
        return 0;
    }
    return static_cast<size_t>(std::floor((freq_end_hz - freq_start_hz) / sweep_preview::STEP_HZ)) +
           1;
}

float SweepPlan::point_hz(size_t index) const {
    return freq_start_hz + static_cast<float>(index) * sweep_preview::STEP_HZ;
}

float SweepPoint::psd_xyz() const {
    return psd_x + psd_y + psd_z;
}

float sweep_column_value(const SweepPoint& point, SweepColumn column) {
    if (!point.clean) {
        return kNaNf;
    }
    switch (column) {
    case SweepColumn::XYZ:
        return point.psd_xyz();
    case SweepColumn::X:
        return point.psd_x;
    case SweepColumn::Y:
        return point.psd_y;
    }
    return kNaNf;
}

float goertzel_psd(const float* x, size_t n, float freq_hz, float rate_hz) {
    if (n < 2 || !(rate_hz > 0.0f) || !(freq_hz > 0.0f)) {
        return 0.0f;
    }

    double mean = 0.0;
    for (size_t i = 0; i < n; ++i) {
        mean += x[i];
    }
    mean /= static_cast<double>(n);

    // Hann window and the bin's phasor both advance by a fixed rotation per
    // sample, so neither needs a cos/sin call inside the loop.
    const double window_step = 2.0 * kPi / static_cast<double>(n - 1);
    const double bin_step = 2.0 * kPi * static_cast<double>(freq_hz) / static_cast<double>(rate_hz);
    const double wc = std::cos(window_step);
    const double ws = std::sin(window_step);
    const double bc = std::cos(bin_step);
    const double bs = std::sin(bin_step);

    double win_re = 1.0; // cos(window_step * i)
    double win_im = 0.0;
    double ph_re = 1.0; // e^{-j bin_step i}
    double ph_im = 0.0;
    double acc_re = 0.0;
    double acc_im = 0.0;
    double sum_w2 = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * win_re;
        const double v = (static_cast<double>(x[i]) - mean) * w;
        acc_re += v * ph_re;
        acc_im += v * ph_im;
        sum_w2 += w * w;

        const double nwr = win_re * wc - win_im * ws;
        win_im = win_re * ws + win_im * wc;
        win_re = nwr;
        const double npr = ph_re * bc + ph_im * bs;
        ph_im = ph_im * bc - ph_re * bs;
        ph_re = npr;
    }
    if (sum_w2 <= 0.0) {
        return 0.0f;
    }
    const double power =
        2.0 * (acc_re * acc_re + acc_im * acc_im) / (static_cast<double>(rate_hz) * sum_w2);
    return static_cast<float>(power);
}

} // namespace helix::calibration
