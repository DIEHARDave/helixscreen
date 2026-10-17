// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver_pipes_sim.h"

#include <algorithm>
#include <cmath>

using helix::ui::screensaver::random_below;

namespace helix::ui {

// ============================================================================
// Grid and growth rules
// ============================================================================

PipeGridDims pipes_grid_dims(int32_t w, int32_t h) {
    const int32_t long_side = std::max(w, h);
    const int32_t short_side = std::min(w, h);
    const int other = long_side > 0
                          ? std::max(1, static_cast<int>(static_cast<int64_t>(PIPES_NODES_LONG) *
                                                         short_side / long_side))
                          : 1;
    if (w >= h) {
        return {PIPES_NODES_LONG, other, PIPES_NODES_LONG};
    }
    return {other, PIPES_NODES_LONG, PIPES_NODES_LONG};
}

PipeNode pipe_step(PipeNode n, PipeDir d) {
    switch (d) {
    case PipeDir::POS_X:
        n.x++;
        break;
    case PipeDir::NEG_X:
        n.x--;
        break;
    case PipeDir::POS_Y:
        n.y++;
        break;
    case PipeDir::NEG_Y:
        n.y--;
        break;
    case PipeDir::POS_Z:
        n.z++;
        break;
    case PipeDir::NEG_Z:
        n.z--;
        break;
    case PipeDir::NONE:
        break;
    }
    return n;
}

PipeDir pipe_opposite(PipeDir d) {
    if (d == PipeDir::NONE) {
        return PipeDir::NONE;
    }
    // Each axis's pair is adjacent: POS then NEG.
    return static_cast<PipeDir>(static_cast<uint8_t>(d) ^ 1u);
}

void PipesGrid::reset(PipeGridDims dims) {
    dims_ = dims;
    const size_t n =
        static_cast<size_t>(std::max(0, dims.x)) * std::max(0, dims.y) * std::max(0, dims.z);
    taken_.assign(n, 0);
    empty_count_ = n;
}

void PipesGrid::release() {
    std::vector<uint8_t>().swap(taken_);
    dims_ = {};
    empty_count_ = 0;
}

bool PipesGrid::in_bounds(PipeNode n) const {
    return n.x >= 0 && n.y >= 0 && n.z >= 0 && n.x < dims_.x && n.y < dims_.y && n.z < dims_.z;
}

size_t PipesGrid::index(PipeNode n) const {
    return (static_cast<size_t>(n.z) * dims_.y + n.y) * dims_.x + n.x;
}

bool PipesGrid::empty(PipeNode n) const {
    return in_bounds(n) && taken_[index(n)] == 0;
}

void PipesGrid::take(PipeNode n) {
    if (!in_bounds(n)) {
        return;
    }
    uint8_t& cell = taken_[index(n)];
    if (cell == 0) {
        cell = 1;
        empty_count_--;
    }
}

int PipesGrid::empty_run(PipeNode from, PipeDir d, int limit) const {
    int run = 0;
    PipeNode n = from;
    while (run < limit) {
        n = pipe_step(n, d);
        if (!empty(n)) {
            break;
        }
        run++;
    }
    return run;
}

bool PipesGrid::random_empty(std::minstd_rand& rng, PipeNode& out) const {
    if (empty_count_ == 0) {
        return false;
    }
    const int count = static_cast<int>(taken_.size());
    for (int tries = 2 * count; tries > 0; tries--) {
        const PipeNode n{random_below(rng, dims_.x), random_below(rng, dims_.y),
                         random_below(rng, dims_.z)};
        if (empty(n)) {
            out = n;
            return true;
        }
    }
    for (int z = 0; z < dims_.z; z++) {
        for (int y = 0; y < dims_.y; y++) {
            for (int x = 0; x < dims_.x; x++) {
                if (empty({x, y, z})) {
                    out = {x, y, z};
                    return true;
                }
            }
        }
    }
    return false;
}

int pipes_choose_weight_straight(std::minstd_rand& rng) {
    if (random_below(rng, 20) == 0) {
        return 25 + random_below(rng, PIPES_MAX_WEIGHT_STRAIGHT - 25 + 1);
    }
    return 1 + random_below(rng, 4);
}

PipeJoint pipes_choose_joint(std::minstd_rand& rng) {
    // Any one value of the draw: the original tests against a fixed number.
    if (random_below(rng, PIPES_TEAPOT_ODDS) == 153) {
        return PipeJoint::TEAPOT;
    }
    return random_below(rng, 3) == 0 ? PipeJoint::BALL : PipeJoint::ELBOW;
}

PipeDir pipes_choose_direction(const PipesGrid& grid, PipeNode pos, PipeDir last,
                               int weight_straight, std::minstd_rand& rng) {
    const bool straight_empty = last != PipeDir::NONE && grid.empty(pipe_step(pos, last));
    if (weight_straight >= PIPES_MAX_WEIGHT_STRAIGHT && straight_empty) {
        return last;
    }
    std::array<PipeDir, PIPE_DIR_COUNT> turns{};
    std::array<int, PIPE_DIR_COUNT> runs{};
    int turn_count = 0;
    for (int i = 0; i < PIPE_DIR_COUNT; i++) {
        const auto d = static_cast<PipeDir>(i);
        if (d == last || !grid.empty(pipe_step(pos, d))) {
            continue;
        }
        turns[static_cast<size_t>(turn_count)] = d;
        runs[static_cast<size_t>(turn_count)] = grid.empty_run(pos, d, PIPES_TURN_LOOKAHEAD);
        turn_count++;
    }
    const int w = straight_empty ? weight_straight : 0;
    if (w + turn_count == 0) {
        return PipeDir::NONE;
    }
    if (random_below(rng, w + turn_count) < w) {
        return last;
    }
    const int best = *std::max_element(runs.begin(), runs.begin() + turn_count);
    std::array<PipeDir, PIPE_DIR_COUNT> ties{};
    int tie_count = 0;
    for (int i = 0; i < turn_count; i++) {
        if (runs[static_cast<size_t>(i)] == best) {
            ties[static_cast<size_t>(tie_count++)] = turns[static_cast<size_t>(i)];
        }
    }
    return ties[static_cast<size_t>(random_below(rng, tie_count))];
}

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
