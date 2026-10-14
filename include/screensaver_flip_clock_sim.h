// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "screensaver_frame.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <vector>

/**
 * @file screensaver_flip_clock_sim.h
 * @brief A split-flap clock that counts a running print down, stepped one frame at a time
 *
 * No LVGL, no clock and no allocation per frame: the saver passes the frame time, the local
 * time of day and the print's state, so a run replays exactly and runs in tests without a
 * display. Text the cards cannot show (the caption line, AM/PM) is described here and drawn by
 * the saver, which owns fonts and translations.
 */

namespace helix::ui {

/// One level of the flip clock ladder.
struct FlipClockLevel {
    uint32_t period_ms; ///< frame period in ms
    bool animate;       ///< fold the top half down on a change, or swap the digits at once
};

/// The ladder, most expensive level first. Nothing is drawn between changes at any level.
inline constexpr FlipClockLevel FLIP_CLOCK_LEVELS[] = {
    {SAVER_FAST_PERIOD, true},
    {33, true},
    {250, false},
};

inline constexpr size_t FLIP_CLOCK_LEVEL_COUNT = std::size(FLIP_CLOCK_LEVELS);

/// How long one card takes to flip.
inline constexpr uint32_t FLIP_DURATION_MS = 300;
/// How long a finished print shows 0:00:00 before the clock returns.
inline constexpr uint32_t FLIP_DONE_HOLD_MS = 60000;
/// A remaining-time estimate this far from the running countdown resets it; nearer ones are
/// ignored, so estimator jitter never flips the seconds backwards.
inline constexpr int32_t FLIP_COUNTDOWN_RESYNC_S = 10;
/// Longest countdown the cards show: 999:59:59.
inline constexpr int32_t FLIP_MAX_COUNTDOWN_S = 999 * 3600 + 59 * 60 + 59;
/// How often the cards drift to a new position, so no pixel stays lit in one place.
inline constexpr uint32_t FLIP_DRIFT_STEP_MS = 60000;

/// What the print is doing, as far as the clock cares.
enum class FlipPrintPhase : uint8_t { NONE, PREPARING, PRINTING, PAUSED, COMPLETE };

/// The print, as the clock reads it.
struct FlipClockPrint {
    FlipPrintPhase phase = FlipPrintPhase::NONE;
    int32_t remaining_s = -1; ///< print time left, negative when unknown
    int32_t progress_pct = 0;
};

/// One frame's worth of outside state.
struct FlipClockInput {
    int32_t seconds_of_day = 0; ///< local wall clock, 0 to 86399
    bool use_24h = false;
    FlipClockPrint print;
};

enum class FlipMode : uint8_t { CLOCK, COUNTDOWN, PAUSED, DONE };

/// The line under the cards.
enum class FlipCaption : uint8_t { NONE, DONE_AT, PAUSED, DONE };

/// The 12-hour marker shown on the hour card.
enum class FlipMeridiem : uint8_t { NONE, AM, PM };

/// One card: a single digit, or blank.
struct FlipCard {
    char digit = '\0'; ///< '0' to '9', or NUL for a blank card

    bool operator==(const FlipCard& o) const {
        return digit == o.digit;
    }
    bool operator!=(const FlipCard& o) const {
        return !(*this == o);
    }
};

/// Hours, minutes, seconds.
inline constexpr size_t FLIP_MAX_GROUPS = 3;
/// Three hour cards, two minute cards, two second cards.
inline constexpr size_t FLIP_MAX_CARDS = 7;

/// How the cards fall into groups (hours, minutes and seconds), one card per digit.
struct FlipShape {
    uint8_t group_count = 0;
    std::array<uint8_t, FLIP_MAX_GROUPS> groups{}; ///< cards in each group

    uint8_t card_count() const;
    /// Only the countdown, three groups, has a line under its cards.
    bool has_caption() const {
        return group_count >= FLIP_MAX_GROUPS;
    }
    bool operator==(const FlipShape& o) const {
        return group_count == o.group_count && groups == o.groups;
    }
    bool operator!=(const FlipShape& o) const {
        return !(*this == o);
    }
};

/// Everything the screen shows, without pixels.
struct FlipFace {
    FlipShape shape;
    std::array<FlipCard, FLIP_MAX_CARDS> cards{}; ///< shape.card_count() of them, left to right
    FlipMeridiem meridiem = FlipMeridiem::NONE;
    FlipCaption caption = FlipCaption::NONE;
    int32_t eta_minute_of_day = 0; ///< DONE_AT: local minute the print ends, 0 to 1439
    int32_t progress_pct = 0;      ///< DONE_AT: percent done
};

/// Two hour and two minute cards for `seconds_of_day`: 24-hour with a leading zero, or
/// 12-hour with the leading card blank below 10 and an AM/PM marker.
FlipFace clock_face(int32_t seconds_of_day, bool use_24h);

/// One card per digit of the hours (no leading zero, as many as needed), minutes and seconds
/// for `seconds`, clamped to 0 and FLIP_MAX_COUNTDOWN_S.
FlipFace countdown_face(int32_t seconds);

/**
 * @brief Decides what the clock shows from the print's state over time
 *
 * Idle, preparing without an estimate, cancelled and failed prints show the time of day. A
 * print with an estimate counts down at wall-clock rate, frozen while paused. A print seen
 * finishing shows 0:00:00 and DONE for FLIP_DONE_HOLD_MS, then the time of day again.
 */
class FlipClockState {
  public:
    /// Advances by `dt_ms` with the outside state now `in`.
    void step(uint32_t dt_ms, const FlipClockInput& in);

    FlipMode mode() const {
        return mode_;
    }
    /// Seconds the countdown shows.
    int32_t countdown_s() const;
    /// What the screen shows for the last step's input.
    FlipFace face() const {
        return face_;
    }
    void reset();

  private:
    FlipFace build_face(const FlipClockInput& in) const;

    FlipMode mode_ = FlipMode::CLOCK;
    bool anchored_ = false;
    int32_t anchor_s_ = 0;
    uint32_t since_anchor_ms_ = 0;
    uint32_t done_ms_ = 0;
    bool done_shown_ = false;
    FlipFace face_;
};

/// Where the cards and their text sit on a screen.
struct FlipLayout {
    int32_t unit = 0; ///< glyph scale: pixels per glyph width unit
    FlipShape shape;
    uint8_t card_count = 0;
    std::array<DirtyRect, FLIP_MAX_CARDS> cards{};
    uint8_t colon_count = 0;
    /// The colon between two groups in a row; a column of groups has none.
    std::array<DirtyRect, FLIP_MAX_GROUPS - 1> colons{};
    DirtyRect caption;        ///< the line under the cards, empty for the clock
    DirtyRect meridiem;       ///< AM/PM, in the first card's top-left corner
    int32_t split_top = 0;    ///< first row of the split line, relative to a card's top
    int32_t split_bottom = 0; ///< first row below the split line
    int32_t corner = 0;       ///< card corner radius

    /// Box around every card and the caption.
    DirtyRect bounds() const;
};

/// Lays out the cards of `shape` as large as fit on a `w` x `h` screen: every group in one
/// row with a colon between groups, or, when that makes them larger (portrait), one group per
/// row; centred, then moved by (`dx`, `dy`).
FlipLayout layout_flip_cards(int32_t w, int32_t h, const FlipShape& shape, int32_t dx, int32_t dy);

/// Largest drift either way that keeps the layout of `shape` on a `w` x `h` screen.
void flip_drift_range(int32_t w, int32_t h, const FlipShape& shape, int32_t& max_dx,
                      int32_t& max_dy);

/// Drift offset `elapsed_ms` into a run, stepping every FLIP_DRIFT_STEP_MS within the range.
void flip_drift_at(uint32_t elapsed_ms, int32_t max_dx, int32_t max_dy, int32_t& dx, int32_t& dy);

/**
 * @brief Draws the clock into a frame, redrawing only the cards that change
 *
 * Each digit has its own card. A card whose digit changes folds its top half down over
 * FLIP_DURATION_MS at an animating level, or swaps at once otherwise; cards that change
 * together fold together, and the rest are not touched. A changed shape or a drift step
 * repaints the whole layout. Between changes a frame writes nothing.
 */
class FlipClockSim {
  public:
    /// Lays out `face` for `frame`, paints the screen and draws the cards.
    void init(FrameTarget& frame, size_t level, const FlipFace& face);

    /**
     * @brief Advances by `dt_ms` towards showing `face` and draws into `frame`
     *
     * Replaces `dirty` with one box per card redrawn, or one box over the old and new layout
     * after a repaint.
     */
    void step(uint32_t dt_ms, FrameTarget& frame, const FlipFace& face,
              std::vector<DirtyRect>& dirty);

    /// Draws card `i` again as it stands and adds its box to `dirty`, for text drawn over it
    /// that has to go.
    void redraw_card(FrameTarget& frame, size_t i, std::vector<DirtyRect>& dirty) const;

    /// Runs at `level` (clamped) from now on; a flip in progress finishes at once when the new
    /// level does not animate.
    void request_level(size_t level);

    size_t level() const {
        return level_;
    }
    const FlipLayout& layout() const {
        return layout_;
    }
    /// True while any card is mid-flip.
    bool flipping() const;
    /// Share of the flip done for card `i`, 1 when it is still.
    float flip_progress(size_t i) const;

    /// Frees the glyph cache.
    void clear();

    /// Background, card, split-line and digit colours.
    static constexpr Rgb BACKGROUND{0, 0, 0};
    static constexpr Rgb CARD_TOP{30, 30, 32};
    static constexpr Rgb CARD_BOTTOM{24, 24, 26};
    static constexpr Rgb DIGIT{222, 222, 222};
    static constexpr Rgb COLON{130, 130, 130};

  private:
    struct CardAnim {
        FlipCard from;
        FlipCard to;
        uint32_t elapsed_ms = FLIP_DURATION_MS;
    };

    void relayout(FrameTarget& frame, const FlipShape& shape, std::vector<DirtyRect>& dirty);
    void build_glyphs();
    void draw_card(FrameTarget& frame, size_t i) const;
    void draw_colons(FrameTarget& frame) const;
    /// Card face pixel at card-local (x, y) for `card`, before the corner mask.
    Rgb face_pixel(const FlipCard& card, int32_t x, int32_t y) const;
    uint8_t glyph_coverage(int digit, int32_t gx, int32_t gy) const;
    uint8_t corner_coverage(int32_t x, int32_t y) const;

    size_t level_ = 0;
    uint32_t elapsed_ms_ = 0;
    int32_t frame_w_ = 0;
    int32_t frame_h_ = 0;
    int32_t drift_x_ = 0;
    int32_t drift_y_ = 0;
    FlipLayout layout_;
    std::array<CardAnim, FLIP_MAX_CARDS> anims_{};
    int32_t glyph_w_ = 0;
    int32_t glyph_h_ = 0;
    /// Coverage of digits 0-9, glyph_w_ x glyph_h_ each, 0-255.
    std::vector<uint8_t> glyphs_;
};

} // namespace helix::ui
