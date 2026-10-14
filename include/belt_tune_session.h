// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "belt_tension_types.h"

#include <optional>
#include <vector>

/**
 * @file belt_tune_session.h
 * @brief The guided belt-tuning loop's logic: zones, peaks, advice and outcomes
 *
 * Pure: no LVGL, no I/O. The Belt Tension panel feeds it the round-0
 * comparison and each round's two windowed sweeps, and renders what it says.
 * A higher peak means a tighter belt, and clockwise tightens (#1736).
 */

namespace helix::calibration {

namespace belt_tune {
/// Peaks under this fraction of the stronger path's in-band maximum never
/// define a zone.
inline constexpr float ZONE_MIN_AMPLITUDE_FRACTION = 0.20f;
inline constexpr float ZONE_HALF_MIN_HZ = 10.0f; ///< narrowest half-width of a zone window
inline constexpr float ZONE_MARGIN_HZ = 5.0f;    ///< window reach beyond each zone peak
/// An unpaired tallest peak steers only against a peak this close on the other path.
inline constexpr float ZONE_FALLBACK_MAX_HZ = 3.0f * belt_verdict::PAIR_MAX_HZ;
inline constexpr float MATCHED_MIN_HZ = 1.0f; ///< matched tolerance floor
inline constexpr float MIN_MOVE_HZ = 0.5f;    ///< smaller peak moves are noise
inline constexpr float FIRST_TURN = 0.25f;
inline constexpr float TURN_STEP = 0.125f;
inline constexpr float TURN_MAX = 1.0f;
inline constexpr int STALL_ROUNDS = 8;        ///< unmatched rounds before the mechanical hint
inline constexpr int SPRING_STALL_ROUNDS = 2; ///< spring rounds in a row without improvement
/// A peak must reach this multiple of the unexcited floor (or, without one,
/// of the window's minimum).
inline constexpr float PEAK_MIN_PROMINENCE = 1.5f;
/// In-band bins outside the window needed to measure the unexcited floor.
inline constexpr int UNEXCITED_MIN_BINS = 8;
/// A maximum competes for "nearest the previous peak" only at this fraction
/// of the window's maximum.
inline constexpr float NEAR_PEAK_MIN_FRACTION = 0.5f;
inline constexpr float GAUGE_WARN_HZ = 3.0f; ///< gap gauge: warning colour up to here
} // namespace belt_tune

enum class BeltZoneSource {
    PAIRED,           ///< a peak pair whose frequencies disagree
    TALLEST_UNPAIRED, ///< an unpaired tallest peak and the other path's nearest peak
};

struct BeltTuneZone {
    BeltSweepWindow window;
    BeltZoneSource source = BeltZoneSource::PAIRED;
    float ref_hz = 0.0f; ///< reference path's peak that defined the zone
    float adj_hz = 0.0f; ///< adjusted path's peak that defined the zone
};

/// True when either path's tallest peak is unpaired and at or above the zone
/// amplitude floor: the curves differ in shape, and the mechanical-causes
/// hint shows.
[[nodiscard]] bool tallest_peak_unpaired(const BeltComparison& cmp);

/// The zone the next rounds tune. Peaks under ZONE_MIN_AMPLITUDE_FRACTION of
/// the stronger path's in-band maximum never define one, nor do pairs no more
/// than MATCHED_MIN_HZ apart or centred within ZONE_HALF_MIN_HZ of a matched
/// zone centre. When tallest_peak_unpaired(), the taller unpaired tallest peak
/// steers against the other path's nearest eligible peak within
/// ZONE_FALLBACK_MAX_HZ (TALLEST_UNPAIRED); with none that close, or
/// otherwise, the widest remaining pair defines the zone (PAIRED). The window
/// is the zone's midpoint +- max(ZONE_HALF_MIN_HZ, gap / 2 + ZONE_MARGIN_HZ),
/// clamped to the band.
[[nodiscard]] std::optional<BeltTuneZone>
pick_tune_zone(const BeltComparison& cmp, BeltPath adjusted, float band_min_hz, float band_max_hz,
               const std::vector<float>& matched_zone_centres_hz);

/// What the belt reads unexcited: the median of the curve's in-band bins
/// outside the window, which a windowed sweep does not drive. nullopt with
/// fewer than UNEXCITED_MIN_BINS such bins.
[[nodiscard]] std::optional<float> unexcited_floor_psd(const BeltCurve& curve,
                                                       BeltSweepWindow window, float band_min_hz,
                                                       float band_max_hz);

/// Whether the window's maximum reaches PEAK_MIN_PROMINENCE times the
/// unexcited floor, or, without one, times the window's minimum.
[[nodiscard]] bool window_holds_peak(const BeltCurve& curve, BeltSweepWindow window,
                                     float band_min_hz, float band_max_hz);

/// The curve's peak inside the window, refined between bins by parabolic
/// interpolation. Candidates are interior local maxima at
/// NEAR_PEAK_MIN_FRACTION of the window's maximum or more and, given a floor,
/// at PEAK_MIN_PROMINENCE times it or more. With several, the one nearest
/// near_hz, else the tallest. nullopt when none qualifies.
[[nodiscard]] std::optional<float>
refine_peak_in_window(const BeltCurve& curve, BeltSweepWindow window, std::optional<float> near_hz,
                      std::optional<float> floor_psd = std::nullopt);

/// max(MATCHED_MIN_HZ, the curve's mean bin spacing).
[[nodiscard]] float matched_tolerance_hz(const BeltCurve& curve);

enum class BeltTuneOutcome {
    CLOSER,    ///< the gap shrank and kept its sign
    OVERSHOT,  ///< the gap changed sign
    WRONG_WAY, ///< the adjusted peak moved against the advice by MIN_MOVE_HZ or more
    /// The adjusted peak moved less than MIN_MOVE_HZ, or moved the advised way
    /// while the gap did not close.
    NO_MOVEMENT,
    MATCHED,       ///< |gap| within the matched tolerance
    NO_PEAK,       ///< a path's window holds no clear peak; nothing measured
    OUT_OF_WINDOW, ///< the adjusted peak sits on the window edge; the window moved
                   ///< (once per zone until a peak is measured again; then NO_PEAK)
    BASELINE,      ///< a zone's first measurement after next_zone(): no gap history yet
};

enum class BeltTurnDirection { TIGHTEN, LOOSEN };

struct BeltTuneAdvice {
    BeltPath path = BeltPath::PATH_A;
    BeltTurnDirection direction = BeltTurnDirection::TIGHTEN;
    std::optional<float> turns; ///< SCREW only, a multiple of TURN_STEP
    bool spring_reseat = false; ///< SPRING: release, settle, lock
};

struct BeltTuneRound {
    float ref_hz = 0.0f;
    float adj_hz = 0.0f;
    float gap_hz = 0.0f;             ///< adjusted - reference
    std::optional<float> turns_made; ///< what the stepper recorded before this round
    BeltTuneOutcome outcome = BeltTuneOutcome::NO_PEAK;
};

/// One tuning run: a zone at a time, a round per pair of windowed sweeps.
/// The gap is always adjusted - reference from the same round, so drift
/// between rounds cancels; negative means the adjusted belt is looser.
class BeltTuneSession {
  public:
    BeltTuneSession(const BeltCurve& a, const BeltCurve& b, const BeltComparison& round0,
                    float band_min_hz, float band_max_hz, BeltTensionerKind kind);

    [[nodiscard]] const std::optional<BeltTuneZone>& zone() const;
    [[nodiscard]] BeltPath adjusted_path() const;
    /// Swaps reference and adjusted, for this and later zones. The window
    /// stays; the learned rate and this zone's rounds belong to the other
    /// belt, so they clear.
    void swap_adjusted_path();
    [[nodiscard]] float gap_hz() const; ///< signed, adjusted - reference
    /// Meaningless while needs_baseline(): measure the zone first.
    [[nodiscard]] BeltTuneAdvice advice() const;
    /// The stepper's value for the next round; SCREW only. Without it a round
    /// records the suggested amount (nothing on a BASELINE round).
    void record_turns(float turns);
    /// Both paths swept over the current window.
    const BeltTuneRound& add_round(const BeltCurve& a_window, const BeltCurve& b_window);
    [[nodiscard]] bool zone_matched() const;
    /// Marks this zone matched and moves to the next, keeping a swapped path;
    /// without a swap the new zone's looser path is adjusted. The earlier
    /// zone's turns moved this zone's peaks, so it needs a baseline round.
    /// False, changing nothing, when no unmatched zone remains.
    bool next_zone();
    [[nodiscard]] bool has_next_zone() const;
    /// True from next_zone() until a round measures both peaks.
    [[nodiscard]] bool needs_baseline() const;
    [[nodiscard]] std::optional<float> hz_per_turn() const;
    /// tallest_peak_unpaired() for round 0, or this zone has stalled.
    [[nodiscard]] bool show_mechanical_hint() const;
    [[nodiscard]] const std::vector<BeltTuneRound>& rounds() const; ///< current zone

    [[nodiscard]] BeltTensionerKind tensioner() const;
    [[nodiscard]] float tolerance_hz() const; ///< the matched band, +- around zero
    [[nodiscard]] float reference_hz() const; ///< latest round's reference peak
    [[nodiscard]] float adjusted_hz() const;
    [[nodiscard]] std::optional<float> previous_gap_hz() const; ///< hollow gauge needle
    /// Signed sum, tighten positive, of the turns recorded while `path` was
    /// the adjusted one. SCREW only.
    [[nodiscard]] float total_turns(BeltPath path) const;
    [[nodiscard]] int total_rounds() const; ///< across zones

  private:
    [[nodiscard]] std::optional<BeltTuneZone> find_zone(std::vector<float> matched) const;
    void enter_zone(const BeltTuneZone& zone, BeltPath path);
    void clear_belt_history();

    BeltCurve a_;
    BeltCurve b_;
    BeltComparison round0_;
    float band_min_hz_;
    float band_max_hz_;
    BeltTensionerKind kind_;
    float tolerance_hz_;
    bool shape_warning_;

    std::optional<BeltTuneZone> zone_;
    std::vector<float> matched_centres_hz_;
    BeltPath adjusted_ = BeltPath::PATH_A;
    bool path_swapped_ = false;
    bool needs_baseline_ = false;
    bool window_widened_ = false; ///< OUT_OF_WINDOW since the last measured round
    float ref_hz_ = 0.0f;
    float adj_hz_ = 0.0f;
    std::optional<float> previous_gap_hz_;
    std::vector<BeltTuneRound> rounds_;
    int total_rounds_ = 0;

    std::optional<float> pending_turns_;
    float unmeasured_turns_ = 0.0f;     ///< signed turns since the last measured round
    float unmeasured_abs_turns_ = 0.0f; ///< the same, as magnitudes
    std::vector<float> rate_samples_;   ///< |peak move| / |turns|, Hz per turn
    float total_turns_a_ = 0.0f;
    float total_turns_b_ = 0.0f;
};

} // namespace helix::calibration
