// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver_heightfield.h"

#include "screensaver_buffer.h"
#include "screensaver_motion.h"
#include "screensaver_pixel_writer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace helix::ui {

namespace {

template <typename T> void resize_to(std::vector<T>& v, size_t n) {
    if (v.size() != n) {
        v.resize(n);
    }
}

uint8_t to_u8(float v) {
    return static_cast<uint8_t>(std::clamp(v, 0.0f, 255.0f) + 0.5f);
}

/// `c` scaled by `cov` / 255, rounded.
Rgb premultiply(Rgb c, uint32_t cov) {
    return {static_cast<uint8_t>((c.r * cov + 127u) / 255u),
            static_cast<uint8_t>((c.g * cov + 127u) / 255u),
            static_cast<uint8_t>((c.b * cov + 127u) / 255u)};
}

} // namespace

float HeightfieldFog::at(float z) const {
    const float span = end - start;
    const float t =
        span > 0.0f ? std::clamp((z - start) / span, 0.0f, 1.0f) : (z >= end ? 1.0f : 0.0f);
    const float u = falling ? 1.0f - t : t;
    if (u <= 0.0f) {
        return 0.0f;
    }
    return power == 1.0f ? u : std::pow(u, power);
}

void build_heightfield_slices(const HeightfieldView& view, std::vector<HeightfieldSlice>& out) {
    const size_t n = std::max<size_t>(1, view.slices);
    out.resize(n);
    const double depth = static_cast<double>(view.z_far) - view.z_near;
    const double grow = view.dz_grow;
    double dz = std::fabs(grow - 1.0) < 1e-9
                    ? depth / static_cast<double>(n)
                    : depth * (grow - 1.0) / (std::pow(grow, static_cast<double>(n)) - 1.0);
    double z = view.z_near;
    const float half_w = static_cast<float>(view.w) / 2.0f;
    const float focal_x = view.focal_x > 0.0f ? view.focal_x : 1.0f;
    for (size_t i = 0; i < n; i++) {
        HeightfieldSlice& s = out[i];
        s.z = static_cast<float>(z);
        s.inv_z = 1.0f / s.z;
        s.dz = static_cast<float>(dz);
        s.sink = view.sink_depth > 0.0f
                     ? screensaver::smoothstep01((s.z - view.z_near) / view.sink_depth)
                     : 1.0f;
        s.fog = view.fog.at(s.z);
        s.x_step = s.z / focal_x;
        s.x_left = (0.5f - half_w) * s.x_step;
        z += dz;
        dz *= grow;
    }
}

float bilinear_sample(const float* grid, int32_t w, int32_t h, float u, float v, bool wrap) {
    if (w <= 0 || h <= 0) {
        return 0.0f;
    }
    int32_t i0 = 0;
    int32_t i1 = 0;
    int32_t j0 = 0;
    int32_t j1 = 0;
    float fu = 0.0f;
    float fv = 0.0f;
    if (wrap) {
        const float u0 = std::floor(u);
        const float v0 = std::floor(v);
        fu = u - u0;
        fv = v - v0;
        i0 = static_cast<int32_t>(u0) % w;
        j0 = static_cast<int32_t>(v0) % h;
        i0 += i0 < 0 ? w : 0;
        j0 += j0 < 0 ? h : 0;
        i1 = i0 + 1 == w ? 0 : i0 + 1;
        j1 = j0 + 1 == h ? 0 : j0 + 1;
    } else {
        u = std::clamp(u, 0.0f, static_cast<float>(w - 1));
        v = std::clamp(v, 0.0f, static_cast<float>(h - 1));
        i0 = std::min(static_cast<int32_t>(u), std::max(0, w - 2));
        j0 = std::min(static_cast<int32_t>(v), std::max(0, h - 2));
        i1 = std::min(i0 + 1, w - 1);
        j1 = std::min(j0 + 1, h - 1);
        fu = u - static_cast<float>(i0);
        fv = v - static_cast<float>(j0);
    }
    const float a = grid[j0 * w + i0] + (grid[j0 * w + i1] - grid[j0 * w + i0]) * fu;
    const float b = grid[j1 * w + i0] + (grid[j1 * w + i1] - grid[j1 * w + i0]) * fu;
    return a + (b - a) * fv;
}

// ---------- HeightfieldRenderer ----------

void HeightfieldRenderer::init(const HeightfieldView& view) {
    set_view(view);
}

void HeightfieldRenderer::set_view(const HeightfieldView& view) {
    view_ = view;
    build_heightfield_slices(view_, slices_);
    const auto w = static_cast<size_t>(std::max(0, view_.w));
    const auto h = static_cast<size_t>(std::max(0, view_.h));
    resize_to(color_, w * h);
    resize_to(cover_, w * h);
    resize_to(depth_, w * h);
    resize_to(top_, w);
    resize_to(silhouette_, w);
    resize_to(silhouette_color_, w);
    resize_to(front_top_, w);
    resize_to(front_color_, w);
    resize_to(last_color_, w);
    resize_to(last_drew_, w);
    resize_to(heights_, w);
    resize_to(colors_, w);
    resize_to(edge_color_, w * 3);
    resize_to(edge_cover_, w * 3);
}

void HeightfieldRenderer::render(HeightfieldScene& scene) {
    const int32_t W = view_.w;
    const int32_t H = view_.h;
    samples_ = 0;
    span_pixels_ = 0;
    if (W <= 0 || H <= 0) {
        return;
    }
    std::fill(color_.begin(), color_.end(), Rgb{});
    std::fill(cover_.begin(), cover_.end(), uint8_t{0});
    std::fill(depth_.begin(), depth_.end(), NO_DEPTH);
    const auto Hf = static_cast<float>(H);
    std::fill(top_.begin(), top_.end(), Hf);
    std::fill(silhouette_.begin(), silhouette_.end(), Hf);
    std::fill(last_color_.begin(), last_color_.end(), view_.below_color);
    std::fill(last_drew_.begin(), last_drew_.end(), view_.first_slice_fills ? 1 : 0);

    const bool soft = view_.edge == HeightfieldEdge::SOFT_UNDER;
    const float feather = std::max(1.0f, view_.feather_px);
    const float inv_feather = 1.0f / feather;

    for (size_t s = 0; s < slices_.size(); s++) {
        const HeightfieldSlice& slice = slices_[s];
        scene.sample_slice(s, slice, heights_.data(), colors_.data());
        samples_ += static_cast<uint32_t>(W);
        const float k = view_.focal_y * slice.inv_z;
        const auto depth_mark = static_cast<uint16_t>(std::min<size_t>(s, NO_DEPTH - 1));
        for (int32_t x = 0; x < W; x++) {
            // Kept within a few screens so the row casts below stay defined.
            const float row = std::clamp(
                view_.horizon_row + (view_.cam_h - heights_[x] * slice.sink) * k, -Hf, 2.0f * Hf);
            const Rgb c = colors_[x];
            if (s == 0) {
                front_top_[x] = row;
                front_color_[x] = c;
            }
            if (row < silhouette_[x]) {
                silhouette_[x] = row;
                silhouette_color_[x] = c;
            }
            const float yb = top_[x];
            if (!(row < yb)) {
                last_drew_[x] = 0;
                continue;
            }
            if (s == 0 && !view_.first_slice_fills) {
                top_[x] = std::min(yb, std::ceil(row));
                last_drew_[x] = 1;
                last_color_[x] = c;
                continue;
            }

            const int32_t r_from = std::max(0, static_cast<int32_t>(std::ceil(row)));
            const int32_t r_to = std::min(H, static_cast<int32_t>(std::ceil(yb)));
            const bool grad = view_.span_gradient && last_drew_[x] != 0;
            const Rgb last = last_color_[x];
            const float inv_span = 1.0f / std::max(yb - row, 1.0f);
            Rgb* col = &color_[static_cast<size_t>(x) * H];
            uint8_t* cov = &cover_[static_cast<size_t>(x) * H];
            uint16_t* dep = &depth_[static_cast<size_t>(x) * H];
            for (int32_t r = r_from; r < r_to; r++) {
                Rgb pix = c;
                if (grad) {
                    const float g =
                        std::clamp((static_cast<float>(r) - row) * inv_span, 0.0f, 1.0f);
                    pix = {to_u8(c.r + (last.r - c.r) * g), to_u8(c.g + (last.g - c.g) * g),
                           to_u8(c.b + (last.b - c.b) * g)};
                }
                if (!soft) {
                    col[r] = pix;
                    cov[r] = 255;
                    if (dep[r] == NO_DEPTH) {
                        dep[r] = depth_mark;
                    }
                    continue;
                }
                const uint32_t have = cov[r];
                if (have >= 255) {
                    continue;
                }
                const float a =
                    std::clamp((static_cast<float>(r) + 0.5f - row) * inv_feather, 0.0f, 1.0f);
                const auto a8 = static_cast<uint32_t>(a * 255.0f + 0.5f);
                const uint32_t wcov = (255u - have) * a8; // 0 to 65025
                if (wcov == 0) {
                    continue;
                }
                const uint32_t now = std::min(255u, have + (wcov + 127u) / 255u);
                cov[r] = static_cast<uint8_t>(now);
                Rgb& dst = col[r];
                dst.r =
                    static_cast<uint8_t>(std::min(255u, dst.r + (pix.r * wcov + 32512u) / 65025u));
                dst.g =
                    static_cast<uint8_t>(std::min(255u, dst.g + (pix.g * wcov + 32512u) / 65025u));
                dst.b =
                    static_cast<uint8_t>(std::min(255u, dst.b + (pix.b * wcov + 32512u) / 65025u));
                if (have < 128 && now >= 128) {
                    dep[r] = depth_mark;
                }
            }
            span_pixels_ += static_cast<uint32_t>(std::max(0, r_to - r_from));
            top_[x] = soft ? std::min(yb, row + feather) : std::min(yb, std::ceil(row));
            last_drew_[x] = 1;
            last_color_[x] = c;
        }
    }

    if (!soft) {
        feather_silhouette();
    }
}

void HeightfieldRenderer::feather_silhouette() {
    const int32_t W = view_.w;
    const int32_t H = view_.h;
    // Fractional coverage on the pixel holding each column's silhouette.
    for (int32_t x = 0; x < W; x++) {
        const float sil = silhouette_[x];
        if (sil >= static_cast<float>(H) || sil < 0.0f) {
            continue;
        }
        const auto r0 = static_cast<int32_t>(std::floor(sil));
        const float frac = static_cast<float>(r0 + 1) - sil;
        const auto cov = static_cast<uint32_t>(frac * 255.0f + 0.5f);
        const size_t i = static_cast<size_t>(x) * H + r0;
        if (cov > cover_[i]) {
            cover_[i] = static_cast<uint8_t>(cov);
            color_[i] = premultiply(silhouette_color_[x], cov);
        }
    }
    // [1 2 1] / 4 across columns over the three rows around each silhouette, read from the
    // unsmoothed buffers before any is written back.
    const auto row_of = [this](int32_t x) {
        return static_cast<int32_t>(std::floor(std::max(silhouette_[x], 0.0f)));
    };
    for (int32_t x = 0; x < W; x++) {
        const int32_t r0 = row_of(x);
        const int32_t xl = std::max(0, x - 1);
        const int32_t xr = std::min(W - 1, x + 1);
        for (int32_t k = 0; k < 3; k++) {
            const int32_t r = r0 - 1 + k;
            const size_t e = static_cast<size_t>(x) * 3 + k;
            if (silhouette_[x] >= static_cast<float>(H) || r < 0 || r >= H) {
                edge_cover_[e] = 0;
                edge_color_[e] = {};
                continue;
            }
            const size_t il = static_cast<size_t>(xl) * H + r;
            const size_t ic = static_cast<size_t>(x) * H + r;
            const size_t ir = static_cast<size_t>(xr) * H + r;
            edge_cover_[e] =
                static_cast<uint8_t>((cover_[il] + 2u * cover_[ic] + cover_[ir] + 2u) / 4u);
            edge_color_[e] = {
                static_cast<uint8_t>((color_[il].r + 2u * color_[ic].r + color_[ir].r + 2u) / 4u),
                static_cast<uint8_t>((color_[il].g + 2u * color_[ic].g + color_[ir].g + 2u) / 4u),
                static_cast<uint8_t>((color_[il].b + 2u * color_[ic].b + color_[ir].b + 2u) / 4u)};
        }
    }
    for (int32_t x = 0; x < W; x++) {
        if (silhouette_[x] >= static_cast<float>(H)) {
            continue;
        }
        const int32_t r0 = row_of(x);
        for (int32_t k = 0; k < 3; k++) {
            const int32_t r = r0 - 1 + k;
            if (r < 0 || r >= H) {
                continue;
            }
            const size_t i = static_cast<size_t>(x) * H + r;
            const size_t e = static_cast<size_t>(x) * 3 + k;
            cover_[i] = edge_cover_[e];
            color_[i] = edge_color_[e];
        }
    }
}

Rgb HeightfieldRenderer::color_at(int32_t x, int32_t y) const {
    return color_[static_cast<size_t>(x) * view_.h + y];
}

uint8_t HeightfieldRenderer::coverage_at(int32_t x, int32_t y) const {
    return cover_[static_cast<size_t>(x) * view_.h + y];
}

uint16_t HeightfieldRenderer::depth_slice_at(int32_t x, int32_t y) const {
    return depth_[static_cast<size_t>(x) * view_.h + y];
}

float HeightfieldRenderer::top_at(int32_t x) const {
    return silhouette_[x];
}

float HeightfieldRenderer::front_top_at(int32_t x) const {
    return front_top_[x];
}

Rgb HeightfieldRenderer::front_color_at(int32_t x) const {
    return front_color_[x];
}

void HeightfieldRenderer::resolve(const Rgb* background, Rgb flat, int32_t y_from, int32_t y_to,
                                  std::vector<Rgb>& frame) const {
    const int32_t W = view_.w;
    const int32_t H = view_.h;
    y_from = std::max(0, y_from);
    y_to = std::min(H, y_to);
    for (int32_t x = 0; x < W; x++) {
        const Rgb* col = &color_[static_cast<size_t>(x) * H];
        const uint8_t* cov = &cover_[static_cast<size_t>(x) * H];
        for (int32_t y = y_from; y < y_to; y++) {
            const size_t o = static_cast<size_t>(y) * W + x;
            const uint32_t a = cov[y];
            const Rgb c = col[y];
            if (a >= 255) {
                frame[o] = c;
                continue;
            }
            const Rgb bg = background != nullptr ? background[o] : flat;
            const uint32_t rest = 255u - a;
            frame[o] = {static_cast<uint8_t>(std::min(255u, c.r + (bg.r * rest + 127u) / 255u)),
                        static_cast<uint8_t>(std::min(255u, c.g + (bg.g * rest + 127u) / 255u)),
                        static_cast<uint8_t>(std::min(255u, c.b + (bg.b * rest + 127u) / 255u))};
        }
    }
}

size_t HeightfieldRenderer::heap_bytes() const {
    return buffer_bytes(slices_) + buffer_bytes(color_) + buffer_bytes(cover_) +
           buffer_bytes(depth_) + buffer_bytes(top_) + buffer_bytes(silhouette_) +
           buffer_bytes(silhouette_color_) + buffer_bytes(front_top_) + buffer_bytes(front_color_) +
           buffer_bytes(last_color_) + buffer_bytes(last_drew_) + buffer_bytes(heights_) +
           buffer_bytes(colors_) + buffer_bytes(edge_color_) + buffer_bytes(edge_cover_);
}

void HeightfieldRenderer::clear() {
    release_buffer(slices_);
    release_buffer(color_);
    release_buffer(cover_);
    release_buffer(depth_);
    release_buffer(top_);
    release_buffer(silhouette_);
    release_buffer(silhouette_color_);
    release_buffer(front_top_);
    release_buffer(front_color_);
    release_buffer(last_color_);
    release_buffer(last_drew_);
    release_buffer(heights_);
    release_buffer(colors_);
    release_buffer(edge_color_);
    release_buffer(edge_cover_);
    samples_ = 0;
    span_pixels_ = 0;
}

// ---------- upscale ----------

namespace {

/// Writes one pixel, dithered at 16 bpp.
inline void write_pixel(uint8_t* row, int32_t x, int32_t y, PixelFormat format, uint32_t r,
                        uint32_t g, uint32_t b) {
    if (format == PixelFormat::RGB565) {
        const uint8_t t = BAYER_4X4[y & 3][x & 3];
        const auto v = static_cast<uint16_t>(dither_channel(static_cast<uint8_t>(r), 31, t) << 11 |
                                             dither_channel(static_cast<uint8_t>(g), 63, t) << 5 |
                                             dither_channel(static_cast<uint8_t>(b), 31, t));
        row[x * 2] = static_cast<uint8_t>(v & 0xFF);
        row[x * 2 + 1] = static_cast<uint8_t>(v >> 8);
        return;
    }
    uint8_t* p = row + static_cast<size_t>(x) * 4;
    p[0] = static_cast<uint8_t>(b);
    p[1] = static_cast<uint8_t>(g);
    p[2] = static_cast<uint8_t>(r);
    p[3] = 0xFF;
}

} // namespace

DirtyRect upscale_frame(const std::vector<Rgb>& frame, int32_t w, int32_t h, int32_t scale,
                        int32_t y_from, int32_t y_to, FrameTarget& out, int32_t x_from,
                        int32_t x_to) {
    scale = std::max(1, scale);
    y_from = std::max(0, y_from);
    y_to = std::min(h, y_to);
    x_from = std::max(0, x_from);
    x_to = std::min(w, x_to);
    const auto ow = static_cast<int32_t>(out.w);
    const auto oh = static_cast<int32_t>(out.h);
    const int32_t cols = std::min(x_to * scale, ow);
    const int32_t ox0 = std::min(x_from * scale, ow);
    const int32_t oy0 = std::min(y_from * scale, oh);
    const int32_t oy1 = std::min(y_to * scale, oh);
    if (cols <= ox0 || oy1 <= oy0) {
        return {};
    }

    for (int32_t oy = oy0; oy < oy1; oy++) {
        uint8_t* row = out.data + static_cast<size_t>(oy) * out.stride;
        const int32_t sy = oy / scale;
        const Rgb* a = &frame[static_cast<size_t>(sy) * w];
        if (scale != 2) {
            for (int32_t ox = ox0; ox < cols; ox++) {
                const Rgb c = a[ox / scale];
                write_pixel(row, ox, oy, out.format, c.r, c.g, c.b);
            }
            continue;
        }
        // Bilinear at weights 3/4 and 1/4: each output pixel leans to the neighbour on its
        // own side, rows clamped to the band being written.
        const int32_t sy2 = std::clamp((oy & 1) == 0 ? sy - 1 : sy + 1, y_from, y_to - 1);
        const Rgb* b = &frame[static_cast<size_t>(sy2) * w];
        const auto vert = [a, b](int32_t sx, uint32_t& r, uint32_t& g, uint32_t& bl) {
            r = 3u * a[sx].r + b[sx].r;
            g = 3u * a[sx].g + b[sx].g;
            bl = 3u * a[sx].b + b[sx].b;
        };
        uint32_t pr = 0, pg = 0, pb = 0;
        uint32_t cr = 0, cg = 0, cb = 0;
        uint32_t nr = 0, ng = 0, nb = 0;
        vert(x_from, cr, cg, cb);
        vert(std::max(0, x_from - 1), pr, pg, pb);
        for (int32_t sx = x_from; sx < x_to; sx++) {
            if (sx + 1 < w) {
                vert(sx + 1, nr, ng, nb);
            } else {
                nr = cr;
                ng = cg;
                nb = cb;
            }
            const int32_t ox = sx * 2;
            if (ox < cols) {
                write_pixel(row, ox, oy, out.format, (3u * cr + pr + 8u) / 16u,
                            (3u * cg + pg + 8u) / 16u, (3u * cb + pb + 8u) / 16u);
            }
            if (ox + 1 < cols) {
                write_pixel(row, ox + 1, oy, out.format, (3u * cr + nr + 8u) / 16u,
                            (3u * cg + ng + 8u) / 16u, (3u * cb + nb + 8u) / 16u);
            }
            pr = cr;
            pg = cg;
            pb = cb;
            cr = nr;
            cg = ng;
            cb = nb;
        }
    }
    return {ox0, oy0, cols - 1, oy1 - 1};
}

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
