// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver_pipes_sim.h"
#include "screensaver_pixel_writer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
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

// ============================================================================
// Renderer
// ============================================================================

namespace {

constexpr uint32_t ROW_PAD = 12;
constexpr uint8_t CANARY = 0xA5;

/// A heap frame of `format`, painted black, rows padded with canary bytes past their pixels.
struct Frame {
    std::vector<uint8_t> bytes;
    FrameTarget target;

    Frame(uint32_t w, uint32_t h, PixelFormat format = PixelFormat::XRGB8888) {
        const uint32_t bpp = format == PixelFormat::RGB565 ? 2 : 4;
        const uint32_t stride = w * bpp + ROW_PAD;
        bytes.assign(static_cast<size_t>(stride) * h, CANARY);
        target = {bytes.data(), stride, w, h, format};
        PixelWriter(target).fill({0, 0, 0});
    }
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;

    Rgb at(int32_t x, int32_t y) const {
        return PixelWriter(target).get(x, y);
    }
    bool canaries_intact() const {
        const uint32_t bpp = target.format == PixelFormat::RGB565 ? 2 : 4;
        for (uint32_t y = 0; y < target.h; y++) {
            const uint8_t* row = target.data + static_cast<size_t>(y) * target.stride;
            for (uint32_t i = target.w * bpp; i < target.stride; i++) {
                if (row[i] != CANARY) {
                    return false;
                }
            }
        }
        return true;
    }
};

uint64_t target_hash(const FrameTarget& f) {
    const uint32_t bpp = f.format == PixelFormat::RGB565 ? 2 : 4;
    uint64_t hash = 1469598103934665603ULL;
    for (uint32_t y = 0; y < f.h; y++) {
        const uint8_t* row = f.data + static_cast<size_t>(y) * f.stride;
        for (uint32_t i = 0; i < f.w * bpp; i++) {
            hash = (hash ^ row[i]) * 1099511628211ULL;
        }
    }
    return hash;
}

bool lit(Rgb c) {
    return c.r != 0 || c.g != 0 || c.b != 0;
}

size_t lit_pixels(const Frame& f) {
    size_t n = 0;
    for (uint32_t y = 0; y < f.target.h; y++) {
        for (uint32_t x = 0; x < f.target.w; x++) {
            n += lit(f.at(static_cast<int32_t>(x), static_cast<int32_t>(y))) ? 1 : 0;
        }
    }
    return n;
}

PipePiece ball_at(PipeVec3 at, uint8_t material) {
    return piece(PipePieceKind::BALL, at, PipeDir::POS_X, PipeDir::POS_X, 0.0f, material);
}

PipePiece cylinder_at(PipeVec3 at, PipeDir dir, float length, uint8_t material) {
    return piece(PipePieceKind::CYLINDER, at, dir, PipeDir::POS_X, length, material);
}

/// The brightest pixel (by channel sum) and its position.
Rgb brightest(const Frame& f, int32_t& bx, int32_t& by) {
    Rgb best{};
    bx = by = -1;
    for (uint32_t y = 0; y < f.target.h; y++) {
        for (uint32_t x = 0; x < f.target.w; x++) {
            const Rgb c = f.at(static_cast<int32_t>(x), static_cast<int32_t>(y));
            if (channel_sum(c) > channel_sum(best)) {
                best = c;
                bx = static_cast<int32_t>(x);
                by = static_cast<int32_t>(y);
            }
        }
    }
    return best;
}

/// Random pieces inside a 15 x 9 x 15 grid: every kind, any heading, any material.
std::vector<PipePiece> random_pieces(uint32_t seed, size_t count) {
    std::minstd_rand rng(seed);
    const PipeGridDims dims{15, 9, 15};
    std::vector<PipePiece> out;
    for (size_t i = 0; i < count; i++) {
        const PipeNode node{screensaver::random_below(rng, dims.x),
                            screensaver::random_below(rng, dims.y),
                            screensaver::random_below(rng, dims.z)};
        PipePiece p;
        p.kind = static_cast<PipePieceKind>(screensaver::random_below(rng, 4));
        p.at = pipes_node_world(node, dims);
        p.material = static_cast<uint8_t>(screensaver::random_below(rng, 16));
        p.dir = static_cast<PipeDir>(screensaver::random_below(rng, PIPE_DIR_COUNT));
        p.from = static_cast<PipeDir>((static_cast<int>(p.dir) + 2) % PIPE_DIR_COUNT);
        p.length = screensaver::random_below(rng, 2) == 0 ? 7.0f : 5.0f;
        out.push_back(p);
    }
    return out;
}

/// Pixels that differ between `before` and the frame, outside `box`.
size_t changed_outside(const std::vector<uint8_t>& before, const FrameTarget& f,
                       const DirtyRect& box, size_t& changed) {
    const uint32_t bpp = f.format == PixelFormat::RGB565 ? 2 : 4;
    size_t outside = 0;
    for (uint32_t y = 0; y < f.h; y++) {
        for (uint32_t x = 0; x < f.w; x++) {
            const size_t i = static_cast<size_t>(y) * f.stride + x * bpp;
            if (std::memcmp(&before[i], f.data + i, bpp) == 0) {
                continue;
            }
            changed++;
            const auto px = static_cast<int32_t>(x);
            const auto py = static_cast<int32_t>(y);
            const bool inside =
                !box.empty() && px >= box.x1 && px <= box.x2 && py >= box.y1 && py <= box.y2;
            outside += inside ? 0 : 1;
        }
    }
    return outside;
}

} // namespace

TEST_CASE("the camera projects the original's view", "[screensaver][pipes_sim]") {
    PipesRenderer r;
    r.init(800, 480, 1);
    r.set_view(0.0f, {15, 9, 15});
    float sx = 0;
    float sy = 0;
    REQUIRE(r.project({0, 0, 0}, sx, sy));
    CHECK(sx == Catch::Approx(400.0f).margin(0.01));
    CHECK(sy == Catch::Approx(240.0f).margin(0.01));
    REQUIRE(r.project({0, 0, 50}, sx, sy));
    CHECK(sx == Catch::Approx(400.0f).margin(0.01));
    CHECK(sy == Catch::Approx(240.0f).margin(0.01));
    REQUIRE(r.project({7, 0, 0}, sx, sy));
    CHECK(sx == Catch::Approx(422.4f).margin(0.01));
    CHECK(sy == Catch::Approx(240.0f).margin(0.01));
    REQUIRE(r.project({0, 7, 0}, sx, sy));
    CHECK(sx == Catch::Approx(400.0f).margin(0.01));
    CHECK(sy == Catch::Approx(217.6f).margin(0.01));

    // Turned 90 degrees, +x points away from the camera.
    r.set_view(90.0f, {15, 9, 15});
    REQUIRE(r.project({10, 0, 0}, sx, sy));
    CHECK(sx == Catch::Approx(400.0f).margin(0.01));
    CHECK(sy == Catch::Approx(240.0f).margin(0.01));
    REQUIRE(r.project({10, 7, 0}, sx, sy));
    CHECK(sy == Catch::Approx(240.0f - 240.0f * 7.0f / 85.0f).margin(0.01));
    CHECK_FALSE(r.project({-80, 0, 0}, sx, sy));

    // Scale 2 rasterizes at half size and reports canvas pixels.
    r.init(800, 480, 2);
    r.set_view(0.0f, {15, 9, 15});
    REQUIRE(r.project({7, 0, 0}, sx, sy));
    CHECK(sx == Catch::Approx(422.4f).margin(0.01));
}

TEST_CASE("draw order never changes the picture", "[screensaver][pipes_sim]") {
    const auto render = [](const std::vector<PipePiece>& pieces, uint64_t& hash, Rgb& centre) {
        PipesRenderer r;
        r.init(800, 480, 1);
        r.set_view(0.0f, {15, 9, 15});
        r.clear_depth();
        Frame f(800, 480);
        for (const PipePiece& p : pieces) {
            r.draw(f.target, p);
        }
        hash = target_hash(f.target);
        centre = f.at(400, 240);
    };
    const PipePiece near_emerald = ball_at({0.5f, 0, 40}, 0);
    const PipePiece far_ruby = ball_at({0, 0, 30}, 3);
    uint64_t h1 = 0;
    uint64_t h2 = 0;
    Rgb c1;
    Rgb c2;
    render({far_ruby, near_emerald}, h1, c1);
    render({near_emerald, far_ruby}, h2, c2);
    CHECK(h1 == h2);
    CHECK(c1.g > 2 * c1.r);

    const PipePiece near_ball = ball_at({0, 0, 40}, 0);
    const PipePiece far_cylinder = cylinder_at({-3.5f, 0, 30}, PipeDir::POS_X, 7.0f, 3);
    render({far_cylinder, near_ball}, h1, c1);
    render({near_ball, far_cylinder}, h2, c2);
    CHECK(h1 == h2);
    CHECK(c1.g > 2 * c1.r);
}

TEST_CASE("a pipe is as fat as its radius over its depth", "[screensaver][pipes_sim]") {
    const auto lit_rows = [](PipeVec3 at) {
        PipesRenderer r;
        r.init(800, 480, 1);
        r.set_view(0.0f, {15, 9, 15});
        Frame f(800, 480);
        r.draw(f.target, cylinder_at(at, PipeDir::POS_X, 7.0f, 15));
        int rows = 0;
        for (int32_t y = 0; y < 480; y++) {
            rows += lit(f.at(400, y)) ? 1 : 0;
        }
        return rows;
    };
    const int near_rows = lit_rows({-3.5f, 0, 50});
    CHECK(near_rows >= 18);
    CHECK(near_rows <= 21);
    const int far_rows = lit_rows({-3.5f, 0, -25});
    CHECK(far_rows >= 4);
    CHECK(far_rows <= 6);
}

TEST_CASE("the highlight sits up-right of centre on every ball", "[screensaver][pipes_sim]") {
    PipesRenderer r;
    r.init(800, 480, 1);
    r.set_view(0.0f, {15, 9, 15});
    float cx = 0;
    float cy = 0;
    int32_t bx = 0;
    int32_t by = 0;

    Frame centred(800, 480);
    r.draw(centred.target, ball_at({0, 0, 40}, 0));
    const Rgb c1 = brightest(centred, bx, by);
    REQUIRE(r.project({0, 0, 40}, cx, cy));
    const float dx1 = static_cast<float>(bx) + 0.5f - cx;
    const float dy1 = static_cast<float>(by) + 0.5f - cy;
    CHECK(dx1 >= 1.0f);
    CHECK(dx1 <= 4.0f);
    CHECK(dy1 >= -4.0f);
    CHECK(dy1 <= -1.0f);

    r.clear_depth();
    Frame offset(800, 480);
    r.draw(offset.target, ball_at({8, 6, 40}, 0));
    const Rgb c2 = brightest(offset, bx, by);
    REQUIRE(r.project({8, 6, 40}, cx, cy));
    CHECK(static_cast<float>(bx) + 0.5f - cx > 0.0f);
    CHECK(static_cast<float>(by) + 0.5f - cy < 0.0f);
    CHECK(rgb_near(c1, c2, 16));
    CHECK(rgb_near(c1, pipes_shade(PIPE_MATERIALS[0], pipes_half_vector()), 16));

    // On a pipe the highlight runs along the side facing up-right.
    r.clear_depth();
    Frame across(800, 480);
    r.draw(across.target, cylinder_at({-3.5f, 0, 40}, PipeDir::POS_X, 7.0f, 0));
    brightest(across, bx, by);
    REQUIRE(r.project({0, 0, 40}, cx, cy));
    CHECK(static_cast<float>(by) + 0.5f < cy);

    r.clear_depth();
    Frame upright(800, 480);
    r.draw(upright.target, cylinder_at({0, -3.5f, 40}, PipeDir::POS_Y, 7.0f, 0));
    brightest(upright, bx, by);
    CHECK(static_cast<float>(bx) + 0.5f > cx);
}

TEST_CASE("an elbow joins its two pipes with no gap", "[screensaver][pipes_sim]") {
    PipesRenderer r;
    r.init(800, 480, 1);
    r.set_view(0.0f, {15, 9, 15});
    Frame f(800, 480);
    const PipeGridDims dims{15, 9, 15};
    const PipeVec3 node = pipes_node_world({7, 4, 7}, dims);
    const PipeVec3 prev = pipes_node_world({6, 4, 7}, dims);
    r.draw(f.target, cylinder_at({prev.x - 1, prev.y, prev.z}, PipeDir::POS_X, 7.0f, 9));
    r.draw(f.target, piece(PipePieceKind::ELBOW, node, PipeDir::POS_Y, PipeDir::POS_X, 0.0f, 9));
    r.draw(f.target, cylinder_at({node.x, node.y + 1, node.z}, PipeDir::POS_Y, 5.0f, 9));

    std::vector<PipeVec3> line;
    for (float t = prev.x - 1.0f; t <= node.x - 1.0f; t += 0.05f) {
        line.push_back({t, 0, 0});
    }
    // The elbow's centreline is a quarter circle of radius r round (node.x - r, node.y + r).
    for (float a = 0.0f; a <= 1.5708f; a += 0.05f) {
        line.push_back({node.x - 1.0f + std::sin(a), node.y + 1.0f - std::cos(a), 0});
    }
    // Up to a pixel short of the short pipe's open end, where the next piece takes over.
    for (float t = node.y + 1.0f; t <= node.y + 5.8f; t += 0.05f) {
        line.push_back({0, t, 0});
    }
    int dark = 0;
    for (const PipeVec3& p : line) {
        float sx = 0;
        float sy = 0;
        REQUIRE(r.project(p, sx, sy));
        dark += lit(f.at(static_cast<int32_t>(sx), static_cast<int32_t>(sy))) ? 0 : 1;
    }
    CHECK(dark == 0);
}

TEST_CASE("the renderer dirties every pixel a piece changes", "[screensaver][pipes_sim]") {
    for (uint8_t scale : {1, 2}) {
        PipesRenderer r;
        r.init(320, 192, scale);
        r.set_view(23.0f, {15, 9, 15});
        Frame f(320, 192);
        size_t total_changed = 0;
        for (const PipePiece& p : random_pieces(41, 50)) {
            const std::vector<uint8_t> before = f.bytes;
            size_t changed = 0;
            const DirtyRect box = r.draw(f.target, p);
            CHECK(changed_outside(before, f.target, box, changed) == 0);
            total_changed += changed;
        }
        CHECK(total_changed > 0);
    }
}

TEST_CASE("RGB565 and XRGB8888 renders of one scene agree to 565 precision",
          "[screensaver][pipes_sim]") {
    PipesRenderer r32;
    PipesRenderer r16;
    r32.init(160, 96, 1);
    r16.init(160, 96, 1);
    r32.set_view(41.0f, {15, 9, 15});
    r16.set_view(41.0f, {15, 9, 15});
    Frame f32(160, 96, PixelFormat::XRGB8888);
    Frame f16(160, 96, PixelFormat::RGB565);
    for (const PipePiece& p : random_pieces(7, 30)) {
        r32.draw(f32.target, p);
        r16.draw(f16.target, p);
    }
    REQUIRE(lit_pixels(f32) > 0);
    int worst_r = 0;
    int worst_g = 0;
    int worst_b = 0;
    for (int32_t y = 0; y < 96; y++) {
        for (int32_t x = 0; x < 160; x++) {
            const Rgb a = f32.at(x, y);
            const Rgb b = f16.at(x, y);
            worst_r = std::max(worst_r, std::abs(a.r - b.r));
            worst_g = std::max(worst_g, std::abs(a.g - b.g));
            worst_b = std::max(worst_b, std::abs(a.b - b.b));
        }
    }
    CHECK(worst_r <= 8);
    CHECK(worst_g <= 4);
    CHECK(worst_b <= 8);
}

TEST_CASE("an XRGB8888 pipes frame keeps every X byte opaque", "[screensaver][pipes_sim]") {
    PipesRenderer r;
    r.init(160, 96, 1);
    r.set_view(0.0f, {15, 9, 15});
    Frame f(160, 96);
    for (const PipePiece& p : random_pieces(3, 40)) {
        r.draw(f.target, p);
    }
    REQUIRE(lit_pixels(f) > 0);
    for (uint32_t y = 0; y < 96; y++) {
        for (uint32_t x = 0; x < 160; x++) {
            REQUIRE(f.target.data[static_cast<size_t>(y) * f.target.stride + x * 4 + 3] == 0xFF);
        }
    }
}

TEST_CASE("frames of every size and scale stay inside their rows", "[screensaver][pipes_sim]") {
    const std::array<std::pair<uint32_t, uint32_t>, 4> sizes{
        {{1, 1}, {7, 5}, {161, 97}, {800, 480}}};
    for (const auto& [w, h] : sizes) {
        for (uint8_t scale : {1, 2}) {
            for (PixelFormat format : {PixelFormat::XRGB8888, PixelFormat::RGB565}) {
                PipesRenderer r;
                r.init(w, h, scale);
                r.set_view(0.0f, pipes_grid_dims(static_cast<int32_t>(w), static_cast<int32_t>(h)));
                Frame f(w, h, format);
                for (const PipePiece& p : random_pieces(w * 31 + scale, 40)) {
                    r.draw(f.target, p);
                }
                CHECK(f.canaries_intact());
            }
        }
    }
}

TEST_CASE("at scale 2 every 2x2 block is one colour", "[screensaver][pipes_sim]") {
    PipesRenderer r;
    r.init(160, 96, 2);
    r.set_view(0.0f, {15, 9, 15});
    Frame f(160, 96);
    r.draw(f.target, ball_at({0, 0, 40}, 8));
    REQUIRE(lit_pixels(f) > 0);
    for (int32_t y = 0; y < 96; y += 2) {
        for (int32_t x = 0; x < 160; x += 2) {
            const Rgb c = f.at(x, y);
            CHECK(f.at(x + 1, y) == c);
            CHECK(f.at(x, y + 1) == c);
            CHECK(f.at(x + 1, y + 1) == c);
        }
    }
}

TEST_CASE("a released renderer holds no buffers", "[screensaver][pipes_sim]") {
    PipesRenderer r;
    r.init(160, 96, 1);
    CHECK(r.buffer_bytes() > 160u * 96u * 4u);
    r.release();
    CHECK(r.buffer_bytes() == 0);
}

// ============================================================================
// Dissolve
// ============================================================================

namespace {

void fill_white(Frame& f) {
    PixelWriter(f.target).fill({255, 255, 255});
}

bool all_black(const Frame& f) {
    return lit_pixels(f) == 0;
}

} // namespace

TEST_CASE("the dissolve cuts the screen into 1/32 blocks", "[screensaver][pipes_sim]") {
    PipesDissolve d;
    d.init(800, 480);
    CHECK(d.block_px() == 15);
    CHECK(d.block_count() == 54u * 32u);
    d.init(480, 272);
    CHECK(d.block_px() == 8);
    CHECK(d.block_count() == 60u * 34u);
    d.init(10, 10);
    CHECK(d.block_px() == 1);
    CHECK(d.block_count() == 100u);
}

TEST_CASE("the dissolve clears by frame time", "[screensaver][pipes_sim]") {
    Frame f(800, 480);
    fill_white(f);
    PipesDissolve d;
    d.init(800, 480);
    std::minstd_rand rng(3);
    d.begin(rng);
    std::vector<DirtyRect> dirty;
    for (int i = 0; i < 25; i++) {
        d.step(40, f.target, dirty);
    }
    CHECK(d.cleared() == 864u);
    d.step(999, f.target, dirty);
    CHECK(d.cleared() == 1727u);
    CHECK(d.active());
    CHECK_FALSE(all_black(f));
    d.step(1, f.target, dirty);
    CHECK(d.cleared() == 1728u);
    CHECK_FALSE(d.active());
    CHECK(all_black(f));

    const auto run = [](uint32_t dt, int calls) {
        Frame g(800, 480);
        fill_white(g);
        PipesDissolve dd;
        dd.init(800, 480);
        std::minstd_rand r(5);
        dd.begin(r);
        std::vector<DirtyRect> rects;
        for (int i = 0; i < calls; i++) {
            dd.step(dt, g.target, rects);
        }
        return target_hash(g.target);
    };
    CHECK(run(16, 50) == run(50, 16));
}

TEST_CASE("each dissolve block is cleared once and dirtied", "[screensaver][pipes_sim]") {
    Frame f(161, 97);
    fill_white(f);
    PipesDissolve d;
    d.init(161, 97);
    std::minstd_rand rng(4);
    d.begin(rng);
    std::set<std::tuple<int32_t, int32_t, int32_t, int32_t>> seen;
    size_t rects = 0;
    while (d.active()) {
        const std::vector<uint8_t> before = f.bytes;
        std::vector<DirtyRect> dirty;
        d.step(33, f.target, dirty);
        DirtyRect all;
        for (const DirtyRect& r : dirty) {
            seen.insert({r.x1, r.y1, r.x2, r.y2});
            rects++;
            all.add(r);
        }
        // Every changed pixel lies in one of this frame's rects.
        const uint32_t bpp = 4;
        for (uint32_t y = 0; y < 97; y++) {
            for (uint32_t x = 0; x < 161; x++) {
                const size_t i = static_cast<size_t>(y) * f.target.stride + x * bpp;
                if (std::memcmp(&before[i], f.target.data + i, bpp) == 0) {
                    continue;
                }
                const auto px = static_cast<int32_t>(x);
                const auto py = static_cast<int32_t>(y);
                const bool covered =
                    std::any_of(dirty.begin(), dirty.end(), [&](const DirtyRect& r) {
                        return px >= r.x1 && px <= r.x2 && py >= r.y1 && py <= r.y2;
                    });
                REQUIRE(covered);
            }
        }
    }
    CHECK(rects == d.block_count());
    CHECK(seen.size() == d.block_count());
    CHECK(all_black(f));
    CHECK(f.canaries_intact());
}

TEST_CASE("two dissolves from one seed clear in the same order", "[screensaver][pipes_sim]") {
    const auto order = [](uint32_t seed) {
        Frame f(160, 96);
        PipesDissolve d;
        d.init(160, 96);
        std::minstd_rand rng(seed);
        d.begin(rng);
        std::vector<DirtyRect> dirty;
        while (d.active()) {
            d.step(16, f.target, dirty);
        }
        std::vector<int32_t> xs;
        for (const DirtyRect& r : dirty) {
            xs.push_back(r.y1 * 1000 + r.x1);
        }
        return xs;
    };
    CHECK(order(11) == order(11));
    CHECK(order(11) != order(12));
}

// ============================================================================
// PipesSim
// ============================================================================

namespace {

bool no_segments(const PipesSim& sim) {
    return std::all_of(sim.screen().pipes().begin(), sim.screen().pipes().end(),
                       [](const PipeState& p) { return p.segment_count == 0; });
}

/// Steps of 100 ms until the screen finishes; the count, or -1 past the bound.
int steps_to_dissolve(PipesSim& sim, Frame& f, std::minstd_rand& rng) {
    std::vector<DirtyRect> dirty;
    for (int k = 1; k <= 20000; k++) {
        sim.step(100, f.target, rng, dirty);
        if (sim.phase() == PipesPhase::DISSOLVING) {
            return k;
        }
    }
    return -1;
}

} // namespace

TEST_CASE("the first screen starts at yrot 0 with its starting pipes drawn",
          "[screensaver][pipes_sim]") {
    Frame f(160, 96);
    fill_white(f);
    PipesSim sim;
    std::minstd_rand rng(1);
    sim.init(f.target, rng, 0);
    CHECK(lit_pixels(f) > 0);
    CHECK(lit_pixels(f) < 160u * 96u / 2u);
    CHECK(sim.phase() == PipesPhase::GROWING);
    CHECK(sim.screens_started() == 1);
    CHECK(sim.yrot_deg() == 0.0f);
}

TEST_CASE("a finished screen dissolves for 2 s, then turns 9.73156 degrees",
          "[screensaver][pipes_sim]") {
    Frame f(160, 96);
    PipesSim sim;
    std::minstd_rand rng(8);
    sim.init(f.target, rng, 0);
    REQUIRE(steps_to_dissolve(sim, f, rng) > 0);
    std::vector<DirtyRect> dirty;
    for (int i = 0; i < 19; i++) {
        sim.step(100, f.target, rng, dirty);
        REQUIRE(sim.phase() == PipesPhase::DISSOLVING);
    }
    sim.step(100, f.target, rng, dirty);
    CHECK(sim.phase() == PipesPhase::GROWING);
    CHECK(sim.yrot_deg() == Catch::Approx(9.73156f));
    CHECK(sim.screens_started() == 2);
    CHECK(no_segments(sim));
    sim.step(99, f.target, rng, dirty);
    CHECK(no_segments(sim));
    sim.step(1, f.target, rng, dirty);
    CHECK_FALSE(no_segments(sim));
}

TEST_CASE("growth steps still due when a screen finishes are dropped", "[screensaver][pipes_sim]") {
    Frame fa(160, 96);
    PipesSim a;
    std::minstd_rand rng_a(8);
    a.init(fa.target, rng_a, 0);
    const int k = steps_to_dissolve(a, fa, rng_a);
    REQUIRE(k > 1);

    Frame fb(160, 96);
    PipesSim b;
    std::minstd_rand rng_b(8);
    b.init(fb.target, rng_b, 0);
    std::vector<DirtyRect> dirty;
    for (int i = 0; i < k - 1; i++) {
        b.step(100, fb.target, rng_b, dirty);
    }
    REQUIRE(b.phase() == PipesPhase::GROWING);
    b.step(300, fb.target, rng_b, dirty);
    CHECK(b.phase() == PipesPhase::DISSOLVING);
    CHECK(b.dissolve().cleared() == 0);
    b.step(1000, fb.target, rng_b, dirty);
    CHECK(b.dissolve().cleared() == b.dissolve().block_count() / 2);

    // The dropped steps leave nothing behind: half way through, both dissolves have cleared
    // the same blocks of the same picture.
    a.step(1000, fa.target, rng_a, dirty);
    CHECK(target_hash(fa.target) == target_hash(fb.target));
}

TEST_CASE("pipes grow one step per 100 ms, at most three per call", "[screensaver][pipes_sim]") {
    Frame fa(160, 96);
    Frame fb(160, 96);
    PipesSim a;
    PipesSim b;
    std::minstd_rand rng_a(14);
    std::minstd_rand rng_b(14);
    a.init(fa.target, rng_a, 0);
    b.init(fb.target, rng_b, 0);
    std::vector<DirtyRect> dirty;
    a.step(300, fa.target, rng_a, dirty);
    for (int i = 0; i < 3; i++) {
        b.step(100, fb.target, rng_b, dirty);
    }
    CHECK(a.screen().pipes() == b.screen().pipes());
    CHECK(target_hash(fa.target) == target_hash(fb.target));

    const auto total_segments = [](const PipesSim& s) {
        int n = 0;
        for (const PipeState& p : s.screen().pipes()) {
            n += p.segment_count;
        }
        return n;
    };
    const auto first_segments = [](const PipesSim& s) {
        return s.screen().pipes()[0].segment_count;
    };
    // Slot 0 is alive after every start, and grows one node per step until it sticks.
    Frame fc(160, 96);
    PipesSim c;
    std::minstd_rand rng_c(14);
    c.init(fc.target, rng_c, 0);
    c.step(450, fc.target, rng_c, dirty);
    CHECK(first_segments(c) == 3);
    c.step(0, fc.target, rng_c, dirty);
    CHECK(first_segments(c) == 3);
    c.step(50, fc.target, rng_c, dirty);
    CHECK(first_segments(c) == 4);
    CHECK(total_segments(c) > 4);
}

TEST_CASE("a level's scale waits for the next screen", "[screensaver][pipes_sim]") {
    Frame f(160, 96);
    PipesSim sim;
    std::minstd_rand rng(5);
    sim.init(f.target, rng, 0);
    CHECK(sim.renderer().scale() == 1);
    sim.set_level(2);
    std::vector<DirtyRect> dirty;
    while (sim.screens_started() < 2) {
        REQUIRE(sim.renderer().scale() == 1);
        sim.step(100, f.target, rng, dirty);
    }
    CHECK(sim.renderer().scale() == 2);
    sim.set_level(0);
    sim.step(100, f.target, rng, dirty);
    CHECK(sim.renderer().scale() == 2);
    while (sim.screens_started() < 3) {
        REQUIRE(sim.renderer().scale() == 2);
        sim.step(100, f.target, rng, dirty);
    }
    CHECK(sim.renderer().scale() == 1);
}

TEST_CASE("PipesSim dirties every pixel its step changes", "[screensaver][pipes_sim]") {
    Frame f(160, 96);
    PipesSim sim;
    std::minstd_rand rng(12);
    sim.init(f.target, rng, 2);
    bool dissolved = false;
    for (int i = 0; i < 3000; i++) {
        const std::vector<uint8_t> before = f.bytes;
        std::vector<DirtyRect> dirty;
        sim.step(50, f.target, rng, dirty);
        dissolved = dissolved || sim.phase() == PipesPhase::DISSOLVING;
        for (uint32_t y = 0; y < 96; y++) {
            for (uint32_t x = 0; x < 160; x++) {
                const size_t idx = static_cast<size_t>(y) * f.target.stride + x * 4;
                if (std::memcmp(&before[idx], f.target.data + idx, 4) == 0) {
                    continue;
                }
                const auto px = static_cast<int32_t>(x);
                const auto py = static_cast<int32_t>(y);
                const bool covered =
                    std::any_of(dirty.begin(), dirty.end(), [&](const DirtyRect& r) {
                        return px >= r.x1 && px <= r.x2 && py >= r.y1 && py <= r.y2;
                    });
                REQUIRE(covered);
            }
        }
    }
    CHECK(dissolved);
    CHECK(sim.screens_started() >= 2);
}

TEST_CASE("a pipes run replays exactly from a seed", "[screensaver][pipes_sim]") {
    Frame fa(160, 96);
    Frame fb(160, 96);
    PipesSim a;
    PipesSim b;
    std::minstd_rand rng_a(21);
    std::minstd_rand rng_b(21);
    a.init(fa.target, rng_a, 0);
    b.init(fb.target, rng_b, 0);
    std::vector<DirtyRect> dirty;
    for (int i = 1; i <= 1500; i++) {
        a.step(33, fa.target, rng_a, dirty);
        b.step(33, fb.target, rng_b, dirty);
        dirty.clear();
        if (i % 100 == 0) {
            REQUIRE(target_hash(fa.target) == target_hash(fb.target));
        }
    }
}

TEST_CASE("both depths run the same scene", "[screensaver][pipes_sim]") {
    Frame f32(160, 96, PixelFormat::XRGB8888);
    Frame f16(160, 96, PixelFormat::RGB565);
    PipesSim a;
    PipesSim b;
    std::minstd_rand rng_a(6);
    std::minstd_rand rng_b(6);
    a.init(f32.target, rng_a, 0);
    b.init(f16.target, rng_b, 0);
    std::vector<DirtyRect> dirty;
    for (int i = 0; i < 400; i++) {
        a.step(100, f32.target, rng_a, dirty);
        b.step(100, f16.target, rng_b, dirty);
        dirty.clear();
        REQUIRE(a.screen().pipes() == b.screen().pipes());
        REQUIRE(a.yrot_deg() == b.yrot_deg());
    }
    for (int32_t y = 0; y < 96; y++) {
        for (int32_t x = 0; x < 160; x++) {
            const Rgb p = f32.at(x, y);
            const Rgb q = f16.at(x, y);
            REQUIRE(std::abs(p.r - q.r) <= 8);
            REQUIRE(std::abs(p.g - q.g) <= 4);
            REQUIRE(std::abs(p.b - q.b) <= 8);
        }
    }
}

TEST_CASE("a cleared pipes sim holds no buffers", "[screensaver][pipes_sim]") {
    Frame f(160, 96);
    PipesSim sim;
    std::minstd_rand rng(2);
    sim.init(f.target, rng, 0);
    CHECK(sim.buffer_bytes() > 0);
    CHECK(sim.screen().grid().buffer_bytes() > 0);
    sim.clear();
    CHECK(sim.buffer_bytes() == 0);
    CHECK(sim.screen().grid().buffer_bytes() == 0);
}

#endif // HELIX_ENABLE_SCREENSAVER
