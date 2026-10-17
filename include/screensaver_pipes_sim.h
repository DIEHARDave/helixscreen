// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver_frame.h"
#include "screensaver_motion.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <random>
#include <vector>

/**
 * @file screensaver_pipes_sim.h
 * @brief The 3D Pipes: node grid, pipe growth, a z-buffered software rasterizer, and the dissolve
 *
 * No LVGL, so every part steps and renders without a display. World units follow the original
 * NT OpenGL sample: nodes 7 apart, pipe radius 1, camera 75 back with a 90 degree vertical FOV.
 */

namespace helix::ui {

inline constexpr int PIPES_NODES_LONG = 15;     ///< nodes along the long screen axis (NUM_DIV - 1)
inline constexpr float PIPES_PITCH = 7.0f;      ///< node spacing (divSize)
inline constexpr float PIPES_RADIUS = 1.0f;     ///< pipe radius: 1/7 of the pitch
inline constexpr float PIPES_VIEW_DIST = 75.0f; ///< camera distance from the grid centre (-zTrans)
inline constexpr float PIPES_YROT_STEP_DEG = 9.73156f; ///< view turn per new screen
inline constexpr int PIPES_SLICES = 16;                ///< facets round a pipe; a multiple of 4
inline constexpr uint32_t PIPES_STEP_MS = 100;         ///< one growth step per pipe
inline constexpr uint32_t PIPES_MAX_STEPS_PER_TICK = 3;
inline constexpr int PIPES_MIN_ACTIVE = 2;
inline constexpr int PIPES_MAX_ACTIVE = 4;
inline constexpr int PIPES_PER_SCREEN = 7; ///< NORMAL_PIPE_COUNT 5 x 1.5
inline constexpr int PIPES_MAX_WEIGHT_STRAIGHT = 100;
inline constexpr int PIPES_TEAPOT_ODDS = 1000;
inline constexpr int PIPES_TURN_LOOKAHEAD = 2;
inline constexpr uint32_t PIPES_DISSOLVE_MS = 2000;
inline constexpr int PIPES_DISSOLVE_DIV = 32; ///< dissolve block = min(w, h) / this

enum class PipeDir : uint8_t { POS_X, NEG_X, POS_Y, NEG_Y, POS_Z, NEG_Z, NONE };
inline constexpr int PIPE_DIR_COUNT = 6;

enum class PipeJoint : uint8_t { ELBOW, BALL, TEAPOT };

struct PipeNode {
    int x = 0;
    int y = 0;
    int z = 0;

    bool operator==(const PipeNode& o) const {
        return x == o.x && y == o.y && z == o.z;
    }
};

struct PipeGridDims {
    int x = 0;
    int y = 0;
    int z = 0;
};

/// PIPES_NODES_LONG along the long screen axis, the other by aspect (at least 1), depth equal
/// to the long axis: 800x480 gives 15 x 9 x 15, 480x800 gives 9 x 15 x 15.
PipeGridDims pipes_grid_dims(int32_t w, int32_t h);

PipeNode pipe_step(PipeNode n, PipeDir d);
PipeDir pipe_opposite(PipeDir d);

/// Which nodes are taken.
class PipesGrid {
  public:
    void reset(PipeGridDims dims);
    void release();
    PipeGridDims dims() const {
        return dims_;
    }
    bool in_bounds(PipeNode n) const;
    /// In bounds and not taken.
    bool empty(PipeNode n) const;
    void take(PipeNode n);
    size_t empty_count() const {
        return empty_count_;
    }
    /// Contiguous empty nodes from `from` (exclusive) along `d`, at most `limit`.
    int empty_run(PipeNode from, PipeDir d, int limit) const;
    /// A random empty node, or false when none is left (random tries, then a scan).
    bool random_empty(std::minstd_rand& rng, PipeNode& out) const;
    size_t buffer_bytes() const {
        return taken_.capacity();
    }

  private:
    size_t index(PipeNode n) const;

    PipeGridDims dims_;
    size_t empty_count_ = 0;
    std::vector<uint8_t> taken_;
};

/// A pipe's straight weight: 1 + rand(4), or 1 time in 20 rand in [25, 100].
int pipes_choose_weight_straight(std::minstd_rand& rng);

/// A turn's joint: a teapot 1 time in PIPES_TEAPOT_ODDS, else a ball 1 time in 3, else an elbow.
PipeJoint pipes_choose_joint(std::minstd_rand& rng);

/**
 * @brief Where a pipe at `pos`, last heading `last`, goes next; NONE when every neighbour is taken
 *
 * Straight (when empty) carries `weight_straight`, each empty other neighbour 1; weight
 * PIPES_MAX_WEIGHT_STRAIGHT always goes straight. A turn goes to whichever empty turn has
 * the longest empty run up to PIPES_TURN_LOOKAHEAD, ties broken at random. Marks nothing.
 */
PipeDir pipes_choose_direction(const PipesGrid& grid, PipeNode pos, PipeDir last,
                               int weight_straight, std::minstd_rand& rng);

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
