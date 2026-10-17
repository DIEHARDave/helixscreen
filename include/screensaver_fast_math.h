// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cmath>
#include <cstdint>

/**
 * @file screensaver_fast_math.h
 * @brief Table sine and e^-s for savers that evaluate them per pixel or per grid cell
 *
 * The tables are built at compile time into .rodata, so a saver that is not running costs no
 * RAM for them. Linear interpolation between entries keeps the sine within 5e-6 of std::sin.
 */

namespace helix::ui {

/// pi and 2 pi as floats, for every saver's angles.
inline constexpr float SAVER_PI = 3.14159265358979f;
inline constexpr float SAVER_TWO_PI = 2.0f * SAVER_PI;

inline constexpr int FAST_SIN_N = 1024;
inline constexpr int FAST_EXP_N = 512;
/// e^-s reads as 0 past this.
inline constexpr double FAST_EXP_MAX = 9.0;

namespace fast_math_detail {

constexpr double sin_series(double x) {
    // x in [-pi, pi]: the Taylor series converges to float precision in a dozen terms.
    double term = x;
    double sum = x;
    for (int n = 1; n < 14; n++) {
        term *= -x * x / static_cast<double>((2 * n) * (2 * n + 1));
        sum += term;
    }
    return sum;
}

constexpr double exp_series(double x) {
    double term = 1.0;
    double sum = 1.0;
    for (int n = 1; n < 60; n++) {
        term *= x / static_cast<double>(n);
        sum += term;
    }
    return sum;
}

} // namespace fast_math_detail

struct FastSinTable {
    float v[FAST_SIN_N + 1];
};

constexpr FastSinTable make_fast_sin_table() {
    FastSinTable t{};
    constexpr double pi = 3.14159265358979323846;
    for (int i = 0; i <= FAST_SIN_N; i++) {
        double a = 2.0 * pi * static_cast<double>(i) / FAST_SIN_N;
        if (a > pi) {
            a -= 2.0 * pi;
        }
        t.v[i] = static_cast<float>(fast_math_detail::sin_series(a));
    }
    return t;
}

struct FastExpTable {
    float v[FAST_EXP_N + 1];
};

constexpr FastExpTable make_fast_exp_table() {
    FastExpTable t{};
    for (int i = 0; i <= FAST_EXP_N; i++) {
        t.v[i] = static_cast<float>(
            1.0 / fast_math_detail::exp_series(FAST_EXP_MAX * static_cast<double>(i) / FAST_EXP_N));
    }
    return t;
}

/// sin over one turn in FAST_SIN_N steps, one entry past the end for interpolation.
inline constexpr FastSinTable FAST_SIN_TABLE = make_fast_sin_table();
/// e^-s for s from 0 to FAST_EXP_MAX in FAST_EXP_N steps.
inline constexpr FastExpTable FAST_EXP_TABLE = make_fast_exp_table();

inline float fast_sin(float x) {
    constexpr float steps_per_radian = static_cast<float>(FAST_SIN_N) / 6.28318530717958f;
    const float f = x * steps_per_radian;
    const float fl = std::floor(f);
    const float frac = f - fl;
    const int i = static_cast<int>(static_cast<int64_t>(fl) & (FAST_SIN_N - 1));
    return FAST_SIN_TABLE.v[i] + (FAST_SIN_TABLE.v[i + 1] - FAST_SIN_TABLE.v[i]) * frac;
}

inline float fast_cos(float x) {
    return fast_sin(x + 1.57079632679490f);
}

/// e^-s for s >= 0, 0 past FAST_EXP_MAX.
inline float fast_exp_neg(float s) {
    if (s <= 0.0f) {
        return 1.0f;
    }
    const float f = s * (static_cast<float>(FAST_EXP_N) / static_cast<float>(FAST_EXP_MAX));
    if (f >= static_cast<float>(FAST_EXP_N)) {
        return 0.0f;
    }
    const auto i = static_cast<int>(f);
    return FAST_EXP_TABLE.v[i] +
           (FAST_EXP_TABLE.v[i + 1] - FAST_EXP_TABLE.v[i]) * (f - static_cast<float>(i));
}

/// e^-(q^2): a Gaussian of `q` standard-ish widths.
inline float fast_gauss(float q) {
    return fast_exp_neg(q * q);
}

} // namespace helix::ui
