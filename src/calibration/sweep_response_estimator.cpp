// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "sweep_response_estimator.h"

#include <algorithm>
#include <array>
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

std::optional<float> find_drive_hz(const AccelSample* samples, size_t n, float rate_hz,
                                   float hint_hz, float hz_per_sec) {
    using namespace sweep_preview;
    if (samples == nullptr || n < 2 || !(rate_hz > 0.0f) || !std::isfinite(hint_hz) ||
        !(hz_per_sec > 0.0f)) {
        return std::nullopt;
    }

    const double lo = static_cast<double>(hint_hz) - LOCK_SEARCH_LEAD_S * hz_per_sec;
    const double hi = static_cast<double>(hint_hz) + LOCK_SEARCH_LAG_S * hz_per_sec;
    std::vector<float> grid;
    for (int k = 0;; ++k) {
        const double f = lo + k * static_cast<double>(LOCK_GRID_STEP_HZ);
        if (f > hi + 1e-6) {
            break;
        }
        if (f > 0.0) {
            grid.push_back(static_cast<float>(f));
        }
    }
    if (grid.size() < 3) {
        return std::nullopt;
    }

    std::array<std::vector<float>, 3> axes;
    for (auto& a : axes) {
        a.resize(n);
    }
    for (size_t i = 0; i < n; ++i) {
        axes[0][i] = samples[i].x;
        axes[1][i] = samples[i].y;
        axes[2][i] = samples[i].z;
    }
    auto power_at = [&](float f) {
        double p = 0.0;
        for (const auto& a : axes) {
            p += goertzel_psd(a.data(), n, f, rate_hz);
        }
        return p;
    };

    std::vector<double> power(grid.size());
    size_t best = 0;
    for (size_t k = 0; k < grid.size(); ++k) {
        power[k] = power_at(grid[k]);
        if (power[k] > power[best]) {
            best = k;
        }
    }
    if (best == 0 || best + 1 == grid.size()) {
        return std::nullopt;
    }

    double reference = 0.0;
    int references = 0;
    for (float offset : LOCK_REFERENCE_OFFSETS_HZ) {
        const float f = grid[best] + offset;
        if (f > 0.0f) {
            reference += power_at(f);
            ++references;
        }
    }
    if (references == 0) {
        return std::nullopt;
    }
    reference /= references;
    if (!(reference > 0.0) || power[best] < LOCK_MIN_CONTRAST * reference) {
        return std::nullopt;
    }

    const double a = power[best - 1];
    const double b = power[best];
    const double c = power[best + 1];
    const double denom = a - 2.0 * b + c;
    double offset = denom != 0.0 ? 0.5 * (a - c) / denom : 0.0;
    offset = std::clamp(offset, -0.5, 0.5);
    return static_cast<float>(grid[best] + offset * LOCK_GRID_STEP_HZ);
}

DriveLock::DriveLock(SweepPlan plan) : plan_(plan) {}

void DriveLock::observe(double at_s, float drive_hz) {
    using namespace sweep_preview;
    if (!plan_.valid() || !std::isfinite(at_s) || !std::isfinite(drive_hz)) {
        return;
    }

    if (locked_) {
        const double model = static_cast<double>(plan_.freq_start_hz) +
                             static_cast<double>(plan_.hz_per_sec) * (at_s - t0_s_);
        if (std::abs(static_cast<double>(drive_hz) - model) <= LOCK_LOST_HZ) {
            disagree_since_s_ = std::numeric_limits<double>::quiet_NaN();
            return;
        }
        if (std::isnan(disagree_since_s_)) {
            disagree_since_s_ = at_s;
        } else if (at_s - disagree_since_s_ >= LOCK_LOST_S) {
            locked_ = false;
            estimates_.clear();
            disagree_since_s_ = std::numeric_limits<double>::quiet_NaN();
        }
        return;
    }

    estimates_.push_back(at_s - (static_cast<double>(drive_hz) - plan_.freq_start_hz) /
                                    static_cast<double>(plan_.hz_per_sec));
    while (estimates_.size() > LOCK_HISTORY) {
        estimates_.pop_front();
    }

    std::vector<double> best_cluster;
    for (double centre : estimates_) {
        std::vector<double> cluster;
        for (double e : estimates_) {
            if (std::abs(e - centre) <= LOCK_AGREE_S + 1e-9) {
                cluster.push_back(e);
            }
        }
        if (cluster.size() > best_cluster.size()) {
            best_cluster = std::move(cluster);
        }
    }
    if (best_cluster.size() < LOCK_AGREE_COUNT) {
        return;
    }
    std::sort(best_cluster.begin(), best_cluster.end());
    const size_t mid = best_cluster.size() / 2;
    t0_s_ = best_cluster.size() % 2 == 1 ? best_cluster[mid]
                                         : 0.5 * (best_cluster[mid - 1] + best_cluster[mid]);
    locked_ = true;
    disagree_since_s_ = std::numeric_limits<double>::quiet_NaN();
}

bool DriveLock::locked() const {
    return locked_;
}

double DriveLock::drive_start_s() const {
    return t0_s_;
}

float DriveLock::model_hz(double at_s) const {
    if (!locked_) {
        return kNaNf;
    }
    return static_cast<float>(static_cast<double>(plan_.freq_start_hz) +
                              static_cast<double>(plan_.hz_per_sec) * (at_s - t0_s_));
}

namespace {

/// Index of the first sample at or after `t_s`.
size_t first_at_or_after(const std::deque<AccelSample>& buffer, double t_s) {
    const auto it =
        std::lower_bound(buffer.begin(), buffer.end(), t_s, [](const AccelSample& s, double t) {
            return static_cast<double>(s.time) < t;
        });
    return static_cast<size_t>(it - buffer.begin());
}

/// Index of the first sample after `t_s`.
size_t first_after(const std::deque<AccelSample>& buffer, double t_s) {
    const auto it =
        std::upper_bound(buffer.begin(), buffer.end(), t_s, [](double t, const AccelSample& s) {
            return t < static_cast<double>(s.time);
        });
    return static_cast<size_t>(it - buffer.begin());
}

bool overlaps_gap(const std::deque<std::pair<double, double>>& gaps, double lo_s, double hi_s) {
    for (const auto& [first, last] : gaps) {
        if (lo_s <= last && first <= hi_s) {
            return true;
        }
    }
    return false;
}

} // namespace

SweepResponseAccumulator::SweepResponseAccumulator(SweepPlan plan) : plan_(plan), lock_(plan) {}

void SweepResponseAccumulator::note_console_frequency(float hz) {
    if (std::isfinite(hz) && hz > 0.0f) {
        hint_hz_ = hz;
    }
}

std::vector<SweepPoint>
SweepResponseAccumulator::add_samples(const std::vector<AccelSample>& samples, bool contiguous,
                                      float rate_hz) {
    using namespace sweep_preview;
    std::vector<SweepPoint> finished;
    if (samples.empty() || !plan_.valid()) {
        return finished;
    }

    buffer_.insert(buffer_.end(), samples.begin(), samples.end());
    stream_time_s_ = static_cast<double>(buffer_.back().time);
    if (!contiguous) {
        gaps_.emplace_back(static_cast<double>(samples.front().time),
                           static_cast<double>(samples.back().time));
    }
    const double keep_from = stream_time_s_ - BUFFER_S;
    while (!buffer_.empty() && static_cast<double>(buffer_.front().time) < keep_from) {
        buffer_.pop_front();
    }
    while (!gaps_.empty() && gaps_.front().second < keep_from) {
        gaps_.pop_front();
    }
    if (!(rate_hz > 0.0f)) {
        return finished;
    }

    // Drive-frequency readings, each over the LOCK_WINDOW_S ending at its time.
    const double front_s = static_cast<double>(buffer_.front().time);
    next_reading_s_ = std::max(next_reading_s_, front_s + LOCK_WINDOW_S);
    std::vector<AccelSample> window;
    for (; next_reading_s_ <= stream_time_s_; next_reading_s_ += LOCK_INTERVAL_S) {
        if (!std::isfinite(hint_hz_)) {
            continue;
        }
        const double lo = next_reading_s_ - LOCK_WINDOW_S;
        if (overlaps_gap(gaps_, lo, next_reading_s_)) {
            continue;
        }
        const size_t begin = first_after(buffer_, lo);
        const size_t end = first_after(buffer_, next_reading_s_);
        window.assign(buffer_.begin() + static_cast<std::ptrdiff_t>(begin),
                      buffer_.begin() + static_cast<std::ptrdiff_t>(end));
        const auto drive =
            find_drive_hz(window.data(), window.size(), rate_hz, hint_hz_, plan_.hz_per_sec);
        if (drive) {
            lock_.observe(next_reading_s_ - LOCK_WINDOW_S / 2.0, *drive);
        }
    }

    // Steps whose span the newest sample has passed. Before the first lock
    // there is no t0 to place them with; after a loss the last one places
    // them, unclean.
    const double t0 = lock_.drive_start_s();
    if (std::isnan(t0)) {
        return finished;
    }
    const size_t count = plan_.point_count();
    const double start = plan_.freq_start_hz;
    const double rate = plan_.hz_per_sec;
    const double half = STEP_HZ / 2.0;
    std::array<std::vector<float>, 3> axes;
    while (next_point_ < count) {
        const double f = plan_.point_hz(next_point_);
        const double f_lo = std::max(f - half, start);
        const double f_hi = std::min(f + half, static_cast<double>(plan_.freq_end_hz));
        const double t_lo = t0 + (f_lo - start) / rate;
        const double t_hi = t0 + (f_hi - start) / rate;
        if (stream_time_s_ <= t_hi) {
            break;
        }

        SweepPoint point;
        point.freq_hz = static_cast<float>(f);
        const size_t begin = first_at_or_after(buffer_, t_lo);
        const size_t end = first_after(buffer_, t_hi);
        point.clean = lock_.locked() && t_lo >= static_cast<double>(buffer_.front().time) &&
                      !overlaps_gap(gaps_, t_lo, t_hi) && end >= begin + 2;
        if (point.clean) {
            for (auto& a : axes) {
                a.clear();
            }
            for (size_t i = begin; i < end; ++i) {
                axes[0].push_back(buffer_[i].x);
                axes[1].push_back(buffer_[i].y);
                axes[2].push_back(buffer_[i].z);
            }
            const auto fz = static_cast<float>(f);
            point.psd_x = goertzel_psd(axes[0].data(), axes[0].size(), fz, rate_hz);
            point.psd_y = goertzel_psd(axes[1].data(), axes[1].size(), fz, rate_hz);
            point.psd_z = goertzel_psd(axes[2].data(), axes[2].size(), fz, rate_hz);
        } else {
            point.psd_x = point.psd_y = point.psd_z = kNaNf;
        }
        finished.push_back(point);
        ++next_point_;
    }
    return finished;
}

bool SweepResponseAccumulator::locked() const {
    return lock_.locked();
}

double SweepResponseAccumulator::drive_start_s() const {
    return lock_.drive_start_s();
}

bool SweepResponseAccumulator::complete() const {
    return plan_.valid() && next_point_ >= plan_.point_count();
}

double SweepResponseAccumulator::stream_time_s() const {
    return stream_time_s_;
}

const SweepPlan& SweepResponseAccumulator::plan() const {
    return plan_;
}

} // namespace helix::calibration
