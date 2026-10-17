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

struct PipeVec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

/// An OpenGL fixed-function material: ambient, diffuse, specular reflectance and the GL
/// shininess exponent (the sample's specExp x 128).
struct PipeMaterial {
    float ka[3];
    float kd[3];
    float ks[3];
    float shininess;
};

/// The sample's 16 "good" teapot materials (COMMON/MATERIAL.C), in its goodMaterials order.
inline constexpr PipeMaterial PIPE_MATERIALS[] = {
    {{0.0215f, 0.1745f, 0.0215f},
     {0.07568f, 0.61424f, 0.07568f},
     {0.633f, 0.727811f, 0.633f},
     0.6f * 128.0f}, // emerald
    {{0.135f, 0.2225f, 0.1575f},
     {0.54f, 0.89f, 0.63f},
     {0.316228f, 0.316228f, 0.316228f},
     0.1f * 128.0f}, // jade
    {{0.25f, 0.20725f, 0.20725f},
     {1.0f, 0.829f, 0.829f},
     {0.296648f, 0.296648f, 0.296648f},
     0.088f * 128.0f}, // pearl
    {{0.1745f, 0.01175f, 0.01175f},
     {0.61424f, 0.04136f, 0.04136f},
     {0.727811f, 0.626959f, 0.626959f},
     0.6f * 128.0f}, // ruby
    {{0.1f, 0.18725f, 0.1745f},
     {0.396f, 0.74151f, 0.69102f},
     {0.297254f, 0.30829f, 0.306678f},
     0.1f * 128.0f}, // turquoise
    {{0.329412f, 0.223529f, 0.027451f},
     {0.780392f, 0.568627f, 0.113725f},
     {0.992157f, 0.941176f, 0.807843f},
     0.21794872f * 128.0f}, // brass
    {{0.2125f, 0.1275f, 0.054f},
     {0.714f, 0.4284f, 0.18144f},
     {0.393548f, 0.271906f, 0.166721f},
     0.2f * 128.0f}, // bronze
    {{0.19125f, 0.0735f, 0.0225f},
     {0.7038f, 0.27048f, 0.0828f},
     {0.256777f, 0.137622f, 0.086014f},
     0.1f * 128.0f}, // copper
    {{0.24725f, 0.1995f, 0.0745f},
     {0.75164f, 0.60648f, 0.22648f},
     {0.628281f, 0.555802f, 0.366065f},
     0.4f * 128.0f}, // gold
    {{0.19225f, 0.19225f, 0.19225f},
     {0.50754f, 0.50754f, 0.50754f},
     {0.508273f, 0.508273f, 0.508273f},
     0.4f * 128.0f}, // silver
    {{0.0f, 0.1f, 0.06f},
     {0.0f, 0.50980392f, 0.50980392f},
     {0.50196078f, 0.50196078f, 0.50196078f},
     0.25f * 128.0f}, // cyan plastic
    {{0.0f, 0.0f, 0.0f},
     {0.55f, 0.55f, 0.55f},
     {0.7f, 0.7f, 0.7f},
     0.25f * 128.0f},                                                             // white plastic
    {{0.0f, 0.0f, 0.0f}, {0.5f, 0.5f, 0.0f}, {0.6f, 0.6f, 0.5f}, 0.25f * 128.0f}, // yellow plastic
    {{0.0f, 0.05f, 0.05f},
     {0.4f, 0.5f, 0.5f},
     {0.04f, 0.7f, 0.7f},
     0.078125f * 128.0f}, // cyan rubber
    {{0.0f, 0.05f, 0.0f},
     {0.4f, 0.5f, 0.4f},
     {0.04f, 0.7f, 0.04f},
     0.078125f * 128.0f}, // green rubber
    {{0.05f, 0.05f, 0.05f},
     {0.5f, 0.5f, 0.5f},
     {0.7f, 0.7f, 0.7f},
     0.078125f * 128.0f}, // white rubber
};
inline constexpr size_t PIPE_MATERIAL_COUNT = std::size(PIPE_MATERIALS);

/// Eye-space light direction, normalize(90, 90, 150): fixed to the camera, so the highlight
/// sits in the same place on every piece.
PipeVec3 pipes_light_dir();
/// Eye-space half vector for an infinite viewer, normalize(L + (0, 0, 1)).
PipeVec3 pipes_half_vector();

/**
 * @brief GL fixed-function colour of `m` at unit eye-space normal `n`
 *
 * 1.1 ka (light-model ambient 1.0 plus light ambient 0.1) + kd max(0, n.L)
 * + ks max(0, n.H)^shininess where n.L > 0, each channel clamped to 1 and rounded to 0..255.
 */
Rgb pipes_shade(const PipeMaterial& m, PipeVec3 n);

enum class PipePieceKind : uint8_t { CYLINDER, ELBOW, BALL, TEAPOT };

/// One drawn primitive, in world units.
struct PipePiece {
    PipePieceKind kind = PipePieceKind::CYLINDER;
    /// CYLINDER: the start of its axis. Otherwise the joint's node centre.
    PipeVec3 at;
    /// CYLINDER: axis direction. ELBOW: the heading out.
    PipeDir dir = PipeDir::POS_X;
    /// ELBOW: the heading in.
    PipeDir from = PipeDir::POS_X;
    /// CYLINDER: axis length (PIPES_PITCH long, PIPES_PITCH - 2 r short).
    float length = 0.0f;
    uint8_t material = 0;
};

/// A pipe as the screen tracks it.
struct PipeState {
    PipeNode pos;
    PipeDir last = PipeDir::POS_X;
    int weight_straight = 1;
    uint8_t material = 0;
    int segment_count = 0; ///< growth steps taken since its start
    bool alive = false;
    bool stuck = false;

    bool operator==(const PipeState& o) const {
        return pos == o.pos && last == o.last && weight_straight == o.weight_straight &&
               material == o.material && segment_count == o.segment_count && alive == o.alive &&
               stuck == o.stuck;
    }
};

/// World position of node `n`'s centre for a grid of `dims`, the grid centred on the origin.
PipeVec3 pipes_node_world(PipeNode n, PipeGridDims dims);

/// Unit vector of `d` (NONE gives zero).
PipeVec3 pipes_dir_vec(PipeDir d);

/// Pieces a pipe starting at `node` heading `first` draws: a ball cap at the node and a short
/// cylinder from r ahead of it.
void pipes_emit_start(PipeNode node, PipeDir first, PipeGridDims dims, uint8_t material,
                      std::vector<PipePiece>& out);
/// Pieces one growth step from `node` draws: straight on, a long cylinder from r behind the
/// node; a turn, `joint` at the node (an ELBOW piece from `last` to `next`, a BALL, or a
/// TEAPOT) then a short cylinder from r ahead.
void pipes_emit_move(PipeNode node, PipeDir last, PipeDir next, PipeJoint joint, PipeGridDims dims,
                     uint8_t material, std::vector<PipePiece>& out);
/// The ball cap a stuck pipe ends with at `node`.
void pipes_emit_end(PipeNode node, PipeGridDims dims, uint8_t material,
                    std::vector<PipePiece>& out);

/**
 * @brief One screen of pipes, without pixels
 *
 * begin() starts min(PIPES_PER_SCREEN, rand[2, 4]) pipes. Each step first replaces the pipes
 * that got stuck (a new pipe while fewer than PIPES_PER_SCREEN have been counted, else the slot
 * dies; a start with no empty node left caps the count there), then grows every live pipe one
 * node. The screen is finished once no slot is live.
 */
class PipesScreen {
  public:
    void begin(PipeGridDims dims, std::minstd_rand& rng, std::vector<PipePiece>& out);
    /// @return false once the screen is finished
    bool step(std::minstd_rand& rng, std::vector<PipePiece>& out);
    void release();

    const PipesGrid& grid() const {
        return grid_;
    }
    /// The grid, for tests that build a crowded screen by hand.
    PipesGrid& grid() {
        return grid_;
    }
    const std::array<PipeState, PIPES_MAX_ACTIVE>& pipes() const {
        return pipes_;
    }
    /// Pipes counted toward PIPES_PER_SCREEN, the starting ones included.
    int pipes_counted() const {
        return counted_;
    }
    int pipes_started() const {
        return started_;
    }
    bool finished() const;
    /// Turn joints drawn since begin(), indexed by PipeJoint. Start and end caps are not joints.
    const std::array<uint32_t, 3>& joint_counts() const {
        return joints_;
    }
    /// Growth steps that went straight on since begin().
    uint32_t straight_steps() const {
        return straight_;
    }

  private:
    /// Starts slot `i` at a random empty node; false when none is left.
    bool start_pipe(size_t i, std::minstd_rand& rng, std::vector<PipePiece>& out);
    void grow_pipe(PipeState& p, std::minstd_rand& rng, std::vector<PipePiece>& out);

    PipesGrid grid_;
    std::array<PipeState, PIPES_MAX_ACTIVE> pipes_{};
    int counted_ = 0;
    int started_ = 0;
    int max_pipes_ = PIPES_PER_SCREEN;
    std::array<uint32_t, 3> joints_{};
    uint32_t straight_ = 0;
};

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
