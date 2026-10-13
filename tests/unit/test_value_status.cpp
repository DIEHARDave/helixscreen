// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_value_status.cpp
 * @brief helix::status classifiers at their thresholds.
 *
 * Run with: ./build/bin/helix-tests "[value_status]"
 */

#include "value_status.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

TEST_CASE("override_status: Warn whenever the override is off 100", "[value_status]") {
    struct Row {
        int pct;
        ValueStatus expected;
    };
    const Row rows[] = {
        {100, ValueStatus::Normal}, {99, ValueStatus::Warn},  {101, ValueStatus::Warn},
        {50, ValueStatus::Warn},    {200, ValueStatus::Warn}, {0, ValueStatus::Warn},
    };
    for (const auto& r : rows) {
        INFO(r.pct);
        CHECK(status::override_status(r.pct) == r.expected);
    }
}

TEST_CASE("z_offset_status: Warn exactly while a save is available", "[value_status]") {
    CHECK(status::z_offset_status(true) == ValueStatus::Warn);
    CHECK(status::z_offset_status(false) == ValueStatus::Normal);
}

TEST_CASE("mesh_flatness_status: bands at 0.15 and 0.30 mm", "[value_status]") {
    struct Row {
        float range_mm;
        ValueStatus expected;
    };
    const Row rows[] = {
        {0.0f, ValueStatus::Normal}, {0.149f, ValueStatus::Normal}, {0.15f, ValueStatus::Warn},
        {0.299f, ValueStatus::Warn}, {0.30f, ValueStatus::Danger},  {1.2f, ValueStatus::Danger},
    };
    for (const auto& r : rows) {
        INFO(r.range_mm);
        CHECK(status::mesh_flatness_status(r.range_mm) == r.expected);
    }
}

TEST_CASE("ValueStatus ints match the humidity status encoding", "[value_status]") {
    CHECK(static_cast<int>(ValueStatus::Normal) == 0);
    CHECK(static_cast<int>(ValueStatus::Warn) == 1);
    CHECK(static_cast<int>(ValueStatus::Danger) == 2);
}
