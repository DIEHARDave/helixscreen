// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver_maze_sim.h"

#include "screensaver_motion.h"
#include "screensaver_pixel_writer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

using helix::ui::screensaver::random_below;

namespace helix::ui {

namespace {

constexpr float PI_F = 3.14159265f;

// Texture and sprite slots.
constexpr size_t TEX_BRICK = 0;
constexpr size_t TEX_BRICK_DARK = 1;
constexpr size_t TEX_PICTURE = 2;
constexpr size_t TEX_PICTURE_DARK = 3;
constexpr size_t TEX_FLOOR = 4;
constexpr size_t TEX_CEIL = 5;
constexpr size_t TEX_COUNT = 6;

constexpr size_t SPR_LOGO = 0;
constexpr size_t SPR_START = 1;
constexpr size_t SPR_EXIT = 2;
constexpr size_t SPR_TOOLHEAD = 3;
constexpr size_t SPR_ROCK0 = 4;
constexpr size_t SPR_COUNT = SPR_ROCK0 + MazeRenderer::ROCK_FRAMES;

constexpr int TEX = MazeRenderer::TEX;

/// Interior walls of a maze that get a picture.
constexpr int PICTURE_WALLS = 6;

/// Logos turn once every this many ms; rocks show a new rotation frame this often.
constexpr uint32_t SPIN_PERIOD_MS = 3000;
constexpr uint32_t ROCK_FRAME_MS = 90;

/// Wall-darkening of faces that run east-west, as Wolfenstein drew them.
constexpr float DARK_SIDE = 0.65f;

struct SpriteStyle {
    float size;     ///< height, tiles
    float center_z; ///< height of the sprite's centre above the floor, tiles (eye is 0.5)
    bool spins;
    bool screen_door; ///< every other pixel left out, so it reads as translucent
    /// Nearer than this along the view (tiles) the sprite is not drawn. The walk passes
    /// through logos and rocks, which would otherwise fill the screen as a blown-up mosaic.
    float near;
};

SpriteStyle style_of(MazeItemKind kind) {
    switch (kind) {
    case MazeItemKind::START:
        return {0.7f, 0.5f, false, false, 0.15f};
    case MazeItemKind::EXIT:
        return {0.6f, 0.5f, false, true, 0.15f};
    case MazeItemKind::LOGO:
        return {0.55f, 0.5f, true, false, 0.4f};
    case MazeItemKind::ROCK:
        return {0.4f, 0.5f, false, false, 0.4f};
    case MazeItemKind::TOOLHEAD:
        return {0.35f, 0.175f, false, false, 0.15f};
    }
    return {0.5f, 0.5f, false, false, 0.15f};
}

float smoothstep(float t) {
    return t * t * (3.0f - 2.0f * t);
}

uint8_t clamp_u8(float v) {
    return static_cast<uint8_t>(std::clamp(v, 0.0f, 255.0f));
}

Rgb scaled(Rgb c, float k) {
    return {clamp_u8(c.r * k), clamp_u8(c.g * k), clamp_u8(c.b * k)};
}

Rgb mix(Rgb a, Rgb b, float t) {
    return {clamp_u8(a.r + (b.r - a.r) * t), clamp_u8(a.g + (b.g - a.g) * t),
            clamp_u8(a.b + (b.b - a.b) * t)};
}

/// Average colour and coverage of the image over the box [x0, x1) x [y0, y1), in image pixels.
struct Rgba {
    Rgb c;
    float a = 0.0f;
};

Rgba sample_box(const MazeImage& img, float x0, float y0, float x1, float y1) {
    const int ix0 = std::clamp(static_cast<int>(std::floor(x0)), 0, img.w - 1);
    const int iy0 = std::clamp(static_cast<int>(std::floor(y0)), 0, img.h - 1);
    const int ix1 = std::clamp(static_cast<int>(std::ceil(x1)), ix0 + 1, img.w);
    const int iy1 = std::clamp(static_cast<int>(std::ceil(y1)), iy0 + 1, img.h);
    float r = 0, g = 0, b = 0, a = 0;
    int n = 0;
    for (int y = iy0; y < iy1; y++) {
        for (int x = ix0; x < ix1; x++) {
            const uint32_t p = img.argb[static_cast<size_t>(y) * img.w + x];
            const float pa = static_cast<float>(p >> 24) / 255.0f;
            r += static_cast<float>(p >> 16 & 0xFF) * pa;
            g += static_cast<float>(p >> 8 & 0xFF) * pa;
            b += static_cast<float>(p & 0xFF) * pa;
            a += pa;
            n++;
        }
    }
    Rgba out;
    if (a > 0.0f) {
        out.c = {clamp_u8(r / a), clamp_u8(g / a), clamp_u8(b / a)};
    }
    out.a = n > 0 ? a / static_cast<float>(n) : 0.0f;
    return out;
}

/// A TEX x TEX image being drawn, before packing: colour and whether the texel draws.
struct Canvas64 {
    Rgb c[TEX][TEX]; // [u][v]
    bool on[TEX][TEX];

    void clear() {
        std::memset(on, 0, sizeof(on));
    }
    void set(int u, int v, Rgb col) {
        if (u >= 0 && v >= 0 && u < TEX && v < TEX) {
            c[u][v] = col;
            on[u][v] = true;
        }
    }
};

/// Fits `img` into the box (bx, by, bw, bh) of `dst`, centred with its aspect kept, and
/// draws the texels it covers at least half; `matte` set blends edges over that colour.
void fit_image(Canvas64& dst, const MazeImage& img, int bx, int by, int bw, int bh,
               const Rgb* matte) {
    const float k = std::min(static_cast<float>(bw) / img.w, static_cast<float>(bh) / img.h);
    const float dw = img.w * k;
    const float dh = img.h * k;
    const float ox = bx + (bw - dw) * 0.5f;
    const float oy = by + (bh - dh) * 0.5f;
    for (int v = by; v < by + bh; v++) {
        for (int u = bx; u < bx + bw; u++) {
            const float sx = (u - ox) / k;
            const float sy = (v - oy) / k;
            if (sx + 1.0f / k <= 0.0f || sy + 1.0f / k <= 0.0f || sx >= img.w || sy >= img.h) {
                continue;
            }
            const Rgba s = sample_box(img, sx, sy, sx + 1.0f / k, sy + 1.0f / k);
            if (matte != nullptr) {
                dst.set(u, v, mix(*matte, s.c, s.a));
            } else if (s.a >= 0.5f) {
                dst.set(u, v, s.c);
            }
        }
    }
}

/// A Helix-like mark for when the icon cannot be decoded: two strands winding round an axis.
void draw_fallback_mark(Canvas64& dst) {
    for (int v = 4; v < 60; v++) {
        const float phase = (v - 4) / 56.0f * 3.0f * PI_F;
        const float s = std::sin(phase);
        const float depth = std::cos(phase);
        const int ua = static_cast<int>(32.0f + 18.0f * s);
        const int ub = static_cast<int>(32.0f - 18.0f * s);
        const Rgb a = scaled({0, 170, 255}, 0.7f + 0.3f * depth);
        const Rgb b = scaled({235, 240, 255}, 0.7f - 0.3f * depth);
        // The strand in front draws last.
        for (int pass = 0; pass < 2; pass++) {
            const bool a_front = depth >= 0.0f;
            const bool draw_a = (pass == 1) == a_front;
            for (int d = -2; d <= 2; d++) {
                dst.set((draw_a ? ua : ub) + d, v, draw_a ? a : b);
            }
        }
    }
}

/// One frame of a slowly tumbling gray icosahedron, flat shaded.
void draw_rock(Canvas64& dst, int frame) {
    static constexpr float T = 1.6180340f;
    static constexpr float VERTS[12][3] = {
        {-1, T, 0},  {1, T, 0},  {-1, -T, 0}, {1, -T, 0}, {0, -1, T},  {0, 1, T},
        {0, -1, -T}, {0, 1, -T}, {T, 0, -1},  {T, 0, 1},  {-T, 0, -1}, {-T, 0, 1},
    };
    static constexpr int FACES[20][3] = {
        {0, 11, 5},  {0, 5, 1},  {0, 1, 7},  {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4},
        {11, 10, 2}, {10, 7, 6}, {7, 1, 8},  {3, 9, 4},  {3, 4, 2},   {3, 2, 6}, {3, 6, 8},
        {3, 8, 9},   {4, 9, 5},  {2, 4, 11}, {6, 2, 10}, {8, 6, 7},   {9, 8, 1},
    };
    const float theta = 2.0f * PI_F * static_cast<float>(frame) / MazeRenderer::ROCK_FRAMES;
    const float tilt = 0.5f;
    float p[12][3];
    for (int i = 0; i < 12; i++) {
        const float x = VERTS[i][0] * std::cos(theta) + VERTS[i][2] * std::sin(theta);
        const float z = -VERTS[i][0] * std::sin(theta) + VERTS[i][2] * std::cos(theta);
        const float y = VERTS[i][1];
        p[i][0] = x;
        p[i][1] = y * std::cos(tilt) - z * std::sin(tilt);
        p[i][2] = y * std::sin(tilt) + z * std::cos(tilt);
    }
    const float radius = 28.0f / std::sqrt(1.0f + T * T);
    const float light[3] = {-0.40f, 0.60f, 0.69f};
    for (const auto& f : FACES) {
        const float* a = p[f[0]];
        const float* b = p[f[1]];
        const float* c = p[f[2]];
        const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const float e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                      e1[0] * e2[1] - e1[1] * e2[0]};
        const float centre[3] = {a[0] + b[0] + c[0], a[1] + b[1] + c[1], a[2] + b[2] + c[2]};
        if (n[0] * centre[0] + n[1] * centre[1] + n[2] * centre[2] < 0.0f) {
            n[0] = -n[0];
            n[1] = -n[1];
            n[2] = -n[2];
        }
        // The solid is convex, so the faces turned toward the viewer never overlap.
        if (n[2] <= 0.0f) {
            continue;
        }
        const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        const float lambert =
            std::max(0.0f, (n[0] * light[0] + n[1] * light[1] + n[2] * light[2]) / len);
        const Rgb shade = scaled({150, 150, 156}, 0.35f + 0.75f * lambert);
        const float ax = 32 + a[0] * radius, ay = 32 - a[1] * radius;
        const float bx = 32 + b[0] * radius, by = 32 - b[1] * radius;
        const float cx = 32 + c[0] * radius, cy = 32 - c[1] * radius;
        const float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
        if (std::fabs(area) < 1e-6f) {
            continue;
        }
        const int u0 = std::max(0, static_cast<int>(std::floor(std::min({ax, bx, cx}))));
        const int u1 = std::min(TEX - 1, static_cast<int>(std::ceil(std::max({ax, bx, cx}))));
        const int v0 = std::max(0, static_cast<int>(std::floor(std::min({ay, by, cy}))));
        const int v1 = std::min(TEX - 1, static_cast<int>(std::ceil(std::max({ay, by, cy}))));
        for (int v = v0; v <= v1; v++) {
            for (int u = u0; u <= u1; u++) {
                const float px = u + 0.5f;
                const float py = v + 0.5f;
                const float w0 = ((bx - px) * (cy - py) - (by - py) * (cx - px)) / area;
                const float w1 = ((cx - px) * (ay - py) - (cy - py) * (ax - px)) / area;
                const float w2 = 1.0f - w0 - w1;
                if (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f) {
                    dst.set(u, v, shade);
                }
            }
        }
    }
}

void draw_smiley(Canvas64& dst) {
    for (int v = 0; v < TEX; v++) {
        for (int u = 0; u < TEX; u++) {
            const float dx = u + 0.5f - 32.0f;
            const float dy = v + 0.5f - 32.0f;
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d > 29.0f) {
                continue;
            }
            Rgb c = d > 27.0f ? Rgb{120, 90, 10} : Rgb{250, 222, 40};
            const float ex = std::fabs(dx) - 9.0f;
            const float ey = dy + 7.0f;
            if ((ex * ex) / 9.0f + (ey * ey) / 30.0f <= 1.0f) {
                c = {20, 20, 20};
            }
            const float my = dy + 2.0f;
            const float md = std::sqrt(dx * dx + my * my);
            if (md >= 15.0f && md <= 19.0f && my > 6.0f) {
                c = {20, 20, 20};
            }
            dst.set(u, v, c);
        }
    }
}

void draw_toolhead(Canvas64& dst) {
    for (int v = 4; v < 44; v++) {
        for (int u = 14; u < 50; u++) {
            const bool edge = u == 14 || u == 49 || v == 4 || v == 43;
            dst.set(u, v, edge ? Rgb{60, 62, 66} : Rgb{115, 120, 126});
        }
    }
    for (int v = 10; v < 35; v++) {
        for (int u = 20; u < 45; u++) {
            const float dx = u + 0.5f - 32.0f;
            const float dy = v + 0.5f - 22.0f;
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d <= 12.0f) {
                const bool blade = std::fmod(std::atan2(dy, dx) + PI_F, PI_F / 3.0f) < 0.5f;
                dst.set(u, v,
                        d <= 3.0f ? Rgb{30, 30, 34}
                        : blade   ? Rgb{40, 90, 160}
                                  : Rgb{25, 55, 105});
            }
        }
    }
    for (int v = 44; v < 54; v++) {
        for (int u = 24; u < 40; u++) {
            dst.set(u, v, Rgb{190, 110, 50});
        }
    }
    for (int v = 54; v < 62; v++) {
        const int half = 4 - (v - 54) / 3;
        for (int u = 32 - half; u < 32 + half; u++) {
            dst.set(u, v, Rgb{220, 180, 60});
        }
    }
}

/// Small integer hash, for per-brick and per-plank variation that needs no random state.
uint32_t hash2(int a, int b) {
    uint32_t h = static_cast<uint32_t>(a) * 374761393u + static_cast<uint32_t>(b) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

float hash_unit(int a, int b) {
    return static_cast<float>(hash2(a, b) & 0xFFFF) / 65535.0f;
}

Rgb brick_texel(int u, int v) {
    const int row = v / 16;
    const int shifted = u + (row % 2) * 16;
    const int col = shifted / 32;
    if (v % 16 < 2 || shifted % 32 < 2) {
        return scaled({170, 165, 155}, 0.9f + 0.2f * hash_unit(u, v));
    }
    const float brick = 0.8f + 0.35f * hash_unit(col, row);
    const float grit = 0.9f + 0.2f * hash_unit(u * 7, v * 3);
    return scaled({150, 58, 40}, brick * grit);
}

Rgb floor_texel(int x, int y) {
    const int plank = y / 16;
    if (y % 16 == 0 || x == static_cast<int>(hash2(plank, 1) % TEX)) {
        return {50, 32, 18};
    }
    const float grain = std::sin((x + plank * 13) * 0.35f + std::sin(y * 0.9f) * 1.5f);
    const float tone = 0.85f + 0.25f * hash_unit(plank, 2) + 0.08f * grain;
    return scaled({135, 88, 48}, tone * (0.94f + 0.12f * hash_unit(x, y)));
}

Rgb ceiling_texel(int x, int y) {
    if (x % 32 == 0 || y % 32 == 0) {
        return {140, 140, 134};
    }
    const float speck = hash_unit(x * 5, y * 11);
    return speck > 0.93f ? Rgb{150, 148, 140} : scaled({205, 202, 192}, 0.95f + 0.05f * speck);
}

template <typename Px>
void blit_rows(const std::vector<Px>& buf, uint32_t iw, uint32_t ih,
               const std::vector<int>& src_rows, uint32_t scale, Px black, std::vector<Px>& row,
               FrameTarget& out) {
    for (uint32_t iy = 0; iy < ih; iy++) {
        const uint32_t oy0 = iy * scale;
        if (oy0 >= out.h) {
            break;
        }
        const int src = src_rows[iy];
        if (src < 0) {
            std::fill(row.begin(), row.begin() + out.w, black);
        } else {
            const Px* from = buf.data() + src;
            uint32_t ox = 0;
            for (uint32_t ix = 0; ix < iw && ox < out.w; ix++) {
                const Px v = from[static_cast<size_t>(ix) * ih];
                for (uint32_t k = 0; k < scale && ox < out.w; k++) {
                    row[ox++] = v;
                }
            }
        }
        for (uint32_t k = 0; k < scale && oy0 + k < out.h; k++) {
            std::memcpy(out.data + static_cast<size_t>(oy0 + k) * out.stride, row.data(),
                        out.w * sizeof(Px));
        }
    }
}

/// A sprite texel that draws nothing: a value no XRGB8888 pixel takes, and for RGB565 pure
/// magenta, which pack_sprite() nudges any real texel off.
template <typename Px> constexpr Px sprite_key() {
    if constexpr (sizeof(Px) == 2) {
        return 0xF81F;
    } else {
        return 0x00FFFFFFu;
    }
}

template <typename Px> Px pack_sprite(Rgb c, PixelFormat format) {
    const auto v = static_cast<Px>(PixelWriter::pack(c, format));
    return v == sprite_key<Px>() ? static_cast<Px>(v ^ 1u) : v;
}

/// Gives a vector's heap back; clear() keeps the capacity.
template <typename T> void free_vector(std::vector<T>& v) {
    std::vector<T>().swap(v);
}

template <typename T> size_t held_bytes(const std::vector<T>& v) {
    return v.capacity() * sizeof(T);
}

} // namespace

void MazeMap::generate(int cells, std::minstd_rand& rng) {
    size_ = 2 * cells + 1;
    tiles_.assign(static_cast<size_t>(size_) * size_, MazeTile::BRICK);
    auto open = [this](int x, int y) {
        tiles_[static_cast<size_t>(y) * size_ + x] = MazeTile::OPEN;
    };
    stack_.clear();
    stack_.push_back(0);
    open(1, 1);
    while (!stack_.empty()) {
        const int c = stack_.back();
        const int cx = c % cells;
        const int cy = c / cells;
        int options[4];
        int n = 0;
        for (int d = 0; d < 4; d++) {
            const int nx = cx + HEADING_DX[d];
            const int ny = cy + HEADING_DY[d];
            if (nx >= 0 && ny >= 0 && nx < cells && ny < cells &&
                tile(2 * nx + 1, 2 * ny + 1) != MazeTile::OPEN) {
                options[n++] = d;
            }
        }
        if (n == 0) {
            stack_.pop_back();
            continue;
        }
        const int d = options[random_below(rng, n)];
        open(2 * cx + 1 + HEADING_DX[d], 2 * cy + 1 + HEADING_DY[d]);
        const int nx = cx + HEADING_DX[d];
        const int ny = cy + HEADING_DY[d];
        open(2 * nx + 1, 2 * ny + 1);
        stack_.push_back(ny * cells + nx);
    }

    int hung = 0;
    for (int attempt = 0; attempt < 400 && hung < PICTURE_WALLS; attempt++) {
        const int x = 1 + random_below(rng, size_ - 2);
        const int y = 1 + random_below(rng, size_ - 2);
        if (tile(x, y) != MazeTile::BRICK) {
            continue;
        }
        const bool faces_open =
            !wall(x - 1, y) || !wall(x + 1, y) || !wall(x, y - 1) || !wall(x, y + 1);
        if (faces_open) {
            tiles_[static_cast<size_t>(y) * size_ + x] = MazeTile::PICTURE;
            hung++;
        }
    }
}

void MazeMap::release() {
    std::vector<MazeTile>().swap(tiles_);
    std::vector<int>().swap(stack_);
    size_ = 0;
}

void MazeMap::set_tiles(int size, std::vector<MazeTile> tiles) {
    size_ = size;
    tiles_ = std::move(tiles);
}

MazeTile MazeMap::tile(int x, int y) const {
    if (x < 0 || y < 0 || x >= size_ || y >= size_) {
        return MazeTile::BRICK;
    }
    return tiles_[static_cast<size_t>(y) * size_ + x];
}

bool MazeMap::passage(int cx, int cy, int heading) const {
    return !wall(2 * cx + 1 + HEADING_DX[heading], 2 * cy + 1 + HEADING_DY[heading]);
}

namespace {

/// Next heading out of cell (cx, cy) for a walk that came in heading `d_in`: toward the hand
/// when that side is open, else straight on, else away, else back.
int next_heading(const MazeMap& map, int cx, int cy, int d_in, bool left_handed) {
    const int hand = left_handed ? 3 : 1;
    const int order[4] = {(d_in + hand) & 3, d_in, (d_in + 4 - hand) & 3, (d_in + 2) & 3};
    for (int d : order) {
        if (map.passage(cx, cy, d)) {
            return d;
        }
    }
    return (d_in + 2) & 3;
}

/// Fraction of a segment covered at time fraction `u`: linear, or slowing to rest at the end
/// from the same starting speed.
float covered(float u, bool ease_out) {
    return ease_out ? u * (2.0f - u) : u;
}

float cell_centre(int c) {
    return 2.0f * static_cast<float>(c) + 1.5f;
}

float heading_angle(float quarters) {
    return quarters * (PI_F * 0.5f);
}

} // namespace

void MazeWalker::reset(int cx, int cy, int heading, bool left_handed, uint32_t move_ms,
                       uint32_t turn_ms) {
    cx_ = cx;
    cy_ = cy;
    d_in_ = heading & 3;
    d_out_ = d_in_;
    left_handed_ = left_handed;
    move_ms_ = std::max<uint32_t>(1, move_ms);
    turn_ms_ = std::max<uint32_t>(1, turn_ms);
    kind_ = Segment::START;
    spin_quarters_ = 0;
    planned_ = false;
    stopped_ = false;
    arrived_ = true;
    ease_out_ = false;
    progress_ms_ = 0;
    duration_ms_ = 1;
    arrive_ms_ = 0;
    moves_ = 0;
    stop_x_ = -1;
    stop_y_ = -1;
}

void MazeWalker::plan_start(const MazeMap& map) {
    // Turn on the spot to the way out, then speed up to walking pace by the doorway.
    d_out_ = next_heading(map, cx_, cy_, d_in_, left_handed_);
    const int diff = (d_out_ - d_in_ + 4) & 3;
    spin_quarters_ = diff == 3 ? -1 : diff == 2 ? (left_handed_ ? 2 : -2) : diff;
    kind_ = Segment::START;
    duration_ms_ = turn_ms_ * static_cast<uint32_t>(std::abs(spin_quarters_)) + move_ms_;
    arrived_ = true;
    progress_ms_ = 0;
    planned_ = true;
}

void MazeWalker::plan_segment(const MazeMap& map) {
    d_out_ = next_heading(map, cx_, cy_, d_in_, left_handed_);
    const int diff = (d_out_ - d_in_ + 4) & 3;
    ease_out_ = cx_ + HEADING_DX[d_out_] == stop_x_ && cy_ + HEADING_DY[d_out_] == stop_y_;
    progress_ms_ = 0;
    arrived_ = false;
    if (diff == 2) {
        kind_ = Segment::DEAD_END;
        spin_quarters_ = left_handed_ ? 2 : -2;
        // In and out at walking pace over one tile each, slowing to rest and setting off.
        duration_ms_ = 2 * move_ms_ + 2 * turn_ms_;
        arrive_ms_ = move_ms_;
        return;
    }
    kind_ = diff == 0 ? Segment::STRAIGHT : Segment::TURN;
    // Two tiles straight, or a quarter arc of radius one tile, at the same speed.
    const float length = diff == 0 ? 2.0f : PI_F * 0.5f;
    const float base = static_cast<float>(move_ms_) * length * 0.5f;
    duration_ms_ =
        std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(base * (ease_out_ ? 2.0f : 1.0f))));
    // The middle of the path: half of it covered.
    const float u_mid = ease_out_ ? 1.0f - std::sqrt(0.5f) : 0.5f;
    arrive_ms_ = static_cast<uint32_t>(std::lround(u_mid * static_cast<float>(duration_ms_)));
}

uint32_t MazeWalker::advance(const MazeMap& map, uint32_t dt_ms, WalkEvent& event) {
    event = WalkEvent::NONE;
    if (stopped_) {
        return dt_ms;
    }
    if (!planned_) {
        plan_start(map);
    }
    uint32_t used = 0;
    while (used < dt_ms) {
        const uint32_t available = dt_ms - used;
        if (!arrived_) {
            const uint32_t to_middle = arrive_ms_ - std::min(arrive_ms_, progress_ms_);
            if (available >= to_middle) {
                progress_ms_ += to_middle;
                used += to_middle;
                arrived_ = true;
                moves_++;
                event = WalkEvent::ARRIVED;
                return used;
            }
        }
        const uint32_t remaining = duration_ms_ - progress_ms_;
        if (available < remaining) {
            progress_ms_ += available;
            used += available;
            break;
        }
        used += remaining;
        cx_ += HEADING_DX[d_out_];
        cy_ += HEADING_DY[d_out_];
        d_in_ = d_out_;
        event = WalkEvent::ENTERED;
        if (cx_ == stop_x_ && cy_ == stop_y_) {
            stopped_ = true;
            progress_ms_ = 0;
            return used;
        }
        plan_segment(map);
        return used;
    }
    return used;
}

MazePose MazeWalker::pose() const {
    const float cx = cell_centre(cx_);
    const float cy = cell_centre(cy_);
    const float in_x = static_cast<float>(HEADING_DX[d_in_]);
    const float in_y = static_cast<float>(HEADING_DY[d_in_]);
    const float out_x = static_cast<float>(HEADING_DX[d_out_]);
    const float out_y = static_cast<float>(HEADING_DY[d_out_]);
    MazePose p;
    if (stopped_) {
        // At the doorway into this cell, looking in.
        p.x = cx - in_x;
        p.y = cy - in_y;
        p.angle = heading_angle(static_cast<float>(d_in_));
        return p;
    }
    const float t = static_cast<float>(progress_ms_);
    switch (kind_) {
    case Segment::START: {
        const float spin_ms =
            static_cast<float>(turn_ms_) * static_cast<float>(std::abs(spin_quarters_));
        p.x = cx;
        p.y = cy;
        if (!planned_ || t < spin_ms) {
            const float f = spin_ms > 0.0f ? smoothstep(t / spin_ms) : 0.0f;
            p.angle = heading_angle(static_cast<float>(d_in_) + f * spin_quarters_);
            return p;
        }
        const float u = (t - spin_ms) / static_cast<float>(move_ms_);
        p.x += out_x * u * u;
        p.y += out_y * u * u;
        p.angle = heading_angle(static_cast<float>(d_out_));
        return p;
    }
    case Segment::STRAIGHT: {
        const float f = covered(t / static_cast<float>(duration_ms_), ease_out_);
        p.x = cx + in_x * (2.0f * f - 1.0f);
        p.y = cy + in_y * (2.0f * f - 1.0f);
        p.angle = heading_angle(static_cast<float>(d_in_));
        return p;
    }
    case Segment::TURN: {
        // A quarter circle of radius one tile round the pillar on the inside of the turn,
        // from the doorway in to the doorway out.
        const float f = covered(t / static_cast<float>(duration_ms_), ease_out_);
        const float sign = d_out_ == ((d_in_ + 1) & 3) ? 1.0f : -1.0f;
        const float pillar_x = cx - in_x + out_x;
        const float pillar_y = cy - in_y + out_y;
        const float phi = std::atan2(-out_y, -out_x) + sign * f * (PI_F * 0.5f);
        p.x = pillar_x + std::cos(phi);
        p.y = pillar_y + std::sin(phi);
        p.angle = heading_angle(static_cast<float>(d_in_) + sign * f);
        return p;
    }
    case Segment::DEAD_END: {
        const auto in_ms = static_cast<float>(move_ms_);
        const float spin_ms = 2.0f * static_cast<float>(turn_ms_);
        if (t < in_ms) {
            const float u = t / in_ms;
            const float d = u * (2.0f - u);
            p.x = cx - in_x + in_x * d;
            p.y = cy - in_y + in_y * d;
            p.angle = heading_angle(static_cast<float>(d_in_));
        } else if (t < in_ms + spin_ms) {
            p.x = cx;
            p.y = cy;
            p.angle = heading_angle(static_cast<float>(d_in_) +
                                    smoothstep((t - in_ms) / spin_ms) * spin_quarters_);
        } else {
            const float u = (t - in_ms - spin_ms) / in_ms;
            p.x = cx + out_x * u * u;
            p.y = cy + out_y * u * u;
            p.angle = heading_angle(static_cast<float>(d_out_));
        }
        return p;
    }
    }
    return p;
}

void MazeRenderer::init(uint32_t w, uint32_t h, PixelFormat format, const MazeArt& art,
                        size_t level) {
    release();
    format_ = format;
    w_ = w;
    h_ = h;
    zbuf_.assign(w, 0.0f);
    wall_top_.assign(w, 0);
    wall_bottom_.assign(w, 0);
    row_dist_q16_.assign(h, 0);
    src_rows_.assign(h, -1);
    sprite_order_.reserve(32);
    if (format == PixelFormat::RGB565) {
        build<uint16_t>(art);
    } else {
        build<uint32_t>(art);
    }
    set_level(level);
}

void MazeRenderer::release() {
    auto free_store = [](auto& st) {
        free_vector(st.frame);
        free_vector(st.textures);
        free_vector(st.sprites);
        free_vector(st.ceil_rows);
        free_vector(st.floor_rows);
        free_vector(st.row);
    };
    free_store(s16_);
    free_store(s32_);
    free_vector(zbuf_);
    free_vector(wall_top_);
    free_vector(wall_bottom_);
    free_vector(row_dist_q16_);
    free_vector(src_rows_);
    free_vector(sprite_order_);
    w_ = h_ = iw_ = ih_ = 0;
}

size_t MazeRenderer::buffer_bytes() const {
    auto store_bytes = [](const auto& s) {
        return held_bytes(s.frame) + held_bytes(s.textures) + held_bytes(s.sprites) +
               held_bytes(s.ceil_rows) + held_bytes(s.floor_rows) + held_bytes(s.row);
    };
    return store_bytes(s16_) + store_bytes(s32_) + held_bytes(zbuf_) + held_bytes(wall_top_) +
           held_bytes(wall_bottom_) + held_bytes(row_dist_q16_) + held_bytes(src_rows_) +
           held_bytes(sprite_order_);
}

uint32_t MazeRenderer::internal_pixel(uint32_t x, uint32_t y) const {
    const size_t i = static_cast<size_t>(x) * ih_ + y;
    return format_ == PixelFormat::RGB565 ? s16_.frame[i] : s32_.frame[i];
}

void MazeRenderer::set_level(size_t level) {
    level_ = std::min(level, MAZE_LEVEL_COUNT - 1);
    if (w_ == 0 || h_ == 0) {
        return;
    }
    const uint32_t s = MAZE_LEVELS[level_].scale;
    iw_ = (w_ + s - 1) / s;
    ih_ = (h_ + s - 1) / s;
    for (uint32_t y = 0; y < ih_; y++) {
        // Distance to the floor (or ceiling) point the centre of row y sees.
        const float denom = std::fabs(2.0f * y + 1.0f - static_cast<float>(ih_));
        const float dist = std::min(static_cast<float>(ih_) / std::max(denom, 1.0f), 1000.0f);
        row_dist_q16_[y] = static_cast<int32_t>(dist * 65536.0f);
    }
    if (format_ == PixelFormat::RGB565) {
        apply_level<uint16_t>();
    } else {
        apply_level<uint32_t>();
    }
}

template <typename Px> void MazeRenderer::apply_level() {
    Store<Px>& st = store<Px>();
    // Sized for the level a run starts at. The gate only ever steps down, to a smaller frame,
    // so a running saver never grows it.
    if (st.frame.size() < static_cast<size_t>(iw_) * ih_) {
        st.frame.resize(static_cast<size_t>(iw_) * ih_);
    }
    const float half = static_cast<float>(ih_) * 0.5f;
    for (uint32_t y = 0; y < ih_; y++) {
        const float t = std::fabs(static_cast<float>(y) + 0.5f - half) / half; // 0 at horizon
        st.ceil_rows[y] =
            static_cast<Px>(PixelWriter::pack(mix({35, 35, 38}, {175, 172, 165}, t), format_));
        st.floor_rows[y] =
            static_cast<Px>(PixelWriter::pack(mix({35, 23, 14}, {130, 86, 48}, t), format_));
    }
}

template <typename Px> void MazeRenderer::build(const MazeArt& art) {
    Store<Px>& st = store<Px>();
    st.ceil_rows.assign(h_, 0);
    st.floor_rows.assign(h_, 0);
    st.row.assign(w_, 0);
    st.textures.assign(TEX_COUNT * TEX * TEX, 0);
    st.sprites.assign(SPR_COUNT * TEX * TEX, 0);
    auto pack = [this](Rgb c) { return static_cast<Px>(PixelWriter::pack(c, format_)); };

    // Heap, not static: the saver holds no memory while it is not running.
    auto pic_owner = std::make_unique<Canvas64>();
    Canvas64& pic = *pic_owner;
    pic.clear();
    has_picture_ = !art.picture.empty();
    if (has_picture_) {
        const Rgb matte{225, 215, 190};
        for (int v = 8; v < 56; v++) {
            for (int u = 10; u < 54; u++) {
                const bool frame = u < 13 || u >= 51 || v < 11 || v >= 53;
                pic.set(u, v, frame ? Rgb{70, 45, 25} : matte);
            }
        }
        fit_image(pic, art.picture, 15, 13, 34, 38, &matte);
    }
    auto tex = [&st](size_t slot, int u, int v) -> Px& {
        return st.textures[(slot * TEX + static_cast<size_t>(u)) * TEX + static_cast<size_t>(v)];
    };
    for (int u = 0; u < TEX; u++) {
        for (int v = 0; v < TEX; v++) {
            const Rgb brick = brick_texel(u, v);
            const Rgb picture = pic.on[u][v] ? pic.c[u][v] : brick;
            tex(TEX_BRICK, u, v) = pack(brick);
            tex(TEX_BRICK_DARK, u, v) = pack(scaled(brick, DARK_SIDE));
            tex(TEX_PICTURE, u, v) = pack(picture);
            tex(TEX_PICTURE_DARK, u, v) = pack(scaled(picture, DARK_SIDE));
            tex(TEX_FLOOR, u, v) = pack(floor_texel(u, v));
            tex(TEX_CEIL, u, v) = pack(ceiling_texel(u, v));
        }
    }
    pic_owner.reset();

    auto canvas_owner = std::make_unique<Canvas64>();
    auto logo_owner = std::make_unique<Canvas64>();
    Canvas64& canvas = *canvas_owner;
    Canvas64& logo = *logo_owner;
    auto keep = [this, &st, &canvas](size_t slot) {
        Px* dst = st.sprites.data() + slot * TEX * TEX;
        for (int u = 0; u < TEX; u++) {
            for (int v = 0; v < TEX; v++) {
                dst[static_cast<size_t>(u) * TEX + v] =
                    canvas.on[u][v] ? pack_sprite<Px>(canvas.c[u][v], format_) : sprite_key<Px>();
            }
        }
    };

    canvas.clear();
    if (art.logo.empty()) {
        draw_fallback_mark(canvas);
    } else {
        fit_image(canvas, art.logo, 2, 2, TEX - 4, TEX - 4, nullptr);
    }
    logo = canvas;
    keep(SPR_LOGO);

    canvas.clear();
    for (int v = 10; v < 54; v++) {
        for (int u = 2; u < 62; u++) {
            const bool border = u < 5 || u >= 59 || v < 13 || v >= 51;
            canvas.set(u, v, border ? Rgb{200, 30, 30} : Rgb{20, 30, 70});
        }
    }
    // The logo at half size, centred on the sign.
    for (int v = 0; v < TEX; v += 2) {
        for (int u = 0; u < TEX; u += 2) {
            if (logo.on[u][v]) {
                canvas.set(16 + u / 2, 16 + v / 2, logo.c[u][v]);
            }
        }
    }
    keep(SPR_START);

    canvas.clear();
    draw_smiley(canvas);
    keep(SPR_EXIT);

    canvas.clear();
    draw_toolhead(canvas);
    keep(SPR_TOOLHEAD);

    for (int f = 0; f < ROCK_FRAMES; f++) {
        canvas.clear();
        draw_rock(canvas, f);
        keep(SPR_ROCK0 + static_cast<size_t>(f));
    }
}

template <typename Px> void MazeRenderer::cast_walls(const MazeMap& map, const MazePose& pose) {
    Store<Px>& st = store<Px>();
    const float dir_x = std::cos(pose.angle);
    const float dir_y = std::sin(pose.angle);
    const float plane_len = 0.5f * static_cast<float>(iw_) / static_cast<float>(ih_);
    const float plane_x = -dir_y * plane_len;
    const float plane_y = dir_x * plane_len;
    const bool textured = MAZE_LEVELS[level_].textured_floor;
    const auto pos_xq = static_cast<int32_t>(pose.x * 65536.0f);
    const auto pos_yq = static_cast<int32_t>(pose.y * 65536.0f);
    const Px* floor_tex = st.textures.data() + TEX_FLOOR * TEX * TEX;
    const Px* ceil_tex = st.textures.data() + TEX_CEIL * TEX * TEX;
    const int ih = static_cast<int>(ih_);
    const int max_steps = 4 * std::max(map.size(), 1);

    for (uint32_t x = 0; x < iw_; x++) {
        const float cam = 2.0f * (static_cast<float>(x) + 0.5f) / static_cast<float>(iw_) - 1.0f;
        const float rdx = dir_x + plane_x * cam;
        const float rdy = dir_y + plane_y * cam;
        int mx = static_cast<int>(std::floor(pose.x));
        int my = static_cast<int>(std::floor(pose.y));
        const float ddx = rdx == 0.0f ? 1e30f : std::fabs(1.0f / rdx);
        const float ddy = rdy == 0.0f ? 1e30f : std::fabs(1.0f / rdy);
        const int sx = rdx < 0.0f ? -1 : 1;
        const int sy = rdy < 0.0f ? -1 : 1;
        float side_x = rdx < 0.0f ? (pose.x - mx) * ddx : (mx + 1.0f - pose.x) * ddx;
        float side_y = rdy < 0.0f ? (pose.y - my) * ddy : (my + 1.0f - pose.y) * ddy;
        int side = 0;
        for (int i = 0; i < max_steps; i++) {
            if (side_x < side_y) {
                side_x += ddx;
                mx += sx;
                side = 0;
            } else {
                side_y += ddy;
                my += sy;
                side = 1;
            }
            if (map.wall(mx, my)) {
                break;
            }
        }
        const float perp = std::max(side == 0 ? side_x - ddx : side_y - ddy, 1e-3f);
        float wall_x = side == 0 ? pose.y + perp * rdy : pose.x + perp * rdx;
        wall_x -= std::floor(wall_x);
        int tex_u = std::min(static_cast<int>(wall_x * TEX), TEX - 1);
        // The camera plane points right of the view, so the faces whose wall coordinate runs
        // right to left on screen are the ones read backwards.
        if ((side == 0 && rdx < 0.0f) || (side == 1 && rdy > 0.0f)) {
            tex_u = TEX - 1 - tex_u;
        }
        const bool picture = has_picture_ && map.tile(mx, my) == MazeTile::PICTURE;
        const size_t tex = (picture ? TEX_PICTURE : TEX_BRICK) + (side == 1 ? 1 : 0);

        const float line_h = static_cast<float>(ih) / perp;
        const float top_f = (static_cast<float>(ih) - line_h) * 0.5f;
        const int top = static_cast<int>(std::floor(top_f));
        const int bottom = static_cast<int>(std::floor(top_f + line_h));
        zbuf_[x] = perp;
        wall_top_[x] = top;
        wall_bottom_[x] = bottom;

        Px* col = st.frame.data() + static_cast<size_t>(x) * ih_;
        const int y0 = std::clamp(top, 0, ih);
        const int y1 = std::clamp(bottom, 0, ih);
        const Px* texcol = st.textures.data() + (tex * TEX + static_cast<size_t>(tex_u)) * TEX;
        // 16.16 texture rows sampled at pixel centres; uint32 wraps keep the low bits the mask
        // reads.
        const auto step = static_cast<uint32_t>(static_cast<int64_t>(TEX * 65536.0 / line_h));
        auto tp = static_cast<uint32_t>(
            static_cast<int64_t>((static_cast<double>(y0) + 0.5 - top_f) * TEX * 65536.0 / line_h));
        for (int y = y0; y < y1; y++) {
            col[y] = texcol[(tp >> 16) & (TEX - 1)];
            tp += step;
        }

        if (!textured) {
            for (int y = 0; y < y0; y++) {
                col[y] = st.ceil_rows[static_cast<size_t>(y)];
            }
            for (int y = y1; y < ih; y++) {
                col[y] = st.floor_rows[static_cast<size_t>(y)];
            }
            continue;
        }
        const auto rdxq = static_cast<int32_t>(rdx * 65536.0f);
        const auto rdyq = static_cast<int32_t>(rdy * 65536.0f);
        auto texel = [&](int y) {
            const int64_t d = row_dist_q16_[static_cast<size_t>(y)];
            const auto fx = static_cast<int32_t>(pos_xq + ((d * rdxq) >> 16));
            const auto fy = static_cast<int32_t>(pos_yq + ((d * rdyq) >> 16));
            return static_cast<size_t>((fx >> 10) & (TEX - 1)) * TEX +
                   static_cast<size_t>((fy >> 10) & (TEX - 1));
        };
        for (int y = 0; y < y0; y++) {
            col[y] = ceil_tex[texel(y)];
        }
        for (int y = y1; y < ih; y++) {
            col[y] = floor_tex[texel(y)];
        }
    }
}

template <typename Px>
void MazeRenderer::draw_sprites(const std::vector<MazeItem>& items, const MazePose& pose,
                                uint32_t elapsed_ms) {
    Store<Px>& st = store<Px>();
    sprite_order_.clear();
    for (size_t i = 0; i < items.size(); i++) {
        if (!items[i].active) {
            continue;
        }
        const float dx = items[i].x - pose.x;
        const float dy = items[i].y - pose.y;
        sprite_order_.push_back({dx * dx + dy * dy, static_cast<uint16_t>(i)});
    }
    std::sort(sprite_order_.begin(), sprite_order_.end(),
              [](const SpriteDraw& a, const SpriteDraw& b) { return a.depth > b.depth; });

    const float dir_x = std::cos(pose.angle);
    const float dir_y = std::sin(pose.angle);
    const float plane_len = 0.5f * static_cast<float>(iw_) / static_cast<float>(ih_);
    const float plane_x = -dir_y * plane_len;
    const float plane_y = dir_x * plane_len;
    const float inv_det = 1.0f / (plane_x * dir_y - dir_x * plane_y);
    const float fw = static_cast<float>(iw_);
    const float fh = static_cast<float>(ih_);
    const Px key = sprite_key<Px>();
    // Phase within one turn, from an integer that wraps instead of a float that loses precision.
    const float spin_phase = 2.0f * PI_F * static_cast<float>(elapsed_ms % SPIN_PERIOD_MS) /
                             static_cast<float>(SPIN_PERIOD_MS);

    for (const SpriteDraw& s : sprite_order_) {
        const MazeItem& item = items[s.item];
        const SpriteStyle style = style_of(item.kind);
        const float rx = item.x - pose.x;
        const float ry = item.y - pose.y;
        const float tx = inv_det * (dir_y * rx - dir_x * ry);
        const float ty = inv_det * (-plane_y * rx + plane_x * ry);
        if (ty < style.near) {
            continue;
        }
        const float screen_x = fw * 0.5f * (1.0f + tx / ty);
        const float h = style.size * fh / ty;
        float width_k = 1.0f;
        bool mirror = false;
        size_t slot = SPR_LOGO;
        switch (item.kind) {
        case MazeItemKind::START:
            slot = SPR_START;
            break;
        case MazeItemKind::EXIT:
            slot = SPR_EXIT;
            break;
        case MazeItemKind::TOOLHEAD:
            slot = SPR_TOOLHEAD;
            break;
        case MazeItemKind::ROCK:
            slot = SPR_ROCK0 + (elapsed_ms / ROCK_FRAME_MS + s.item) % ROCK_FRAMES;
            break;
        case MazeItemKind::LOGO:
            slot = SPR_LOGO;
            break;
        }
        if (style.spins) {
            const float c = std::cos(spin_phase + 0.7f * static_cast<float>(s.item));
            width_k = std::fabs(c);
            mirror = c < 0.0f;
        }
        const float w = h * width_k;
        if (w < 1.0f) {
            continue;
        }
        const float centre_y = fh * 0.5f + (0.5f - style.center_z) * fh / ty;
        const float x0 = screen_x - w * 0.5f;
        const float y0 = centre_y - h * 0.5f;
        const int xs = std::max(0, static_cast<int>(std::ceil(x0 - 0.5f)));
        const int xe = std::min(static_cast<int>(iw_), static_cast<int>(std::ceil(x0 + w - 0.5f)));
        const int ys = std::max(0, static_cast<int>(std::ceil(y0 - 0.5f)));
        const int ye = std::min(static_cast<int>(ih_), static_cast<int>(std::ceil(y0 + h - 0.5f)));
        if (xs >= xe || ys >= ye) {
            continue;
        }
        const auto vstep = static_cast<uint32_t>(TEX * 65536.0f / h);
        const auto vstart =
            static_cast<uint32_t>((static_cast<float>(ys) + 0.5f - y0) * TEX * 65536.0f / h);
        const Px* spr = st.sprites.data() + slot * TEX * TEX;
        for (int x = xs; x < xe; x++) {
            if (ty >= zbuf_[static_cast<size_t>(x)]) {
                continue;
            }
            int u = std::clamp(static_cast<int>((static_cast<float>(x) + 0.5f - x0) * TEX / w), 0,
                               TEX - 1);
            if (mirror) {
                u = TEX - 1 - u;
            }
            const Px* scol = spr + static_cast<size_t>(u) * TEX;
            Px* col = st.frame.data() + static_cast<size_t>(x) * ih_;
            uint32_t vp = vstart;
            for (int y = ys; y < ye; y++, vp += vstep) {
                const Px px = scol[std::min<uint32_t>(vp >> 16, TEX - 1)];
                if (px == key || (style.screen_door && ((x + y) & 1) != 0)) {
                    continue;
                }
                col[y] = px;
            }
        }
    }
}

template <typename Px> void MazeRenderer::blit(float flip, FrameTarget& out) {
    Store<Px>& st = store<Px>();
    const int ih = static_cast<int>(ih_);
    if (flip <= 0.0f) {
        for (int y = 0; y < ih; y++) {
            src_rows_[static_cast<size_t>(y)] = y;
        }
    } else if (flip >= 1.0f) {
        for (int y = 0; y < ih; y++) {
            src_rows_[static_cast<size_t>(y)] = ih - 1 - y;
        }
    } else {
        // Rows squash toward the horizon, through zero, and open out mirrored.
        const float s = std::cos(flip * PI_F);
        const float c = static_cast<float>(ih) * 0.5f;
        for (int y = 0; y < ih; y++) {
            int src = -1;
            if (std::fabs(s) >= 0.02f) {
                const int r =
                    static_cast<int>(std::floor(c + (static_cast<float>(y) + 0.5f - c) / s));
                src = r >= 0 && r < ih ? r : -1;
            }
            src_rows_[static_cast<size_t>(y)] = src;
        }
    }
    const auto black = static_cast<Px>(PixelWriter::pack({0, 0, 0}, format_));
    blit_rows<Px>(st.frame, iw_, ih_, src_rows_, MAZE_LEVELS[level_].scale, black, st.row, out);
}

template <typename Px>
void MazeRenderer::render_as(const MazeMap& map, const std::vector<MazeItem>& items,
                             const MazePose& pose, float flip, uint32_t elapsed_ms,
                             FrameTarget& out) {
    cast_walls<Px>(map, pose);
    draw_sprites<Px>(items, pose, elapsed_ms);
    blit<Px>(flip, out);
}

void MazeRenderer::render(const MazeMap& map, const std::vector<MazeItem>& items,
                          const MazePose& pose, float flip, uint32_t elapsed_ms, FrameTarget& out) {
    if (iw_ == 0 || ih_ == 0 || out.w != w_ || out.h != h_ || out.format != format_) {
        return;
    }
    if (format_ == PixelFormat::RGB565) {
        render_as<uint16_t>(map, items, pose, flip, elapsed_ms, out);
    } else {
        render_as<uint32_t>(map, items, pose, flip, elapsed_ms, out);
    }
}

void MazeSim::init(const FrameTarget& frame, const MazeArt& art, std::minstd_rand& rng,
                   size_t level) {
    renderer_.init(frame.w, frame.h, frame.format, art, level);
    items_.clear();
    items_.reserve(4 + LOGO_COUNT + ROCK_COUNT);
    const size_t cells = static_cast<size_t>(CELLS) * CELLS;
    taken_.reserve(cells);
    dist_.reserve(cells);
    queue_.reserve(cells);
    elapsed_ms_ = 0;
    new_maze(rng);
    mazes_ = 0;
}

void MazeSim::clear() {
    renderer_.release();
    map_.release();
    free_vector(items_);
    free_vector(taken_);
    free_vector(dist_);
    free_vector(queue_);
    flip_left_ms_ = 0;
    exit_left_ms_ = 0;
}

uint32_t MazeSim::step_bound() const {
    // A hand on the wall of a perfect maze crosses each passage at most twice; every rock
    // swaps hands and may start that tour again.
    return 4u * CELLS * CELLS * (ROCK_COUNT + 1);
}

float MazeSim::flip() const {
    if (flip_left_ms_ == 0) {
        return upside_down_ ? 1.0f : 0.0f;
    }
    const float t = 1.0f - static_cast<float>(flip_left_ms_) / FLIP_MS;
    return upside_down_ ? t : 1.0f - t;
}

void MazeSim::new_maze(std::minstd_rand& rng) {
    map_.generate(CELLS, rng);
    const int n = CELLS;
    const auto cells = static_cast<size_t>(n * n);
    taken_.assign(cells, 0);
    dist_.assign(cells, -1);
    queue_.clear();
    auto passages = [this](int c) {
        int count = 0;
        for (int d = 0; d < 4; d++) {
            count += map_.passage(c % CELLS, c / CELLS, d) ? 1 : 0;
        }
        return count;
    };

    // Start in a dead end facing out of it, so the walk opens on the start sign ahead.
    int start = random_below(rng, n * n);
    for (int i = 0; i < n * n; i++) {
        const int c = (start + i) % (n * n);
        if (passages(c) == 1) {
            start = c;
            break;
        }
    }
    int heading = 0;
    while (heading < 3 && !map_.passage(start % n, start / n, heading)) {
        heading++;
    }

    // The exit is the cell furthest from the start, which in a tree is always a dead end.
    queue_.push_back(start);
    dist_[static_cast<size_t>(start)] = 0;
    int exit = start;
    for (size_t qi = 0; qi < queue_.size(); qi++) {
        const int c = queue_[qi];
        if (dist_[static_cast<size_t>(c)] > dist_[static_cast<size_t>(exit)]) {
            exit = c;
        }
        for (int d = 0; d < 4; d++) {
            if (!map_.passage(c % n, c / n, d)) {
                continue;
            }
            const int next = (c / n + HEADING_DY[d]) * n + c % n + HEADING_DX[d];
            if (dist_[static_cast<size_t>(next)] < 0) {
                dist_[static_cast<size_t>(next)] = dist_[static_cast<size_t>(c)] + 1;
                queue_.push_back(next);
            }
        }
    }

    items_.clear();
    taken_[static_cast<size_t>(start)] = 1;
    taken_[static_cast<size_t>(exit)] = 1;
    items_.push_back({MazeItemKind::START, cell_centre(start % n) + 0.8f * HEADING_DX[heading],
                      cell_centre(start / n) + 0.8f * HEADING_DY[heading], true});
    items_.push_back({MazeItemKind::EXIT, cell_centre(exit % n), cell_centre(exit / n), true});
    auto place = [&](MazeItemKind kind, int count) {
        for (int placed = 0; placed < count;) {
            const int c = random_below(rng, n * n);
            if (taken_[static_cast<size_t>(c)] != 0) {
                continue;
            }
            taken_[static_cast<size_t>(c)] = 1;
            items_.push_back({kind, cell_centre(c % n), cell_centre(c / n), true});
            placed++;
        }
    };
    place(MazeItemKind::LOGO, LOGO_COUNT);
    place(MazeItemKind::ROCK, ROCK_COUNT);

    const int tool_cell = random_below(rng, n * n);
    int tool_heading = 0;
    while (tool_heading < 3 && !map_.passage(tool_cell % n, tool_cell / n, tool_heading)) {
        tool_heading++;
    }
    toolhead_.reset(tool_cell % n, tool_cell / n, tool_heading, random_below(rng, 2) == 1, 1100,
                    600);
    toolhead_item_ = items_.size();
    items_.push_back(
        {MazeItemKind::TOOLHEAD, cell_centre(tool_cell % n), cell_centre(tool_cell / n), true});

    walker_.reset(start % n, start / n, heading, false);
    // The walk eases to a stop in the exit's doorway, with the smiley a tile ahead.
    walker_.set_stop_cell(exit % n, exit / n);
    upside_down_ = false;
    flip_left_ms_ = 0;
    exit_left_ms_ = 0;
    mazes_++;
}

void MazeSim::on_arrival(std::minstd_rand& rng) {
    const float x = cell_centre(walker_.cell_x());
    const float y = cell_centre(walker_.cell_y());
    for (MazeItem& item : items_) {
        if (item.active && item.kind == MazeItemKind::ROCK && item.x == x && item.y == y) {
            item.active = false;
            upside_down_ = !upside_down_;
            walker_.set_left_handed(upside_down_);
            flip_left_ms_ = FLIP_MS;
        }
    }
    if (walker_.moves() > step_bound()) {
        new_maze(rng);
    }
}

void MazeSim::advance(uint32_t dt_ms, std::minstd_rand& rng) {
    elapsed_ms_ += dt_ms;

    uint32_t left = dt_ms;
    while (left > 0) {
        WalkEvent event = WalkEvent::NONE;
        const uint32_t used = toolhead_.advance(map_, left, event);
        if (used == 0) {
            break;
        }
        left -= used;
    }
    const MazePose tool = toolhead_.pose();
    items_[toolhead_item_].x = tool.x;
    items_[toolhead_item_].y = tool.y;

    left = dt_ms;
    while (left > 0) {
        if (flip_left_ms_ > 0) {
            const uint32_t used = std::min(left, flip_left_ms_);
            flip_left_ms_ -= used;
            left -= used;
            continue;
        }
        if (exit_left_ms_ > 0) {
            const uint32_t used = std::min(left, exit_left_ms_);
            exit_left_ms_ -= used;
            left -= used;
            if (exit_left_ms_ == 0) {
                new_maze(rng);
            }
            continue;
        }
        WalkEvent event = WalkEvent::NONE;
        const uint32_t used = walker_.advance(map_, left, event);
        left -= used;
        if (event == WalkEvent::ARRIVED) {
            on_arrival(rng);
        } else if (event == WalkEvent::ENTERED && walker_.stopped()) {
            exit_left_ms_ = EXIT_HOLD_MS;
        } else if (used == 0) {
            break;
        }
    }
}

void MazeSim::step(uint32_t dt_ms, FrameTarget& frame, std::minstd_rand& rng,
                   std::vector<DirtyRect>& dirty) {
    advance(dt_ms, rng);
    renderer_.render(map_, items_, walker_.pose(), flip(), elapsed_ms_, frame);
    dirty.clear();
    dirty.push_back({0, 0, static_cast<int32_t>(frame.w) - 1, static_cast<int32_t>(frame.h) - 1});
}

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
