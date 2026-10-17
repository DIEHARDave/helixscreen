// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver_pipes_sim.h"

#include <array>
#include <random>
#include <set>
#include <tuple>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::ui;

namespace {

bool dims_eq(PipeGridDims a, PipeGridDims b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

} // namespace

// ============================================================================
// Grid and growth rules
// ============================================================================

TEST_CASE("the grid follows the screen's aspect with 15 nodes on the long axis",
          "[screensaver][pipes_sim]") {
    CHECK(dims_eq(pipes_grid_dims(800, 480), {15, 9, 15}));
    CHECK(dims_eq(pipes_grid_dims(480, 800), {9, 15, 15}));
    CHECK(dims_eq(pipes_grid_dims(480, 480), {15, 15, 15}));
    CHECK(dims_eq(pipes_grid_dims(1024, 600), {15, 8, 15}));
    CHECK(dims_eq(pipes_grid_dims(480, 272), {15, 8, 15}));
    CHECK(pipes_grid_dims(4000, 10).y == 1);
    CHECK(pipes_grid_dims(10, 4000).x == 1);
}

TEST_CASE("a grid counts what is taken and finds its last empty node", "[screensaver][pipes_sim]") {
    PipesGrid grid;
    grid.reset({3, 2, 2});
    CHECK(grid.empty_count() == 12);
    const PipeNode last{1, 1, 0};
    for (int z = 0; z < 2; z++) {
        for (int y = 0; y < 2; y++) {
            for (int x = 0; x < 3; x++) {
                if (!(PipeNode{x, y, z} == last)) {
                    grid.take({x, y, z});
                }
            }
        }
    }
    CHECK(grid.empty_count() == 1);
    // Taking a taken node again counts nothing.
    grid.take({0, 0, 0});
    CHECK(grid.empty_count() == 1);
    for (uint32_t seed = 1; seed <= 20; seed++) {
        std::minstd_rand rng(seed);
        PipeNode found;
        REQUIRE(grid.random_empty(rng, found));
        CHECK(found == last);
    }
    grid.take(last);
    std::minstd_rand rng(1);
    PipeNode found;
    CHECK_FALSE(grid.random_empty(rng, found));
    CHECK(grid.empty_count() == 0);
    CHECK_FALSE(grid.in_bounds({-1, 0, 0}));
    CHECK_FALSE(grid.in_bounds({3, 0, 0}));
    CHECK(grid.in_bounds({2, 1, 1}));
    CHECK_FALSE(grid.empty({3, 0, 0}));
    CHECK_FALSE(grid.empty({0, -1, 0}));
}

TEST_CASE("empty_run stops at the first taken node and at the limit", "[screensaver][pipes_sim]") {
    PipesGrid grid;
    grid.reset({15, 9, 15});
    CHECK(grid.empty_run({7, 4, 7}, PipeDir::POS_X, 2) == 2);
    CHECK(grid.empty_run({7, 4, 7}, PipeDir::POS_X, 5) == 5);
    grid.take({9, 4, 7});
    CHECK(grid.empty_run({7, 4, 7}, PipeDir::POS_X, 2) == 1);
    CHECK(grid.empty_run({7, 0, 7}, PipeDir::NEG_Y, 2) == 0);
    CHECK(grid.empty_run({7, 7, 7}, PipeDir::POS_Y, 5) == 1);
}

TEST_CASE("a pipe with no empty neighbour goes nowhere", "[screensaver][pipes_sim]") {
    PipesGrid grid;
    grid.reset({1, 1, 1});
    grid.take({0, 0, 0});
    std::minstd_rand rng(3);
    for (int d = 0; d < PIPE_DIR_COUNT; d++) {
        for (int w : {1, 4, 50, 100}) {
            CHECK(pipes_choose_direction(grid, {0, 0, 0}, static_cast<PipeDir>(d), w, rng) ==
                  PipeDir::NONE);
        }
    }
}

TEST_CASE("the heaviest straight weight always goes straight while it can",
          "[screensaver][pipes_sim]") {
    PipesGrid grid;
    grid.reset({15, 9, 15});
    std::minstd_rand rng(11);
    for (int i = 0; i < 1000; i++) {
        REQUIRE(pipes_choose_direction(grid, {7, 4, 7}, PipeDir::POS_X, 100, rng) ==
                PipeDir::POS_X);
    }
    grid.take({8, 4, 7});
    for (int i = 0; i < 1000; i++) {
        const PipeDir d = pipes_choose_direction(grid, {7, 4, 7}, PipeDir::POS_X, 100, rng);
        REQUIRE(d != PipeDir::POS_X);
        REQUIRE(d != PipeDir::NONE);
    }
}

TEST_CASE("straight carries its weight against the empty turns", "[screensaver][pipes_sim]") {
    PipesGrid grid;
    grid.reset({15, 9, 15});
    std::minstd_rand rng(5);
    const auto straight_fraction = [&]() {
        int straight = 0;
        for (int i = 0; i < 6000; i++) {
            straight +=
                pipes_choose_direction(grid, {7, 4, 7}, PipeDir::POS_X, 4, rng) == PipeDir::POS_X
                    ? 1
                    : 0;
        }
        return straight / 6000.0;
    };
    // The reverse is empty: five turns against a weight of 4.
    const double open = straight_fraction();
    CHECK(open > 0.41);
    CHECK(open < 0.48);
    grid.take({6, 4, 7});
    const double closed = straight_fraction();
    CHECK(closed > 0.47);
    CHECK(closed < 0.53);
}

TEST_CASE("a turn goes where the most empty nodes lie, two ahead", "[screensaver][pipes_sim]") {
    PipesGrid grid;
    grid.reset({15, 9, 15});
    const PipeNode pos{7, 4, 7};
    grid.take({8, 4, 7});
    grid.take({6, 4, 7});
    grid.take({7, 6, 7}); // +Y run 1
    grid.take({7, 4, 9}); // +Z run 1
    grid.take({7, 4, 5}); // -Z run 1
    std::minstd_rand rng(17);
    const auto seen = [&]() {
        std::set<PipeDir> dirs;
        for (int i = 0; i < 500; i++) {
            dirs.insert(pipes_choose_direction(grid, pos, PipeDir::POS_X, 1, rng));
        }
        return dirs;
    };
    CHECK(seen() == std::set<PipeDir>{PipeDir::NEG_Y});

    // A fresh grid with +Y open two ahead ties -Y, whose four empty nodes count as two.
    grid.reset({15, 9, 15});
    grid.take({8, 4, 7});
    grid.take({6, 4, 7});
    grid.take({7, 4, 9});
    grid.take({7, 4, 5});
    CHECK(seen() == std::set<PipeDir>{PipeDir::POS_Y, PipeDir::NEG_Y});

    grid.reset({15, 9, 15});
    grid.take({8, 4, 7});
    grid.take({6, 4, 7});
    grid.take({7, 4, 10}); // +Z run 2
    grid.take({7, 4, 5});
    CHECK(seen() == std::set<PipeDir>{PipeDir::POS_Y, PipeDir::NEG_Y, PipeDir::POS_Z});
}

TEST_CASE("joints mix a third balls, the rest elbows, and a teapot 1 in 1000",
          "[screensaver][pipes_sim]") {
    std::minstd_rand rng(42);
    std::array<uint32_t, 3> n{};
    for (int i = 0; i < 1'000'000; i++) {
        n[static_cast<size_t>(pipes_choose_joint(rng))]++;
    }
    CHECK(n[2] >= 850);
    CHECK(n[2] <= 1150);
    const double ball = double(n[1]) / (n[0] + n[1]);
    CHECK(ball > 0.330);
    CHECK(ball < 0.337);
}

TEST_CASE("a pipe's straight weight is 1-4, or 25-100 one time in 20", "[screensaver][pipes_sim]") {
    std::minstd_rand rng(9);
    std::array<int, 101> counts{};
    for (int i = 0; i < 100000; i++) {
        const int w = pipes_choose_weight_straight(rng);
        REQUIRE(((w >= 1 && w <= 4) || (w >= 25 && w <= 100)));
        counts[static_cast<size_t>(w)]++;
    }
    int heavy = 0;
    for (int w = 25; w <= 100; w++) {
        heavy += counts[static_cast<size_t>(w)];
    }
    CHECK(heavy / 100000.0 > 0.045);
    CHECK(heavy / 100000.0 < 0.055);
    for (int w = 1; w <= 4; w++) {
        CHECK(counts[static_cast<size_t>(w)] / 100000.0 > 0.23);
        CHECK(counts[static_cast<size_t>(w)] / 100000.0 < 0.245);
    }
    CHECK(counts[25] > 0);
    CHECK(counts[100] > 0);
}

#endif // HELIX_ENABLE_SCREENSAVER
