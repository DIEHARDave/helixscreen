// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "live_sweep_preview.h"

#include "text_io.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace helix::calibration {

namespace {

std::optional<std::string> non_empty(const std::string& chip) {
    const auto trimmed = text_io::trim(chip);
    if (trimmed.empty()) {
        return std::nullopt;
    }
    return std::string(trimmed);
}

} // namespace

std::optional<std::string> accel_chip_for_sweep(const ResonanceTesterConfig& cfg, SweepAxis axis) {
    const auto chip = non_empty(cfg.accel_chip);
    const auto chip_x = non_empty(cfg.accel_chip_x);
    const auto chip_y = non_empty(cfg.accel_chip_y);
    switch (axis) {
    case SweepAxis::X:
        return chip_x ? chip_x : chip;
    case SweepAxis::Y:
        return chip_y ? chip_y : chip;
    case SweepAxis::DIAGONAL:
        if (!chip_x && !chip_y) {
            return chip;
        }
        // Klipper's resonance tester reads equal per-axis chips as one chip.
        if (chip_x && chip_y && *chip_x == *chip_y) {
            return chip_x;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<SweepPlan> sweep_plan_for(const ResonanceTesterConfig& cfg,
                                        std::optional<BeltSweepWindow> window) {
    SweepPlan plan{cfg.min_freq, cfg.max_freq, cfg.hz_per_sec};
    if (window) {
        plan.freq_start_hz = std::floor(window->freq_start_hz);
        plan.freq_end_hz = std::ceil(window->freq_end_hz);
    }
    if (!plan.valid()) {
        return std::nullopt;
    }
    return plan;
}

std::optional<LiveSweepTarget> live_sweep_target(const LiveSweepFacts& facts, SweepAxis axis,
                                                 std::optional<BeltSweepWindow> window) {
    if (facts.socket_path.empty() || !facts.socket_reachable) {
        return std::nullopt;
    }
    auto chip = accel_chip_for_sweep(facts.cfg, axis);
    if (!chip) {
        return std::nullopt;
    }
    const auto plan = sweep_plan_for(facts.cfg, window);
    if (!plan) {
        return std::nullopt;
    }
    return LiveSweepTarget{facts.socket_path, std::move(*chip), *plan};
}

PreviewSeries layout_preview(const std::vector<SweepPoint>& points, SweepColumn column,
                             float axis_lo_hz, float axis_hi_hz) {
    PreviewSeries out;
    if (!std::isfinite(axis_lo_hz) || !std::isfinite(axis_hi_hz) || axis_hi_hz <= axis_lo_hz) {
        return out;
    }
    const double lo = axis_lo_hz;
    const double hi = axis_hi_hz;
    const auto n = static_cast<size_t>(std::lround((hi - lo) / sweep_preview::STEP_HZ)) + 1;
    const double spacing = n > 1 ? (hi - lo) / static_cast<double>(n - 1) : 0.0;
    const double reach = sweep_preview::STEP_HZ / 2.0;
    out.freqs.resize(n);
    out.amps.assign(n, std::numeric_limits<float>::quiet_NaN());

    for (size_t i = 0; i < n; ++i) {
        const double slot = lo + static_cast<double>(i) * spacing;
        out.freqs[i] = static_cast<float>(slot);

        // Points arrive in sweep order, so frequency ascending.
        const auto above =
            std::lower_bound(points.begin(), points.end(), slot, [](const SweepPoint& p, double f) {
                return static_cast<double>(p.freq_hz) < f;
            });
        const SweepPoint* nearest = nullptr;
        double nearest_distance = std::numeric_limits<double>::infinity();
        if (above != points.begin()) {
            const auto& below = *(above - 1);
            nearest = &below;
            nearest_distance = slot - static_cast<double>(below.freq_hz);
        }
        // Strictly closer: a tie keeps the lower frequency's point.
        if (above != points.end() &&
            static_cast<double>(above->freq_hz) - slot < nearest_distance) {
            nearest = &*above;
            nearest_distance = static_cast<double>(above->freq_hz) - slot;
        }
        if (nearest == nullptr || nearest_distance > reach) {
            continue;
        }
        const float amp = sweep_column_value(*nearest, column);
        out.amps[i] = amp;
        if (std::isfinite(amp)) {
            out.max_amp = std::max(out.max_amp, amp);
        }
    }
    return out;
}

std::string parse_klippy_uds_address(const nlohmann::json& data) {
    if (!data.is_object()) {
        return {};
    }
    const nlohmann::json* root = &data;
    if (const auto result = data.find("result"); result != data.end() && result->is_object()) {
        root = &*result;
    }
    const auto config = root->find("config");
    if (config == root->end() || !config->is_object()) {
        return {};
    }
    const auto server = config->find("server");
    if (server == config->end() || !server->is_object()) {
        return {};
    }
    const auto address = server->find("klippy_uds_address");
    if (address == server->end() || !address->is_string()) {
        return {};
    }
    return address->get_ref<const std::string&>();
}

} // namespace helix::calibration
