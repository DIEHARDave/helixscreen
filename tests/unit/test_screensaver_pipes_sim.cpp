// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver_pipes_sim.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <random>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::ui;

namespace {

bool dims_eq(PipeGridDims a, PipeGridDims b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

bool vec_near(PipeVec3 a, PipeVec3 b, float eps = 1e-4f) {
    return std::fabs(a.x - b.x) <= eps && std::fabs(a.y - b.y) <= eps &&
           std::fabs(a.z - b.z) <= eps;
}

bool same_piece(const PipePiece& a, const PipePiece& b) {
    return a.kind == b.kind && vec_near(a.at, b.at) && a.dir == b.dir && a.from == b.from &&
           a.length == b.length && a.material == b.material;
}

PipePiece piece(PipePieceKind kind, PipeVec3 at, PipeDir dir = PipeDir::POS_X,
                PipeDir from = PipeDir::POS_X, float length = 0.0f, uint8_t material = 0) {
    PipePiece p;
    p.kind = kind;
    p.at = at;
    p.dir = dir;
    p.from = from;
    p.length = length;
    p.material = material;
    return p;
}

bool rgb_near(Rgb a, Rgb b, int tol = 1) {
    return std::abs(a.r - b.r) <= tol && std::abs(a.g - b.g) <= tol && std::abs(a.b - b.b) <= tol;
}

int channel_sum(Rgb c) {
    return c.r + c.g + c.b;
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

// ============================================================================
// One screen of pipes
// ============================================================================

TEST_CASE("pieces sit where the original draws them", "[screensaver][pipes_sim]") {
    const PipeGridDims dims{15, 9, 15};
    CHECK(vec_near(pipes_node_world({7, 4, 7}, dims), {0, 0, 0}));
    CHECK(vec_near(pipes_node_world({0, 0, 0}, dims), {-49, -28, -49}));
    CHECK(vec_near(pipes_node_world({14, 8, 14}, dims), {49, 28, 49}));

    std::vector<PipePiece> out;
    pipes_emit_start({7, 4, 7}, PipeDir::POS_Y, dims, 3, out);
    REQUIRE(out.size() == 2);
    CHECK(same_piece(
        out[0], piece(PipePieceKind::BALL, {0, 0, 0}, PipeDir::POS_X, PipeDir::POS_X, 0.0f, 3)));
    CHECK(same_piece(out[1], piece(PipePieceKind::CYLINDER, {0, 1, 0}, PipeDir::POS_Y,
                                   PipeDir::POS_X, 5.0f, 3)));

    out.clear();
    pipes_emit_move({7, 4, 7}, PipeDir::POS_X, PipeDir::POS_X, PipeJoint::BALL, dims, 5, out);
    REQUIRE(out.size() == 1);
    CHECK(same_piece(out[0], piece(PipePieceKind::CYLINDER, {-1, 0, 0}, PipeDir::POS_X,
                                   PipeDir::POS_X, 7.0f, 5)));

    out.clear();
    pipes_emit_move({7, 4, 7}, PipeDir::POS_X, PipeDir::POS_Z, PipeJoint::ELBOW, dims, 6, out);
    REQUIRE(out.size() == 2);
    CHECK(same_piece(
        out[0], piece(PipePieceKind::ELBOW, {0, 0, 0}, PipeDir::POS_Z, PipeDir::POS_X, 0.0f, 6)));
    CHECK(same_piece(out[1], piece(PipePieceKind::CYLINDER, {0, 0, 1}, PipeDir::POS_Z,
                                   PipeDir::POS_X, 5.0f, 6)));

    for (auto [joint, kind] : {std::pair{PipeJoint::BALL, PipePieceKind::BALL},
                               std::pair{PipeJoint::TEAPOT, PipePieceKind::TEAPOT}}) {
        out.clear();
        pipes_emit_move({7, 4, 7}, PipeDir::NEG_Y, PipeDir::NEG_X, joint, dims, 7, out);
        REQUIRE(out.size() == 2);
        CHECK(same_piece(out[0], piece(kind, {0, 0, 0}, PipeDir::POS_X, PipeDir::POS_X, 0.0f, 7)));
        CHECK(same_piece(out[1], piece(PipePieceKind::CYLINDER, {-1, 0, 0}, PipeDir::NEG_X,
                                       PipeDir::POS_X, 5.0f, 7)));
    }

    out.clear();
    pipes_emit_end({0, 0, 0}, dims, 9, out);
    REQUIRE(out.size() == 1);
    CHECK(same_piece(out[0], piece(PipePieceKind::BALL, {-49, -28, -49}, PipeDir::POS_X,
                                   PipeDir::POS_X, 0.0f, 9)));
}

TEST_CASE("a screen begins with 2 to 4 pipes on distinct taken nodes", "[screensaver][pipes_sim]") {
    std::set<int> counts;
    for (uint32_t seed = 1; seed <= 300; seed++) {
        std::minstd_rand rng(seed);
        PipesScreen screen;
        std::vector<PipePiece> out;
        screen.begin({15, 9, 15}, rng, out);
        int alive = 0;
        std::set<std::tuple<int, int, int>> positions;
        for (const PipeState& p : screen.pipes()) {
            if (!p.alive) {
                continue;
            }
            alive++;
            positions.insert({p.pos.x, p.pos.y, p.pos.z});
            CHECK_FALSE(screen.grid().empty(p.pos));
            CHECK(p.material < PIPE_MATERIAL_COUNT);
            CHECK(p.segment_count == 0);
        }
        CHECK(alive >= PIPES_MIN_ACTIVE);
        CHECK(alive <= PIPES_MAX_ACTIVE);
        CHECK(alive == screen.pipes_counted());
        CHECK(alive == screen.pipes_started());
        CHECK(positions.size() == static_cast<size_t>(alive));
        CHECK_FALSE(out.empty());
        counts.insert(alive);
    }
    CHECK(counts == std::set<int>{2, 3, 4});
}

TEST_CASE("a screen starts exactly 7 pipes before it finishes", "[screensaver][pipes_sim]") {
    for (uint32_t seed = 1; seed <= 40; seed++) {
        std::minstd_rand rng(seed);
        PipesScreen screen;
        std::vector<PipePiece> out;
        screen.begin({15, 9, 15}, rng, out);
        int steps = 0;
        while (screen.step(rng, out) && steps < 20000) {
            steps++;
        }
        REQUIRE(steps < 20000);
        CHECK(screen.finished());
        CHECK(screen.pipes_started() == PIPES_PER_SCREEN);
        CHECK(screen.grid().empty_count() > 0);
    }
}

TEST_CASE("a pipe stops only where every neighbour is taken", "[screensaver][pipes_sim]") {
    std::minstd_rand rng(5);
    PipesScreen screen;
    std::vector<PipePiece> out;
    screen.begin({15, 9, 15}, rng, out);
    bool any_stuck = false;
    for (int s = 0; s < 20000 && screen.step(rng, out); s++) {
        for (const PipeState& p : screen.pipes()) {
            if (!p.alive || !p.stuck) {
                continue;
            }
            any_stuck = true;
            for (int d = 0; d < PIPE_DIR_COUNT; d++) {
                CHECK_FALSE(screen.grid().empty(pipe_step(p.pos, static_cast<PipeDir>(d))));
            }
        }
    }
    REQUIRE(any_stuck);
}

TEST_CASE("a grid that runs out of nodes ends the screen", "[screensaver][pipes_sim]") {
    for (uint32_t seed = 1; seed <= 20; seed++) {
        std::minstd_rand rng(seed);
        PipesScreen screen;
        std::vector<PipePiece> out;
        // Two nodes: the first pipe takes both, the rest find nothing.
        screen.begin({2, 1, 1}, rng, out);
        CHECK(screen.pipes_started() == 1);
        CHECK(screen.grid().empty_count() == 0);
        CHECK(screen.step(rng, out));
        CHECK_FALSE(screen.step(rng, out));
        CHECK(screen.pipes_started() == 1);

        // One node: a teapot, then nothing.
        out.clear();
        screen.begin({1, 1, 1}, rng, out);
        REQUIRE(out.size() == 1);
        CHECK(out[0].kind == PipePieceKind::TEAPOT);
        CHECK_FALSE(screen.step(rng, out));
        CHECK(screen.pipes_started() == 1);
    }
}

TEST_CASE("over many turns a third of the joints are balls", "[screensaver][pipes_sim]") {
    std::minstd_rand rng(99);
    PipesScreen screen;
    std::vector<PipePiece> out;
    std::array<uint64_t, 3> joints{};
    uint64_t straight = 0;
    while (joints[0] + joints[1] + joints[2] < 20000) {
        screen.begin({15, 9, 15}, rng, out);
        while (screen.step(rng, out)) {
            out.clear();
        }
        for (size_t i = 0; i < 3; i++) {
            joints[i] += screen.joint_counts()[i];
        }
        straight += screen.straight_steps();
        out.clear();
    }
    const uint64_t total = joints[0] + joints[1] + joints[2];
    const double ball = double(joints[1]) / double(joints[0] + joints[1]);
    CHECK(ball > 0.31);
    CHECK(ball < 0.355);
    CHECK(double(joints[2]) <= 0.004 * double(total));
    // Straight on carries its weight (2.5 on average) against up to four empty turns, so it
    // wins about 40% of steps, well above the 20% an unweighted pick among five would give.
    const double straight_share = double(straight) / double(straight + total);
    CHECK(straight_share > 0.33);
    CHECK(straight_share < 0.5);
}

TEST_CASE("a screen replays exactly from a seed", "[screensaver][pipes_sim]") {
    std::minstd_rand rng_a(77);
    std::minstd_rand rng_b(77);
    PipesScreen a;
    PipesScreen b;
    std::vector<PipePiece> out_a;
    std::vector<PipePiece> out_b;
    a.begin({15, 9, 15}, rng_a, out_a);
    b.begin({15, 9, 15}, rng_b, out_b);
    for (int s = 0; s < 300; s++) {
        a.step(rng_a, out_a);
        b.step(rng_b, out_b);
        REQUIRE(a.pipes() == b.pipes());
    }
    REQUIRE(out_a.size() == out_b.size());
    for (size_t i = 0; i < out_a.size(); i++) {
        CHECK(same_piece(out_a[i], out_b[i]));
    }
}

// ============================================================================
// Materials and lighting
// ============================================================================

TEST_CASE("the material table is the original's 16", "[screensaver][pipes_sim]") {
    REQUIRE(PIPE_MATERIAL_COUNT == 16);
    CHECK(PIPE_MATERIALS[0].kd[0] == Catch::Approx(0.07568f));
    CHECK(PIPE_MATERIALS[0].kd[1] == Catch::Approx(0.61424f));
    CHECK(PIPE_MATERIALS[0].shininess == Catch::Approx(76.8f));
    CHECK(PIPE_MATERIALS[2].kd[0] == Catch::Approx(1.0f));
    CHECK(PIPE_MATERIALS[2].kd[1] == Catch::Approx(0.829f));
    CHECK(PIPE_MATERIALS[2].shininess == Catch::Approx(11.264f));
    CHECK(PIPE_MATERIALS[5].shininess == Catch::Approx(27.897436f));
    CHECK(PIPE_MATERIALS[15].ks[0] == Catch::Approx(0.7f));
    CHECK(PIPE_MATERIALS[15].ks[2] == Catch::Approx(0.7f));
    CHECK(PIPE_MATERIALS[15].shininess == Catch::Approx(10.0f));
}

TEST_CASE("the light and half vector are fixed in eye space", "[screensaver][pipes_sim]") {
    CHECK(vec_near(pipes_light_dir(), {0.457496f, 0.457496f, 0.762493f}, 1e-5f));
    CHECK(vec_near(pipes_half_vector(), {0.243674f, 0.243674f, 0.938747f}, 1e-5f));
}

TEST_CASE("pipes_shade is GL fixed-function lighting", "[screensaver][pipes_sim]") {
    const PipeMaterial& emerald = PIPE_MATERIALS[0];
    const PipeMaterial& ruby = PIPE_MATERIALS[3];
    const PipeMaterial& white_rubber = PIPE_MATERIALS[15];
    CHECK(rgb_near(pipes_shade(emerald, pipes_light_dir()), {27, 207, 27}));
    CHECK(rgb_near(pipes_shade(emerald, pipes_half_vector()), {186, 255, 186}));
    CHECK(rgb_near(pipes_shade(emerald, {0, 0, -1}), {6, 49, 6}));
    CHECK(rgb_near(pipes_shade(emerald, {0, 0, 1}), {22, 170, 22}));
    CHECK(rgb_near(pipes_shade(ruby, pipes_light_dir()), {207, 15, 15}));
    CHECK(rgb_near(pipes_shade(ruby, pipes_half_vector()), {255, 173, 173}));
    CHECK(rgb_near(pipes_shade(white_rubber, {0, 0, 1}), {206, 206, 206}));
    CHECK(rgb_near(pipes_shade(white_rubber, {0, 0, -1}), {14, 14, 14}));
}

TEST_CASE("the brightest point of a material faces the half vector", "[screensaver][pipes_sim]") {
    std::minstd_rand rng(31);
    std::normal_distribution<float> gauss;
    for (const PipeMaterial& m : PIPE_MATERIALS) {
        const int peak = channel_sum(pipes_shade(m, pipes_half_vector()));
        for (int i = 0; i < 200; i++) {
            PipeVec3 n{gauss(rng), gauss(rng), gauss(rng)};
            const float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
            n = {n.x / len, n.y / len, n.z / len};
            // The diffuse term pulls the true maximum a hair from H toward L, worth under a
            // unit per channel; anywhere else is darker.
            CHECK(peak + 3 >= channel_sum(pipes_shade(m, n)));
        }
    }
}

// ============================================================================
// Meshes
// ============================================================================

namespace {

PipeVec3 sub(PipeVec3 a, PipeVec3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

PipeVec3 cross(PipeVec3 a, PipeVec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

float dot3(PipeVec3 a, PipeVec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

float len(PipeVec3 a) {
    return std::sqrt(dot3(a, a));
}

/// Triangles whose winding agrees with their vertex normals, out of those with any area.
std::pair<size_t, size_t> outward_winding(const PipeMesh& m) {
    size_t good = 0;
    size_t total = 0;
    for (size_t t = 0; t + 2 < m.tris.size(); t += 3) {
        const PipeVertex& a = m.verts[m.tris[t]];
        const PipeVertex& b = m.verts[m.tris[t + 1]];
        const PipeVertex& c = m.verts[m.tris[t + 2]];
        const PipeVec3 face = cross(sub(b.p, a.p), sub(c.p, a.p));
        if (len(face) < 1e-6f) {
            continue;
        }
        const PipeVec3 n{a.n.x + b.n.x + c.n.x, a.n.y + b.n.y + c.n.y, a.n.z + b.n.z + c.n.z};
        total++;
        good += dot3(face, n) > 0.0f ? 1 : 0;
    }
    return {good, total};
}

/// The 16 world positions of a ring of `mesh` starting at vertex `first`.
std::vector<PipeVec3> ring(const PipeMesh& mesh, size_t first, const PipeXform& xf) {
    std::vector<PipeVec3> out;
    for (size_t k = 0; k < static_cast<size_t>(PIPES_SLICES); k++) {
        out.push_back(pipes_xform_point(xf, mesh.verts[first + k].p));
    }
    return out;
}

bool same_point_sets(const std::vector<PipeVec3>& a, const std::vector<PipeVec3>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (const PipeVec3& p : a) {
        const bool found =
            std::any_of(b.begin(), b.end(), [&](const PipeVec3& q) { return vec_near(p, q); });
        if (!found) {
            return false;
        }
    }
    return true;
}

} // namespace

TEST_CASE("every mesh has unit outward normals and outward winding", "[screensaver][pipes_sim]") {
    PipeMesh cyl;
    PipeMesh ball;
    PipeMesh elbow;
    PipeMesh teapot;
    pipes_build_cylinder(cyl);
    pipes_build_ball(ball);
    pipes_build_elbow(elbow);
    pipes_build_teapot(teapot);
    REQUIRE(!cyl.tris.empty());
    REQUIRE(!ball.tris.empty());
    REQUIRE(!elbow.tris.empty());
    REQUIRE(!teapot.tris.empty());

    for (const PipeVertex& v : cyl.verts) {
        CHECK(len(v.n) == Catch::Approx(1.0f).margin(1e-4));
        CHECK(dot3(v.n, {v.p.x, v.p.y, 0.0f}) > 0.0f);
    }
    for (const PipeVertex& v : ball.verts) {
        CHECK(len(v.n) == Catch::Approx(1.0f).margin(1e-4));
        CHECK(dot3(v.n, v.p) > 0.0f);
    }
    for (const PipeVertex& v : elbow.verts) {
        CHECK(len(v.n) == Catch::Approx(1.0f).margin(1e-4));
    }
    for (const PipeMesh* m : {&cyl, &ball, &elbow}) {
        const auto [good, total] = outward_winding(*m);
        CHECK(total > 0);
        CHECK(good == total);
    }
    for (const PipeVertex& v : teapot.verts) {
        CHECK(len(v.n) == Catch::Approx(1.0f).margin(1e-3));
    }
    const auto [good, total] = outward_winding(teapot);
    REQUIRE(total > 1000);
    CHECK(static_cast<double>(good) >= 0.98 * static_cast<double>(total));
}

TEST_CASE("elbow end rings meet the cylinders without a crack", "[screensaver][pipes_sim]") {
    PipeMesh cyl;
    PipeMesh elbow;
    pipes_build_cylinder(cyl);
    pipes_build_elbow(elbow);
    const size_t last_ring = elbow.verts.size() - static_cast<size_t>(PIPES_SLICES);
    int pairs = 0;
    for (int f = 0; f < PIPE_DIR_COUNT; f++) {
        for (int d = 0; d < PIPE_DIR_COUNT; d++) {
            const auto from = static_cast<PipeDir>(f);
            const auto dir = static_cast<PipeDir>(d);
            if (dir == from || dir == pipe_opposite(from)) {
                continue;
            }
            pairs++;
            const PipeVec3 node{3.0f, -2.0f, 5.0f};
            PipePiece joint;
            joint.kind = PipePieceKind::ELBOW;
            joint.at = node;
            joint.from = from;
            joint.dir = dir;
            const PipeXform jx = pipes_piece_xform(joint);

            const PipeVec3 fv = pipes_dir_vec(from);
            PipePiece in;
            in.kind = PipePieceKind::CYLINDER;
            in.dir = from;
            in.length = 7.0f;
            in.at = {node.x - 8.0f * fv.x, node.y - 8.0f * fv.y, node.z - 8.0f * fv.z};
            CHECK(same_point_sets(ring(elbow, 0, jx),
                                  ring(cyl, PIPES_SLICES, pipes_piece_xform(in))));

            const PipeVec3 dv = pipes_dir_vec(dir);
            PipePiece out;
            out.kind = PipePieceKind::CYLINDER;
            out.dir = dir;
            out.length = 5.0f;
            out.at = {node.x + dv.x, node.y + dv.y, node.z + dv.z};
            CHECK(
                same_point_sets(ring(elbow, last_ring, jx), ring(cyl, 0, pipes_piece_xform(out))));
        }
    }
    CHECK(pairs == 24);
}

TEST_CASE("the ball is the original's big ball", "[screensaver][pipes_sim]") {
    PipeMesh ball;
    pipes_build_ball(ball);
    CHECK(pipes_ball_radius() == Catch::Approx(1.441921f).margin(1e-5));
    for (const PipeVertex& v : ball.verts) {
        CHECK(len(v.p) == Catch::Approx(1.441921f).margin(1e-5));
    }
}

TEST_CASE("the teapot is the original's size", "[screensaver][pipes_sim]") {
    PipeMesh teapot;
    pipes_build_teapot(teapot);
    PipeVec3 lo{1e9f, 1e9f, 1e9f};
    PipeVec3 hi{-1e9f, -1e9f, -1e9f};
    for (const PipeVertex& v : teapot.verts) {
        lo = {std::min(lo.x, v.p.x), std::min(lo.y, v.p.y), std::min(lo.z, v.p.z)};
        hi = {std::max(hi.x, v.p.x), std::max(hi.y, v.p.y), std::max(hi.z, v.p.z)};
    }
    CHECK(lo.x == Catch::Approx(-3.75f).margin(0.02));
    // The spout's tip lies inside its control hull, which reaches 3.525 data units (4.40625).
    CHECK(hi.x > 4.25f);
    CHECK(hi.x <= 4.40625f);
    CHECK(lo.y == Catch::Approx(-1.875f).margin(0.02));
    CHECK(hi.y == Catch::Approx(2.0625f).margin(0.02));
    CHECK(lo.z == Catch::Approx(-2.5f).margin(0.02));
    CHECK(hi.z == Catch::Approx(2.5f).margin(0.02));
}

TEST_CASE("an elbow's centreline bends round (0, r, -r)", "[screensaver][pipes_sim]") {
    PipeMesh elbow;
    pipes_build_elbow(elbow);
    REQUIRE(elbow.verts.size() % static_cast<size_t>(PIPES_SLICES) == 0);
    for (size_t first = 0; first < elbow.verts.size(); first += PIPES_SLICES) {
        PipeVec3 mean{};
        for (size_t k = 0; k < static_cast<size_t>(PIPES_SLICES); k++) {
            const PipeVec3& p = elbow.verts[first + k].p;
            mean = {mean.x + p.x / PIPES_SLICES, mean.y + p.y / PIPES_SLICES,
                    mean.z + p.z / PIPES_SLICES};
        }
        CHECK(mean.x == Catch::Approx(0.0f).margin(1e-5));
        const float dy = mean.y - PIPES_RADIUS;
        const float dz = mean.z + PIPES_RADIUS;
        CHECK(std::sqrt(dy * dy + dz * dz) == Catch::Approx(PIPES_RADIUS).margin(1e-5));
    }
}

#endif // HELIX_ENABLE_SCREENSAVER
