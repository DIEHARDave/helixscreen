// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver_flip_clock_sim.h"

#include "screensaver_pixel_writer.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace helix::ui {

namespace {

constexpr float PI = 3.14159265358979f;

// ---------- Faces ----------

FlipCard card_of(int32_t value, bool leading_zero) {
    FlipCard card;
    value = std::clamp(value, 0, 99);
    if (value >= 10 || leading_zero) {
        card.text[0] = static_cast<char>('0' + value / 10);
        card.text[1] = static_cast<char>('0' + value % 10);
    } else {
        card.text[0] = static_cast<char>('0' + value);
    }
    return card;
}

// ---------- Glyphs ----------
//
// Digits are strokes of width GLYPH_STROKE along lines and circular arcs, in a box one unit
// wide and GLYPH_PATH_H tall (y down). Angles are degrees, 0 pointing right and growing
// clockwise on screen, so -90 is the top of a circle.

constexpr float GLYPH_PATH_H = 1.6f;
constexpr float GLYPH_STROKE = 0.2f;
constexpr float GLYPH_W = 1.0f + GLYPH_STROKE;
constexpr float GLYPH_H = GLYPH_PATH_H + GLYPH_STROKE;

struct Stroke {
    bool arc;
    float a, b, c, d, e; ///< line: x0 y0 x1 y1; arc: cx cy r start sweep
};

constexpr Stroke line(float x0, float y0, float x1, float y1) {
    return {false, x0, y0, x1, y1, 0.0f};
}
constexpr Stroke arc(float cx, float cy, float r, float start, float sweep) {
    return {true, cx, cy, r, start, sweep};
}

constexpr size_t MAX_STROKES = 4;

struct GlyphDef {
    size_t count;
    Stroke strokes[MAX_STROKES];
};

constexpr GlyphDef SIX = {3,
                          {arc(0.5f, 1.1f, 0.5f, 0.0f, 360.0f), line(0.0f, 1.1f, 0.0f, 0.5f),
                           arc(0.5f, 0.5f, 0.5f, 180.0f, 125.0f)}};

constexpr GlyphDef GLYPHS[10] = {
    {4,
     {arc(0.5f, 0.5f, 0.5f, 180.0f, 180.0f), arc(0.5f, 1.1f, 0.5f, 0.0f, 180.0f),
      line(0.0f, 0.5f, 0.0f, 1.1f), line(1.0f, 0.5f, 1.0f, 1.1f)}},
    {2, {line(0.55f, 0.0f, 0.55f, 1.6f), line(0.55f, 0.0f, 0.2f, 0.3f)}},
    {3,
     {arc(0.5f, 0.5f, 0.5f, 180.0f, 215.0f), line(0.91f, 0.787f, 0.0f, 1.6f),
      line(0.0f, 1.6f, 1.0f, 1.6f)}},
    {2, {arc(0.5f, 0.38f, 0.38f, 200.0f, 250.0f), arc(0.5f, 1.18f, 0.42f, 270.0f, 250.0f)}},
    {3,
     {line(0.72f, 0.0f, 0.72f, 1.6f), line(0.72f, 0.0f, 0.0f, 1.08f),
      line(0.0f, 1.08f, 1.0f, 1.08f)}},
    {3,
     {line(0.92f, 0.0f, 0.12f, 0.0f), line(0.12f, 0.0f, 0.09f, 0.813f),
      arc(0.5f, 1.1f, 0.5f, 215.0f, 280.0f)}},
    SIX,
    {2, {line(0.0f, 0.0f, 1.0f, 0.0f), line(1.0f, 0.0f, 0.32f, 1.6f)}},
    {2, {arc(0.5f, 0.38f, 0.38f, 0.0f, 360.0f), arc(0.5f, 1.18f, 0.42f, 0.0f, 360.0f)}},
    {0, {}}, // a six turned upside down
};

float dist_to_line(const Stroke& s, float px, float py) {
    const float vx = s.c - s.a;
    const float vy = s.d - s.b;
    const float len2 = vx * vx + vy * vy;
    float t = len2 > 0.0f ? ((px - s.a) * vx + (py - s.b) * vy) / len2 : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    return std::hypot(px - (s.a + t * vx), py - (s.b + t * vy));
}

float dist_to_arc(const Stroke& s, float px, float py) {
    const float dx = px - s.a;
    const float dy = py - s.b;
    const float dist = std::hypot(dx, dy);
    if (s.e >= 360.0f) {
        return std::fabs(dist - s.c);
    }
    const float angle = std::atan2(dy, dx) * 180.0f / PI;
    const float rel = std::fmod(angle - s.d + 720.0f, 360.0f);
    if (rel <= s.e) {
        return std::fabs(dist - s.c);
    }
    const float a0 = s.d * PI / 180.0f;
    const float a1 = (s.d + s.e) * PI / 180.0f;
    return std::min(std::hypot(px - (s.a + s.c * std::cos(a0)), py - (s.b + s.c * std::sin(a0))),
                    std::hypot(px - (s.a + s.c * std::cos(a1)), py - (s.b + s.c * std::sin(a1))));
}

/// Distance in glyph units from (px, py) to the nearest stroke centre line of `digit`.
float glyph_distance(int digit, float px, float py) {
    // Nine is six turned half a turn about the path box's centre.
    if (digit == 9) {
        return glyph_distance(6, 1.0f - px, GLYPH_PATH_H - py);
    }
    const GlyphDef& def = GLYPHS[digit];
    float best = 1e9f;
    for (size_t i = 0; i < def.count; i++) {
        const Stroke& s = def.strokes[i];
        best = std::min(best, s.arc ? dist_to_arc(s, px, py) : dist_to_line(s, px, py));
    }
    return best;
}

// ---------- Layout ----------
//
// Sizes in glyph units: a card holds two digit boxes side by side.

constexpr float CARD_PAD_X = 0.32f;
constexpr float CARD_PAD_Y = 0.45f;
constexpr float DIGIT_GAP = 0.18f;
constexpr float CARD_GAP = 0.3f;
constexpr float CAPTION_GAP = 0.2f;
constexpr float CAPTION_H = 0.45f;
/// Share of the screen the layout fills along its tighter axis; the rest is drift room.
constexpr float FILL = 0.9f;
constexpr int32_t MIN_CAPTION_H = 16;

struct Metrics {
    int32_t glyph_w;
    int32_t glyph_h;
    int32_t pad_x;
    int32_t pad_y;
    int32_t digit_gap;
    int32_t card_w;
    int32_t card_h;
    int32_t card_gap;
    int32_t caption_gap;
    int32_t caption_h;
};

Metrics metrics_for(int32_t unit) {
    const auto px = [unit](float v) { return static_cast<int32_t>(std::lround(v * unit)); };
    Metrics m{};
    m.glyph_w = px(GLYPH_W);
    m.glyph_h = px(GLYPH_H);
    m.pad_x = px(CARD_PAD_X);
    m.pad_y = px(CARD_PAD_Y);
    m.digit_gap = px(DIGIT_GAP);
    m.card_w = 2 * m.glyph_w + m.digit_gap + 2 * m.pad_x;
    m.card_h = m.glyph_h + 2 * m.pad_y;
    m.card_gap = px(CARD_GAP);
    m.caption_gap = px(CAPTION_GAP);
    m.caption_h = std::max(MIN_CAPTION_H, px(CAPTION_H));
    return m;
}

struct Arrangement {
    int32_t unit;
    bool column;
    int32_t content_w;
    int32_t content_h;
};

/// Only the countdown, three cards, has a line under it; the clock centres on its own.
bool has_caption(uint8_t count) {
    return count >= FLIP_MAX_CARDS;
}

Arrangement arrange(int32_t w, int32_t h, uint8_t count) {
    const float n = count;
    const float card_w = 2 * GLYPH_W + DIGIT_GAP + 2 * CARD_PAD_X;
    const float card_h = GLYPH_H + 2 * CARD_PAD_Y;
    const float tail = has_caption(count) ? CAPTION_GAP + CAPTION_H : 0.0f;
    const float row = std::min(FILL * static_cast<float>(w) / (n * card_w + (n - 1) * CARD_GAP),
                               FILL * static_cast<float>(h) / (card_h + tail));
    const float col =
        std::min(FILL * static_cast<float>(w) / card_w,
                 FILL * static_cast<float>(h) / (n * card_h + (n - 1) * CARD_GAP + tail));
    Arrangement a{};
    a.column = col > row;
    a.unit = std::max(1, static_cast<int32_t>(std::floor(a.column ? col : row)));
    const Metrics m = metrics_for(a.unit);
    const int32_t tail_px = has_caption(count) ? m.caption_gap + m.caption_h : 0;
    if (a.column) {
        a.content_w = m.card_w;
        a.content_h = count * m.card_h + (count - 1) * m.card_gap + tail_px;
    } else {
        a.content_w = count * m.card_w + (count - 1) * m.card_gap;
        a.content_h = m.card_h + tail_px;
    }
    return a;
}

uint8_t blend_channel(uint8_t from, uint8_t to, uint32_t cov) {
    return static_cast<uint8_t>((from * (255u - cov) + to * cov + 127u) / 255u);
}

Rgb blend(Rgb from, Rgb to, uint32_t cov) {
    return {blend_channel(from.r, to.r, cov), blend_channel(from.g, to.g, cov),
            blend_channel(from.b, to.b, cov)};
}

Rgb shade(Rgb c, uint32_t level) {
    return {static_cast<uint8_t>(c.r * level / 255u), static_cast<uint8_t>(c.g * level / 255u),
            static_cast<uint8_t>(c.b * level / 255u)};
}

} // namespace

FlipFace clock_face(int32_t seconds_of_day, bool use_24h) {
    const int32_t sod = ((seconds_of_day % 86400) + 86400) % 86400;
    const int32_t hour = sod / 3600;
    const int32_t minute = sod / 60 % 60;
    FlipFace face;
    face.card_count = 2;
    if (use_24h) {
        face.cards[0] = card_of(hour, true);
    } else {
        face.cards[0] = card_of(hour % 12 == 0 ? 12 : hour % 12, false);
        face.meridiem = hour < 12 ? FlipMeridiem::AM : FlipMeridiem::PM;
    }
    face.cards[1] = card_of(minute, true);
    return face;
}

FlipFace countdown_face(int32_t seconds) {
    const int32_t s = std::clamp(seconds, 0, FLIP_MAX_COUNTDOWN_S);
    FlipFace face;
    face.card_count = 3;
    face.cards[0] = card_of(s / 3600, false);
    face.cards[1] = card_of(s / 60 % 60, true);
    face.cards[2] = card_of(s % 60, true);
    return face;
}

// ---------- FlipClockState ----------

int32_t FlipClockState::countdown_s() const {
    return std::max(0, anchor_s_ - static_cast<int32_t>(since_anchor_ms_ / 1000));
}

void FlipClockState::reset() {
    *this = FlipClockState{};
}

void FlipClockState::step(uint32_t dt_ms, const FlipClockInput& in) {
    if (mode_ == FlipMode::COUNTDOWN) {
        since_anchor_ms_ += dt_ms;
    }
    if (mode_ == FlipMode::DONE) {
        done_ms_ += dt_ms;
    }
    const auto anchor = [this](int32_t seconds) {
        anchored_ = true;
        anchor_s_ = seconds;
        since_anchor_ms_ = 0;
    };

    const FlipClockPrint& print = in.print;
    switch (print.phase) {
    case FlipPrintPhase::PREPARING:
    case FlipPrintPhase::PRINTING:
        if (!anchored_ && print.remaining_s > 0) {
            anchor(print.remaining_s);
        } else if (anchored_ && print.remaining_s >= 0 &&
                   std::abs(print.remaining_s - countdown_s()) > FLIP_COUNTDOWN_RESYNC_S) {
            anchor(print.remaining_s);
        }
        mode_ = anchored_ ? FlipMode::COUNTDOWN : FlipMode::CLOCK;
        break;
    case FlipPrintPhase::PAUSED:
        if (!anchored_ && print.remaining_s > 0) {
            anchor(print.remaining_s);
        }
        if (anchored_) {
            anchor(countdown_s());
            mode_ = FlipMode::PAUSED;
        } else {
            mode_ = FlipMode::CLOCK;
        }
        break;
    case FlipPrintPhase::COMPLETE:
        if (mode_ == FlipMode::COUNTDOWN || mode_ == FlipMode::PAUSED) {
            mode_ = FlipMode::DONE;
            done_ms_ = 0;
        } else if (mode_ == FlipMode::DONE && done_ms_ >= FLIP_DONE_HOLD_MS) {
            mode_ = FlipMode::CLOCK;
        }
        anchored_ = false;
        break;
    case FlipPrintPhase::NONE:
        mode_ = FlipMode::CLOCK;
        anchored_ = false;
        break;
    }
    face_ = build_face(in);
}

FlipFace FlipClockState::build_face(const FlipClockInput& in) const {
    switch (mode_) {
    case FlipMode::CLOCK:
        return clock_face(in.seconds_of_day, in.use_24h);
    case FlipMode::COUNTDOWN: {
        FlipFace face = countdown_face(countdown_s());
        face.caption = FlipCaption::DONE_AT;
        const int32_t sod = ((in.seconds_of_day % 86400) + 86400) % 86400;
        face.eta_minute_of_day =
            static_cast<int32_t>((static_cast<int64_t>(sod) + countdown_s()) / 60 % 1440);
        face.progress_pct = std::clamp(in.print.progress_pct, 0, 100);
        return face;
    }
    case FlipMode::PAUSED: {
        FlipFace face = countdown_face(countdown_s());
        face.caption = FlipCaption::PAUSED;
        return face;
    }
    case FlipMode::DONE: {
        FlipFace face = countdown_face(0);
        face.caption = FlipCaption::DONE;
        return face;
    }
    }
    return clock_face(in.seconds_of_day, in.use_24h);
}

// ---------- Layout ----------

DirtyRect FlipLayout::bounds() const {
    DirtyRect box;
    for (size_t i = 0; i < card_count; i++) {
        box.add(cards[i]);
    }
    box.add(caption);
    return box;
}

FlipLayout layout_flip_cards(int32_t w, int32_t h, uint8_t card_count, int32_t dx, int32_t dy) {
    card_count = static_cast<uint8_t>(std::clamp<int>(card_count, 1, FLIP_MAX_CARDS));
    const Arrangement a = arrange(w, h, card_count);
    const Metrics m = metrics_for(a.unit);
    FlipLayout layout;
    layout.unit = a.unit;
    layout.card_count = card_count;
    const int32_t x0 = (w - a.content_w) / 2 + dx;
    const int32_t y0 = (h - a.content_h) / 2 + dy;
    for (int32_t i = 0; i < card_count; i++) {
        const int32_t cx = a.column ? x0 : x0 + i * (m.card_w + m.card_gap);
        const int32_t cy = a.column ? y0 + i * (m.card_h + m.card_gap) : y0;
        layout.cards[i] = {cx, cy, cx + m.card_w - 1, cy + m.card_h - 1};
    }
    const int32_t cards_bottom = layout.cards[card_count - 1].y2;
    if (has_caption(card_count)) {
        layout.caption = {x0, cards_bottom + 1 + m.caption_gap, x0 + a.content_w - 1,
                          cards_bottom + m.caption_gap + m.caption_h};
    }

    const int32_t split = std::max(2, static_cast<int32_t>(std::lround(0.04f * a.unit)));
    layout.split_top = (m.card_h - split) / 2;
    layout.split_bottom = layout.split_top + split;
    layout.corner = std::max(3, static_cast<int32_t>(std::lround(0.14f * a.unit)));

    const DirtyRect& first = layout.cards[0];
    const int32_t mx = first.x1 + layout.corner;
    const int32_t my = first.y1 + std::max(1, static_cast<int32_t>(std::lround(0.06f * a.unit)));
    layout.meridiem = {mx, my, mx + static_cast<int32_t>(std::lround(0.9f * a.unit)) - 1,
                       my + std::max(8, static_cast<int32_t>(std::lround(0.34f * a.unit))) - 1};
    layout.meridiem.y2 = std::min(layout.meridiem.y2, first.y1 + m.pad_y - 1);
    return layout;
}

void flip_drift_range(int32_t w, int32_t h, uint8_t card_count, int32_t& max_dx, int32_t& max_dy) {
    const Arrangement a = arrange(w, h, card_count);
    max_dx = std::max(0, std::min((w - a.content_w) / 2 - 1, std::max(4, w / 25)));
    max_dy = std::max(0, std::min((h - a.content_h) / 2 - 1, std::max(4, h / 25)));
}

void flip_drift_at(uint32_t elapsed_ms, int32_t max_dx, int32_t max_dy, int32_t& dx, int32_t& dy) {
    const auto step = static_cast<float>(elapsed_ms / FLIP_DRIFT_STEP_MS);
    // Two incommensurate rates, so the path does not retrace itself for hours.
    dx = static_cast<int32_t>(std::lround(static_cast<float>(max_dx) * std::sin(step * 0.61f)));
    dy = static_cast<int32_t>(std::lround(static_cast<float>(max_dy) * std::sin(step * 0.37f)));
}

// ---------- FlipClockSim ----------

void FlipClockSim::init(FrameTarget& frame, size_t level, const FlipFace& face) {
    level_ = std::min(level, FLIP_CLOCK_LEVEL_COUNT - 1);
    elapsed_ms_ = 0;
    frame_w_ = static_cast<int32_t>(frame.w);
    frame_h_ = static_cast<int32_t>(frame.h);
    drift_x_ = 0;
    drift_y_ = 0;
    PixelWriter(frame).fill(BACKGROUND);
    layout_ = layout_flip_cards(frame_w_, frame_h_, face.card_count, 0, 0);
    build_glyphs();
    for (size_t i = 0; i < FLIP_MAX_CARDS; i++) {
        anims_[i] = {face.cards[i], face.cards[i], FLIP_DURATION_MS};
    }
    for (size_t i = 0; i < layout_.card_count; i++) {
        draw_card(frame, i);
    }
}

void FlipClockSim::redraw_card(FrameTarget& frame, size_t i, std::vector<DirtyRect>& dirty) const {
    if (i >= layout_.card_count) {
        return;
    }
    draw_card(frame, i);
    dirty.push_back(layout_.cards[i]);
}

void FlipClockSim::request_level(size_t level) {
    level_ = std::min(level, FLIP_CLOCK_LEVEL_COUNT - 1);
}

bool FlipClockSim::flipping() const {
    for (size_t i = 0; i < layout_.card_count; i++) {
        if (anims_[i].elapsed_ms < FLIP_DURATION_MS) {
            return true;
        }
    }
    return false;
}

float FlipClockSim::flip_progress(size_t i) const {
    if (i >= FLIP_MAX_CARDS) {
        return 1.0f;
    }
    return static_cast<float>(std::min(anims_[i].elapsed_ms, FLIP_DURATION_MS)) /
           static_cast<float>(FLIP_DURATION_MS);
}

void FlipClockSim::clear() {
    glyphs_.clear();
    glyphs_.shrink_to_fit();
    glyph_w_ = 0;
    glyph_h_ = 0;
    layout_ = {};
}

void FlipClockSim::step(uint32_t dt_ms, FrameTarget& frame, const FlipFace& face,
                        std::vector<DirtyRect>& dirty) {
    dirty.clear();
    elapsed_ms_ += dt_ms;
    const bool animate = FLIP_CLOCK_LEVELS[level_].animate;

    int32_t max_dx = 0;
    int32_t max_dy = 0;
    flip_drift_range(frame_w_, frame_h_, face.card_count, max_dx, max_dy);
    int32_t dx = 0;
    int32_t dy = 0;
    flip_drift_at(elapsed_ms_, max_dx, max_dy, dx, dy);

    if (face.card_count != layout_.card_count || dx != drift_x_ || dy != drift_y_) {
        drift_x_ = dx;
        drift_y_ = dy;
        for (size_t i = 0; i < FLIP_MAX_CARDS; i++) {
            const bool same =
                face.card_count == layout_.card_count && animate && anims_[i].to == face.cards[i];
            if (same) {
                anims_[i].elapsed_ms = std::min(anims_[i].elapsed_ms + dt_ms, FLIP_DURATION_MS);
            } else {
                anims_[i] = {face.cards[i], face.cards[i], FLIP_DURATION_MS};
            }
        }
        relayout(frame, face.card_count, dirty);
        return;
    }

    for (size_t i = 0; i < layout_.card_count; i++) {
        CardAnim& anim = anims_[i];
        bool redraw = false;
        if (face.cards[i] != anim.to) {
            anim.from = anim.to;
            anim.to = face.cards[i];
            anim.elapsed_ms = animate ? 0 : FLIP_DURATION_MS;
            redraw = true;
        } else if (anim.elapsed_ms < FLIP_DURATION_MS) {
            anim.elapsed_ms =
                animate ? std::min(anim.elapsed_ms + dt_ms, FLIP_DURATION_MS) : FLIP_DURATION_MS;
            redraw = true;
        }
        if (redraw) {
            draw_card(frame, i);
            dirty.push_back(layout_.cards[i]);
        }
    }
}

void FlipClockSim::relayout(FrameTarget& frame, uint8_t card_count, std::vector<DirtyRect>& dirty) {
    PixelWriter writer(frame);
    DirtyRect area = layout_.bounds();
    const auto clip = [this](DirtyRect r) {
        r.x1 = std::max(r.x1, 0);
        r.y1 = std::max(r.y1, 0);
        r.x2 = std::min(r.x2, frame_w_ - 1);
        r.y2 = std::min(r.y2, frame_h_ - 1);
        return r;
    };
    const DirtyRect old_area = clip(area);
    for (int32_t y = old_area.y1; y <= old_area.y2; y++) {
        for (int32_t x = old_area.x1; x <= old_area.x2; x++) {
            writer.put(x, y, BACKGROUND);
        }
    }
    const int32_t old_unit = layout_.unit;
    layout_ = layout_flip_cards(frame_w_, frame_h_, card_count, drift_x_, drift_y_);
    if (layout_.unit != old_unit || glyphs_.empty()) {
        build_glyphs();
    }
    for (size_t i = 0; i < layout_.card_count; i++) {
        draw_card(frame, i);
    }
    area.add(layout_.bounds());
    area = clip(area);
    if (!area.empty()) {
        dirty.push_back(area);
    }
}

void FlipClockSim::build_glyphs() {
    const Metrics m = metrics_for(layout_.unit);
    glyph_w_ = m.glyph_w;
    glyph_h_ = m.glyph_h;
    glyphs_.assign(static_cast<size_t>(glyph_w_) * glyph_h_ * 10, 0);
    const auto unit = static_cast<float>(layout_.unit);
    const float half = GLYPH_STROKE / 2.0f;
    for (int digit = 0; digit < 10; digit++) {
        uint8_t* out = &glyphs_[static_cast<size_t>(digit) * glyph_w_ * glyph_h_];
        for (int32_t y = 0; y < glyph_h_; y++) {
            const float py = (static_cast<float>(y) + 0.5f) / unit - half;
            for (int32_t x = 0; x < glyph_w_; x++) {
                const float px = (static_cast<float>(x) + 0.5f) / unit - half;
                const float edge = (half - glyph_distance(digit, px, py)) * unit + 0.5f;
                out[static_cast<size_t>(y) * glyph_w_ + x] =
                    static_cast<uint8_t>(std::lround(std::clamp(edge, 0.0f, 1.0f) * 255.0f));
            }
        }
    }
}

uint8_t FlipClockSim::glyph_coverage(int digit, int32_t gx, int32_t gy) const {
    if (digit < 0 || digit > 9 || gx < 0 || gy < 0 || gx >= glyph_w_ || gy >= glyph_h_) {
        return 0;
    }
    return glyphs_[(static_cast<size_t>(digit) * glyph_h_ + gy) * glyph_w_ + gx];
}

uint8_t FlipClockSim::corner_coverage(int32_t x, int32_t y) const {
    const DirtyRect& card = layout_.cards[0];
    const int32_t w = card.x2 - card.x1 + 1;
    const int32_t h = card.y2 - card.y1 + 1;
    const int32_t r = layout_.corner;
    const int32_t cx = x < r ? r : (x >= w - r ? w - r : x);
    const int32_t cy = y < r ? r : (y >= h - r ? h - r : y);
    if (cx == x && cy == y) {
        return 255;
    }
    const float dist = std::hypot(static_cast<float>(x) + 0.5f - static_cast<float>(cx),
                                  static_cast<float>(y) + 0.5f - static_cast<float>(cy));
    const float cov = std::clamp(static_cast<float>(r) - dist + 0.5f, 0.0f, 1.0f);
    return static_cast<uint8_t>(std::lround(cov * 255.0f));
}

Rgb FlipClockSim::face_pixel(const FlipCard& card, int32_t x, int32_t y) const {
    if (y >= layout_.split_top && y < layout_.split_bottom) {
        return BACKGROUND;
    }
    const Rgb base = y < layout_.split_top ? CARD_TOP : CARD_BOTTOM;
    const Metrics m = metrics_for(layout_.unit);
    const int32_t gy = y - m.pad_y;
    int32_t cov = 0;
    if (card.text[1] == '\0') {
        const int32_t left = (m.card_w - glyph_w_) / 2;
        cov = glyph_coverage(card.text[0] - '0', x - left, gy);
    } else {
        cov =
            std::max(glyph_coverage(card.text[0] - '0', x - m.pad_x, gy),
                     glyph_coverage(card.text[1] - '0', x - m.pad_x - glyph_w_ - m.digit_gap, gy));
    }
    return cov == 0 ? base : blend(base, DIGIT, static_cast<uint32_t>(cov));
}

void FlipClockSim::draw_card(FrameTarget& frame, size_t i) const {
    PixelWriter writer(frame);
    const DirtyRect& box = layout_.cards[i];
    const CardAnim& anim = anims_[i];
    const int32_t w = box.x2 - box.x1 + 1;
    const int32_t h = box.y2 - box.y1 + 1;
    const int32_t top_h = layout_.split_top;
    const int32_t bottom_h = h - layout_.split_bottom;
    const float p = static_cast<float>(std::min(anim.elapsed_ms, FLIP_DURATION_MS)) /
                    static_cast<float>(FLIP_DURATION_MS);
    const bool moving = anim.elapsed_ms < FLIP_DURATION_MS;

    // The top half folds down: first the old digits' top half shrinks onto the split line,
    // then the new digits' bottom half unfolds below it. The projected height of the flap is
    // its full height times |cos| of the fold angle.
    const float fold = std::cos(PI * p);
    int32_t flap_y1 = 0;
    int32_t flap_y2 = -1;
    if (moving && p < 0.5f) {
        const int32_t fh = static_cast<int32_t>(std::lround(top_h * fold));
        flap_y1 = top_h - fh;
        flap_y2 = top_h - 1;
    } else if (moving) {
        const int32_t fh = static_cast<int32_t>(std::lround(bottom_h * -fold));
        flap_y1 = layout_.split_bottom;
        flap_y2 = layout_.split_bottom + fh - 1;
    }
    const int32_t flap_h = flap_y2 - flap_y1 + 1;
    const auto flap_shade =
        static_cast<uint32_t>(std::lround(255.0f * (0.55f + 0.45f * std::fabs(fold))));

    for (int32_t y = 0; y < h; y++) {
        const bool in_flap = flap_h > 0 && y >= flap_y1 && y <= flap_y2;
        int32_t src_y = 0;
        if (in_flap) {
            src_y = p < 0.5f ? (y - flap_y1) * top_h / flap_h
                             : layout_.split_bottom + (y - flap_y1) * bottom_h / flap_h;
        }
        // Behind the flap the top half already shows the new digits and the bottom half
        // still the old ones.
        const FlipCard& still = (moving && y >= layout_.split_top) ? anim.from : anim.to;
        const FlipCard& flap_card = p < 0.5f ? anim.from : anim.to;
        for (int32_t x = 0; x < w; x++) {
            Rgb c = blend(BACKGROUND, face_pixel(still, x, y), corner_coverage(x, y));
            if (in_flap) {
                const Rgb flap = shade(face_pixel(flap_card, x, src_y), flap_shade);
                c = blend(c, flap, corner_coverage(x, src_y));
            }
            writer.put(box.x1 + x, box.y1 + y, c);
        }
    }
}

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
