// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver_highfly_sim.h"

#include "screensaver_fast_math.h"
#include "screensaver_motion.h"
#include "screensaver_pixel_writer.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace helix::ui {

using screensaver::unit_random;

namespace {

constexpr float PI = 3.14159265358979f;
constexpr float TWO_PI = 2.0f * PI;
constexpr int32_t N = HIGHFLY_FIELD;

// Camera and deck, as tuned at a 400 x 240 internal frame.
constexpr float HORIZON = 0.56f;
constexpr float CAM_H = 15.0f;
constexpr float FOCAL_Y_AT_240 = 150.0f;
constexpr float FOV = 1.0f; ///< half-width tan
constexpr float Z_NEAR = 10.0f;
constexpr float Z_FAR = 350.0f;
constexpr float DZ_GROW = 1.025f;
constexpr float FEATHER_AT_240 = 10.0f;
constexpr float SINK = 40.0f;
constexpr float NEAR_WHITE = 40.0f;
constexpr float NEAR_K = 0.7f;
constexpr float HAZE = 0.22f;
/// World x of the field under the screen's centre.
constexpr float FIELD_X0 = 64.0f;

constexpr Rgb C_SHADOW{45, 130, 182};
constexpr Rgb C_MIDTONE{150, 212, 235};
constexpr Rgb C_LIT{252, 254, 255};
constexpr Rgb C_FOG{30, 110, 162};
constexpr Rgb INSIDE{200, 216, 230};
constexpr Rgb NUMERAL{226, 238, 250};
constexpr float NUMERAL_OPACITY = 0.85f;
constexpr float DIGIT_STROKE = 0.06f;

const HeightfieldFog FOG{25.0f, 300.0f, 0.8f, false};

// Billboards.
constexpr float BAND_Z = 260.0f;
constexpr float BAND_Q = 0.55f;
constexpr float BAND_HB = -6.0f;
constexpr float BAND_DRIFT = 1.5f; ///< world units per second sideways
constexpr float LAYER_Q = 0.16f;
constexpr float LAYER_ZFAR = 170.0f;
constexpr float LAYER_ZNEAR = 8.0f;
constexpr float LAYER_HB = -7.0f;
constexpr float TOWER_Q = 0.22f;
constexpr float TOWER_HB = -8.0f;
constexpr float TOWER_Z_ENTER = 330.0f;
/// A tower enters this far to the side, as a share of the half-width at its entry depth; it
/// keeps its world x, so it slides outward as it nears, and drifts on the wind besides.
constexpr float TOWER_SIDE_MIN = 0.30f;
constexpr float TOWER_SIDE_MAX = 0.50f;
constexpr float TOWER_WIND_MIN = 1.0f;
constexpr float TOWER_WIND_MAX = 2.5f;
/// Towers fade out from TOWER_FADE_FROM to TOWER_Z_LEAVE as they come near, then go, so no
/// more than two are ever up with one entering every 13 to 14 s.
constexpr float TOWER_FADE_FROM = 120.0f;
constexpr float TOWER_Z_LEAVE = 70.0f;
constexpr float TOWER_FOGMUL = 0.25f;
constexpr float BODY_Q = 0.09f;
/// The fly-through cloud is this far ahead when its whiteout begins.
constexpr float FLY_FACE = 10.0f;

constexpr size_t HISTORY_CAP = 64;
/// Rows the deck may rise in a frame: the band repaints this far above last frame's top.
constexpr int32_t DECK_RISE = 3;
constexpr size_t BOX_CAP = 8;

float smooth01(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

float uniform(std::minstd_rand& rng, float lo, float hi) {
    return lo + (hi - lo) * unit_random(rng);
}

float normal(std::minstd_rand& rng) {
    const float u1 = std::max(unit_random(rng), 1e-7f);
    const float u2 = unit_random(rng);
    return std::sqrt(-2.0f * std::log(u1)) * std::cos(TWO_PI * u2);
}

template <typename T> void free_vector(std::vector<T>& v) {
    std::vector<T>().swap(v);
}

template <typename T> size_t bytes_of(const std::vector<T>& v) {
    return v.capacity() * sizeof(T);
}

Rgb lerp(Rgb a, Rgb b, float t) {
    return {static_cast<uint8_t>(std::clamp(a.r + (b.r - a.r) * t, 0.0f, 255.0f) + 0.5f),
            static_cast<uint8_t>(std::clamp(a.g + (b.g - a.g) * t, 0.0f, 255.0f) + 0.5f),
            static_cast<uint8_t>(std::clamp(a.b + (b.b - a.b) * t, 0.0f, 255.0f) + 0.5f)};
}

/// Cloud shade 0-1 to colour: shadow blue to midtone, then midtone to sunlit white.
Rgb shade_to_rgb(float s) {
    s = std::clamp(s, 0.0f, 1.0f);
    if (s < 0.55f) {
        return lerp(C_SHADOW, C_MIDTONE, s / 0.55f);
    }
    return lerp(C_MIDTONE, C_LIT, (s - 0.55f) / 0.45f);
}

// ---------- Grid helpers ----------

/// Box blur of radius `r`, three passes, each axis wrapping or clamping at its ends.
void box_blur(std::vector<float>& a, int32_t w, int32_t h, int32_t r, bool wrap_x, bool wrap_y,
              std::vector<float>& tmp) {
    if (r <= 0) {
        return;
    }
    tmp.resize(a.size());
    const float inv = 1.0f / static_cast<float>(2 * r + 1);
    const auto pass = [&](bool along_x) {
        const int32_t len = along_x ? w : h;
        const int32_t lines = along_x ? h : w;
        const bool wrap = along_x ? wrap_x : wrap_y;
        for (int32_t l = 0; l < lines; l++) {
            const auto at = [&](int32_t i) -> float {
                if (wrap) {
                    i = ((i % len) + len) % len;
                } else {
                    i = std::clamp(i, 0, len - 1);
                }
                return along_x ? a[static_cast<size_t>(l) * w + i]
                               : a[static_cast<size_t>(i) * w + l];
            };
            float sum = 0.0f;
            for (int32_t k = -r; k <= r; k++) {
                sum += at(k);
            }
            for (int32_t i = 0; i < len; i++) {
                const size_t o =
                    along_x ? static_cast<size_t>(l) * w + i : static_cast<size_t>(i) * w + l;
                tmp[o] = sum * inv;
                sum += at(i + r + 1) - at(i - r);
            }
        }
        a.swap(tmp);
    };
    for (int pass_n = 0; pass_n < 3; pass_n++) {
        pass(true);
        pass(false);
    }
}

float stddev(const std::vector<float>& a) {
    double sum = 0.0;
    double sq = 0.0;
    for (const float v : a) {
        sum += v;
        sq += static_cast<double>(v) * v;
    }
    const double n = static_cast<double>(a.size());
    const double mean = sum / n;
    return static_cast<float>(std::sqrt(std::max(sq / n - mean * mean, 1e-12)));
}

/// Fractal value noise: smoothstep-interpolated normal grids at a few cell counts, summed and
/// normalised to unit deviation. Wraps across x when asked.
void fbm(std::minstd_rand& rng, int32_t h, int32_t w, bool wrap_x,
         std::initializer_list<std::pair<int32_t, float>> octaves, std::vector<float>& out) {
    out.assign(static_cast<size_t>(w) * h, 0.0f);
    std::vector<float> g;
    for (const auto& [cells, amp] : octaves) {
        const int32_t gw = cells;
        const int32_t gh = std::max(2, cells * h / w + 2);
        g.resize(static_cast<size_t>(gh) * (gw + 1));
        for (float& v : g) {
            v = normal(rng);
        }
        if (wrap_x) {
            for (int32_t y = 0; y < gh; y++) {
                g[static_cast<size_t>(y) * (gw + 1) + gw] = g[static_cast<size_t>(y) * (gw + 1)];
            }
        }
        for (int32_t y = 0; y < h; y++) {
            const float fy = static_cast<float>(gh - 1.001) * static_cast<float>(y) /
                             static_cast<float>(std::max(1, h - 1));
            const auto y0 = static_cast<int32_t>(fy);
            const float ty = smooth01(fy - static_cast<float>(y0));
            for (int32_t x = 0; x < w; x++) {
                const float fx = wrap_x ? static_cast<float>(gw) * static_cast<float>(x) / w
                                        : static_cast<float>(gw) * static_cast<float>(x) /
                                              static_cast<float>(std::max(1, w - 1));
                const auto x0 = std::min(static_cast<int32_t>(fx), gw - 1);
                const float tx = smooth01(fx - static_cast<float>(x0));
                const size_t r0 = static_cast<size_t>(y0) * (gw + 1);
                const size_t r1 = r0 + static_cast<size_t>(gw + 1);
                const float a = g[r0 + x0] + (g[r0 + x0 + 1] - g[r0 + x0]) * tx;
                const float b = g[r1 + x0] + (g[r1 + x0 + 1] - g[r1 + x0]) * tx;
                out[static_cast<size_t>(y) * w + x] += amp * (a + (b - a) * ty);
            }
        }
    }
    const float sd = stddev(out);
    for (float& v : out) {
        v /= sd;
    }
}

/// Adds a Gaussian blob exp(-d^2 / r^2) * amp, wrapping across x when asked.
void add_blob(std::vector<float>& d, int32_t w, int32_t h, float cx, float cy, float r, bool wrap_x,
              float amp) {
    const float reach = 3.0f * r;
    const auto y0 = static_cast<int32_t>(std::max(0.0f, std::floor(cy - reach)));
    const auto y1 =
        static_cast<int32_t>(std::min(static_cast<float>(h - 1), std::ceil(cy + reach)));
    const auto x0 = static_cast<int32_t>(std::floor(cx - reach));
    const auto x1 = static_cast<int32_t>(std::ceil(cx + reach));
    const float inv_r = 1.0f / r;
    for (int32_t y = y0; y <= y1; y++) {
        const float dy = (static_cast<float>(y) - cy) * inv_r;
        for (int32_t xx = x0; xx <= x1; xx++) {
            int32_t x = xx;
            if (wrap_x) {
                x = ((x % w) + w) % w;
            } else if (x < 0 || x >= w) {
                continue;
            }
            const float dx = (static_cast<float>(xx) - cx) * inv_r;
            d[static_cast<size_t>(y) * w + x] += amp * fast_exp_neg(dx * dx + dy * dy);
        }
    }
}

/// Density to premultiplied RGBA, lit from above by the light the cloud lets through.
void shade_cloud(std::vector<float>& d, const std::vector<float>& wisp, float wisp_scale, int32_t w,
                 int32_t h, bool wrap_x, float dens_k, float absorb, int32_t smooth, float bias,
                 bool crown, HighFlyTexture& out) {
    std::vector<float> tmp;
    for (size_t i = 0; i < d.size(); i++) {
        const float dc = std::clamp(d[i], 0.0f, 1.0f);
        d[i] = std::max(d[i] + 0.45f * wisp[i] * wisp_scale * dc * (1.0f - dc) * 4.0f, 0.0f);
    }
    box_blur(d, w, h, smooth, wrap_x, false, tmp);
    // Optical depth from the top, the light that reaches each texel.
    std::vector<float> od(d.size());
    for (int32_t x = 0; x < w; x++) {
        float acc = 0.0f;
        for (int32_t y = 0; y < h; y++) {
            acc += d[static_cast<size_t>(y) * w + x];
            od[static_cast<size_t>(y) * w + x] = acc / static_cast<float>(h) * 18.0f;
        }
    }
    box_blur(od, w, h, 2, wrap_x, false, tmp);
    std::vector<float> soft = d;
    box_blur(soft, w, h, 3, wrap_x, false, tmp);

    out.w = w;
    out.h = h;
    out.rgba.assign(static_cast<size_t>(w) * h * 4, 0);
    for (int32_t y = 0; y < h; y++) {
        const int32_t yu = std::max(0, y - 1);
        const int32_t yd = std::min(h - 1, y + 1);
        for (int32_t x = 0; x < w; x++) {
            const size_t i = static_cast<size_t>(y) * w + x;
            const float alpha = 1.0f - std::exp(-dens_k * d[i]);
            const float lit = std::exp(-absorb * od[i]);
            const float gy =
                (soft[static_cast<size_t>(yd) * w + x] - soft[static_cast<size_t>(yu) * w + x]) /
                static_cast<float>(std::max(1, yd - yu));
            const float relief = std::clamp(gy * 6.0f, -1.0f, 1.0f);
            const float edge = std::clamp(1.0f - alpha, 0.0f, 1.0f);
            float s = bias + 0.50f + 0.55f * lit + 0.30f * edge + 0.12f * relief +
                      0.03f * wisp[i] * wisp_scale - 0.25f * (1.0f - lit) * (1.0f - lit);
            if (crown) {
                // Sunlit tops read white, not bluish.
                s += 0.25f * std::clamp(1.0f - od[i] / 2.0f, 0.0f, 1.0f);
            }
            const Rgb c = shade_to_rgb(s);
            uint8_t* p = &out.rgba[i * 4];
            p[0] = static_cast<uint8_t>(c.r * alpha + 0.5f);
            p[1] = static_cast<uint8_t>(c.g * alpha + 0.5f);
            p[2] = static_cast<uint8_t>(c.b * alpha + 0.5f);
            p[3] = static_cast<uint8_t>(alpha * 255.0f + 0.5f);
        }
    }
}

void make_band(std::minstd_rand& rng, HighFlyTexture& out) {
    const int32_t w = 1024;
    const int32_t h = 90;
    std::vector<float> d(static_cast<size_t>(w) * h, 0.0f);
    float x = 0.0f;
    while (x < static_cast<float>(w)) {
        const float r = uniform(rng, 14.0f, 30.0f);
        const float top = uniform(rng, 10.0f, 50.0f);
        add_blob(d, w, h, x, top + r, r, true, 1.0f);
        for (int i = 0; i < 4; i++) {
            const float sx = x + uniform(rng, -r, r);
            const float sy = top + uniform(rng, 0.0f, r);
            add_blob(d, w, h, sx, sy, r * uniform(rng, 0.3f, 0.55f), true, 0.8f);
        }
        x += uniform(rng, 18.0f, 40.0f);
    }
    for (int32_t y = 55; y < h; y++) {
        const float add = 2.5f * static_cast<float>(y - 55) / static_cast<float>(h - 56);
        for (int32_t xx = 0; xx < w; xx++) {
            d[static_cast<size_t>(y) * w + xx] += add;
        }
    }
    std::vector<float> wisp;
    fbm(rng, h, w, true, {{8, 1.0f}, {16, 0.6f}, {32, 0.35f}, {64, 0.2f}}, wisp);
    shade_cloud(d, wisp, 1.0f, w, h, true, 2.2f, 0.9f, 1, 0.0f, false, out);
}

void make_layer(std::minstd_rand& rng, HighFlyTexture& out) {
    const int32_t w = 512;
    const int32_t h = 128;
    std::vector<float> d(static_cast<size_t>(w) * h, 0.0f);
    for (int i = 0; i < 70; i++) {
        const float r = uniform(rng, 8.0f, 26.0f);
        const float cx = uniform(rng, 0.0f, static_cast<float>(w));
        const float cy = uniform(rng, 30.0f, 115.0f);
        add_blob(d, w, h, cx, cy, r, true, uniform(rng, 0.5f, 1.0f));
    }
    for (int32_t y = 80; y < h; y++) {
        const float add = 2.0f * static_cast<float>(y - 80) / static_cast<float>(h - 81);
        for (int32_t x = 0; x < w; x++) {
            d[static_cast<size_t>(y) * w + x] += add;
        }
    }
    std::vector<float> wisp;
    fbm(rng, h, w, true, {{8, 1.0f}, {16, 0.6f}, {32, 0.35f}, {64, 0.2f}}, wisp);
    for (size_t i = 0; i < d.size(); i++) {
        d[i] *= std::clamp(0.8f + 0.25f * wisp[i], 0.0f, 2.0f);
    }
    shade_cloud(d, wisp, 1.0f, w, h, true, 1.8f, 0.9f, 1, 0.0f, false, out);
}

/// Cumulus colour by shade 0-1: blue-grey under the lobes to sunlit white on top.
Rgb tower_rgb(float s) {
    constexpr Rgb SHADE{118, 152, 184};
    constexpr Rgb MID{196, 218, 236};
    s = std::clamp(s, 0.0f, 1.0f);
    if (s < 0.5f) {
        return lerp(SHADE, MID, s / 0.5f);
    }
    return lerp(MID, C_LIT, (s - 0.5f) / 0.5f);
}

/// A towering cumulus lit from above and a little in front: each lobe takes Lambert light on a
/// normal from the smoothed density, so the crown and the tops of the lobes read sunlit white
/// and their undersides blue-grey, darkening toward the base.
void shade_tower(std::vector<float>& d, int32_t w, int32_t h, HighFlyTexture& out) {
    std::vector<float> tmp;
    box_blur(d, w, h, 2, false, false, tmp);
    std::vector<float> soft = d;
    box_blur(soft, w, h, 6, false, false, tmp);
    std::vector<float> od(d.size());
    for (int32_t x = 0; x < w; x++) {
        float acc = 0.0f;
        for (int32_t y = 0; y < h; y++) {
            acc += d[static_cast<size_t>(y) * w + x];
            od[static_cast<size_t>(y) * w + x] = acc / static_cast<float>(h) * 18.0f;
        }
    }
    box_blur(od, w, h, 3, false, false, tmp);
    // Light from above, a little to the left and toward the camera; y grows downward.
    const float lx = -0.35f;
    const float ly = -0.80f;
    const float lz = 0.48f;
    out.w = w;
    out.h = h;
    out.rgba.assign(static_cast<size_t>(w) * h * 4, 0);
    for (int32_t y = 0; y < h; y++) {
        const int32_t yu = std::max(0, y - 1);
        const int32_t yd = std::min(h - 1, y + 1);
        for (int32_t x = 0; x < w; x++) {
            const int32_t xl = std::max(0, x - 1);
            const int32_t xr = std::min(w - 1, x + 1);
            const size_t i = static_cast<size_t>(y) * w + x;
            const float alpha = 1.0f - std::exp(-2.2f * d[i]);
            if (alpha < 0.004f) {
                continue;
            }
            const float gx =
                (soft[static_cast<size_t>(y) * w + xr] - soft[static_cast<size_t>(y) * w + xl]) *
                0.5f;
            const float gy =
                (soft[static_cast<size_t>(yd) * w + x] - soft[static_cast<size_t>(yu) * w + x]) *
                0.5f;
            // The surface faces away from where the density grows.
            const float nx = -gx * 5.0f;
            const float ny = -gy * 5.0f;
            const float inv = 1.0f / std::sqrt(nx * nx + ny * ny + 1.0f);
            const float diffuse = std::clamp((nx * lx + ny * ly + lz) * inv, 0.0f, 1.0f);
            const float lit = std::exp(-0.35f * od[i]);
            const float base = static_cast<float>(y) / static_cast<float>(h);
            const float s = 0.30f + 0.50f * diffuse + 0.30f * lit - 0.25f * base * base +
                            0.25f * std::clamp(1.0f - od[i] / 2.0f, 0.0f, 1.0f);
            const Rgb c = tower_rgb(s);
            uint8_t* p = &out.rgba[i * 4];
            p[0] = static_cast<uint8_t>(c.r * alpha + 0.5f);
            p[1] = static_cast<uint8_t>(c.g * alpha + 0.5f);
            p[2] = static_cast<uint8_t>(c.b * alpha + 0.5f);
            p[3] = static_cast<uint8_t>(alpha * 255.0f + 0.5f);
        }
    }
}

void make_tower(std::minstd_rand& rng, HighFlyTexture& out) {
    const int32_t w = 256;
    const int32_t h = 300;
    std::vector<float> d(static_cast<size_t>(w) * h, 0.0f);
    const float cx = static_cast<float>(w) / 2.0f;
    // A column of billows narrowing upward to a domed crown, each tier a main blob with
    // smaller lobes bulging out round its upper half.
    float drift = 0.0f;
    for (int i = 0; i < 12; i++) {
        const float t = static_cast<float>(i) / 11.0f; // 0 base .. 1 crown
        const float y = static_cast<float>(h) - 45.0f - t * static_cast<float>(h - 110);
        const float rad = 60.0f * (1.0f - 0.35f * t) * uniform(rng, 0.85f, 1.1f);
        drift += uniform(rng, -8.0f, 8.0f);
        const float xo = cx + std::clamp(drift, -20.0f, 20.0f);
        add_blob(d, w, h, xo, y, rad * 0.75f, false, 1.2f);
        const int lobes = 4 + static_cast<int>(unit_random(rng) * 3.0f);
        for (int k = 0; k < lobes; k++) {
            const float a = uniform(rng, PI * 1.0f, PI * 2.0f);
            add_blob(d, w, h, xo + std::cos(a) * rad * 0.7f, y + std::sin(a) * rad * 0.5f,
                     rad * uniform(rng, 0.30f, 0.45f), false, 0.9f);
        }
    }
    // The crown: a dome with a few lobes of its own.
    const float crown_y = 70.0f;
    add_blob(d, w, h, cx + drift * 0.5f, crown_y, 34.0f, false, 1.1f);
    for (int k = 0; k < 5; k++) {
        const float a = uniform(rng, PI * 1.05f, PI * 1.95f);
        add_blob(d, w, h, cx + drift * 0.5f + std::cos(a) * 30.0f, crown_y + std::sin(a) * 18.0f,
                 uniform(rng, 12.0f, 18.0f), false, 0.9f);
    }
    for (int32_t y = h - 50; y < h; y++) {
        const float add = 2.0f * static_cast<float>(y - (h - 50)) / 49.0f;
        for (int32_t x = 0; x < w; x++) {
            d[static_cast<size_t>(y) * w + x] += add;
        }
    }
    std::vector<float> wisp;
    fbm(rng, h, w, false, {{8, 1.0f}, {16, 0.6f}, {32, 0.35f}, {64, 0.2f}}, wisp);
    for (int32_t y = 0; y < h; y++) {
        for (int32_t x = 0; x < w; x++) {
            const size_t i = static_cast<size_t>(y) * w + x;
            const float xn = std::fabs(static_cast<float>(x) - cx) / cx;
            // Torn edges, and sides that fade so the base melts into the deck.
            d[i] *= std::clamp(0.85f + 0.2f * wisp[i], 0.0f, 2.0f) *
                    std::clamp(1.6f - xn * xn * 1.6f, 0.0f, 1.0f);
            const float dc = std::clamp(d[i], 0.0f, 1.0f);
            d[i] = std::max(d[i] + 0.25f * wisp[i] * dc * (1.0f - dc), 0.0f);
        }
    }
    shade_tower(d, w, h, out);
}

} // namespace

size_t HighFlyTexture::heap_bytes() const {
    return rgba.capacity();
}

// ---------- Field ----------

void build_highfly_field(std::minstd_rand& rng, HighFlyField& out) {
    std::vector<float> hf(static_cast<size_t>(N) * N, 0.0f);
    std::vector<float> tmp;
    const auto splat = [&hf](float cx, float cy, float r, float base) {
        const auto x0 = static_cast<int32_t>(std::floor(cx - r));
        const auto x1 = static_cast<int32_t>(std::ceil(cx + r));
        const auto y0 = static_cast<int32_t>(std::floor(cy - r));
        const auto y1 = static_cast<int32_t>(std::ceil(cy + r));
        const float r2 = r * r;
        for (int32_t yy = y0; yy <= y1; yy++) {
            const float dy = static_cast<float>(yy) - cy;
            const int32_t y = ((yy % N) + N) % N;
            for (int32_t xx = x0; xx <= x1; xx++) {
                const float dx = static_cast<float>(xx) - cx;
                const float d2 = dx * dx + dy * dy;
                if (d2 >= r2) {
                    continue;
                }
                const int32_t x = ((xx % N) + N) % N;
                float& h = hf[static_cast<size_t>(y) * N + x];
                h = std::max(h, base + std::sqrt(r2 - d2));
            }
        }
    };
    const auto height_at = [&hf](float cx, float cy) {
        const int32_t x = static_cast<int32_t>(cx) % N;
        const int32_t y = static_cast<int32_t>(cy) % N;
        return hf[static_cast<size_t>(y) * N + x];
    };
    const auto fN = static_cast<float>(N);
    // Big billows: sphere centres below the floor, so the caps are wide domes.
    for (int i = 0; i < 30; i++) {
        const float cx = uniform(rng, 0.0f, fN);
        const float cy = uniform(rng, 0.0f, fN);
        const float r = uniform(rng, 18.0f, 34.0f);
        splat(cx, cy, r, uniform(rng, -16.0f, 8.0f));
    }
    // Lumps riding on the billows.
    for (int i = 0; i < 170; i++) {
        const float cx = uniform(rng, 0.0f, fN);
        const float cy = uniform(rng, 0.0f, fN);
        const float r = uniform(rng, 8.0f, 16.0f);
        splat(cx, cy, r, height_at(cx, cy) - r * uniform(rng, 0.55f, 0.85f));
    }
    // Cauliflower.
    for (int i = 0; i < 1500; i++) {
        const float cx = uniform(rng, 0.0f, fN);
        const float cy = uniform(rng, 0.0f, fN);
        const float r = uniform(rng, 2.5f, 5.5f);
        splat(cx, cy, r, height_at(cx, cy) - r * uniform(rng, 0.5f, 0.8f));
    }
    // Noise, stronger where the cloud is thicker.
    std::vector<float> fb(hf.size(), 0.0f);
    std::vector<float> g(hf.size());
    for (const auto& [cells, amp] :
         {std::pair<int32_t, float>{16, 1.0f}, {32, 0.5f}, {64, 0.25f}}) {
        std::vector<float> coarse(static_cast<size_t>(cells) * cells);
        for (float& v : coarse) {
            v = normal(rng);
        }
        const int32_t block = N / cells;
        for (int32_t y = 0; y < N; y++) {
            for (int32_t x = 0; x < N; x++) {
                g[static_cast<size_t>(y) * N + x] =
                    coarse[static_cast<size_t>(y / block) * cells + x / block];
            }
        }
        box_blur(g, N, N, std::max(1, N / cells / 2), true, true, tmp);
        for (size_t i = 0; i < fb.size(); i++) {
            fb[i] += amp * g[i];
        }
    }
    const float fb_sd = stddev(fb);
    for (size_t i = 0; i < hf.size(); i++) {
        hf[i] += 1.5f * fb[i] / fb_sd * std::clamp(hf[i] / 10.0f, 0.0f, 1.0f);
    }
    // Talus: sphere caps leave vertical walls the renderer would draw as curtains, so every
    // texel rises to within HIGHFLY_TALUS of its highest neighbour.
    for (int it = 0; it < 20; it++) {
        g = hf;
        for (int32_t y = 0; y < N; y++) {
            const int32_t yu = (y + N - 1) % N;
            const int32_t yd = (y + 1) % N;
            for (int32_t x = 0; x < N; x++) {
                const int32_t xl = (x + N - 1) % N;
                const int32_t xr = (x + 1) % N;
                const float nb = std::max(
                    {g[static_cast<size_t>(yu) * N + x], g[static_cast<size_t>(yd) * N + x],
                     g[static_cast<size_t>(y) * N + xl], g[static_cast<size_t>(y) * N + xr]});
                float& h = hf[static_cast<size_t>(y) * N + x];
                h = std::max(h, nb - HIGHFLY_TALUS);
            }
        }
    }
    box_blur(hf, N, N, 1, true, true, tmp);

    // Shade: Lambert from a sun above and a little beyond, ambient occlusion at two scales,
    // height, and a little noise.
    std::vector<float> blur6 = hf;
    box_blur(blur6, N, N, 6, true, true, tmp);
    std::vector<float> blur2 = hf;
    box_blur(blur2, N, N, 2, true, true, tmp);
    std::vector<float> noise(hf.size());
    for (float& v : noise) {
        v = normal(rng);
    }
    box_blur(noise, N, N, 1, true, true, tmp);
    const float noise_sd = stddev(noise);
    const auto [lo_it, hi_it] = std::minmax_element(hf.begin(), hf.end());
    const float lo = *lo_it;
    const float range = std::max(*hi_it - lo, 1e-6f);
    const float sun_len = std::sqrt(0.3f * 0.3f + 0.3f * 0.3f + 1.0f);
    const float sx = 0.3f / sun_len;
    const float sy = 0.3f / sun_len;
    const float sz = 1.0f / sun_len;
    out.height = hf;
    out.shade.assign(hf.size(), 0.0f);
    for (int32_t y = 0; y < N; y++) {
        const int32_t yu = (y + N - 1) % N;
        const int32_t yd = (y + 1) % N;
        for (int32_t x = 0; x < N; x++) {
            const size_t i = static_cast<size_t>(y) * N + x;
            const float gx = (hf[static_cast<size_t>(y) * N + (x + 1) % N] -
                              hf[static_cast<size_t>(y) * N + (x + N - 1) % N]) *
                             0.5f;
            const float gy =
                (hf[static_cast<size_t>(yd) * N + x] - hf[static_cast<size_t>(yu) * N + x]) * 0.5f;
            const float inv = 1.0f / std::sqrt(gx * gx + gy * gy + 1.0f);
            const float lam = std::clamp((-gx * sx - gy * sy + sz) * inv, 0.0f, 1.0f);
            const float ao = std::clamp(0.5f + (hf[i] - blur6[i]) / 10.0f, 0.0f, 1.0f);
            const float ao2 = std::clamp(0.5f + (hf[i] - blur2[i]) / 3.0f, 0.0f, 1.0f);
            const float hnorm = (hf[i] - lo) / range;
            const float s = -0.05f + 0.40f * lam + 0.75f * ao + 0.15f * hnorm +
                            0.03f * noise[i] / noise_sd + 0.5f * (ao2 - 0.5f);
            out.shade[i] = std::clamp(s, 0.0f, 1.0f);
        }
    }
}

float highfly_whiteout(float since_start_s) {
    if (since_start_s <= 0.0f) {
        return 0.0f;
    }
    if (since_start_s < HIGHFLY_FLY_IN_S) {
        return smooth01(since_start_s / HIGHFLY_FLY_IN_S);
    }
    const float hold_end = HIGHFLY_FLY_IN_S + HIGHFLY_FLY_HOLD_S;
    if (since_start_s < hold_end) {
        return 1.0f;
    }
    return 1.0f - smooth01((since_start_s - hold_end) / HIGHFLY_FLY_OUT_S);
}

Rgb highfly_inside(Rgb scene, float amt) {
    return lerp(scene, INSIDE, std::clamp(amt, 0.0f, 1.0f) * HIGHFLY_WHITEOUT_MAX);
}

float highfly_layer_visibility(float deck_z, float layer_z) {
    if (!std::isfinite(deck_z)) {
        return 1.0f;
    }
    return std::clamp((deck_z - layer_z) / (0.25f * layer_z) + 0.5f, 0.0f, 1.0f);
}

// ---------- Clock layout ----------

HighFlyClockLayout layout_highfly_clock(int32_t w, int32_t h, int32_t dx, int32_t dy) {
    const float s = std::min(static_cast<float>(h) / 480.0f, static_cast<float>(w) / 800.0f);
    HighFlyClockLayout l;
    const float path_h = STROKE_DIGIT_PATH_H + DIGIT_STROKE;
    l.digit_unit = std::max(4, static_cast<int32_t>(std::lround(110.0f * s / path_h)));
    const auto u = static_cast<float>(l.digit_unit);
    const auto px = [](float v) { return static_cast<int32_t>(std::lround(v)); };
    const int32_t gw = px((1.0f + DIGIT_STROKE) * u);
    const int32_t gh = px(path_h * u);
    const int32_t gap = std::max(1, px(0.15f * u));
    const int32_t colon_w = std::max(2, px(0.45f * u));
    const int32_t total = 4 * gw + 4 * gap + colon_w;
    const int32_t y2 = px(0.60f * static_cast<float>(h)) + dy;
    const int32_t y1 = y2 - gh + 1;
    int32_t x = w / 2 - total / 2 + dx;
    for (size_t i = 0; i < 5; i++) {
        const int32_t width = i == 2 ? colon_w : gw;
        l.digits[i] = {x, y1, x + width - 1, y2};
        x += width + gap;
    }
    l.second_unit = std::max(2, px(0.35f * u));
    const auto su = static_cast<float>(l.second_unit);
    const int32_t sgw = px((1.0f + DIGIT_STROKE) * su);
    const int32_t sgh = px(path_h * su);
    const int32_t sgap = std::max(1, px(0.15f * su));
    const int32_t sx = l.digits[4].x2 + 1 + px(0.15f * u);
    l.seconds[0] = {sx, y1, sx + sgw - 1, y1 + sgh - 1};
    l.seconds[1] = {sx + sgw + sgap, y1, sx + 2 * sgw + sgap - 1, y1 + sgh - 1};
    const int32_t my = l.seconds[0].y2 + 1 + px(0.1f * u);
    l.meridiem = {sx, my, std::max(l.seconds[1].x2, sx + px(1.2f * u)), my + px(0.3f * gh) - 1};
    // The date block sits in the sky beside the numerals' upper half, clear of the clouds.
    const int32_t line_h = std::max(10, px(0.055f * static_cast<float>(h)));
    const int32_t lx1 = px(0.05f * static_cast<float>(w)) + dx;
    const int32_t lx2 =
        std::min(px(0.30f * static_cast<float>(w)) + dx, l.digits[0].x1 - px(0.1f * u));
    const int32_t date_y = y1 + px(0.10f * static_cast<float>(gh));
    l.date_line = {lx1, date_y, lx2, date_y + line_h - 1};
    l.day_line = {lx1, date_y + line_h, lx2, date_y + 2 * line_h - 1};
    l.bounds = {};
    for (const DirtyRect& d : l.digits) {
        l.bounds.add(d);
    }
    for (const DirtyRect& d : l.seconds) {
        l.bounds.add(d);
    }
    l.bounds.add(l.meridiem);
    l.bounds.add(l.date_line);
    l.bounds.add(l.day_line);
    return l;
}

namespace {

void highfly_drift_range(int32_t w, int32_t h, int32_t& max_dx, int32_t& max_dy) {
    const HighFlyClockLayout l = layout_highfly_clock(w, h, 0, 0);
    const int32_t room_x = std::min(l.bounds.x1, w - 1 - l.bounds.x2);
    const int32_t room_y = std::min(l.bounds.y1, h - 1 - l.bounds.y2);
    max_dx = std::max(0, std::min(room_x, std::max(4, w / 25)));
    max_dy = std::max(0, std::min(room_y, std::max(4, h / 25)));
}

bool same_face(const ClockReading& a, const ClockReading& b) {
    return a.time == b.time && a.use_24h == b.use_24h;
}

} // namespace

// ---------- HighFlySim ----------

/// The deck: the baked field scrolling toward the camera, shaded through per-slice tables.
class HighFlySim::Scene : public HeightfieldScene {
  public:
    explicit Scene(const HighFlySim& sim) : sim_(sim) {}

    void sample_slice(size_t index, const HeightfieldSlice& slice, float* heights,
                      Rgb* colors) override {
        const HighFlySim& s = sim_;
        const float* hf = s.field_.height.data();
        const float* sh = s.field_.shade.data();
        const Rgb* lut = &s.slice_lut_[index * 256];
        const float wy = (-s.t_ * HIGHFLY_SPEED - slice.z) * HIGHFLY_TEXELS_PER_UNIT;
        const float wy_floor = std::floor(wy);
        const float fv = wy - wy_floor;
        const int32_t v0 = static_cast<int32_t>(static_cast<int64_t>(wy_floor) & (N - 1));
        const int32_t v1 = (v0 + 1) & (N - 1);
        const float* row0h = hf + static_cast<size_t>(v0) * N;
        const float* row1h = hf + static_cast<size_t>(v1) * N;
        const float* row0s = sh + static_cast<size_t>(v0) * N;
        const float* row1s = sh + static_cast<size_t>(v1) * N;
        const float inv_xs = 1.0f / HIGHFLY_TEXELS_PER_UNIT;
        for (int32_t c = 0; c < s.iw_; c++) {
            const float wx = (FIELD_X0 + slice.x_left + static_cast<float>(c) * slice.x_step) *
                             HIGHFLY_TEXELS_PER_UNIT;
            const float wx_floor = std::floor(wx);
            const float fu = wx - wx_floor;
            const int32_t u0 = static_cast<int32_t>(static_cast<int64_t>(wx_floor) & (N - 1));
            const int32_t u1 = (u0 + 1) & (N - 1);
            const float h0 = row0h[u0] + (row0h[u1] - row0h[u0]) * fu;
            const float h1 = row1h[u0] + (row1h[u1] - row1h[u0]) * fu;
            heights[c] = (h0 + (h1 - h0) * fv) * inv_xs;
            const float s0 = row0s[u0] + (row0s[u1] - row0s[u0]) * fu;
            const float s1 = row1s[u0] + (row1s[u1] - row1s[u0]) * fu;
            const float shade = s0 + (s1 - s0) * fv;
            colors[c] = lut[static_cast<size_t>(std::clamp(shade, 0.0f, 1.0f) * 255.0f + 0.5f)];
        }
    }

  private:
    const HighFlySim& sim_;
};

void HighFlySim::init(const FrameTarget& frame, std::minstd_rand& rng, size_t level,
                      const ClockReading& reading) {
    w_ = static_cast<int32_t>(frame.w);
    h_ = static_cast<int32_t>(frame.h);
    t_ = 0.0f;
    elapsed_ms_ = 0;
    build_highfly_field(rng, field_);
    make_band(rng, band_);
    for (HighFlyTexture& layer : layers_) {
        make_layer(rng, layer);
    }
    for (HighFlyTexture& tower : towers_tex_) {
        make_tower(rng, tower);
    }
    std::vector<float> wisp;
    fbm(rng, 64, 128, true, {{8, 1.0f}, {32, 0.6f}}, wisp);
    for (float& v : wisp) {
        v = std::clamp(v * 0.6f, 0.0f, 1.0f);
    }
    std::vector<float> tmp;
    box_blur(wisp, 128, 64, 3, false, true, tmp);
    wisp_ = wisp;

    towers_ = {};
    tower_entries_.clear();
    tower_entries_.reserve(HISTORY_CAP);
    fly_starts_.clear();
    fly_starts_.reserve(HISTORY_CAP);
    boxes_.clear();
    boxes_.reserve(BOX_CAP);
    last_boxes_.clear();
    last_boxes_.reserve(BOX_CAP);
    next_variant_ = 0;
    next_left_ = unit_random(rng) < 0.5f;
    // One tower already up when the run starts, part way in.
    spawn_tower(towers_[0], -uniform(rng, 7.0f, 17.0f), rng);
    next_tower_at_ = uniform(rng, 5.0f, 9.0f);
    next_tower_s_ = next_tower_at_;
    fly_start_s_ = uniform(rng, HIGHFLY_FLY_MIN_S, HIGHFLY_FLY_MAX_S);
    next_fly_s_ = fly_start_s_;

    level_ = 0;
    apply_view();
    level_ = std::min(level, HIGHFLY_LEVEL_COUNT - 1);
    apply_view();
    drift_x_ = 0;
    drift_y_ = 0;
    update_clock_layout();
    build_stroke_digits(clock_.digit_unit, DIGIT_STROKE, digits_);
    build_stroke_digits(clock_.second_unit, DIGIT_STROKE, second_digits_);

    FrameTarget out = frame;
    paint_all(out, reading);
    shown_ = reading;
}

void HighFlySim::apply_view() {
    const HighFlyLevel& lv = HIGHFLY_LEVELS[level_];
    scale_ = std::max<int32_t>(1, lv.scale);
    iw_ = (w_ + scale_ - 1) / scale_;
    ih_ = (h_ + scale_ - 1) / scale_;
    horizon_ = HORIZON * static_cast<float>(ih_);
    focal_x_ = (static_cast<float>(iw_) / 2.0f) / FOV;
    focal_y_ = FOCAL_Y_AT_240 * static_cast<float>(ih_) / 240.0f;

    HeightfieldView v;
    v.w = iw_;
    v.h = ih_;
    v.horizon_row = horizon_;
    v.focal_x = focal_x_;
    v.focal_y = focal_y_;
    v.cam_h = CAM_H;
    v.z_near = Z_NEAR;
    v.z_far = Z_FAR;
    v.dz_grow = DZ_GROW;
    v.slices = lv.slices;
    v.sink_depth = SINK;
    v.edge = HeightfieldEdge::SOFT_UNDER;
    v.feather_px = FEATHER_AT_240 * static_cast<float>(ih_) / 240.0f;
    v.span_gradient = true;
    v.first_slice_fills = true;
    v.below_color = C_LIT;
    v.fog = FOG;
    renderer_.set_view(v);

    slice_lut_.resize(renderer_.slices().size() * 256);
    for (size_t s = 0; s < renderer_.slices().size(); s++) {
        const HeightfieldSlice& slice = renderer_.slices()[s];
        const float near = NEAR_K * std::clamp(1.0f - slice.z / NEAR_WHITE, 0.0f, 1.0f);
        for (int32_t i = 0; i < 256; i++) {
            Rgb c = shade_to_rgb(static_cast<float>(i) / 255.0f);
            c = lerp(c, C_LIT, near);
            c = lerp(c, C_FOG, slice.fog);
            slice_lut_[s * 256 + i] = c;
        }
    }
    sky_.resize(static_cast<size_t>(iw_) * ih_);
    frame_.resize(static_cast<size_t>(iw_) * ih_);
    polar_.resize(static_cast<size_t>(iw_) * ih_ * 3);
    col_u0_.resize(static_cast<size_t>(iw_));
    col_fu_.resize(static_cast<size_t>(iw_));
    build_sky();
    build_polar();

    // The far band's top: the first row any of its texels reaches with real cover.
    Billboard band;
    band.tex = &band_;
    band.z = BAND_Z;
    band.q = BAND_Q;
    band.hb = BAND_HB;
    band.wrap = true;
    int32_t first_row = band_.h;
    for (int32_t y = 0; y < band_.h && first_row == band_.h; y++) {
        for (int32_t x = 0; x < band_.w; x++) {
            if (band_.rgba[(static_cast<size_t>(y) * band_.w + x) * 4 + 3] > 2) {
                first_row = y;
                break;
            }
        }
    }
    const float top =
        horizon_ + (CAM_H - (BAND_HB + static_cast<float>(band_.h) * BAND_Q)) * focal_y_ / BAND_Z;
    const float sy = BAND_Q * focal_y_ / BAND_Z;
    band_tex_top_ = std::max(0, static_cast<int32_t>(std::floor(top + first_row * sy)) - 1);
    band_top_ = band_tex_top_;
    painted_ = false;
}

void HighFlySim::build_sky() {
    const auto W = static_cast<float>(iw_);
    const auto H = static_cast<float>(ih_);
    const float cx = W * 0.5f;
    const float cy = -H * 0.12f;
    constexpr Rgb TOP{40, 128, 180};
    constexpr Rgb BOTTOM{12, 78, 124};
    constexpr Rgb GLOW{215, 242, 252};
    struct Shaft {
        float a0, width, strength;
    };
    constexpr Shaft SHAFTS[] = {{-0.30f, 0.17f, 0.085f},
                                {-0.02f, 0.22f, 0.10f},
                                {0.27f, 0.19f, 0.085f},
                                {0.55f, 0.16f, 0.05f}};
    for (int32_t y = 0; y < ih_; y++) {
        const float fy = static_cast<float>(y);
        const float t = std::pow(std::clamp(fy / (H * HORIZON), 0.0f, 1.0f), 0.8f);
        const Rgb base = lerp(TOP, BOTTOM, t);
        const float fall = std::pow(std::clamp(1.0f - (fy - cy) / (H * 0.8f), 0.0f, 1.0f), 1.6f);
        for (int32_t x = 0; x < iw_; x++) {
            const float fx = static_cast<float>(x);
            const float dx = (fx - cx) / W;
            const float dy = (fy - cy) / H;
            const float d = std::sqrt(dx * dx + 1.6f * dy * dy);
            const float glow = 0.8f * std::exp(-(d / 0.22f) * (d / 0.22f)) +
                               0.3f * std::exp(-(d / 0.55f) * (d / 0.55f));
            Rgb c = lerp(base, GLOW, std::min(glow, 1.0f));
            const float ang = std::atan2(fx - cx, fy - cy);
            float shafts = 0.0f;
            for (const Shaft& sh : SHAFTS) {
                const float q = (ang - sh.a0) / sh.width;
                shafts += sh.strength * std::exp(-q * q);
            }
            c = lerp(c, GLOW, std::min(shafts * fall, 1.0f));
            sky_[static_cast<size_t>(y) * iw_ + x] = c;
        }
    }
}

void HighFlySim::build_polar() {
    const auto W = static_cast<float>(iw_);
    const auto H = static_cast<float>(ih_);
    for (int32_t y = 0; y < ih_; y++) {
        for (int32_t x = 0; x < iw_; x++) {
            const float dx = (static_cast<float>(x) - W / 2.0f) / (W / 2.0f);
            const float dy = (static_cast<float>(y) - H * 0.5f) / (H / 2.0f);
            const float r = std::sqrt(dx * dx + dy * dy) + 1e-3f;
            const float lr = std::log(r) * 40.0f;                            // about -276 to 14
            const float ang = (std::atan2(dy, dx) / TWO_PI + 0.5f) * 128.0f; // 0 to 128
            const size_t i = (static_cast<size_t>(y) * iw_ + x) * 3;
            polar_[i] = static_cast<uint16_t>(std::clamp((lr + 300.0f) * 64.0f, 0.0f, 65535.0f));
            polar_[i + 1] = static_cast<uint16_t>(std::clamp(ang * 256.0f, 0.0f, 65535.0f));
            polar_[i + 2] = static_cast<uint16_t>(std::clamp(r * 1.2f, 0.0f, 1.0f) * 65535.0f);
        }
    }
}

HighFlySim::Billboard HighFlySim::tower_billboard(const Tower& t) const {
    const HighFlyTexture& tex = towers_tex_[t.variant];
    Billboard b;
    b.tex = &tex;
    b.z = t.z;
    b.q = TOWER_Q * t.size;
    b.xw0 = t.x + t.drift * (t_ - t.entered_s) - static_cast<float>(tex.w) * b.q / 2.0f;
    b.hb = TOWER_HB;
    b.alpha = std::clamp((TOWER_Z_ENTER - t.z) / 50.0f, 0.0f, 1.0f) *
              std::clamp((t.z - TOWER_Z_LEAVE) / (TOWER_FADE_FROM - TOWER_Z_LEAVE), 0.0f, 1.0f);
    b.fogmul = TOWER_FOGMUL;
    return b;
}

DirtyRect HighFlySim::tower_box(size_t i) const {
    if (i >= towers_.size() || !towers_[i].live) {
        return {};
    }
    return billboard_box(tower_billboard(towers_[i]));
}

int HighFlySim::tower_variant(size_t i) const {
    if (i >= towers_.size() || !towers_[i].live) {
        return -1;
    }
    return towers_[i].variant;
}

float HighFlySim::fly_since_s() const {
    const float since = t_ - fly_start_s_;
    if (since < 0.0f || since > HIGHFLY_FLY_IN_S + HIGHFLY_FLY_HOLD_S + HIGHFLY_FLY_OUT_S) {
        return -1.0f;
    }
    return since;
}

size_t HighFlySim::towers_visible() const {
    size_t n = 0;
    for (const Tower& t : towers_) {
        n += t.live ? 1 : 0;
    }
    return n;
}

void HighFlySim::spawn_tower(Tower& slot, float entered_s, std::minstd_rand& rng) {
    const float side = next_left_ ? -1.0f : 1.0f;
    slot.entered_s = entered_s;
    slot.z = TOWER_Z_ENTER - (t_ - entered_s) * HIGHFLY_SPEED;
    slot.x = side * uniform(rng, TOWER_SIDE_MIN, TOWER_SIDE_MAX) * TOWER_Z_ENTER * FOV;
    slot.drift = side * uniform(rng, TOWER_WIND_MIN, TOWER_WIND_MAX);
    slot.size = uniform(rng, 0.95f, 1.2f);
    slot.variant = next_variant_;
    slot.live = true;
    next_variant_ = static_cast<uint8_t>((next_variant_ + 1) % HIGHFLY_TOWER_VARIANTS);
    next_left_ = !next_left_;
}

void HighFlySim::update_schedule(std::minstd_rand& rng) {
    // Towers come on at the deck's speed and leave once faded near the camera or gone off
    // the side.
    for (Tower& t : towers_) {
        if (!t.live) {
            continue;
        }
        t.z = TOWER_Z_ENTER - (t_ - t.entered_s) * HIGHFLY_SPEED;
        Billboard b = tower_billboard(t);
        b.alpha = 1.0f; // off screen is a matter of where it is, not how faded
        if (t.z < TOWER_Z_LEAVE || billboard_box(b).empty()) {
            t.live = false;
        }
    }
    while (t_ >= next_tower_at_) {
        Tower* slot = nullptr;
        for (Tower& t : towers_) {
            if (!t.live) {
                slot = &t;
                break;
            }
        }
        if (slot == nullptr) {
            // Both slots up: the nearer one goes to make room.
            slot = towers_[0].z < towers_[1].z ? &towers_[0] : &towers_[1];
        }
        spawn_tower(*slot, next_tower_at_, rng);
        if (tower_entries_.size() < tower_entries_.capacity()) {
            tower_entries_.push_back(next_tower_at_);
        }
        next_tower_at_ += uniform(rng, HIGHFLY_TOWER_MIN_S, HIGHFLY_TOWER_MAX_S);
    }
    next_tower_s_ = next_tower_at_ - t_;

    const float fly_end = HIGHFLY_FLY_IN_S + HIGHFLY_FLY_HOLD_S + HIGHFLY_FLY_OUT_S;
    if (t_ - fly_start_s_ > fly_end) {
        if (fly_starts_.size() < fly_starts_.capacity()) {
            fly_starts_.push_back(fly_start_s_);
        }
        fly_start_s_ += uniform(rng, HIGHFLY_FLY_MIN_S, HIGHFLY_FLY_MAX_S);
    }
    next_fly_s_ = fly_start_s_ - t_;
}

size_t HighFlySim::collect_billboards(Billboard* out, size_t cap) const {
    const HighFlyLevel& lv = HIGHFLY_LEVELS[level_];
    size_t n = 0;
    const float span = LAYER_ZFAR - LAYER_ZNEAR;
    const size_t layers = std::min<size_t>(lv.layers, HIGHFLY_LAYER_COUNT);
    for (size_t i = 0; i < layers && n < cap; i++) {
        const float travel =
            t_ * HIGHFLY_SPEED + static_cast<float>(i) * span / HIGHFLY_LAYER_COUNT;
        const float z = LAYER_ZFAR - std::fmod(travel, span);
        const auto cycle = static_cast<int64_t>(travel / span);
        const float fin = std::clamp((LAYER_ZFAR - z) / 30.0f, 0.0f, 1.0f);
        const float fout = std::clamp((z - LAYER_ZNEAR) / 22.0f, 0.0f, 1.0f);
        Billboard& b = out[n++];
        b.tex =
            &layers_[static_cast<size_t>((static_cast<int64_t>(i) + cycle) % HIGHFLY_LAYER_COUNT)];
        b.z = z;
        b.xw0 = static_cast<float>((static_cast<int64_t>(i) * 97 + cycle * 61) % 300) - 150.0f;
        b.hb = LAYER_HB;
        b.q = LAYER_Q;
        b.wrap = true;
        b.alpha = 0.85f * fin * fout;
    }
    if (lv.towers) {
        for (const Tower& t : towers_) {
            if (!t.live || n >= cap) {
                continue;
            }
            out[n++] = tower_billboard(t);
        }
    }
    // The cloud the camera is about to fly into.
    const float since = t_ - fly_start_s_;
    const float body_z = FLY_FACE - since * HIGHFLY_SPEED;
    if (body_z > 0.8f && body_z < 330.0f && n < cap) {
        const HighFlyTexture& tex = towers_tex_[0];
        Billboard& b = out[n++];
        b.tex = &tex;
        b.z = body_z;
        b.q = BODY_Q;
        b.xw0 = -static_cast<float>(tex.w) * BODY_Q / 2.0f;
        b.hb = CAM_H - static_cast<float>(tex.h) * BODY_Q * 0.6f;
        b.alpha = std::clamp((330.0f - body_z) / 50.0f, 0.0f, 1.0f) *
                  (1.0f - highfly_whiteout(since)) * 0.9f;
    }
    std::sort(out, out + n, [](const Billboard& a, const Billboard& b) { return a.z > b.z; });
    return n;
}

DirtyRect HighFlySim::billboard_box(const Billboard& b) const {
    if (b.tex == nullptr || b.z <= 0.5f || b.alpha <= 0.003f) {
        return {};
    }
    const float sx = b.q * focal_x_ / b.z;
    const float sy = b.q * focal_y_ / b.z;
    const float top =
        horizon_ + (CAM_H - (b.hb + static_cast<float>(b.tex->h) * b.q)) * focal_y_ / b.z;
    const float xl = static_cast<float>(iw_) / 2.0f + b.xw0 * focal_x_ / b.z;
    DirtyRect box;
    box.y1 = std::max(0, static_cast<int32_t>(std::floor(top)));
    box.y2 =
        std::min(ih_, static_cast<int32_t>(std::ceil(top + static_cast<float>(b.tex->h) * sy))) - 1;
    if (b.wrap) {
        box.x1 = 0;
        box.x2 = iw_ - 1;
    } else {
        box.x1 = std::max(0, static_cast<int32_t>(std::floor(xl)));
        box.x2 =
            std::min(iw_, static_cast<int32_t>(std::ceil(xl + static_cast<float>(b.tex->w) * sx))) -
            1;
    }
    return box;
}

void HighFlySim::draw_billboard(const Billboard& b) {
    const DirtyRect box = billboard_box(b);
    if (box.empty()) {
        return;
    }
    const HighFlyTexture& tex = *b.tex;
    const float sx = b.q * focal_x_ / b.z;
    const float sy = b.q * focal_y_ / b.z;
    const float top =
        horizon_ + (CAM_H - (b.hb + static_cast<float>(tex.h) * b.q)) * focal_y_ / b.z;
    const float xl = static_cast<float>(iw_) / 2.0f + b.xw0 * focal_x_ / b.z;
    const float nw = NEAR_K * std::clamp(1.0f - b.z / NEAR_WHITE, 0.0f, 1.0f);
    const float f = FOG.at(b.z) * b.fogmul;
    // Columns: texel and weight, or -1 outside the texture.
    for (int32_t x = box.x1; x <= box.x2; x++) {
        const float xs = (static_cast<float>(x) + 0.5f - xl) / sx - 0.5f;
        if (b.wrap) {
            const float fl = std::floor(xs);
            col_u0_[x] = static_cast<int32_t>(((static_cast<int64_t>(fl) % tex.w) + tex.w) % tex.w);
            col_fu_[x] = xs - fl;
        } else if (xs < -0.5f || xs > static_cast<float>(tex.w) - 0.5f) {
            col_u0_[x] = -1;
        } else {
            const float ux = std::clamp(xs, 0.0f, static_cast<float>(tex.w) - 1.001f);
            col_u0_[x] = static_cast<int32_t>(ux);
            col_fu_[x] = ux - static_cast<float>(col_u0_[x]);
        }
    }
    const std::vector<HeightfieldSlice>& slices = renderer_.slices();
    const uint8_t* rgba = tex.rgba.data();
    const auto alpha8 = b.alpha;
    for (int32_t y = box.y1; y <= box.y2; y++) {
        const float ys = (static_cast<float>(y) + 0.5f - top) / sy - 0.5f;
        if (!b.wrap && (ys < -0.5f || ys > static_cast<float>(tex.h) - 0.5f)) {
            continue;
        }
        const float vy = std::clamp(ys, 0.0f, static_cast<float>(tex.h) - 1.001f);
        const auto v0 = static_cast<int32_t>(vy);
        const float fv = vy - static_cast<float>(v0);
        const uint8_t* r0 = rgba + static_cast<size_t>(v0) * tex.w * 4;
        const uint8_t* r1 = r0 + static_cast<size_t>(tex.w) * 4;
        Rgb* out = &frame_[static_cast<size_t>(y) * iw_];
        for (int32_t x = box.x1; x <= box.x2; x++) {
            const int32_t u0 = col_u0_[x];
            if (u0 < 0) {
                continue;
            }
            const int32_t u1 = b.wrap ? (u0 + 1 == tex.w ? 0 : u0 + 1) : u0 + 1;
            const float fu = col_fu_[x];
            float s[4];
            for (int ch = 0; ch < 4; ch++) {
                const float top_v = r0[u0 * 4 + ch] + (r0[u1 * 4 + ch] - r0[u0 * 4 + ch]) * fu;
                const float bot_v = r1[u0 * 4 + ch] + (r1[u1 * 4 + ch] - r1[u0 * 4 + ch]) * fu;
                s[ch] = (top_v + (bot_v - top_v) * fv) * alpha8;
            }
            float a = s[3] / 255.0f;
            if (a <= 0.002f) {
                continue;
            }
            float rr = s[0];
            float gg = s[1];
            float bb = s[2];
            // Near clouds glow white; fog is premultiplied like the colour.
            rr += (C_LIT.r * a - rr) * nw;
            gg += (C_LIT.g * a - gg) * nw;
            bb += (C_LIT.b * a - bb) * nw;
            rr = rr * (1.0f - f) + C_FOG.r * a * f;
            gg = gg * (1.0f - f) + C_FOG.g * a * f;
            bb = bb * (1.0f - f) + C_FOG.b * a * f;
            if (b.depth_test) {
                const uint16_t depth = renderer_.depth_slice_at(x, y);
                const float deck_z = depth == HeightfieldRenderer::NO_DEPTH
                                         ? std::numeric_limits<float>::infinity()
                                         : slices[depth].z;
                const float vis = highfly_layer_visibility(deck_z, b.z);
                a *= vis;
                rr *= vis;
                gg *= vis;
                bb *= vis;
            }
            Rgb& o = out[x];
            const float keep = 1.0f - a;
            o = {static_cast<uint8_t>(std::clamp(rr + o.r * keep, 0.0f, 255.0f)),
                 static_cast<uint8_t>(std::clamp(gg + o.g * keep, 0.0f, 255.0f)),
                 static_cast<uint8_t>(std::clamp(bb + o.b * keep, 0.0f, 255.0f))};
        }
    }
}

DirtyRect HighFlySim::clock_box_internal() const {
    const DirtyRect& b = clock_.bounds;
    DirtyRect box{std::max(0, b.x1 / scale_ - 1), std::max(0, b.y1 / scale_ - 1),
                  std::min(iw_ - 1, b.x2 / scale_ + 1), std::min(ih_ - 1, b.y2 / scale_ + 1)};
    return box;
}

void HighFlySim::compose(int32_t from) {
    const auto copy_sky = [this](const DirtyRect& r) {
        for (int32_t y = std::max(0, r.y1); y <= std::min(ih_ - 1, r.y2); y++) {
            const size_t o = static_cast<size_t>(y) * iw_;
            std::copy(sky_.begin() + static_cast<ptrdiff_t>(o + std::max(0, r.x1)),
                      sky_.begin() + static_cast<ptrdiff_t>(o + std::min(iw_ - 1, r.x2) + 1),
                      frame_.begin() + static_cast<ptrdiff_t>(o + std::max(0, r.x1)));
        }
    };
    copy_sky({0, from, iw_ - 1, ih_ - 1});
    for (const DirtyRect& r : boxes_) {
        copy_sky({r.x1, r.y1, r.x2, std::min(r.y2, from - 1)});
    }

    Billboard band;
    band.tex = &band_;
    band.z = BAND_Z;
    band.q = BAND_Q;
    band.hb = BAND_HB;
    band.wrap = true;
    band.xw0 = -BAND_DRIFT * t_;
    band.depth_test = false;
    draw_billboard(band);

    Scene scene(*this);
    renderer_.render(scene);
    renderer_.resolve(frame_.data(), Rgb{}, from, ih_, frame_);

    Billboard boards[HIGHFLY_LAYER_COUNT + 3];
    const size_t n = collect_billboards(boards, std::size(boards));
    for (size_t i = 0; i < n; i++) {
        draw_billboard(boards[i]);
    }

    // A static white haze over the bottom rows: the reference's glowing foreground.
    const auto H = static_cast<float>(ih_);
    for (int32_t y = std::max(from, static_cast<int32_t>((1.0f - HAZE) * H)); y < ih_; y++) {
        const float t = std::pow(
            std::clamp((static_cast<float>(y) - H * (1.0f - HAZE)) / (H * HAZE), 0.0f, 1.0f), 1.5f);
        Rgb* row = &frame_[static_cast<size_t>(y) * iw_];
        for (int32_t x = 0; x < iw_; x++) {
            row[x] = lerp(row[x], C_LIT, t);
        }
    }

    const float since = t_ - fly_start_s_;
    const float amt = highfly_whiteout(since);
    if (amt > 0.0f) {
        const float shift = t_ * 120.0f;
        for (int32_t y = 0; y < ih_; y++) {
            Rgb* row = &frame_[static_cast<size_t>(y) * iw_];
            for (int32_t x = 0; x < iw_; x++) {
                const Rgb c = highfly_inside(row[x], amt);
                const size_t pi = (static_cast<size_t>(y) * iw_ + x) * 3;
                const float lr = static_cast<float>(polar_[pi]) / 64.0f - 300.0f - shift;
                const float ang = static_cast<float>(polar_[pi + 1]) / 256.0f;
                const float rf = static_cast<float>(polar_[pi + 2]) / 65535.0f;
                const float n_wisp = bilinear_sample(wisp_.data(), 128, 64, ang, lr, true);
                const float a = std::clamp(1.5f * n_wisp, 0.0f, 1.0f) * rf * amt * 0.7f;
                row[x] = lerp(c, C_LIT, a);
            }
        }
    }
}

void HighFlySim::draw_numerals(FrameTarget& frame, const ClockReading& reading) const {
    PixelWriter writer(frame);
    const auto glyph = [&](const StrokeDigitSet& set, const DirtyRect& box, int digit) {
        for (int32_t gy = 0; gy < set.h; gy++) {
            for (int32_t gx = 0; gx < set.w; gx++) {
                const uint8_t cov = set.at(digit, gx, gy);
                const int32_t x = box.x1 + gx;
                const int32_t y = box.y1 + gy;
                if (cov == 0 || !writer.contains(x, y)) {
                    continue;
                }
                const float a = NUMERAL_OPACITY * static_cast<float>(cov) / 255.0f;
                writer.put_dithered(x, y, lerp(writer.get(x, y), NUMERAL, a));
            }
        }
    };
    const int32_t hour = reading.time.hour;
    const int32_t minute = reading.time.minute;
    const int32_t second = reading.time.second;
    glyph(digits_, clock_.digits[0], hour / 10 % 10);
    glyph(digits_, clock_.digits[1], hour % 10);
    glyph(digits_, clock_.digits[3], minute / 10);
    glyph(digits_, clock_.digits[4], minute % 10);
    glyph(second_digits_, clock_.seconds[0], second / 10);
    glyph(second_digits_, clock_.seconds[1], second % 10);

    // The colon: two round dots.
    const DirtyRect& c = clock_.digits[2];
    const float r = std::max(1.0f, 0.05f * static_cast<float>(clock_.digit_unit));
    const float cx = static_cast<float>(c.x1 + c.x2 + 1) / 2.0f;
    for (const float share : {0.35f, 0.75f}) {
        const float cy = static_cast<float>(c.y1) + static_cast<float>(c.y2 - c.y1 + 1) * share;
        for (int32_t y = static_cast<int32_t>(cy - r - 1.0f);
             y <= static_cast<int32_t>(cy + r + 1.0f); y++) {
            for (int32_t x = static_cast<int32_t>(cx - r - 1.0f);
                 x <= static_cast<int32_t>(cx + r + 1.0f); x++) {
                if (!writer.contains(x, y)) {
                    continue;
                }
                const float d = std::hypot(static_cast<float>(x) + 0.5f - cx,
                                           static_cast<float>(y) + 0.5f - cy);
                const float cov = std::clamp(r - d + 0.5f, 0.0f, 1.0f);
                if (cov > 0.0f) {
                    writer.put_dithered(x, y,
                                        lerp(writer.get(x, y), NUMERAL, NUMERAL_OPACITY * cov));
                }
            }
        }
    }
}

void HighFlySim::update_clock_layout() {
    clock_ = layout_highfly_clock(w_, h_, drift_x_, drift_y_);
}

void HighFlySim::paint_all(FrameTarget& frame, const ClockReading& reading) {
    boxes_.clear();
    last_boxes_.clear();
    compose(0);
    upscale_frame(frame_, iw_, ih_, scale_, 0, ih_, frame);
    draw_numerals(frame, reading);
    painted_ = true;
    last_full_ = false;
}

void HighFlySim::request_level(size_t level) {
    const size_t next = std::min(level, HIGHFLY_LEVEL_COUNT - 1);
    if (next == level_) {
        return;
    }
    level_ = next;
    apply_view();
}

void HighFlySim::step(uint32_t dt_ms, FrameTarget& frame, std::minstd_rand& rng,
                      const ClockReading& reading, std::vector<DirtyRect>& dirty) {
    dirty.clear();
    elapsed_ms_ += dt_ms;
    const HighFlyLevel& lv = HIGHFLY_LEVELS[level_];
    const DirtyRect whole{0, 0, w_ - 1, h_ - 1};

    int32_t max_dx = 0;
    int32_t max_dy = 0;
    highfly_drift_range(w_, h_, max_dx, max_dy);
    int32_t dx = 0;
    int32_t dy = 0;
    clock_drift_at(elapsed_ms_, max_dx, max_dy, dx, dy);
    const bool moved = dx != drift_x_ || dy != drift_y_;
    const DirtyRect old_clock = clock_box_internal();
    if (moved) {
        drift_x_ = dx;
        drift_y_ = dy;
        update_clock_layout();
    }

    if (lv.still) {
        // The scene stands still: paint it once, then only the clock when it changes.
        if (!painted_) {
            paint_all(frame, reading);
            dirty.push_back(whole);
        } else if (moved || !same_face(reading, shown_)) {
            DirtyRect box = clock_box_internal();
            box.add(old_clock);
            const DirtyRect out = upscale_frame(frame_, iw_, ih_, scale_, box.y1, box.y2 + 1, frame,
                                                box.x1, box.x2 + 1);
            draw_numerals(frame, reading);
            if (!out.empty()) {
                dirty.push_back(out);
            }
        }
        shown_ = reading;
        return;
    }

    t_ += static_cast<float>(dt_ms) / 1000.0f;
    update_schedule(rng);
    const float since = t_ - fly_start_s_;
    const bool whiteout = highfly_whiteout(since) > 0.0f;
    const bool full = !painted_ || whiteout || last_full_;

    // Boxes above the band: the clock (where it is and where it was), the towers and the
    // approaching cloud, this frame and last.
    last_boxes_.swap(boxes_);
    boxes_.clear();
    boxes_.push_back(clock_box_internal());
    if (moved) {
        boxes_.push_back(old_clock);
    }
    Billboard boards[HIGHFLY_LAYER_COUNT + 3];
    const size_t n = collect_billboards(boards, std::size(boards));
    for (size_t i = 0; i < n && boxes_.size() < BOX_CAP; i++) {
        if (!boards[i].wrap) {
            const DirtyRect b = billboard_box(boards[i]);
            if (!b.empty()) {
                boxes_.push_back(b);
            }
        }
    }
    const size_t mine = boxes_.size();
    for (const DirtyRect& b : last_boxes_) {
        if (boxes_.size() < BOX_CAP) {
            boxes_.push_back(b);
        }
    }

    // The band: from the far billows' top or the deck's highest point last frame, less a few
    // rows for the deck rising, and down from wherever last frame's band began.
    int32_t top = band_tex_top_;
    if (painted_) {
        for (int32_t x = 0; x < iw_; x++) {
            top = std::min(top, static_cast<int32_t>(std::floor(renderer_.top_at(x))) - DECK_RISE);
        }
    }
    top = std::max(0, top);
    int32_t from = full ? 0 : std::min(top, band_top_);
    compose(from);

    const DirtyRect band = upscale_frame(frame_, iw_, ih_, scale_, from, ih_, frame);
    if (!band.empty()) {
        dirty.push_back(band);
    }
    if (!full) {
        for (const DirtyRect& b : boxes_) {
            if (b.y1 >= from) {
                continue;
            }
            const DirtyRect out = upscale_frame(frame_, iw_, ih_, scale_, b.y1,
                                                std::min(b.y2 + 1, from), frame, b.x1, b.x2 + 1);
            if (!out.empty()) {
                dirty.push_back(out);
            }
        }
    }
    draw_numerals(frame, reading);
    band_top_ = top;
    boxes_.resize(mine);
    painted_ = true;
    last_full_ = whiteout;
    if (full) {
        dirty.assign(1, whole);
    } else {
        merge_dirty_areas(dirty, SAVER_MAX_DIRTY_AREAS);
        int64_t covered = 0;
        for (const DirtyRect& b : dirty) {
            covered += b.area();
        }
        if (covers_whole_canvas(covered, w_, h_)) {
            dirty.assign(1, whole);
        }
    }
    shown_ = reading;
}

size_t HighFlySim::heap_bytes() const {
    size_t n = bytes_of(field_.height) + bytes_of(field_.shade) + renderer_.heap_bytes() +
               bytes_of(sky_) + bytes_of(frame_) + bytes_of(slice_lut_) + band_.heap_bytes() +
               bytes_of(wisp_) + bytes_of(polar_) + bytes_of(col_u0_) + bytes_of(col_fu_) +
               bytes_of(tower_entries_) + bytes_of(fly_starts_) + bytes_of(boxes_) +
               bytes_of(last_boxes_) + digits_.heap_bytes() + second_digits_.heap_bytes();
    for (const HighFlyTexture& t : layers_) {
        n += t.heap_bytes();
    }
    for (const HighFlyTexture& t : towers_tex_) {
        n += t.heap_bytes();
    }
    return n;
}

void HighFlySim::clear() {
    free_vector(field_.height);
    free_vector(field_.shade);
    renderer_.clear();
    free_vector(sky_);
    free_vector(frame_);
    free_vector(slice_lut_);
    free_vector(band_.rgba);
    for (HighFlyTexture& t : layers_) {
        free_vector(t.rgba);
    }
    for (HighFlyTexture& t : towers_tex_) {
        free_vector(t.rgba);
    }
    free_vector(wisp_);
    free_vector(polar_);
    free_vector(col_u0_);
    free_vector(col_fu_);
    free_vector(tower_entries_);
    free_vector(fly_starts_);
    free_vector(boxes_);
    free_vector(last_boxes_);
    digits_.clear();
    second_digits_.clear();
    towers_ = {};
    painted_ = false;
}

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
