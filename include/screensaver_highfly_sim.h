// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "screensaver_clock_reading.h"
#include "screensaver_frame.h"
#include "screensaver_heightfield.h"
#include "screensaver_stroke_digits.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <random>
#include <vector>

/**
 * @file screensaver_highfly_sim.h
 * @brief High Fly: flying just above a sunlit cloud deck under a deep blue sky
 *
 * Back to front: a static sky with a glow and four soft shafts, a far billow band drifting
 * sideways so the skyline is never flat, the cloud deck (the shared column renderer over a
 * baked tileable heightfield) scrolling toward the camera, translucent full-width layers and
 * now and then a towering cumulus, each depth-tested against the deck, and about once a
 * minute or two a flight through a cloud. A thin clock sits at the cloud line.
 *
 * No LVGL, no clock: the saver passes the frame time, its random sequence, the clock reading
 * and the frame. Textures are baked in init() from the seed; a frame allocates nothing.
 */

namespace helix::ui {

struct HighFlyLevel {
    uint32_t period_ms;
    uint8_t scale;   ///< the scene renders at canvas / scale
    uint16_t slices; ///< deck depth slices
    uint8_t layers;  ///< translucent layers drawn, 0 to HIGHFLY_LAYER_COUNT
    bool towers;
    bool still; ///< draw the scene once, then only the clock
};

/// Most expensive first: layers go before the deck coarsens; the last rung stands still.
inline constexpr HighFlyLevel HIGHFLY_LEVELS[] = {
    {SAVER_FAST_PERIOD, 2, 117, 3, true, false},
    {33, 2, 117, 3, true, false},
    {33, 2, 117, 0, true, false},
    {50, 4, 70, 0, false, false},
    {250, 2, 117, 3, true, true},
};
inline constexpr size_t HIGHFLY_LEVEL_COUNT = std::size(HIGHFLY_LEVELS);

inline constexpr size_t HIGHFLY_LAYER_COUNT = 3;
inline constexpr size_t HIGHFLY_TOWER_VARIANTS = 3;
/// Deck heightfield side, texels; tileable both ways.
inline constexpr int32_t HIGHFLY_FIELD = 256;
/// Deck scroll toward the camera, world units per second.
inline constexpr float HIGHFLY_SPEED = 10.0f;
/// Fly-throughs start this many seconds apart, uniformly.
inline constexpr float HIGHFLY_FLY_MIN_S = 60.0f;
inline constexpr float HIGHFLY_FLY_MAX_S = 120.0f;
/// Fly-through envelope: ease in, hold, clear, seconds.
inline constexpr float HIGHFLY_FLY_IN_S = 1.0f;
inline constexpr float HIGHFLY_FLY_HOLD_S = 0.5f;
inline constexpr float HIGHFLY_FLY_OUT_S = 1.3f;
/// Interior colour's opacity at the depth of the fly-through.
inline constexpr float HIGHFLY_WHITEOUT_MAX = 0.62f;
/// Towers enter this far apart in travel time, uniformly.
inline constexpr float HIGHFLY_TOWER_MIN_S = 13.0f;
inline constexpr float HIGHFLY_TOWER_MAX_S = 14.0f;

/// A premultiplied RGBA texture, row-major.
struct HighFlyTexture {
    int32_t w = 0;
    int32_t h = 0;
    std::vector<uint8_t> rgba; ///< premultiplied, 4 bytes per texel
    size_t heap_bytes() const;
};

/// Field texels per world unit: a world height is the stored height / HIGHFLY_TEXELS_PER_UNIT.
inline constexpr float HIGHFLY_TEXELS_PER_UNIT = 2.0f;

/// The baked deck: height (texel units) and shade (0-1) per texel, HIGHFLY_FIELD^2 row-major.
struct HighFlyField {
    std::vector<float> height;
    std::vector<float> shade;
};

/// Builds the deck at HIGHFLY_FIELD^2 from `rng`: puffs at three scales, noise, a talus pass
/// that limits each texel to HIGHFLY_TALUS above its lowest neighbour's limit, a 1-texel blur,
/// then Lambert + ambient occlusion + noise shading.
void build_highfly_field(std::minstd_rand& rng, HighFlyField& out);
/// Slope limit of the talus pass, height units per texel.
inline constexpr float HIGHFLY_TALUS = 2.0f;

/// Fly-through amount, 0 to 1, `since_start_s` after one began; 0 before and after.
float highfly_whiteout(float since_start_s);

/// How much of a billboard at depth `layer_z` shows over a deck pixel that reached half
/// coverage at `deck_z`: clamp((deck_z - layer_z) / (0.25 layer_z) + 0.5, 0, 1). 1 where no
/// deck is there (deck_z infinite).
float highfly_layer_visibility(float deck_z, float layer_z);

/// Most the fly-through interior and its wisps together may cover the scene over the clock,
/// so the pale numerals stay readable.
inline constexpr float HIGHFLY_CLOCK_COVER_MAX = 0.70f;

/// `scene` seen from inside the fly-through cloud `amt` (0 to 1) of the way in: eased toward
/// the blue-grey interior, never more than HIGHFLY_WHITEOUT_MAX of the way, then lit toward
/// white by a wisp of strength `wisp`, held so the two together cover at most `most` of the
/// scene (where the interior alone already covers more, the wisp adds nothing).
Rgb highfly_inside(Rgb scene, float amt, float wisp = 0.0f, float most = 1.0f);

/// Where the clock sits on a w x h canvas, moved by the drift (dx, dy).
struct HighFlyClockLayout {
    int32_t digit_unit = 0;            ///< big numerals' glyph unit
    int32_t second_unit = 0;           ///< seconds' glyph unit
    std::array<DirtyRect, 5> digits{}; ///< H H : M M, the hours with a leading zero
    std::array<DirtyRect, 2> seconds{};
    DirtyRect meridiem;  ///< under the seconds
    DirtyRect date_line; ///< left block, first line
    DirtyRect day_line;  ///< left block, second line
    DirtyRect bounds;
};
HighFlyClockLayout layout_highfly_clock(int32_t w, int32_t h, int32_t dx, int32_t dy);

class HighFlySim {
  public:
    friend class HighFlySimTestAccess;
    void init(const FrameTarget& frame, std::minstd_rand& rng, size_t level,
              const ClockReading& reading);
    /// Advances by `dt_ms` and draws the cloud band (the whole frame during a fly-through)
    /// and the numerals into `frame`, replacing `dirty` with the boxes written.
    void step(uint32_t dt_ms, FrameTarget& frame, std::minstd_rand& rng,
              const ClockReading& reading, std::vector<DirtyRect>& dirty);
    void request_level(size_t level);
    size_t level() const {
        return level_;
    }

    const HeightfieldRenderer& renderer() const {
        return renderer_;
    }
    const HighFlyClockLayout& clock_layout() const {
        return clock_;
    }
    /// Seconds since init(), and since the current fly-through's whiteout began (negative when
    /// none runs; the cloud it flies into shows for a while before, see next_fly_s()).
    double elapsed_s() const {
        return t_;
    }
    float fly_since_s() const;
    /// Seconds until the next fly-through and the next tower.
    float next_fly_s() const {
        return next_fly_s_;
    }
    float next_tower_s() const {
        return next_tower_s_;
    }
    /// Towers on screen now.
    size_t towers_visible() const;
    /// Top internal row of the band of rows the last frame repainted whole: the far billow
    /// band and the deck. Above it only the clock, towers and a fly-through repaint the sky.
    int32_t band_top() const {
        return band_top_;
    }
    /// Internal frame scale at the current rung.
    int32_t scale() const {
        return scale_;
    }
    /// Shape (0 to HIGHFLY_TOWER_VARIANTS - 1) of tower slot `i`, or -1 when the slot is empty.
    int tower_variant(size_t i) const;
    /// Internal-frame box tower slot `i` covers, empty when the slot is empty or off screen.
    DirtyRect tower_box(size_t i) const;
    /// Seconds into the run each tower so far entered.
    const std::vector<double>& tower_entries() const {
        return tower_entries_;
    }
    /// Seconds into the run each fly-through so far began.
    const std::vector<double>& fly_starts() const {
        return fly_starts_;
    }
    const HighFlyTexture& tower_texture(size_t i) const {
        return towers_tex_[i];
    }
    size_t heap_bytes() const;
    void clear();

  private:
    struct Tower {
        float z = 0.0f;
        double entered_s = 0.0; ///< run time it entered at TOWER_Z_ENTER
        float x = 0.0f;         ///< world x of its centre when it entered
        float drift = 0.0f;     ///< wind, world units per second sideways
        float size = 1.0f;
        uint8_t variant = 0;
        bool live = false;
    };
    /// A billboard to composite: a texture at depth z, its left world x and base height.
    struct Billboard {
        const HighFlyTexture* tex = nullptr;
        float z = 0.0f;
        float xw0 = 0.0f;
        float hb = 0.0f;
        float q = 1.0f;
        bool wrap = false;
        float alpha = 1.0f;
        float fogmul = 1.0f;
        bool depth_test = true;
    };
    class Scene;

    /// Sizes the renderer and internal buffers for level_, and with `bake` builds the static
    /// sky and the fly-through's polar table when the internal size changed.
    void apply_view(bool bake = true);
    void build_sky();
    void build_polar();
    void update_schedule(std::minstd_rand& rng);
    /// Starts a tower in `slot` that entered at run time `entered_s`.
    void spawn_tower(Tower& slot, double entered_s, std::minstd_rand& rng);
    /// The billboard tower `t` draws as.
    Billboard tower_billboard(const Tower& t) const;
    /// Composes the internal frame: rows from `from` down, plus the boxes in `boxes_`.
    void compose(int32_t from);
    void draw_billboard(const Billboard& b);
    /// Internal box a billboard covers, empty when off screen.
    DirtyRect billboard_box(const Billboard& b) const;
    size_t collect_billboards(Billboard* out, size_t cap) const;
    void draw_numerals(FrameTarget& frame, const ClockReading& reading) const;
    void update_clock_layout();
    /// The clock bounds as an internal box.
    DirtyRect clock_box_internal() const;
    /// Paints everything once and draws the numerals.
    void paint_all(FrameTarget& frame, const ClockReading& reading);

    size_t level_ = 0;
    int32_t w_ = 0;
    int32_t h_ = 0;
    int32_t scale_ = 2;
    int32_t iw_ = 0;
    int32_t ih_ = 0;
    float horizon_ = 0.0f;
    float focal_x_ = 1.0f;
    float focal_y_ = 1.0f;
    double t_ = 0.0;           ///< seconds since init(); phases are reduced from it in double
    float deck_texels_ = 0.0f; ///< the deck's scroll, reduced to one field tile
    int32_t baked_w_ = 0;      ///< internal size the sky and polar table were built for
    int32_t baked_h_ = 0;
    uint32_t elapsed_ms_ = 0;
    double fly_start_s_ = -1.0; ///< when the scheduled fly-through's whiteout begins
    float next_fly_s_ = 0.0f;
    float next_tower_s_ = 0.0f;
    double next_tower_at_ = 0.0;
    int32_t band_top_ = 0;
    int32_t band_tex_top_ = 0; ///< the far band's top row, fixed for the view
    uint8_t next_variant_ = 0;
    bool next_left_ = true;
    bool painted_ = false;
    bool last_full_ = false;
    int32_t drift_x_ = 0;
    int32_t drift_y_ = 0;
    HighFlyField field_;
    HeightfieldRenderer renderer_;
    std::vector<Rgb> sky_;       ///< internal row-major, static
    std::vector<Rgb> frame_;     ///< internal row-major
    std::vector<Rgb> slice_lut_; ///< per slice: 256 deck shades through near-white and fog
    HighFlyTexture band_;
    std::array<HighFlyTexture, HIGHFLY_LAYER_COUNT> layers_{};
    std::array<HighFlyTexture, HIGHFLY_TOWER_VARIANTS> towers_tex_{};
    std::vector<float> wisp_;     ///< polar wisp noise, 64 log-radius rows x 128 angles
    std::vector<uint16_t> polar_; ///< per internal pixel: log-radius, angle, radial weight
    std::vector<int32_t> col_u0_; ///< billboard sampling, per internal column
    std::vector<float> col_fu_;
    std::array<Tower, 2> towers_{};
    std::vector<double> tower_entries_;
    std::vector<double> fly_starts_;
    std::vector<DirtyRect> boxes_;      ///< internal boxes above the band repainted this frame
    std::vector<DirtyRect> last_boxes_; ///< and last frame
    HighFlyClockLayout clock_;
    StrokeDigitSet digits_;
    StrokeDigitSet second_digits_;
    ClockReading shown_;
};

} // namespace helix::ui
