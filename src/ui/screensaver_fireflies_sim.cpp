// SPDX-License-Identifier: GPL-3.0-or-later

#include "screensaver_fireflies_sim.h"

#include "screensaver_pixel_writer.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace helix::ui {

namespace {

using screensaver::random_below;
using screensaver::unit_random;

// Baits wander a box inset by this share of each half-extent.
constexpr float WAYPOINT_INSET = 0.9f;
constexpr float FLY_PACE_MIN = 0.75f;
constexpr float SWARM_HUE_JITTER_DEG = 30.0f;

// Tail points are stored in fixed point, this many steps to a world unit.
constexpr float TAIL_FIXED_SCALE = 64.0f;

int16_t to_fixed(float v) {
    return static_cast<int16_t>(
        std::clamp(std::lround(v * TAIL_FIXED_SCALE), long{INT16_MIN}, long{INT16_MAX}));
}

float uniform(std::minstd_rand& rng, float lo, float hi) {
    return lo + (hi - lo) * unit_random(rng);
}

template <typename T> size_t bytes_of(const std::vector<T>& v) {
    return v.capacity() * sizeof(T);
}

/// Empties `v` and returns its storage, which clear() alone keeps.
template <typename T> void free_buffer(std::vector<T>& v) {
    std::vector<T>().swap(v);
}

constexpr float STEP_S = static_cast<float>(FIREFLIES_STEP_MS) / 1000.0f;

// A bait takes a new waypoint once this near it, or when its timer runs out.
constexpr float WAYPOINT_REACHED = 5.0f;
constexpr int32_t RETARGET_MIN_MS = 2000;
constexpr int32_t RETARGET_MAX_MS = 5000;

FireflyVec operator+(FireflyVec a, FireflyVec b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
FireflyVec operator-(FireflyVec a, FireflyVec b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
FireflyVec operator*(FireflyVec a, float k) {
    return {a.x * k, a.y * k, a.z * k};
}
float length_sq(FireflyVec v) {
    return v.x * v.x + v.y * v.y + v.z * v.z;
}

/// `v` shortened to at most `max_len`.
FireflyVec clamp_length(FireflyVec v, float max_len) {
    const float sq = length_sq(v);
    if (sq <= max_len * max_len || sq == 0.0f) {
        return v;
    }
    return v * (max_len / std::sqrt(sq));
}

/**
 * @brief Steers `vel` toward `desired` for one step
 *
 * The velocity changes toward `desired` at up to `accel` units/s^2, landing on it rather than
 * passing it, and the result is capped at `speed`. There is no slow-down on arrival, so a chaser
 * swings through its target instead of settling on it.
 */
void steer(FireflyVec& vel, FireflyVec desired, float speed, float accel) {
    vel = clamp_length(vel + clamp_length(desired - vel, accel * STEP_S), speed);
}

/// Velocity of top speed `speed` from `from` toward `to`; zero when they coincide.
FireflyVec seek(FireflyVec from, FireflyVec to, float speed) {
    const FireflyVec d = to - from;
    const float sq = length_sq(d);
    return sq == 0.0f ? FireflyVec{} : d * (speed / std::sqrt(sq));
}

int32_t random_ms(std::minstd_rand& rng, int32_t lo, int32_t hi) {
    return lo + random_below(rng, hi - lo + 1);
}

constexpr float PI = 3.14159265f;

// Modes: how long NORMAL runs between the others, LOOP's circle, and SPLIT's hue step.
constexpr int32_t NORMAL_MIN_MS = 8000;
constexpr int32_t NORMAL_MAX_MS = 15000;
constexpr float LOOP_RADIUS = 15.0f;
constexpr float LOOP_RATE = 40.0f / LOOP_RADIUS; // rad/s: 40 units/s round the circle
constexpr float SPLIT_HUE_DEG = 120.0f;
// A gust blows this many times the wind, easing in and out over GUST_EASE_MS.
constexpr float GUST_MULT = 4.0f;
constexpr float GUST_EASE_MS = 500.0f;

/// `deg` wrapped into [0, 360).
float wrap360(float deg) {
    float d = std::fmod(deg, 360.0f);
    if (d < 0.0f) {
        d += 360.0f;
    }
    return d >= 360.0f ? 0.0f : d;
}

static_assert(FIREFLIES_TAIL_SAMPLE_MS % FIREFLIES_STEP_MS == 0,
              "tail samples fall on whole steps");

// The wind: its strength in units per second, how much flatter than level it blows, and how
// long a new heading takes to ease in.
constexpr float WIND_MIN = 2.0f;
constexpr float WIND_MAX = 6.0f;
constexpr float WIND_RISE = 0.3f;
constexpr float WIND_EASE_MS = 2000.0f;

} // namespace

float fireflies_tail_brightness(float u) {
    if (!(u >= 0.0f && u <= 1.0f)) {
        return 0.0f;
    }
    return std::sin(PI * u) * (1.0f - 0.5f * u);
}

int32_t fireflies_tail_width(float u, float max_px) {
    const float clamped = std::clamp(u, 0.0f, 1.0f);
    const float w = 1.0f + (max_px - 1.0f) * std::sin(PI * clamped);
    return std::max<int32_t>(1, static_cast<int32_t>(std::lround(w)));
}

FireflyVec FirefliesSim::random_waypoint(std::minstd_rand& rng) const {
    return {uniform(rng, -0.5f, 0.5f) * world_w_ * WAYPOINT_INSET,
            uniform(rng, -0.5f, 0.5f) * FIREFLIES_WORLD_H * WAYPOINT_INSET,
            uniform(rng, -0.5f, 0.5f) * FIREFLIES_WORLD_D * WAYPOINT_INSET};
}

float FirefliesSim::speed_mult() const {
    return mode_ == FirefliesMode::HYPERSPEED ? 3.0f : 1.0f;
}

void FirefliesSim::init(const FrameTarget& frame, std::minstd_rand& rng, size_t level) {
    clear();
    w_ = static_cast<int32_t>(frame.w);
    h_ = static_cast<int32_t>(frame.h);
    world_w_ = FIREFLIES_WORLD_H * static_cast<float>(w_) / static_cast<float>(std::max(h_, 1));
    focal_ = static_cast<float>(h_) / 2.0f * 100.0f / (FIREFLIES_WORLD_H / 2.0f);
    level_ = std::min(level, FIREFLIES_LEVEL_COUNT - 1);
    pending_level_ = level_;
    PixelWriter(frame).fill({0, 0, 0});
    tiles_x_ = (w_ + FIREFLIES_TILE_PX - 1) / FIREFLIES_TILE_PX;
    tiles_y_ = (h_ + FIREFLIES_TILE_PX - 1) / FIREFLIES_TILE_PX;
    drawn_tiles_.assign(static_cast<size_t>(tiles_x_) * static_cast<size_t>(tiles_y_), 0);
    now_tiles_.assign(drawn_tiles_.size(), 0);

    baits_.assign(FIREFLIES_MAX_BAITS, Bait{});
    const size_t n =
        FIREFLIES_MIN_BAITS +
        static_cast<size_t>(random_below(rng, FIREFLIES_MAX_BAITS - FIREFLIES_MIN_BAITS + 1));
    for (size_t i = 0; i < n; i++) {
        Bait& b = baits_[i];
        b.alive = true;
        b.pos = random_waypoint(rng);
        b.target = random_waypoint(rng);
        b.retarget_ms = random_ms(rng, RETARGET_MIN_MS, RETARGET_MAX_MS);
        b.hue_deg = 360.0f * static_cast<float>(i) / static_cast<float>(n) +
                    uniform(rng, 0.0f, SWARM_HUE_JITTER_DEG);
        hue_origin_[i] = b.hue_deg;
    }

    const size_t flies =
        FIREFLIES_MIN_FLIES +
        static_cast<size_t>(random_below(rng, FIREFLIES_MAX_FLIES - FIREFLIES_MIN_FLIES + 1));
    flies_.resize(flies);
    for (Fly& f : flies_) {
        f.pos = {uniform(rng, -0.5f, 0.5f) * world_w_,
                 uniform(rng, -0.5f, 0.5f) * FIREFLIES_WORLD_H,
                 uniform(rng, -0.5f, 0.5f) * FIREFLIES_WORLD_D};
        f.pace = uniform(rng, FLY_PACE_MIN, 1.0f);
        f.bait = static_cast<uint8_t>(random_below(rng, static_cast<int>(n)));
    }

    tail_.resize(flies * FIREFLIES_TAIL_POINTS * 3);
    for (size_t i = 0; i < flies; i++) {
        const int16_t fx = to_fixed(flies_[i].pos.x);
        const int16_t fy = to_fixed(flies_[i].pos.y);
        const int16_t fz = to_fixed(flies_[i].pos.z);
        int16_t* t = &tail_[i * FIREFLIES_TAIL_POINTS * 3];
        for (size_t p = 0; p < FIREFLIES_TAIL_POINTS; p++) {
            t[p * 3] = fx;
            t[p * 3 + 1] = fy;
            t[p * 3 + 2] = fz;
        }
    }
    tail_head_ = 0;
    tail_count_ = 1;
    mode_ = FirefliesMode::NORMAL;
    mode_left_ms_ = 0;
    mode_total_ms_ = 0;
    std::fill(std::begin(modes_begun_), std::end(modes_begun_), 0u);
    next_mode_ms_ = random_ms(rng, NORMAL_MIN_MS, NORMAL_MAX_MS);

    base_wind_ = {};
    new_wind_heading(rng);
    wind_from_ = wind_to_;
    wind_changes_ = 0;
    last_wind_change_ms_ = 0;
    update_wind();
}

void FirefliesSim::new_wind_heading(std::minstd_rand& rng) {
    const float heading = uniform(rng, 0.0f, 2.0f * PI);
    const float rise = uniform(rng, -1.0f, 1.0f) * WIND_RISE;
    const float strength = uniform(rng, WIND_MIN, WIND_MAX);
    const FireflyVec dir{std::cos(heading), rise, std::sin(heading)};
    wind_from_ = base_wind_;
    wind_to_ = dir * (strength / std::sqrt(length_sq(dir)));
    wind_left_ms_ = random_ms(rng, static_cast<int32_t>(FIREFLIES_WIND_MIN_MS),
                              static_cast<int32_t>(FIREFLIES_WIND_MAX_MS));
    wind_changes_++;
    last_wind_change_ms_ = elapsed_ms_;
}

void FirefliesSim::update_wind() {
    const float eased =
        std::min(1.0f, static_cast<float>(elapsed_ms_ - last_wind_change_ms_) / WIND_EASE_MS);
    base_wind_ = wind_from_ + (wind_to_ - wind_from_) * eased;
    wind_ = base_wind_;
    if (mode_ == FirefliesMode::WIND_GUST) {
        const float in = static_cast<float>(mode_total_ms_ - mode_left_ms_) / GUST_EASE_MS;
        const float out = static_cast<float>(mode_left_ms_) / GUST_EASE_MS;
        wind_ =
            base_wind_ * (1.0f + (GUST_MULT - 1.0f) * std::clamp(std::min(in, out), 0.0f, 1.0f));
    }
}

float FirefliesSim::fly_hue_deg(size_t i) const {
    const Fly& f = flies_[i];
    const float speed = std::sqrt(length_sq(f.vel));
    const float swing = -FIREFLIES_HUE_SWING_DEG + 2.0f * FIREFLIES_HUE_SWING_DEG *
                                                       std::min(1.0f, speed / FIREFLIES_FLY_SPEED);
    return wrap360(baits_[f.bait].hue_deg + swing);
}

uint32_t FirefliesSim::tail_age_ms() const {
    if (tail_count_ == 0) {
        return 0;
    }
    return static_cast<uint32_t>(tail_count_ - 1) * FIREFLIES_TAIL_SAMPLE_MS +
           static_cast<uint32_t>(elapsed_ms_ % FIREFLIES_TAIL_SAMPLE_MS);
}

void FirefliesSim::clear() {
    free_buffer(baits_);
    free_buffer(flies_);
    free_buffer(tail_);
    tail_head_ = 0;
    tail_count_ = 0;
    steps_.reset();
    elapsed_ms_ = 0;
    free_buffer(drawn_tiles_);
    free_buffer(now_tiles_);
    tiles_x_ = 0;
    tiles_y_ = 0;
}

size_t FirefliesSim::heap_bytes() const {
    return bytes_of(baits_) + bytes_of(flies_) + bytes_of(tail_) + bytes_of(drawn_tiles_) +
           bytes_of(now_tiles_);
}

void FirefliesSim::request_level(size_t level) {
    pending_level_ = std::min(level, FIREFLIES_LEVEL_COUNT - 1);
}

size_t FirefliesSim::baits_alive() const {
    return static_cast<size_t>(
        std::count_if(baits_.begin(), baits_.end(), [](const Bait& b) { return b.alive; }));
}

uint32_t FirefliesSim::modes_begun(FirefliesMode mode) const {
    return modes_begun_[static_cast<size_t>(mode)];
}

bool FirefliesSim::begin_mode(FirefliesMode mode, uint32_t duration_ms, std::minstd_rand& rng) {
    switch (mode) {
    case FirefliesMode::SPLIT:
        if (!split(rng)) {
            return false;
        }
        break;
    case FirefliesMode::MERGE:
        if (!merge()) {
            return false;
        }
        break;
    case FirefliesMode::STOP:
        for (Bait& b : baits_) {
            b.target = b.pos;
        }
        break;
    case FirefliesMode::LOOP: {
        // The circle stays inside the box the waypoints keep to.
        const float lim_x = std::max(0.0f, world_w_ / 2.0f * WAYPOINT_INSET - LOOP_RADIUS);
        const float lim_y = FIREFLIES_WORLD_H / 2.0f * WAYPOINT_INSET;
        const float lim_z = std::max(0.0f, FIREFLIES_WORLD_D / 2.0f * WAYPOINT_INSET - LOOP_RADIUS);
        for (size_t i = 0; i < baits_.size(); i++) {
            Bait& b = baits_[i];
            b.loop_centre = {std::clamp(b.pos.x, -lim_x, lim_x), std::clamp(b.pos.y, -lim_y, lim_y),
                             std::clamp(b.pos.z, -lim_z, lim_z)};
            loop_phase_[i] = std::atan2(b.vel.z, b.vel.x);
        }
        break;
    }
    default:
        break;
    }
    const bool instant = mode == FirefliesMode::SPLIT || mode == FirefliesMode::MERGE;
    mode_ = instant ? FirefliesMode::NORMAL : mode;
    mode_total_ms_ = instant ? 0 : static_cast<int32_t>(duration_ms);
    mode_left_ms_ = mode_total_ms_;
    modes_begun_[static_cast<size_t>(mode)]++;
    return true;
}

bool FirefliesSim::split(std::minstd_rand& rng) {
    const std::array<size_t, FIREFLIES_MAX_BAITS> sizes = swarm_sizes();
    size_t parent = FIREFLIES_MAX_BAITS;
    size_t slot = FIREFLIES_MAX_BAITS;
    for (size_t i = 0; i < baits_.size(); i++) {
        if (!baits_[i].alive) {
            slot = std::min(slot, i);
        } else if (parent == FIREFLIES_MAX_BAITS || sizes[i] > sizes[parent]) {
            parent = i;
        }
    }
    if (slot == FIREFLIES_MAX_BAITS || parent == FIREFLIES_MAX_BAITS) {
        return false;
    }
    const Bait& from = baits_[parent];
    Bait& child = baits_[slot];
    child = Bait{};
    child.alive = true;
    child.pos = from.pos;
    child.vel = from.vel * -1.0f;
    child.target = random_waypoint(rng);
    child.retarget_ms = random_ms(rng, RETARGET_MIN_MS, RETARGET_MAX_MS);
    hue_origin_[slot] = hue_origin_[parent] + SPLIT_HUE_DEG;
    child.hue_deg = wrap360(from.hue_deg + SPLIT_HUE_DEG);
    child.loop_centre = from.loop_centre;
    loop_phase_[slot] = loop_phase_[parent] + PI;
    bool odd = false;
    for (Fly& f : flies_) {
        if (f.bait != parent) {
            continue;
        }
        if (odd) {
            f.bait = static_cast<uint8_t>(slot);
        }
        odd = !odd;
    }
    return true;
}

bool FirefliesSim::merge() {
    if (baits_alive() <= FIREFLIES_MIN_BAITS) {
        return false;
    }
    const std::array<size_t, FIREFLIES_MAX_BAITS> sizes = swarm_sizes();
    size_t dying = FIREFLIES_MAX_BAITS;
    for (size_t i = 0; i < baits_.size(); i++) {
        if (baits_[i].alive && (dying == FIREFLIES_MAX_BAITS || sizes[i] < sizes[dying])) {
            dying = i;
        }
    }
    size_t heir = FIREFLIES_MAX_BAITS;
    float heir_sq = 0.0f;
    for (size_t i = 0; i < baits_.size(); i++) {
        if (i == dying || !baits_[i].alive) {
            continue;
        }
        const float sq = length_sq(baits_[i].pos - baits_[dying].pos);
        if (heir == FIREFLIES_MAX_BAITS || sq < heir_sq) {
            heir = i;
            heir_sq = sq;
        }
    }
    for (Fly& f : flies_) {
        if (f.bait == dying) {
            f.bait = static_cast<uint8_t>(heir);
        }
    }
    baits_[dying].alive = false;
    return true;
}

std::array<size_t, FIREFLIES_MAX_BAITS> FirefliesSim::swarm_sizes() const {
    std::array<size_t, FIREFLIES_MAX_BAITS> sizes{};
    for (const Fly& f : flies_) {
        sizes[f.bait]++;
    }
    return sizes;
}

void FirefliesSim::schedule_next_mode(std::minstd_rand& rng) {
    struct Span {
        int32_t lo, hi;
    };
    // Indexed by FirefliesMode; SPLIT and MERGE act at once.
    constexpr Span DURATIONS[FIREFLIES_MODE_COUNT] = {
        {0, 0},       {4000, 6000}, {6000, 10000}, {4000, 6000},
        {2000, 4000}, {0, 0},       {0, 0},        {3000, 5000},
    };
    for (;;) {
        const auto mode = static_cast<FirefliesMode>(
            1 + random_below(rng, static_cast<int>(FIREFLIES_MODE_COUNT) - 1));
        const Span span = DURATIONS[static_cast<size_t>(mode)];
        const int32_t duration = span.hi == 0 ? 0 : random_ms(rng, span.lo, span.hi);
        if (begin_mode(mode, static_cast<uint32_t>(duration), rng)) {
            break;
        }
    }
    if (mode_ == FirefliesMode::NORMAL) {
        next_mode_ms_ = random_ms(rng, NORMAL_MIN_MS, NORMAL_MAX_MS);
    }
}

void FirefliesSim::advance_one(std::minstd_rand& rng) {
    if (mode_ != FirefliesMode::NORMAL) {
        mode_left_ms_ -= static_cast<int32_t>(FIREFLIES_STEP_MS);
        if (mode_left_ms_ <= 0) {
            mode_ = FirefliesMode::NORMAL;
            next_mode_ms_ = random_ms(rng, NORMAL_MIN_MS, NORMAL_MAX_MS);
        }
    } else {
        next_mode_ms_ -= static_cast<int32_t>(FIREFLIES_STEP_MS);
        if (next_mode_ms_ <= 0) {
            schedule_next_mode(rng);
        }
    }
    const float mode_s = static_cast<float>(mode_total_ms_ - mode_left_ms_) / 1000.0f;

    const float mult = speed_mult();
    const float bait_speed = FIREFLIES_BAIT_SPEED * mult;
    const float bait_accel = FIREFLIES_BAIT_ACCEL * mult;
    const FireflyVec half{world_w_ / 2.0f, FIREFLIES_WORLD_H / 2.0f, FIREFLIES_WORLD_D / 2.0f};
    for (size_t i = 0; i < baits_.size(); i++) {
        Bait& b = baits_[i];
        if (!b.alive) {
            continue;
        }
        FireflyVec desired{};
        if (mode_ == FirefliesMode::LOOP) {
            const float a = loop_phase_[i] + LOOP_RATE * mode_s;
            b.target = b.loop_centre + FireflyVec{std::cos(a), 0.0f, std::sin(a)} * LOOP_RADIUS;
            desired = seek(b.pos, b.target, bait_speed);
        } else if (mode_ != FirefliesMode::STOP) {
            b.retarget_ms -= static_cast<int32_t>(FIREFLIES_STEP_MS);
            if (b.retarget_ms <= 0 ||
                length_sq(b.target - b.pos) < WAYPOINT_REACHED * WAYPOINT_REACHED) {
                b.target = random_waypoint(rng);
                b.retarget_ms = random_ms(rng, RETARGET_MIN_MS, RETARGET_MAX_MS);
            }
            desired = seek(b.pos, b.target, bait_speed);
        }
        steer(b.vel, desired, bait_speed, bait_accel);
        b.pos = b.pos + b.vel * STEP_S;
        // The walls of the box stop a bait that overshoots its waypoint.
        float* p[3] = {&b.pos.x, &b.pos.y, &b.pos.z};
        float* v[3] = {&b.vel.x, &b.vel.y, &b.vel.z};
        const float lim[3] = {half.x, half.y, half.z};
        for (int a = 0; a < 3; a++) {
            if (std::fabs(*p[a]) > lim[a]) {
                *p[a] = std::copysign(lim[a], *p[a]);
                *v[a] = 0.0f;
            }
        }
    }

    for (Fly& f : flies_) {
        const float speed = FIREFLIES_FLY_SPEED * f.pace * mult;
        steer(f.vel, seek(f.pos, baits_[f.bait].pos, speed), speed,
              FIREFLIES_FLY_ACCEL * f.pace * mult);
        f.pos = f.pos + f.vel * STEP_S;
    }

    elapsed_ms_ += FIREFLIES_STEP_MS;

    const double seconds = static_cast<double>(elapsed_ms_) / 1000.0;
    for (size_t i = 0; i < baits_.size(); i++) {
        baits_[i].hue_deg = wrap360(static_cast<float>(std::fmod(
            hue_origin_[i] + static_cast<double>(FIREFLIES_HUE_RATE_DPS) * seconds, 360.0)));
    }

    wind_left_ms_ -= static_cast<int32_t>(FIREFLIES_STEP_MS);
    if (wind_left_ms_ <= 0) {
        new_wind_heading(rng);
    }
    update_wind();

    if (elapsed_ms_ % FIREFLIES_TAIL_SAMPLE_MS == 0) {
        tail_head_ = (tail_head_ + FIREFLIES_TAIL_POINTS - 1) % FIREFLIES_TAIL_POINTS;
        tail_count_ = std::min(tail_count_ + 1, FIREFLIES_TAIL_POINTS);
        for (size_t i = 0; i < flies_.size(); i++) {
            int16_t* t = &tail_[(i * FIREFLIES_TAIL_POINTS + tail_head_) * 3];
            t[0] = to_fixed(flies_[i].pos.x);
            t[1] = to_fixed(flies_[i].pos.y);
            t[2] = to_fixed(flies_[i].pos.z);
        }
    }
}

namespace {

// The camera sits CAM_DIST units in front of the box's centre, its focal length set so the box's
// middle depth plane fills the frame's height.
constexpr float CAM_DIST = 100.0f;
// Nothing nearer than this is drawn: a nearer point magnifies past 4x into long, wide spans.
constexpr float NEAREST = CAM_DIST / 4.0f;

// Sizes are for a 480 px tall frame and scale with the frame's height.
constexpr float REFERENCE_HEIGHT = 480.0f;
constexpr float DART_LENGTH_PX = 6.0f;
constexpr float DART_HALF_WIDTH_PX = 2.0f;
constexpr float HALO_RADIUS_PX = 8.0f;

// Light, of 255 per channel: a tail at its brightest, a halo at its centre, and GLOW's gain.
constexpr float TAIL_LIGHT = 160.0f;
constexpr float HALO_LIGHT = 80.0f;
constexpr float GLOW_GAIN = 1.6f;

// The frame rows are written as whole uint32_t / uint16_t pixels, which match PixelWriter's byte
// layout only on a little-endian CPU.
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
              "pixels are written as little-endian words");

/// A world point on screen: its pixel position and how much nearer than the box's middle it is.
struct Projected {
    float x;
    float y;
    float scale;
};

/// Light added to one frame, clipped to it, and the box over what it touched.
template <typename Px> class LightPainter {
  public:
    LightPainter(FrameTarget& frame, uint8_t* tiles, int32_t tiles_x)
        : frame_(frame), w_(static_cast<int32_t>(frame.w)), h_(static_cast<int32_t>(frame.h)),
          tiles_(tiles), tiles_x_(tiles_x) {}

    void add(int32_t x, int32_t y, uint32_t r, uint32_t g, uint32_t b) {
        if (x < 0 || y < 0 || x >= w_ || y >= h_ || (r | g | b) == 0) {
            return;
        }
        Px* p = reinterpret_cast<Px*>(frame_.data + static_cast<size_t>(y) * frame_.stride) + x;
        if constexpr (sizeof(Px) == 2) {
            add_light(p, r, g, b, BAYER_4X4[y & 3][x & 3]);
        } else {
            add_light(p, r, g, b);
        }
    }

    /// Marks every tile the box (x1, y1)-(x2, y2) overlaps, clipped to the frame.
    void touch(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
        const int32_t tx1 = std::max(x1, 0) / FIREFLIES_TILE_PX;
        const int32_t ty1 = std::max(y1, 0) / FIREFLIES_TILE_PX;
        const int32_t tx2 = std::min(x2, w_ - 1) / FIREFLIES_TILE_PX;
        const int32_t ty2 = std::min(y2, h_ - 1) / FIREFLIES_TILE_PX;
        for (int32_t ty = ty1; ty <= ty2; ty++) {
            uint8_t* row = tiles_ + static_cast<size_t>(ty) * static_cast<size_t>(tiles_x_);
            std::fill(row + tx1, row + tx2 + 1, uint8_t{1});
        }
    }

    /// True when the box (x1, y1)-(x2, y2) misses the frame.
    bool outside(int32_t x1, int32_t y1, int32_t x2, int32_t y2) const {
        return x2 < 0 || y2 < 0 || x1 >= w_ || y1 >= h_;
    }

  private:
    FrameTarget& frame_;
    int32_t w_;
    int32_t h_;
    uint8_t* tiles_;
    int32_t tiles_x_;
};

uint32_t scaled(uint8_t channel, float light) {
    return static_cast<uint32_t>(static_cast<float>(channel) * light / 255.0f);
}

int32_t round_px(float v) {
    return static_cast<int32_t>(std::lround(v));
}

} // namespace

template <typename Px> void FirefliesSim::draw(FrameTarget& frame, std::vector<DirtyRect>& dirty) {
    // Black out what the last frame drew, tile by tile.
    const auto black = static_cast<Px>(sizeof(Px) == 4 ? 0xFF000000u : 0u);
    for (int32_t ty = 0; ty < tiles_y_; ty++) {
        const uint8_t* row = &drawn_tiles_[static_cast<size_t>(ty) * static_cast<size_t>(tiles_x_)];
        for (int32_t tx = 0; tx < tiles_x_;) {
            if (row[tx] == 0) {
                tx++;
                continue;
            }
            const int32_t start = tx;
            while (tx < tiles_x_ && row[tx] != 0) {
                tx++;
            }
            const int32_t x1 = start * FIREFLIES_TILE_PX;
            const int32_t x2 = std::min(tx * FIREFLIES_TILE_PX, w_);
            const int32_t y_end = std::min((ty + 1) * FIREFLIES_TILE_PX, h_);
            for (int32_t y = ty * FIREFLIES_TILE_PX; y < y_end; y++) {
                Px* out = reinterpret_cast<Px*>(frame.data + static_cast<size_t>(y) * frame.stride);
                std::fill(out + x1, out + x2, black);
            }
        }
    }
    std::fill(now_tiles_.begin(), now_tiles_.end(), uint8_t{0});
    const FirefliesLevel& lv = FIREFLIES_LEVELS[level_];
    const float height_scale = static_cast<float>(h_) / REFERENCE_HEIGHT;
    const float glow = mode_ == FirefliesMode::GLOW ? GLOW_GAIN : 1.0f;
    const bool halo = mode_ == FirefliesMode::GLOW && lv.halo;
    const float half_w = static_cast<float>(w_) / 2.0f;
    const float half_h = static_cast<float>(h_) / 2.0f;
    const auto project = [&](FireflyVec p, Projected& out) {
        const float depth = p.z + CAM_DIST;
        if (depth < NEAREST) {
            return false;
        }
        const float k = focal_ / depth;
        out = {half_w + p.x * k, half_h + p.y * k, CAM_DIST / depth};
        return true;
    };

    const size_t count = std::min<size_t>(lv.max_flies, flies_.size());
    const uint32_t since_sample = static_cast<uint32_t>(elapsed_ms_ % FIREFLIES_TAIL_SAMPLE_MS);
    const auto tail_ms = static_cast<float>(lv.tail_ms);
    for (size_t i = 0; i < count; i++) {
        const Fly& fly = flies_[i];
        LightPainter<Px> paint(frame, now_tiles_.data(), tiles_x_);
        const Rgb color = hue_color(static_cast<uint32_t>(fly_hue_deg(i) * 256.0f / 360.0f));

        // The tail, head to oldest: one segment per pair of recorded points, each point blown
        // downwind by its age.
        Projected a{};
        if (!project(fly.pos, a)) {
            continue;
        }
        const Projected head = a;
        float age_a = 0.0f;
        // Whether the previous segment drew its last point, which this one then skips.
        bool joined = false;
        const int16_t* tail = &tail_[i * FIREFLIES_TAIL_POINTS * 3];
        for (size_t k = 0; k < tail_count_; k++) {
            const size_t slot = (tail_head_ + k) % FIREFLIES_TAIL_POINTS;
            const float age_b = static_cast<float>(since_sample + k * FIREFLIES_TAIL_SAMPLE_MS);
            if (age_b > tail_ms) {
                break;
            }
            const FireflyVec wp{static_cast<float>(tail[slot * 3]) / TAIL_FIXED_SCALE,
                                static_cast<float>(tail[slot * 3 + 1]) / TAIL_FIXED_SCALE,
                                static_cast<float>(tail[slot * 3 + 2]) / TAIL_FIXED_SCALE};
            Projected b{};
            if (!project(wp + wind_ * (age_b / 1000.0f), b)) {
                break;
            }
            const float ua = age_a / tail_ms;
            const float ub = age_b / tail_ms;
            const float light_a = fireflies_tail_brightness(ua) * TAIL_LIGHT * glow;
            const float light_b = fireflies_tail_brightness(ub) * TAIL_LIGHT * glow;
            const float max_px =
                static_cast<float>(lv.tail_width_px) * height_scale * (a.scale + b.scale) / 2.0f;
            const int32_t width = fireflies_tail_width((ua + ub) / 2.0f, max_px);
            const int32_t x0 = round_px(a.x);
            const int32_t y0 = round_px(a.y);
            const int32_t x1 = round_px(b.x);
            const int32_t y1 = round_px(b.y);
            const int32_t lo = -(width - 1) / 2;
            const int32_t hi = width / 2;
            const int32_t bx1 = std::min(x0, x1) + lo;
            const int32_t by1 = std::min(y0, y1) + lo;
            const int32_t bx2 = std::max(x0, x1) + hi;
            const int32_t by2 = std::max(y0, y1) + hi;
            if ((light_a > 0.0f || light_b > 0.0f) && !paint.outside(bx1, by1, bx2, by2)) {
                paint.touch(bx1, by1, bx2, by2);
                const bool x_major = std::abs(x1 - x0) >= std::abs(y1 - y0);
                const int32_t n = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
                int32_t t = 0;
                PixelWriter::line(x0, y0, x1, y1, [&](int32_t x, int32_t y) {
                    // A segment's first point is the previous segment's last.
                    if (t++ == 0 && joined) {
                        return;
                    }
                    const float f = n == 0 ? 0.0f : static_cast<float>(t - 1) / n;
                    const float light = light_a + (light_b - light_a) * f;
                    const uint32_t r = scaled(color.r, light);
                    const uint32_t g = scaled(color.g, light);
                    const uint32_t bl = scaled(color.b, light);
                    for (int32_t d = lo; d <= hi; d++) {
                        if (x_major) {
                            paint.add(x, y + d, r, g, bl);
                        } else {
                            paint.add(x + d, y, r, g, bl);
                        }
                    }
                });
                joined = true;
            } else {
                joined = false;
            }
            a = b;
            age_a = age_b;
        }

        // The body: a dart pointing along the fly's flight, in its colour half way to white.
        Projected ahead{};
        if (!project(fly.pos + fly.vel * 0.05f, ahead)) {
            ahead = {head.x + 1.0f, head.y, head.scale};
        }
        float dx = ahead.x - head.x;
        float dy = ahead.y - head.y;
        const float dlen = std::sqrt(dx * dx + dy * dy);
        if (dlen < 1e-3f) {
            dx = 1.0f;
            dy = 0.0f;
        } else {
            dx /= dlen;
            dy /= dlen;
        }
        const float len = DART_LENGTH_PX * height_scale * head.scale;
        const float half = DART_HALF_WIDTH_PX * height_scale * head.scale;
        const float bxc = head.x - dx * len;
        const float byc = head.y - dy * len;
        const float vx[3] = {head.x, bxc - dy * half, bxc + dy * half};
        const float vy[3] = {head.y, byc + dx * half, byc - dx * half};
        const auto body_light = [&](uint8_t c) {
            return std::min<uint32_t>(
                255, static_cast<uint32_t>(static_cast<float>((c + 255u) / 2u) * glow));
        };
        const uint32_t br = body_light(color.r);
        const uint32_t bg = body_light(color.g);
        const uint32_t bb = body_light(color.b);
        const int32_t tx1 = static_cast<int32_t>(std::floor(std::min({vx[0], vx[1], vx[2]})));
        const int32_t ty1 = static_cast<int32_t>(std::floor(std::min({vy[0], vy[1], vy[2]})));
        const int32_t tx2 = static_cast<int32_t>(std::ceil(std::max({vx[0], vx[1], vx[2]})));
        const int32_t ty2 = static_cast<int32_t>(std::ceil(std::max({vy[0], vy[1], vy[2]})));
        if (!paint.outside(tx1, ty1, tx2, ty2)) {
            paint.touch(tx1, ty1, tx2, ty2);
            const auto edge = [&](int e, float px, float py) {
                const int n = (e + 1) % 3;
                return (vx[n] - vx[e]) * (py - vy[e]) - (vy[n] - vy[e]) * (px - vx[e]);
            };
            const float area = edge(0, vx[2], vy[2]);
            bool any = false;
            for (int32_t y = std::max(ty1, 0); y <= std::min(ty2, h_ - 1); y++) {
                for (int32_t x = std::max(tx1, 0); x <= std::min(tx2, w_ - 1); x++) {
                    const float px = static_cast<float>(x) + 0.5f;
                    const float py = static_cast<float>(y) + 0.5f;
                    const float e0 = edge(0, px, py);
                    const float e1 = edge(1, px, py);
                    const float e2 = edge(2, px, py);
                    const bool in = area >= 0.0f ? (e0 >= 0 && e1 >= 0 && e2 >= 0)
                                                 : (e0 <= 0 && e1 <= 0 && e2 <= 0);
                    if (in) {
                        paint.add(x, y, br, bg, bb);
                        any = true;
                    }
                }
            }
            if (!any) {
                paint.add(round_px(head.x), round_px(head.y), br, bg, bb);
            }
        }

        if (halo) {
            const float radius = HALO_RADIUS_PX * height_scale;
            const int32_t r = std::max<int32_t>(1, static_cast<int32_t>(std::ceil(radius)));
            const int32_t cx = round_px(head.x);
            const int32_t cy = round_px(head.y);
            if (!paint.outside(cx - r, cy - r, cx + r, cy + r)) {
                paint.touch(cx - r, cy - r, cx + r, cy + r);
                for (int32_t y = -r; y <= r; y++) {
                    for (int32_t x = -r; x <= r; x++) {
                        const float d = std::sqrt(static_cast<float>(x * x + y * y)) / radius;
                        if (d >= 1.0f) {
                            continue;
                        }
                        const float light = (1.0f - d) * (1.0f - d) * HALO_LIGHT;
                        paint.add(cx + x, cy + y, scaled(color.r, light), scaled(color.g, light),
                                  scaled(color.b, light));
                    }
                }
            }
        }
    }

    collect_dirty(dirty);
}

void FirefliesSim::collect_dirty(std::vector<DirtyRect>& dirty) {
    // Runs of drawn tiles in a row join across short gaps, a run spanning the same columns as
    // one in the row above extends it, and the rest merge down to FIREFLIES_MAX_DIRTY_AREAS.
    constexpr int32_t JOIN_GAP_TILES = 6;
    dirty.clear();
    for (int32_t ty = 0; ty < tiles_y_; ty++) {
        const size_t base = static_cast<size_t>(ty) * static_cast<size_t>(tiles_x_);
        const int32_t y1 = ty * FIREFLIES_TILE_PX;
        const int32_t y2 = std::min((ty + 1) * FIREFLIES_TILE_PX, h_) - 1;
        int32_t run_start = -1;
        int32_t run_end = -1;
        const auto flush = [&]() {
            const int32_t x1 = run_start * FIREFLIES_TILE_PX;
            const int32_t x2 = std::min((run_end + 1) * FIREFLIES_TILE_PX, w_) - 1;
            for (DirtyRect& r : dirty) {
                if (r.x1 == x1 && r.x2 == x2 && r.y2 == y1 - 1) {
                    r.y2 = y2;
                    return;
                }
            }
            dirty.push_back({x1, y1, x2, y2});
        };
        for (int32_t tx = 0; tx < tiles_x_; tx++) {
            if ((drawn_tiles_[base + static_cast<size_t>(tx)] |
                 now_tiles_[base + static_cast<size_t>(tx)]) == 0) {
                continue;
            }
            if (run_start >= 0 && tx - run_end > JOIN_GAP_TILES) {
                flush();
                run_start = -1;
            }
            if (run_start < 0) {
                run_start = tx;
            }
            run_end = tx;
        }
        if (run_start >= 0) {
            flush();
        }
    }
    merge_dirty_areas(dirty, FIREFLIES_MAX_DIRTY_AREAS);
    drawn_tiles_.swap(now_tiles_);
}

void FirefliesSim::step(uint32_t dt_ms, FrameTarget& frame, std::minstd_rand& rng,
                        std::vector<DirtyRect>& dirty) {
    level_ = pending_level_;
    const uint32_t steps = steps_.steps_due(dt_ms, FIREFLIES_STEP_MS, MAX_STEPS_PER_FRAME);
    for (uint32_t i = 0; i < steps; i++) {
        advance_one(rng);
    }
    if (frame.format == PixelFormat::RGB565) {
        draw<uint16_t>(frame, dirty);
    } else {
        draw<uint32_t>(frame, dirty);
    }
}

} // namespace helix::ui
