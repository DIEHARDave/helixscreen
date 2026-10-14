// SPDX-License-Identifier: GPL-3.0-or-later

#include "screensaver_hyperspace_sim.h"

#include "screensaver_motion.h"
#include "screensaver_pixel_writer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace helix::ui {

namespace {

using screensaver::random_below;
using screensaver::unit_random;

constexpr float PI = 3.14159265f;

// Distances are for a 480 px tall frame and scale with the frame's height.
constexpr float REFERENCE_HEIGHT = 480.0f;

// Camera space: x right, y down, z forward, in units where the focal length is half the frame's
// height. Stars live in a box around the flight path between Z_NEAR and Z_FAR.
constexpr float Z_NEAR = 0.2f;
constexpr float Z_FAR = 6.0f;
constexpr float SPREAD = 1.6f;     // stars sit within this of the path on either axis
constexpr float CLEARANCE = 0.12f; // ...and no nearer than this, so none flies into the lens
constexpr float STAR_SIZE = 0.02f; // sprite radius at z = 1, as a share of the frame height
constexpr float FLARE_SIZE = 0.05f;
constexpr float MAX_SPRITE_PX = 28.0f;

constexpr float BASE_SPEED = 1.6f; // depth units per second
// Two slow sways make up the roll, as shares of HYPERSPACE_MAX_ROLL that never sum past it.
constexpr float ROLL_SLOW = 0.65f;
constexpr float ROLL_FAST = 0.25f;
static_assert(ROLL_SLOW + ROLL_FAST <= 1.0f, "the roll must stay within HYPERSPACE_MAX_ROLL");
constexpr float DRIFT_SHARE = 0.1f; // vanishing point's largest offset, as a share of the height
// The tunnel lookup reaches this far past half the frame, so the drift never runs off it.
constexpr float MARGIN_SHARE = DRIFT_SHARE + 0.01f;

// Starbursts: where they burst, how fast their flares fly and how long they burn.
constexpr float BURST_DEPTH = 3.6f;
constexpr float BURST_SPEED = 1.1f;
constexpr uint32_t BURST_LIFE_MIN_MS = 2200;
constexpr uint32_t BURST_LIFE_SPAN_MS = 1000;
constexpr int32_t FIRST_BURST_MIN_MS = 2000;
constexpr int32_t BURST_GAP_MIN_MS = 5000;
constexpr int32_t BURST_GAP_SPAN_MS = 6000;

// Phases of the flight.
constexpr int32_t FIRST_OPEN_MIN_MS = 3000;
constexpr int32_t OPEN_MIN_MS = 7000;
constexpr int32_t OPEN_SPAN_MS = 5000;
constexpr int32_t SWEEP_MS = 2500;
constexpr int32_t TUNNEL_MIN_MS = 10000;
constexpr int32_t TUNNEL_SPAN_MS = 6000;

constexpr uint32_t HUE_CYCLE_MS = 24000; // once round the colour wheel

// Tunnel geometry: a pixel r half-heights from the vanishing point sees the wall TUNNEL_DEPTH / r
// texture rows away. Depth is capped, and shade levels darken the far wall toward the centre.
constexpr float TUNNEL_DEPTH = 64.0f;
constexpr int DEPTH_LEVELS = 1024;
// The lookup keeps depth and angle in sixteenths of a texel, and an ordered dither picks between
// neighbouring texels, so the magnified near wall shows grain instead of stair steps.
constexpr int SUBTEXEL_BITS = 4;
constexpr int32_t QUARTER_TURN = 256 << SUBTEXEL_BITS; // angle units in a quarter of the wall
constexpr int SHADE_LEVELS = 16;
constexpr float FOG_DEPTH = 220.0f;
constexpr float SCROLL_PER_UNIT = 40.0f; // texture rows flown per depth unit

// Tunnel and goo share each channel: their brightest colours sum to at most 255, so adding two
// packed entries never carries into the next channel, in either format.
constexpr int TUNNEL_MAX = 207;
constexpr int GOO_MAX = 48;
static_assert(TUNNEL_MAX + GOO_MAX <= 255, "tunnel and goo must add without carrying");
static_assert(TUNNEL_MAX / 8 + GOO_MAX / 8 <= 31 && TUNNEL_MAX / 4 + GOO_MAX / 4 <= 63,
              "tunnel and goo must add without carrying at RGB565");

/// Fully saturated colour `h` of 256 round the wheel.
Rgb hue_color(uint32_t h) {
    h &= 255;
    const uint32_t sector = h * 6 / 256;
    const auto ramp = static_cast<uint8_t>(h * 6 - sector * 256);
    const auto fall = static_cast<uint8_t>(255 - ramp);
    switch (sector) {
    case 0:
        return {255, ramp, 0};
    case 1:
        return {fall, 255, 0};
    case 2:
        return {0, 255, ramp};
    case 3:
        return {0, fall, 255};
    case 4:
        return {ramp, 0, 255};
    default:
        return {255, 0, fall};
    }
}

// The frame rows are written as whole uint32_t / uint16_t pixels, which match PixelWriter's byte
// layout only on a little-endian CPU.
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
              "pixels are written as little-endian words");

/// An XRGB8888 word's X byte, which goo colours leave 0 so they add onto an opaque colour.
constexpr uint32_t X_BYTE = 0xFF000000u;

uint8_t scale8(uint32_t v, uint32_t amount) {
    return static_cast<uint8_t>(std::min<uint32_t>(255, v * amount / 255));
}

/// Adds light (r, g, b) to the pixel at `p`, each channel saturating.
inline void add_light(uint32_t* p, uint32_t r, uint32_t g, uint32_t b) {
    const uint32_t v = *p;
    const uint32_t nr = std::min<uint32_t>(255, (v >> 16 & 0xFF) + r);
    const uint32_t ng = std::min<uint32_t>(255, (v >> 8 & 0xFF) + g);
    const uint32_t nb = std::min<uint32_t>(255, (v & 0xFF) + b);
    *p = 0xFF000000u | nr << 16 | ng << 8 | nb;
}

inline void add_light(uint16_t* p, uint32_t r, uint32_t g, uint32_t b) {
    const uint32_t v = *p;
    const uint32_t nr = std::min<uint32_t>(31, (v >> 11) + (r >> 3));
    const uint32_t ng = std::min<uint32_t>(63, (v >> 5 & 0x3F) + (g >> 2));
    const uint32_t nb = std::min<uint32_t>(31, (v & 0x1F) + (b >> 3));
    *p = static_cast<uint16_t>(nr << 11 | ng << 5 | nb);
}

template <typename Px> Px* row_at(FrameTarget& frame, int32_t y) {
    return reinterpret_cast<Px*>(frame.data + static_cast<size_t>(y) * frame.stride);
}

/// Empties `v` and returns its storage, which clear() alone keeps.
template <typename T> void free_buffer(std::vector<T>& v) {
    std::vector<T>().swap(v);
}

/// A sine wave folded into 0..85, so three of them sum within a byte.
uint8_t wave85(float phase) {
    return static_cast<uint8_t>(42.5f + 42.0f * std::sin(phase));
}

} // namespace

void HyperspaceSim::init(const FrameTarget& frame, std::minstd_rand& rng, size_t level) {
    clear();
    w_ = static_cast<int32_t>(frame.w);
    h_ = static_cast<int32_t>(frame.h);
    format_ = frame.format;
    scale_ = static_cast<float>(h_) / REFERENCE_HEIGHT;
    level_ = std::min(level, HYPERSPACE_LEVEL_COUNT - 1);
    pending_level_ = level_;
    elapsed_ms_ = 0;
    camera_ = {};
    scroll_ = 0.0f;

    goo_palette_.resize(256);
    goo_col_.resize(static_cast<size_t>(w_));
    goo_row_.resize(static_cast<size_t>(h_));
    goo_diag_.resize(static_cast<size_t>(w_ + h_));
    row_.resize(static_cast<size_t>(w_));

    // A radial falloff per sprite radius: a hot core inside a soft halo.
    const int max_radius = std::max(2, static_cast<int>(std::lround(MAX_SPRITE_PX * scale_)));
    sprite_at_.resize(static_cast<size_t>(max_radius) + 1);
    size_t total = 0;
    for (int r = 0; r <= max_radius; r++) {
        sprite_at_[static_cast<size_t>(r)] = static_cast<uint32_t>(total);
        total += static_cast<size_t>(2 * r + 1) * static_cast<size_t>(2 * r + 1);
    }
    sprites_.resize(total);
    for (int r = 0; r <= max_radius; r++) {
        uint8_t* s = &sprites_[sprite_at_[static_cast<size_t>(r)]];
        const float radius = std::max(1.0f, static_cast<float>(r));
        for (int dy = -r; dy <= r; dy++) {
            for (int dx = -r; dx <= r; dx++) {
                const float d = std::sqrt(static_cast<float>(dx * dx + dy * dy)) / radius;
                const float core = std::exp(-(d / 0.16f) * (d / 0.16f));
                const float halo = d < 1.0f ? 0.8f * (1.0f - d) * std::sqrt(1.0f - d) : 0.0f;
                *s++ = static_cast<uint8_t>(std::clamp((core + halo) * 255.0f, 0.0f, 255.0f));
            }
        }
    }

    stars_.resize(MAX_STARS);
    for (Star& star : stars_) {
        place_star(star, rng, true);
    }
    flares_.assign(BURST_FLARES, Flare{});

    phase_ = Phase::OPEN;
    phase_left_ms_ = FIRST_OPEN_MIN_MS + random_below(rng, 2000);
    tunnel_amount_ = 0;
    burst_in_ms_ = FIRST_BURST_MIN_MS + random_below(rng, 3000);
    bursts_started_ = 0;
    tables_stale_ = true;
}

void HyperspaceSim::clear() {
    free_buffer(depth_);
    free_buffer(angle_);
    free_buffer(shade_of_depth_);
    free_buffer(texture_);
    free_buffer(tunnel_palette_);
    free_buffer(goo_palette_);
    free_buffer(goo_col_);
    free_buffer(goo_row_);
    free_buffer(goo_diag_);
    free_buffer(row_);
    free_buffer(sprites_);
    free_buffer(sprite_at_);
    free_buffer(stars_);
    free_buffer(flares_);
    quad_w_ = 0;
    quad_h_ = 0;
}

size_t HyperspaceSim::tunnel_bytes() const {
    return depth_.capacity() * sizeof(uint16_t) + angle_.capacity() * sizeof(uint16_t) +
           shade_of_depth_.capacity() + texture_.capacity() +
           tunnel_palette_.capacity() * sizeof(uint32_t);
}

size_t HyperspaceSim::heap_bytes() const {
    return tunnel_bytes() + goo_palette_.capacity() * sizeof(uint32_t) + goo_col_.capacity() +
           goo_row_.capacity() + goo_diag_.capacity() + row_.capacity() * sizeof(uint32_t) +
           sprites_.capacity() + sprite_at_.capacity() * sizeof(uint32_t) +
           stars_.capacity() * sizeof(Star) + flares_.capacity() * sizeof(Flare);
}

void HyperspaceSim::request_level(size_t level) {
    pending_level_ = std::min(level, HYPERSPACE_LEVEL_COUNT - 1);
}

size_t HyperspaceSim::burst_flares_alive() const {
    return static_cast<size_t>(
        std::count_if(flares_.begin(), flares_.end(), [](const Flare& f) { return f.alive; }));
}

uint8_t HyperspaceSim::hue() const {
    return static_cast<uint8_t>((elapsed_ms_ % HUE_CYCLE_MS) * 256 / HUE_CYCLE_MS);
}

void HyperspaceSim::place_star(Star& star, std::minstd_rand& rng, bool anywhere) {
    do {
        star.x = (unit_random(rng) * 2.0f - 1.0f) * SPREAD;
        star.y = (unit_random(rng) * 2.0f - 1.0f) * SPREAD;
    } while (star.x * star.x + star.y * star.y < CLEARANCE * CLEARANCE);
    star.z =
        anywhere ? Z_NEAR + unit_random(rng) * (Z_FAR - Z_NEAR) : Z_FAR - unit_random(rng) * 0.4f;
    star.hue_offset = static_cast<uint8_t>(random_below(rng, 112) - 56);
}

void HyperspaceSim::throw_starburst(std::minstd_rand& rng) {
    bursts_started_++;
    const float cx = (unit_random(rng) * 2.0f - 1.0f) * 0.35f;
    const float cy = (unit_random(rng) * 2.0f - 1.0f) * 0.25f;
    const bool ring = random_below(rng, 2) == 0;
    const auto hue_offset = static_cast<uint8_t>(random_below(rng, 256));
    const float tilt = (unit_random(rng) * 2.0f - 1.0f) * 0.5f;
    for (size_t i = 0; i < BURST_FLARES; i++) {
        Flare& f = flares_[i];
        float dx;
        float dy;
        float dz;
        if (ring) {
            const float a = 2.0f * PI * static_cast<float>(i) / static_cast<float>(BURST_FLARES);
            dx = std::cos(a);
            dy = std::sin(a) * std::cos(tilt);
            dz = std::sin(a) * std::sin(tilt);
        } else {
            // Even points on a sphere: a golden-angle spiral from pole to pole.
            const float k = (static_cast<float>(i) + 0.5f) / static_cast<float>(BURST_FLARES);
            dz = 1.0f - 2.0f * k;
            const float r = std::sqrt(std::max(0.0f, 1.0f - dz * dz));
            const float a = 2.39996323f * static_cast<float>(i);
            dx = r * std::cos(a);
            dy = r * std::sin(a);
        }
        const float speed = BURST_SPEED * (0.85f + 0.3f * unit_random(rng));
        f.x = cx;
        f.y = cy;
        f.z = BURST_DEPTH;
        f.vx = dx * speed;
        f.vy = dy * speed;
        f.vz = dz * speed * 0.6f;
        f.age_ms = 0;
        f.life_ms = static_cast<uint16_t>(
            BURST_LIFE_MIN_MS +
            static_cast<uint32_t>(random_below(rng, static_cast<int>(BURST_LIFE_SPAN_MS))));
        f.hue_offset = static_cast<uint8_t>(hue_offset + random_below(rng, 24));
        f.alive = true;
    }
}

void HyperspaceSim::advance_phase(uint32_t dt_ms, std::minstd_rand& rng) {
    phase_left_ms_ -= static_cast<int32_t>(dt_ms);
    while (phase_left_ms_ <= 0) {
        switch (phase_) {
        case Phase::OPEN:
            phase_ = Phase::SWEEP_IN;
            phase_left_ms_ += SWEEP_MS;
            break;
        case Phase::SWEEP_IN:
            phase_ = Phase::TUNNEL;
            phase_left_ms_ += TUNNEL_MIN_MS + random_below(rng, TUNNEL_SPAN_MS);
            break;
        case Phase::TUNNEL:
            phase_ = Phase::SWEEP_OUT;
            phase_left_ms_ += SWEEP_MS;
            break;
        case Phase::SWEEP_OUT:
            phase_ = Phase::OPEN;
            phase_left_ms_ += OPEN_MIN_MS + random_below(rng, OPEN_SPAN_MS);
            break;
        }
    }
    const int32_t swept = std::clamp(SWEEP_MS - phase_left_ms_, 0, SWEEP_MS);
    switch (phase_) {
    case Phase::OPEN:
        tunnel_amount_ = 0;
        break;
    case Phase::SWEEP_IN:
        tunnel_amount_ = static_cast<uint8_t>(swept * 255 / SWEEP_MS);
        break;
    case Phase::TUNNEL:
        tunnel_amount_ = 255;
        break;
    case Phase::SWEEP_OUT:
        tunnel_amount_ = static_cast<uint8_t>(255 - swept * 255 / SWEEP_MS);
        break;
    }
}

void HyperspaceSim::advance(uint32_t dt_ms, std::minstd_rand& rng) {
    elapsed_ms_ += dt_ms;
    const double t = static_cast<double>(elapsed_ms_) / 1000.0;
    const auto wave = [t](double rate, double phase) {
        return static_cast<float>(std::sin(t * rate + phase));
    };
    camera_.speed = BASE_SPEED * (1.0f + 0.3f * wave(0.21, 0.0) + 0.15f * wave(0.53, 1.0));
    camera_.roll =
        HYPERSPACE_MAX_ROLL * (ROLL_SLOW * wave(0.13, 0.5) + ROLL_FAST * wave(0.31, 1.0));
    const float drift = DRIFT_SHARE * static_cast<float>(h_);
    camera_.drift_x = drift * (0.7f * wave(0.17, 0.0) + 0.3f * wave(0.41, 2.0));
    camera_.drift_y = drift * 0.6f * (0.7f * wave(0.23, 1.0) + 0.3f * wave(0.37, 0.0));

    const float dt = static_cast<float>(dt_ms) / 1000.0f;
    const float forward = camera_.speed * dt;
    scroll_ = std::fmod(scroll_ + forward * SCROLL_PER_UNIT, 256.0f);

    for (Star& star : stars_) {
        star.z -= forward;
        if (star.z < Z_NEAR) {
            place_star(star, rng, false);
        }
    }
    for (Flare& f : flares_) {
        if (!f.alive) {
            continue;
        }
        f.x += f.vx * dt;
        f.y += f.vy * dt;
        f.z += f.vz * dt - forward;
        f.age_ms = static_cast<uint16_t>(std::min<uint32_t>(f.life_ms, f.age_ms + dt_ms));
        if (f.age_ms >= f.life_ms || f.z < Z_NEAR) {
            f.alive = false;
        }
    }

    burst_in_ms_ -= static_cast<int32_t>(dt_ms);
    if (burst_in_ms_ <= 0) {
        throw_starburst(rng);
        burst_in_ms_ = BURST_GAP_MIN_MS + random_below(rng, BURST_GAP_SPAN_MS);
    }
    advance_phase(dt_ms, rng);
}

void HyperspaceSim::build_texture() {
    shade_of_depth_.resize(DEPTH_LEVELS);
    const float nearest = TUNNEL_DEPTH / 2.0f;
    for (int d = 0; d < DEPTH_LEVELS; d++) {
        const float fog = std::exp(-(static_cast<float>(d) - nearest) / FOG_DEPTH);
        shade_of_depth_[static_cast<size_t>(d)] = static_cast<uint8_t>(
            std::clamp(fog * (SHADE_LEVELS - 1) + 0.5f, 0.0f, SHADE_LEVELS - 1.0f));
    }

    // Ribbons of light that wind along the tunnel, brightening in rings. A texel's high nibble is
    // its brightness and its low nibble how far round the wheel its colour sits, in bands that
    // flow along the tunnel and bend with the ribbons.
    texture_.resize(256 * 256);
    for (int v = 0; v < 256; v++) {
        for (int u = 0; u < 256; u++) {
            const float fu = static_cast<float>(u) / 256.0f;
            const float fv = static_cast<float>(v) / 256.0f;
            const float wind = 1.6f * std::sin(2.0f * PI * (4.0f * fv + fu));
            const float ribbon = 0.5f + 0.5f * std::sin(2.0f * PI * 3.0f * fu + wind);
            const float rings = 0.7f + 0.3f * std::sin(2.0f * PI * 8.0f * fv);
            const float glow = std::clamp((0.3f + 0.7f * ribbon * ribbon) * rings, 0.0f, 1.0f);
            const float band = 2.0f * fv + 0.15f * wind;
            const float tri = 1.0f - std::fabs(2.0f * (band - std::floor(band)) - 1.0f);
            const auto bright = static_cast<uint32_t>(glow * 15.0f + 0.5f);
            const auto shift = static_cast<uint32_t>(tri * 15.0f + 0.5f);
            texture_[static_cast<size_t>(v) * 256 + static_cast<size_t>(u)] =
                static_cast<uint8_t>(bright << 4 | shift);
        }
    }

    tunnel_palette_.resize(static_cast<size_t>(SHADE_LEVELS) * 256);
}

void HyperspaceSim::build_tables() {
    const int32_t block = HYPERSPACE_LEVELS[level_].block;
    tables_stale_ = false;
    if (block == 0) {
        free_buffer(depth_);
        free_buffer(angle_);
        free_buffer(shade_of_depth_);
        free_buffer(texture_);
        free_buffer(tunnel_palette_);
        quad_w_ = 0;
        quad_h_ = 0;
        return;
    }
    if (texture_.empty()) {
        build_texture();
    }
    // One quadrant at this level's block size, reaching past half the frame by the drift.
    const int32_t margin =
        (static_cast<int32_t>(std::ceil(MARGIN_SHARE * static_cast<float>(h_))) + block - 1) /
        block;
    quad_w_ = ((w_ + block - 1) / block + 1) / 2 + margin + 2;
    quad_h_ = ((h_ + block - 1) / block + 1) / 2 + margin + 2;
    const size_t quad = static_cast<size_t>(quad_w_) * static_cast<size_t>(quad_h_);
    if (depth_.size() != quad) {
        std::vector<uint16_t>(quad).swap(depth_);
        std::vector<uint16_t>(quad).swap(angle_);
    }

    // Each entry is the block centred (ax + 0.5, ay + 0.5) blocks from the vanishing point.
    const float half_h = static_cast<float>(h_) / 2.0f;
    for (int32_t ay = 0; ay < quad_h_; ay++) {
        for (int32_t ax = 0; ax < quad_w_; ax++) {
            const float fx = (static_cast<float>(ax) + 0.5f) * static_cast<float>(block);
            const float fy = (static_cast<float>(ay) + 0.5f) * static_cast<float>(block);
            const float r = std::sqrt(fx * fx + fy * fy) / half_h;
            const float depth = std::min(TUNNEL_DEPTH / r, static_cast<float>(DEPTH_LEVELS - 1)) *
                                static_cast<float>(1 << SUBTEXEL_BITS);
            const float quarter =
                std::atan2(fy, fx) / (PI / 2.0f) * static_cast<float>(QUARTER_TURN);
            const size_t i =
                static_cast<size_t>(ay) * static_cast<size_t>(quad_w_) + static_cast<size_t>(ax);
            depth_[i] = static_cast<uint16_t>(depth);
            angle_[i] = static_cast<uint16_t>(
                std::clamp(quarter, 0.0f, static_cast<float>(QUARTER_TURN - 1)));
        }
    }
}

void HyperspaceSim::build_palettes() {
    const uint32_t hue = this->hue();
    const uint32_t fade = tunnel_amount_;
    // Each texel's colour band sits up to 3/4 of the way round the wheel, and the brightest
    // texels burn toward white.
    // A level without a tunnel holds no tunnel palette.
    for (uint32_t texel = 0; texel < tunnel_palette_.size() / SHADE_LEVELS; texel++) {
        const uint32_t bright = (texel >> 4) * 255 / 15;
        const Rgb base = hue_color(hue + (texel & 15) * 12);
        const uint32_t white = bright * bright / 255 * bright / 255 / 3;
        const Rgb lit = {static_cast<uint8_t>(std::min<uint32_t>(255, base.r + white)),
                         static_cast<uint8_t>(std::min<uint32_t>(255, base.g + white)),
                         static_cast<uint8_t>(std::min<uint32_t>(255, base.b + white))};
        for (uint32_t shade = 0; shade < SHADE_LEVELS; shade++) {
            const uint32_t amount =
                bright * shade / (SHADE_LEVELS - 1) * fade / 255 * TUNNEL_MAX / 255;
            tunnel_palette_[shade << 8 | texel] = PixelWriter::pack(
                {scale8(lit.r, amount), scale8(lit.g, amount), scale8(lit.b, amount)}, format_);
        }
    }
    // Goo glows in three soft bands of its plasma value, in colours opposite the tunnel's. Its X
    // byte is 0, so it adds onto an opaque tunnel colour without carrying.
    for (uint32_t g = 0; g < 256; g++) {
        const float band = std::sin(2.0f * PI * 3.0f * static_cast<float>(g) / 256.0f);
        const auto amount =
            static_cast<uint32_t>(band > 0.0f ? band * band * band * GOO_MAX : 0.0f);
        const Rgb base = hue_color(hue + 128 + g / 2);
        goo_palette_[g] =
            PixelWriter::pack(
                {scale8(base.r, amount), scale8(base.g, amount), scale8(base.b, amount)}, format_) &
            ~X_BYTE;
    }
}

template <typename Px> void HyperspaceSim::draw_background(FrameTarget& frame) {
    const HyperspaceLevel& lv = HYPERSPACE_LEVELS[level_];
    const bool tunnel = lv.block != 0 && tunnel_amount_ != 0;
    const bool goo = lv.goo && lv.block != 0;
    if (!tunnel && !goo) {
        for (int32_t y = 0; y < h_; y++) {
            Px* out = row_at<Px>(frame, y);
            if (sizeof(Px) == 4) {
                std::fill(out, out + w_, static_cast<Px>(0xFF000000u));
            } else {
                std::fill(out, out + w_, static_cast<Px>(0));
            }
        }
        return;
    }

    const int32_t block = lv.block;
    const int32_t rw = (w_ + block - 1) / block;
    const int32_t rh = (h_ + block - 1) / block;
    const float max_drift = DRIFT_SHARE * static_cast<float>(h_);
    const auto cx = static_cast<int32_t>(
        (static_cast<float>(w_) / 2.0f + std::clamp(camera_.drift_x, -max_drift, max_drift)) /
        static_cast<float>(block));
    const auto cy = static_cast<int32_t>(
        (static_cast<float>(h_) / 2.0f + std::clamp(camera_.drift_y, -max_drift, max_drift)) /
        static_cast<float>(block));
    const uint32_t opaque = sizeof(Px) == 4 ? 0xFF000000u : 0u;

    // Goo: three slow sine sweeps, across, down and diagonally, summed per block.
    if (goo) {
        const float t = static_cast<float>(elapsed_ms_ % 3600000u) / 1000.0f;
        const float k = 2.0f * PI / (static_cast<float>(h_) * 0.9f) * static_cast<float>(block);
        for (int32_t x = 0; x < rw; x++) {
            goo_col_[static_cast<size_t>(x)] =
                wave85(static_cast<float>(x) * k * 0.8f + t * 0.45f +
                       1.3f * std::sin(static_cast<float>(x) * k * 0.31f - t * 0.2f));
        }
        for (int32_t y = 0; y < rh; y++) {
            goo_row_[static_cast<size_t>(y)] =
                wave85(static_cast<float>(y) * k * 1.1f - t * 0.37f +
                       1.1f * std::sin(static_cast<float>(y) * k * 0.47f + t * 0.25f));
        }
        for (int32_t d = 0; d < rw + rh; d++) {
            goo_diag_[static_cast<size_t>(d)] =
                wave85(static_cast<float>(d) * k * 0.55f + t * 0.3f);
        }
    }

    // Angles run 4 * QUARTER_TURN round the wall, which the texture spans twice.
    const int32_t roll = static_cast<int32_t>(camera_.roll / (2.0f * PI) * 4.0f * QUARTER_TURN);
    const auto spin = static_cast<int32_t>(elapsed_ms_ * 2 / 5 % (4 * QUARTER_TURN));
    const auto scroll = static_cast<uint32_t>(scroll_);
    const uint32_t* tpal = tunnel_palette_.data();
    const uint32_t* gpal = goo_palette_.data();
    const uint8_t* tex = texture_.data();
    const uint8_t* shade = shade_of_depth_.data();

    for (int32_t ry = 0; ry < rh; ry++) {
        const int32_t dy = ry - cy;
        const bool below = dy >= 0;
        const int32_t ay = std::min(below ? dy : -1 - dy, quad_h_ - 1);
        const uint16_t* drow = &depth_[static_cast<size_t>(ay) * static_cast<size_t>(quad_w_)];
        const uint16_t* arow = &angle_[static_cast<size_t>(ay) * static_cast<size_t>(quad_w_)];
        const uint32_t grow = goo ? goo_row_[static_cast<size_t>(ry)] : 0;

        // One span left of the vanishing point and one right of it; within each the angle
        // runs one way round the quadrant.
        const auto span = [&](int32_t x0, int32_t x1, int32_t ax0, int32_t ax_step, int32_t base,
                              int32_t sign) {
            int32_t ax = ax0;
            const uint8_t* dither = BAYER_4X4[ry & 3];
            for (int32_t rx = x0; rx < x1; rx++, ax += ax_step) {
                const int32_t axc = std::min(ax, quad_w_ - 1);
                uint32_t px = opaque;
                if (tunnel) {
                    const uint32_t d = drow[axc];
                    const uint32_t jitter = dither[rx & 3];
                    const auto a = static_cast<uint32_t>(base + sign * arow[axc] + roll + spin);
                    const uint32_t u = ((a + 2 * jitter) >> (SUBTEXEL_BITS + 1)) & 255;
                    const uint32_t v = (((d + jitter) >> SUBTEXEL_BITS) + scroll) & 255;
                    px = tpal[static_cast<uint32_t>(shade[d >> SUBTEXEL_BITS]) << 8 |
                              tex[v << 8 | u]];
                }
                if (goo) {
                    px += gpal[goo_col_[static_cast<size_t>(rx)] + grow +
                               goo_diag_[static_cast<size_t>(rx + ry)]];
                }
                row_[static_cast<size_t>(rx)] = px;
            }
        };
        const int32_t split = std::clamp(cx, 0, rw);
        span(0, split, cx - 1, -1, 2 * QUARTER_TURN, below ? -1 : 1);
        span(split, rw, split - cx, 1, below ? 0 : 4 * QUARTER_TURN, below ? 1 : -1);

        const int32_t y0 = ry * block;
        Px* out = row_at<Px>(frame, y0);
        if (block == 1) {
            for (int32_t x = 0; x < w_; x++) {
                out[x] = static_cast<Px>(row_[static_cast<size_t>(x)]);
            }
        } else {
            for (int32_t rx = 0; rx < rw; rx++) {
                const auto px = static_cast<Px>(row_[static_cast<size_t>(rx)]);
                const int32_t x_end = std::min(w_, (rx + 1) * block);
                for (int32_t x = rx * block; x < x_end; x++) {
                    out[x] = px;
                }
            }
            for (int32_t y = y0 + 1; y < std::min(h_, y0 + block); y++) {
                std::memcpy(row_at<Px>(frame, y), out, static_cast<size_t>(w_) * sizeof(Px));
            }
        }
    }
}

template <typename Px>
void HyperspaceSim::draw_flare(FrameTarget& frame, float x, float y, float z, float size,
                               uint8_t hue_offset, uint32_t brightness) {
    const float half_h = static_cast<float>(h_) / 2.0f;
    const float c = std::cos(camera_.roll);
    const float s = std::sin(camera_.roll);
    const float sx = static_cast<float>(w_) / 2.0f + camera_.drift_x + (x * c - y * s) * half_h / z;
    const float sy = static_cast<float>(h_) / 2.0f + camera_.drift_y + (x * s + y * c) * half_h / z;
    const int max_radius = static_cast<int>(sprite_at_.size()) - 1;
    const int r =
        std::clamp(static_cast<int>(std::lround(size * static_cast<float>(h_) / z)), 1, max_radius);
    const auto px = static_cast<int32_t>(std::lround(sx));
    const auto py = static_cast<int32_t>(std::lround(sy));
    if (px + r < 0 || py + r < 0 || px - r >= w_ || py - r >= h_) {
        return;
    }
    const Rgb color = hue_color(static_cast<uint32_t>(hue() + hue_offset));
    const uint8_t* sprite = &sprites_[sprite_at_[static_cast<size_t>(r)]];
    const int32_t side = 2 * r + 1;
    for (int32_t dy = -r; dy <= r; dy++) {
        const int32_t y = py + dy;
        if (y < 0 || y >= h_) {
            continue;
        }
        Px* out = row_at<Px>(frame, y);
        const uint8_t* srow = sprite + static_cast<size_t>((dy + r) * side);
        for (int32_t dx = -r; dx <= r; dx++) {
            const int32_t x = px + dx;
            if (x < 0 || x >= w_) {
                continue;
            }
            const uint32_t i = srow[dx + r] * brightness >> 8;
            if (i == 0) {
                continue;
            }
            const uint32_t white = i * i >> 9;
            add_light(&out[x], (color.r * i >> 8) + white, (color.g * i >> 8) + white,
                      (color.b * i >> 8) + white);
        }
    }
}

void HyperspaceSim::step(uint32_t dt_ms, FrameTarget& frame, std::minstd_rand& rng,
                         std::vector<DirtyRect>& dirty) {
    if (pending_level_ != level_) {
        const bool new_block =
            HYPERSPACE_LEVELS[pending_level_].block != HYPERSPACE_LEVELS[level_].block;
        level_ = pending_level_;
        tables_stale_ = tables_stale_ || new_block;
    }
    // A long stall advances the flight a bounded step, so nothing jumps across the screen.
    advance(std::min<uint32_t>(dt_ms, 250), rng);
    if (tables_stale_) {
        build_tables();
    }
    // A level without a tunnel draws neither tunnel nor goo, so nothing reads the palettes.
    if (HYPERSPACE_LEVELS[level_].block != 0) {
        build_palettes();
    }

    const auto draw = [&](auto pixel_type) {
        using Px = decltype(pixel_type);
        draw_background<Px>(frame);
        const size_t count = std::min<size_t>(HYPERSPACE_LEVELS[level_].stars, stars_.size());
        for (size_t i = 0; i < count; i++) {
            const Star& star = stars_[i];
            // Stars fade in out of the distance.
            const float near = std::clamp((Z_FAR - star.z) / (0.3f * Z_FAR), 0.0f, 1.0f);
            draw_flare<Px>(frame, star.x, star.y, star.z, STAR_SIZE, star.hue_offset,
                           static_cast<uint32_t>(near * 256.0f));
        }
        for (const Flare& f : flares_) {
            if (!f.alive) {
                continue;
            }
            const float age = static_cast<float>(f.age_ms) / static_cast<float>(f.life_ms);
            const float glow = std::min(1.0f, age * 8.0f) * std::min(1.0f, (1.0f - age) * 2.5f);
            draw_flare<Px>(frame, f.x, f.y, f.z, FLARE_SIZE, f.hue_offset,
                           static_cast<uint32_t>(glow * 256.0f));
        }
    };
    if (format_ == PixelFormat::RGB565) {
        draw(uint16_t{});
    } else {
        draw(uint32_t{});
    }

    dirty.clear();
    dirty.push_back({0, 0, w_ - 1, h_ - 1});
}

} // namespace helix::ui
