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

// ============================================================================
// Materials and lighting
// ============================================================================

namespace {

PipeVec3 normalized(PipeVec3 v) {
    const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (len <= 0.0f) {
        return {};
    }
    return {v.x / len, v.y / len, v.z / len};
}

float dot(PipeVec3 a, PipeVec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

/// Light-model ambient (1.0) plus the light's own ambient (0.1).
constexpr float AMBIENT_SCALE = 1.1f;

} // namespace

PipeVec3 pipes_light_dir() {
    return normalized({90.0f, 90.0f, 150.0f});
}

PipeVec3 pipes_half_vector() {
    const PipeVec3 l = pipes_light_dir();
    return normalized({l.x, l.y, l.z + 1.0f});
}

Rgb pipes_shade(const PipeMaterial& m, PipeVec3 n) {
    const float nl = dot(n, pipes_light_dir());
    const float nh = std::max(0.0f, dot(n, pipes_half_vector()));
    const float diffuse = std::max(0.0f, nl);
    const float spec = nl > 0.0f ? std::pow(nh, m.shininess) : 0.0f;
    std::array<uint8_t, 3> c{};
    for (size_t i = 0; i < 3; i++) {
        const float v = AMBIENT_SCALE * m.ka[i] + m.kd[i] * diffuse + m.ks[i] * spec;
        c[i] = static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
    }
    return {c[0], c[1], c[2]};
}

// ============================================================================
// One screen of pipes
// ============================================================================

namespace {

PipeVec3 add_scaled(PipeVec3 a, PipeVec3 d, float s) {
    return {a.x + d.x * s, a.y + d.y * s, a.z + d.z * s};
}

PipePiece joint_piece(PipePieceKind kind, PipeVec3 at, uint8_t material) {
    PipePiece p;
    p.kind = kind;
    p.at = at;
    p.material = material;
    return p;
}

PipePiece cylinder_piece(PipeVec3 at, PipeDir dir, float length, uint8_t material) {
    PipePiece p;
    p.kind = PipePieceKind::CYLINDER;
    p.at = at;
    p.dir = dir;
    p.length = length;
    p.material = material;
    return p;
}

constexpr float SHORT_PIPE = PIPES_PITCH - 2.0f * PIPES_RADIUS;

} // namespace

PipeVec3 pipes_node_world(PipeNode n, PipeGridDims dims) {
    const auto axis = [](int i, int dim) {
        return (static_cast<float>(i) - static_cast<float>(dim - 1) * 0.5f) * PIPES_PITCH;
    };
    return {axis(n.x, dims.x), axis(n.y, dims.y), axis(n.z, dims.z)};
}

PipeVec3 pipes_dir_vec(PipeDir d) {
    switch (d) {
    case PipeDir::POS_X:
        return {1, 0, 0};
    case PipeDir::NEG_X:
        return {-1, 0, 0};
    case PipeDir::POS_Y:
        return {0, 1, 0};
    case PipeDir::NEG_Y:
        return {0, -1, 0};
    case PipeDir::POS_Z:
        return {0, 0, 1};
    case PipeDir::NEG_Z:
        return {0, 0, -1};
    case PipeDir::NONE:
        break;
    }
    return {};
}

void pipes_emit_start(PipeNode node, PipeDir first, PipeGridDims dims, uint8_t material,
                      std::vector<PipePiece>& out) {
    const PipeVec3 c = pipes_node_world(node, dims);
    out.push_back(joint_piece(PipePieceKind::BALL, c, material));
    out.push_back(cylinder_piece(add_scaled(c, pipes_dir_vec(first), PIPES_RADIUS), first,
                                 SHORT_PIPE, material));
}

void pipes_emit_move(PipeNode node, PipeDir last, PipeDir next, PipeJoint joint, PipeGridDims dims,
                     uint8_t material, std::vector<PipePiece>& out) {
    const PipeVec3 c = pipes_node_world(node, dims);
    const PipeVec3 d = pipes_dir_vec(next);
    if (next == last) {
        out.push_back(cylinder_piece(add_scaled(c, d, -PIPES_RADIUS), next, PIPES_PITCH, material));
        return;
    }
    switch (joint) {
    case PipeJoint::ELBOW: {
        PipePiece elbow = joint_piece(PipePieceKind::ELBOW, c, material);
        elbow.from = last;
        elbow.dir = next;
        out.push_back(elbow);
        break;
    }
    case PipeJoint::BALL:
        out.push_back(joint_piece(PipePieceKind::BALL, c, material));
        break;
    case PipeJoint::TEAPOT:
        out.push_back(joint_piece(PipePieceKind::TEAPOT, c, material));
        break;
    }
    out.push_back(cylinder_piece(add_scaled(c, d, PIPES_RADIUS), next, SHORT_PIPE, material));
}

void pipes_emit_end(PipeNode node, PipeGridDims dims, uint8_t material,
                    std::vector<PipePiece>& out) {
    out.push_back(joint_piece(PipePieceKind::BALL, pipes_node_world(node, dims), material));
}

void PipesScreen::begin(PipeGridDims dims, std::minstd_rand& rng, std::vector<PipePiece>& out) {
    grid_.reset(dims);
    pipes_ = {};
    joints_ = {};
    straight_ = 0;
    started_ = 0;
    max_pipes_ = PIPES_PER_SCREEN;
    const int n =
        std::min(PIPES_PER_SCREEN,
                 PIPES_MIN_ACTIVE + random_below(rng, PIPES_MAX_ACTIVE - PIPES_MIN_ACTIVE + 1));
    counted_ = n;
    for (size_t i = 0; i < static_cast<size_t>(n); i++) {
        if (!start_pipe(i, rng, out)) {
            pipes_[i].alive = false;
            max_pipes_ = counted_;
        }
    }
}

bool PipesScreen::start_pipe(size_t i, std::minstd_rand& rng, std::vector<PipePiece>& out) {
    PipeState& p = pipes_[i];
    PipeNode node;
    if (!grid_.random_empty(rng, node)) {
        return false;
    }
    grid_.take(node);
    p = {};
    p.alive = true;
    p.pos = node;
    p.material = static_cast<uint8_t>(random_below(rng, static_cast<int>(PIPE_MATERIAL_COUNT)));
    p.weight_straight = pipes_choose_weight_straight(rng);
    p.last = static_cast<PipeDir>(random_below(rng, PIPE_DIR_COUNT));
    started_++;
    const PipeDir first = pipes_choose_direction(grid_, node, p.last, p.weight_straight, rng);
    if (first == PipeDir::NONE) {
        out.push_back(
            joint_piece(PipePieceKind::TEAPOT, pipes_node_world(node, grid_.dims()), p.material));
        p.stuck = true;
        return true;
    }
    pipes_emit_start(node, first, grid_.dims(), p.material, out);
    p.pos = pipe_step(node, first);
    grid_.take(p.pos);
    p.last = first;
    return true;
}

void PipesScreen::grow_pipe(PipeState& p, std::minstd_rand& rng, std::vector<PipePiece>& out) {
    const PipeDir next = pipes_choose_direction(grid_, p.pos, p.last, p.weight_straight, rng);
    if (next == PipeDir::NONE) {
        pipes_emit_end(p.pos, grid_.dims(), p.material, out);
        p.stuck = true;
        return;
    }
    PipeJoint joint = PipeJoint::ELBOW;
    if (next != p.last) {
        joint = pipes_choose_joint(rng);
        joints_[static_cast<size_t>(joint)]++;
    } else {
        straight_++;
    }
    pipes_emit_move(p.pos, p.last, next, joint, grid_.dims(), p.material, out);
    p.pos = pipe_step(p.pos, next);
    grid_.take(p.pos);
    p.last = next;
    p.segment_count++;
}

bool PipesScreen::step(std::minstd_rand& rng, std::vector<PipePiece>& out) {
    for (size_t i = 0; i < pipes_.size(); i++) {
        PipeState& p = pipes_[i];
        if (!p.alive || !p.stuck) {
            continue;
        }
        if (++counted_ > max_pipes_) {
            p.alive = false;
        } else if (!start_pipe(i, rng, out)) {
            p.alive = false;
            max_pipes_ = counted_;
        }
    }
    for (PipeState& p : pipes_) {
        if (p.alive && !p.stuck) {
            grow_pipe(p, rng, out);
        }
    }
    return !finished();
}

bool PipesScreen::finished() const {
    return std::none_of(pipes_.begin(), pipes_.end(), [](const PipeState& p) { return p.alive; });
}

void PipesScreen::release() {
    grid_.release();
    pipes_ = {};
    counted_ = 0;
    started_ = 0;
    joints_ = {};
    straight_ = 0;
}

// ============================================================================
// Meshes
// ============================================================================

namespace {

constexpr float PI_F = 3.14159265358979f;

/// Bend segments of an elbow.
constexpr int ELBOW_SEGMENTS = 6;
/// Latitude bands of a ball.
constexpr int BALL_STACKS = 8;

/// Joins ring `a` to ring `b` (PIPES_SLICES vertices each, b ahead of a along the tube) with
/// triangles facing out when each ring runs counter-clockwise about the direction of travel.
void join_rings(uint16_t a, uint16_t b, std::vector<uint16_t>& tris) {
    for (int k = 0; k < PIPES_SLICES; k++) {
        const int k1 = (k + 1) % PIPES_SLICES;
        const auto a0 = static_cast<uint16_t>(a + k);
        const auto a1 = static_cast<uint16_t>(a + k1);
        const auto b0 = static_cast<uint16_t>(b + k);
        const auto b1 = static_cast<uint16_t>(b + k1);
        tris.insert(tris.end(), {a0, a1, b1, a0, b1, b0});
    }
}

float slice_angle(int k) {
    return static_cast<float>(k) * 2.0f * PI_F / static_cast<float>(PIPES_SLICES);
}

} // namespace

float pipes_ball_radius() {
    return std::sqrt(2.0f) * PIPES_RADIUS / std::cos(PI_F / static_cast<float>(PIPES_SLICES));
}

void pipes_build_cylinder(PipeMesh& out) {
    out.verts.clear();
    out.tris.clear();
    for (int ring = 0; ring < 2; ring++) {
        for (int k = 0; k < PIPES_SLICES; k++) {
            const float c = std::cos(slice_angle(k));
            const float s = std::sin(slice_angle(k));
            out.verts.push_back(
                {{PIPES_RADIUS * c, PIPES_RADIUS * s, static_cast<float>(ring)}, {c, s, 0.0f}});
        }
    }
    join_rings(0, PIPES_SLICES, out.tris);
}

void pipes_build_ball(PipeMesh& out) {
    out.verts.clear();
    out.tris.clear();
    const float radius = pipes_ball_radius();
    // Poles on the z axis, rings of PIPES_SLICES between them.
    out.verts.push_back({{0, 0, -radius}, {0, 0, -1}});
    for (int stack = 1; stack < BALL_STACKS; stack++) {
        const float polar = PI_F * static_cast<float>(stack) / static_cast<float>(BALL_STACKS);
        const float z = -std::cos(polar);
        const float rr = std::sin(polar);
        for (int k = 0; k < PIPES_SLICES; k++) {
            const PipeVec3 n{rr * std::cos(slice_angle(k)), rr * std::sin(slice_angle(k)), z};
            out.verts.push_back({{n.x * radius, n.y * radius, n.z * radius}, n});
        }
    }
    const auto top = static_cast<uint16_t>(out.verts.size());
    out.verts.push_back({{0, 0, radius}, {0, 0, 1}});
    for (int k = 0; k < PIPES_SLICES; k++) {
        const auto k1 = static_cast<uint16_t>(1 + (k + 1) % PIPES_SLICES);
        out.tris.insert(out.tris.end(), {0, k1, static_cast<uint16_t>(1 + k)});
    }
    for (int stack = 1; stack + 1 < BALL_STACKS; stack++) {
        join_rings(static_cast<uint16_t>(1 + (stack - 1) * PIPES_SLICES),
                   static_cast<uint16_t>(1 + stack * PIPES_SLICES), out.tris);
    }
    const auto last = static_cast<uint16_t>(1 + (BALL_STACKS - 2) * PIPES_SLICES);
    for (int k = 0; k < PIPES_SLICES; k++) {
        const auto k1 = static_cast<uint16_t>(last + (k + 1) % PIPES_SLICES);
        out.tris.insert(out.tris.end(), {static_cast<uint16_t>(last + k), k1, top});
    }
}

void pipes_build_elbow(PipeMesh& out) {
    out.verts.clear();
    out.tris.clear();
    const float r = PIPES_RADIUS;
    for (int seg = 0; seg <= ELBOW_SEGMENTS; seg++) {
        const float phi = 0.5f * PI_F * static_cast<float>(seg) / ELBOW_SEGMENTS;
        // Centreline round (0, r, -r); the ring's frame is x and e2, with x cross e2 the
        // direction of travel.
        const PipeVec3 centre{0.0f, r - r * std::cos(phi), -r + r * std::sin(phi)};
        const PipeVec3 e2{0.0f, std::cos(phi), -std::sin(phi)};
        for (int k = 0; k < PIPES_SLICES; k++) {
            const float c = std::cos(slice_angle(k));
            const float s = std::sin(slice_angle(k));
            const PipeVec3 n{c, s * e2.y, s * e2.z};
            out.verts.push_back({{centre.x + r * n.x, centre.y + r * n.y, centre.z + r * n.z}, n});
        }
    }
    for (int seg = 0; seg < ELBOW_SEGMENTS; seg++) {
        join_rings(static_cast<uint16_t>(seg * PIPES_SLICES),
                   static_cast<uint16_t>((seg + 1) * PIPES_SLICES), out.tris);
    }
}

namespace {

PipeVec3 cross3(PipeVec3 a, PipeVec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

/// The fixed axis-aligned pair across `d`, with x cross y = d.
void cylinder_basis(PipeDir d, PipeVec3& x, PipeVec3& y) {
    switch (d) {
    case PipeDir::POS_X:
        x = {0, 1, 0};
        y = {0, 0, 1};
        return;
    case PipeDir::NEG_X:
        x = {0, 1, 0};
        y = {0, 0, -1};
        return;
    case PipeDir::POS_Y:
        x = {0, 0, 1};
        y = {1, 0, 0};
        return;
    case PipeDir::NEG_Y:
        x = {0, 0, 1};
        y = {-1, 0, 0};
        return;
    case PipeDir::POS_Z:
        x = {1, 0, 0};
        y = {0, 1, 0};
        return;
    case PipeDir::NEG_Z:
    case PipeDir::NONE:
        x = {1, 0, 0};
        y = {0, -1, 0};
        return;
    }
}

} // namespace

PipeXform pipes_piece_xform(const PipePiece& piece) {
    PipeXform xf;
    xf.t = piece.at;
    switch (piece.kind) {
    case PipePieceKind::CYLINDER: {
        cylinder_basis(piece.dir, xf.x, xf.y);
        const PipeVec3 d = pipes_dir_vec(piece.dir);
        xf.z = {d.x * piece.length, d.y * piece.length, d.z * piece.length};
        break;
    }
    case PipePieceKind::ELBOW:
        xf.z = pipes_dir_vec(piece.from);
        xf.y = pipes_dir_vec(piece.dir);
        xf.x = cross3(xf.y, xf.z);
        break;
    case PipePieceKind::BALL:
    case PipePieceKind::TEAPOT:
        xf.x = {1, 0, 0};
        xf.y = {0, 1, 0};
        xf.z = {0, 0, 1};
        break;
    }
    return xf;
}

PipeVec3 pipes_xform_point(const PipeXform& xf, PipeVec3 p) {
    return {p.x * xf.x.x + p.y * xf.y.x + p.z * xf.z.x + xf.t.x,
            p.x * xf.x.y + p.y * xf.y.y + p.z * xf.z.y + xf.t.y,
            p.x * xf.x.z + p.y * xf.y.z + p.z * xf.z.z + xf.t.z};
}

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
