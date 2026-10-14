// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file belt_tune_session.cpp
 * @brief Zone choice, peak refinement, advice and round outcomes for the
 *        guided belt-tuning loop
 */

#include "belt_tune_session.h"

#include "spdlog/spdlog.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace helix::calibration {

using namespace belt_tune;

namespace {

/// The zone two peaks define, seen from the adjusted path.
BeltTuneZone make_zone(float a_hz, float b_hz, BeltZoneSource source, BeltPath adjusted,
                       float band_min_hz, float band_max_hz) {
    BeltTuneZone z;
    z.source = source;
    z.adj_hz = adjusted == BeltPath::PATH_A ? a_hz : b_hz;
    z.ref_hz = adjusted == BeltPath::PATH_A ? b_hz : a_hz;
    const float mid = (a_hz + b_hz) / 2.0f;
    const float half = std::max(ZONE_HALF_MIN_HZ, std::abs(a_hz - b_hz) / 2.0f + ZONE_MARGIN_HZ);
    z.window.freq_start_hz = std::max(band_min_hz, mid - half);
    z.window.freq_end_hz = std::min(band_max_hz, mid + half);
    return z;
}

bool near_matched(float centre_hz, const std::vector<float>& matched_centres_hz) {
    return std::any_of(matched_centres_hz.begin(), matched_centres_hz.end(), [centre_hz](float m) {
        return std::abs(centre_hz - m) <= ZONE_HALF_MIN_HZ;
    });
}

/// One path's tallest peak, and whether it belongs to a pair.
struct Tallest {
    std::optional<BeltPeak> peak;
    bool paired = false;

    void offer(const BeltPeak& p, bool is_paired) {
        if (!peak || p.amplitude > peak->amplitude) {
            peak = p;
            paired = is_paired;
        }
    }
};

/// Each path's peaks, tallest peak and the zone amplitude floor.
struct PeakCensus {
    std::vector<BeltPeak> a;
    std::vector<BeltPeak> b;
    Tallest tallest_a;
    Tallest tallest_b;
    float floor = 0.0f;

    explicit PeakCensus(const BeltPeakPairing& pk) {
        for (const auto& pair : pk.pairs) {
            a.push_back(pair.a);
            b.push_back(pair.b);
            tallest_a.offer(pair.a, true);
            tallest_b.offer(pair.b, true);
        }
        for (const auto& p : pk.unpaired_a) {
            a.push_back(p);
            tallest_a.offer(p, false);
        }
        for (const auto& p : pk.unpaired_b) {
            b.push_back(p);
            tallest_b.offer(p, false);
        }
        const float top = std::max(tallest_a.peak ? tallest_a.peak->amplitude : 0.0f,
                                   tallest_b.peak ? tallest_b.peak->amplitude : 0.0f);
        floor = ZONE_MIN_AMPLITUDE_FRACTION * top;
    }

    [[nodiscard]] bool unpaired_above_floor(const Tallest& t) const {
        return t.peak && !t.paired && t.peak->amplitude >= floor;
    }

    /// The taller of the two tallest peaks that are unpaired and above the
    /// floor, with the path it belongs to.
    [[nodiscard]] std::optional<std::pair<BeltPeak, BeltPath>> unpaired_tallest() const {
        const bool ua = unpaired_above_floor(tallest_a);
        const bool ub = unpaired_above_floor(tallest_b);
        if (ua && (!ub || tallest_a.peak->amplitude >= tallest_b.peak->amplitude)) {
            return std::make_pair(*tallest_a.peak, BeltPath::PATH_A);
        }
        if (ub) {
            return std::make_pair(*tallest_b.peak, BeltPath::PATH_B);
        }
        return std::nullopt;
    }
};

/// The in-window bin indices [first, last] of an ascending curve; first > last
/// when the window holds no bin.
std::pair<size_t, size_t> window_bins(const BeltCurve& curve, BeltSweepWindow window) {
    size_t first = 0;
    while (first < curve.size() && curve[first].first < window.freq_start_hz) {
        ++first;
    }
    size_t end = first;
    while (end < curve.size() && curve[end].first <= window.freq_end_hz) {
        ++end;
    }
    return {first, end == 0 ? 0 : end - 1};
}

/// The frequency of the window's edge bin when it is the in-window maximum
/// and rises above its inward neighbour: the peak sits at or past the edge.
std::optional<float> edge_maximum(const BeltCurve& curve, BeltSweepWindow window) {
    const auto [first, last] = window_bins(curve, window);
    if (curve.empty() || first >= last) {
        return std::nullopt;
    }
    size_t top = first;
    for (size_t i = first + 1; i <= last; ++i) {
        if (curve[i].second > curve[top].second) {
            top = i;
        }
    }
    if (top == first && curve[first].second > curve[first + 1].second) {
        return curve[first].first;
    }
    if (top == last && curve[last].second > curve[last - 1].second) {
        return curve[last].first;
    }
    return std::nullopt;
}

/// Whether the in-window maximum stands PEAK_MIN_PROMINENCE above the
/// in-window minimum. A flat or gently sloped window holds no clear peak.
bool prominent(const BeltCurve& curve, BeltSweepWindow window) {
    const auto [first, last] = window_bins(curve, window);
    if (curve.empty() || first > last || last >= curve.size()) {
        return false;
    }
    float lo = curve[first].second;
    float hi = curve[first].second;
    for (size_t i = first + 1; i <= last; ++i) {
        lo = std::min(lo, curve[i].second);
        hi = std::max(hi, curve[i].second);
    }
    return hi > 0.0f && hi >= PEAK_MIN_PROMINENCE * lo;
}

float round_to_turn_step(float turns) {
    return std::clamp(std::round(turns / TURN_STEP) * TURN_STEP, TURN_STEP, TURN_MAX);
}

} // namespace

// ============================================================================
// Free functions
// ============================================================================

bool tallest_peak_unpaired(const BeltComparison& cmp) {
    return cmp.valid && PeakCensus(cmp.peaks).unpaired_tallest().has_value();
}

std::optional<BeltTuneZone> pick_tune_zone(const BeltComparison& cmp, BeltPath adjusted,
                                           float band_min_hz, float band_max_hz,
                                           const std::vector<float>& matched_zone_centres_hz) {
    if (!cmp.valid) {
        return std::nullopt;
    }
    const PeakCensus census(cmp.peaks);

    const auto usable = [&](float a_hz, float b_hz) {
        return std::abs(a_hz - b_hz) > MATCHED_MIN_HZ &&
               !near_matched((a_hz + b_hz) / 2.0f, matched_zone_centres_hz);
    };

    // An unpaired tallest peak steers against the other path's nearest
    // floor-eligible peak, but never across two unrelated modes.
    if (const auto x = census.unpaired_tallest()) {
        const auto& [peak, path] = *x;
        const auto& others = path == BeltPath::PATH_A ? census.b : census.a;
        const BeltPeak* partner = nullptr;
        for (const auto& o : others) {
            const float d = std::abs(o.freq_hz - peak.freq_hz);
            if (o.amplitude >= census.floor && d <= ZONE_FALLBACK_MAX_HZ &&
                (!partner || d < std::abs(partner->freq_hz - peak.freq_hz))) {
                partner = &o;
            }
        }
        if (partner) {
            const float a_hz = path == BeltPath::PATH_A ? peak.freq_hz : partner->freq_hz;
            const float b_hz = path == BeltPath::PATH_A ? partner->freq_hz : peak.freq_hz;
            if (usable(a_hz, b_hz)) {
                return make_zone(a_hz, b_hz, BeltZoneSource::TALLEST_UNPAIRED, adjusted,
                                 band_min_hz, band_max_hz);
            }
        }
    }

    const BeltPeakPair* widest = nullptr;
    for (const auto& pair : cmp.peaks.pairs) {
        if (pair.a.amplitude < census.floor || pair.b.amplitude < census.floor ||
            !usable(pair.a.freq_hz, pair.b.freq_hz)) {
            continue;
        }
        if (!widest || std::abs(pair.a.freq_hz - pair.b.freq_hz) >
                           std::abs(widest->a.freq_hz - widest->b.freq_hz)) {
            widest = &pair;
        }
    }
    if (!widest) {
        return std::nullopt;
    }
    return make_zone(widest->a.freq_hz, widest->b.freq_hz, BeltZoneSource::PAIRED, adjusted,
                     band_min_hz, band_max_hz);
}

std::optional<float> refine_peak_in_window(const BeltCurve& curve, BeltSweepWindow window,
                                           std::optional<float> near_hz) {
    const auto peaks = detect_belt_peaks(curve, window.freq_start_hz, window.freq_end_hz);
    if (peaks.empty()) {
        return std::nullopt;
    }
    const auto chosen =
        near_hz ? std::min_element(peaks.begin(), peaks.end(),
                                   [n = *near_hz](const BeltPeak& l, const BeltPeak& r) {
                                       return std::abs(l.freq_hz - n) < std::abs(r.freq_hz - n);
                                   })
                : std::max_element(peaks.begin(), peaks.end(),
                                   [](const BeltPeak& l, const BeltPeak& r) {
                                       return l.amplitude < r.amplitude;
                                   });

    // detect_belt_peaks copies bins, so the frequency matches exactly, and a
    // peak always has both neighbours in the curve.
    const auto it = std::find_if(curve.begin(), curve.end(),
                                 [&](const auto& bin) { return bin.first == chosen->freq_hz; });
    if (it == curve.begin() || it == curve.end() || std::next(it) == curve.end()) {
        return chosen->freq_hz;
    }
    const auto& [f0, y0] = *std::prev(it);
    const auto& [f1, y1] = *it;
    const auto& [f2, y2] = *std::next(it);
    const float denom = y0 - 2.0f * y1 + y2;
    if (std::abs(denom) < 1e-12f) {
        return f1;
    }
    const float offset = std::clamp(0.5f * (y0 - y2) / denom, -0.5f, 0.5f);
    return offset >= 0.0f ? f1 + offset * (f2 - f1) : f1 + offset * (f1 - f0);
}

float matched_tolerance_hz(const BeltCurve& curve) {
    if (curve.size() < 2) {
        return MATCHED_MIN_HZ;
    }
    const float bin_hz =
        (curve.back().first - curve.front().first) / static_cast<float>(curve.size() - 1);
    return std::max(MATCHED_MIN_HZ, bin_hz);
}

// ============================================================================
// BeltTuneSession
// ============================================================================

BeltTuneSession::BeltTuneSession(const BeltCurve& a, const BeltCurve& b,
                                 const BeltComparison& round0, float band_min_hz, float band_max_hz,
                                 BeltTensionerKind kind)
    : a_(a), b_(b), round0_(round0), band_min_hz_(band_min_hz), band_max_hz_(band_max_hz),
      kind_(kind), tolerance_hz_(std::max(matched_tolerance_hz(a), matched_tolerance_hz(b))),
      shape_warning_(tallest_peak_unpaired(round0)) {
    if (auto z = find_zone(matched_centres_hz_)) {
        enter_zone(*z);
    }
}

std::optional<BeltTuneZone> BeltTuneSession::find_zone(std::vector<float> matched) const {
    // pick_tune_zone only knows the 1 Hz floor; a zone within this session's
    // tolerance is already matched, so it is skipped the same way.
    for (;;) {
        auto z = pick_tune_zone(round0_, BeltPath::PATH_A, band_min_hz_, band_max_hz_, matched);
        if (!z || std::abs(z->adj_hz - z->ref_hz) > tolerance_hz_) {
            return z;
        }
        matched.push_back((z->adj_hz + z->ref_hz) / 2.0f);
    }
}

void BeltTuneSession::enter_zone(const BeltTuneZone& zone) {
    // zone is seen from PATH_A. The looser path, the lower peak, is adjusted.
    const BeltPath path = zone.adj_hz <= zone.ref_hz ? BeltPath::PATH_A : BeltPath::PATH_B;
    if (path != adjusted_) {
        clear_belt_history();
    }
    adjusted_ = path;
    zone_ = zone;
    if (path == BeltPath::PATH_B) {
        std::swap(zone_->adj_hz, zone_->ref_hz);
    }
    const BeltCurve& ref_curve = adjusted_ == BeltPath::PATH_A ? b_ : a_;
    const BeltCurve& adj_curve = adjusted_ == BeltPath::PATH_A ? a_ : b_;
    ref_hz_ =
        refine_peak_in_window(ref_curve, zone_->window, zone_->ref_hz).value_or(zone_->ref_hz);
    adj_hz_ =
        refine_peak_in_window(adj_curve, zone_->window, zone_->adj_hz).value_or(zone_->adj_hz);
    previous_gap_hz_.reset();
    rounds_.clear();
    spdlog::debug("[BeltTune] Zone {:.1f}-{:.1f} Hz ({}), adjusting {}: {:.2f} vs {:.2f} Hz",
                  zone_->window.freq_start_hz, zone_->window.freq_end_hz,
                  zone_->source == BeltZoneSource::PAIRED ? "paired" : "tallest unpaired",
                  adjusted_ == BeltPath::PATH_A ? "A" : "B", adj_hz_, ref_hz_);
}

void BeltTuneSession::clear_belt_history() {
    rate_samples_.clear();
    unmeasured_turns_ = 0.0f;
    pending_turns_.reset();
}

const std::optional<BeltTuneZone>& BeltTuneSession::zone() const {
    return zone_;
}

BeltPath BeltTuneSession::adjusted_path() const {
    return adjusted_;
}

void BeltTuneSession::swap_adjusted_path() {
    adjusted_ = adjusted_ == BeltPath::PATH_A ? BeltPath::PATH_B : BeltPath::PATH_A;
    if (zone_) {
        std::swap(zone_->adj_hz, zone_->ref_hz);
    }
    std::swap(adj_hz_, ref_hz_);
    clear_belt_history();
    previous_gap_hz_.reset();
    rounds_.clear();
}

float BeltTuneSession::gap_hz() const {
    return adj_hz_ - ref_hz_;
}

BeltTuneAdvice BeltTuneSession::advice() const {
    BeltTuneAdvice out;
    out.path = adjusted_;
    out.direction = gap_hz() > 0.0f ? BeltTurnDirection::LOOSEN : BeltTurnDirection::TIGHTEN;
    out.spring_reseat = kind_ == BeltTensionerKind::SPRING;
    if (kind_ != BeltTensionerKind::SCREW) {
        return out;
    }
    if (const auto rate = hz_per_turn()) {
        out.turns = round_to_turn_step(std::abs(gap_hz()) / *rate);
    } else if (!rounds_.empty() && rounds_.back().outcome == BeltTuneOutcome::NO_MOVEMENT &&
               rounds_.back().turns_made) {
        // Nothing learned yet and the last turn did nothing: try a bigger one.
        out.turns = round_to_turn_step(2.0f * *rounds_.back().turns_made);
    } else {
        out.turns = FIRST_TURN;
    }
    return out;
}

void BeltTuneSession::record_turns(float turns) {
    if (kind_ == BeltTensionerKind::SCREW) {
        pending_turns_ = std::abs(turns);
    }
}

const BeltTuneRound& BeltTuneSession::add_round(const BeltCurve& a_window,
                                                const BeltCurve& b_window) {
    const BeltTuneAdvice advised = advice();
    BeltTuneRound r;
    if (kind_ == BeltTensionerKind::SCREW) {
        r.turns_made = pending_turns_ ? pending_turns_ : advised.turns;
        const float signed_turns =
            advised.direction == BeltTurnDirection::TIGHTEN ? *r.turns_made : -*r.turns_made;
        total_turns_ += signed_turns;
        unmeasured_turns_ += signed_turns;
    }
    pending_turns_.reset();
    ++total_rounds_;
    r.ref_hz = ref_hz_;
    r.adj_hz = adj_hz_;
    r.gap_hz = gap_hz();

    const auto finish = [&](BeltTuneOutcome outcome) -> const BeltTuneRound& {
        r.outcome = outcome;
        rounds_.push_back(r);
        spdlog::debug("[BeltTune] Round {}: ref {:.2f} adj {:.2f} gap {:+.2f} Hz, outcome {}",
                      total_rounds_, r.ref_hz, r.adj_hz, r.gap_hz, static_cast<int>(outcome));
        return rounds_.back();
    };

    if (!zone_) {
        return finish(BeltTuneOutcome::NO_PEAK);
    }
    const BeltSweepWindow window = zone_->window;
    const BeltCurve& ref_curve = adjusted_ == BeltPath::PATH_A ? b_window : a_window;
    const BeltCurve& adj_curve = adjusted_ == BeltPath::PATH_A ? a_window : b_window;
    if (!prominent(ref_curve, window) || !prominent(adj_curve, window)) {
        return finish(BeltTuneOutcome::NO_PEAK);
    }
    const auto ref = refine_peak_in_window(ref_curve, window, ref_hz_);
    const auto adj = refine_peak_in_window(adj_curve, window, adj_hz_);

    if (!adj) {
        if (const auto edge = edge_maximum(adj_curve, window)) {
            // A peak still past the edge after one move is not chased further.
            if (!rounds_.empty() && rounds_.back().outcome == BeltTuneOutcome::OUT_OF_WINDOW) {
                return finish(BeltTuneOutcome::NO_PEAK);
            }
            // Re-centre on the reference, reaching past the edge the adjusted
            // peak ran into, so the next sweep covers it. No gap from an edge bin.
            const float centre = ref.value_or(ref_hz_);
            const float half = std::max((window.freq_end_hz - window.freq_start_hz) / 2.0f,
                                        std::abs(*edge - centre) + ZONE_MARGIN_HZ);
            zone_->window.freq_start_hz = std::max(band_min_hz_, centre - half);
            zone_->window.freq_end_hz = std::min(band_max_hz_, centre + half);
            return finish(BeltTuneOutcome::OUT_OF_WINDOW);
        }
    }
    if (!ref || !adj) {
        return finish(BeltTuneOutcome::NO_PEAK);
    }

    const float prev_gap = gap_hz();
    const float move = *adj - adj_hz_;
    ref_hz_ = *ref;
    adj_hz_ = *adj;
    const float gap = gap_hz();
    previous_gap_hz_ = prev_gap;
    r.ref_hz = ref_hz_;
    r.adj_hz = adj_hz_;
    r.gap_hz = gap;

    if (std::abs(move) >= MIN_MOVE_HZ && unmeasured_turns_ != 0.0f) {
        rate_samples_.push_back(std::abs(move) / std::abs(unmeasured_turns_));
    }
    unmeasured_turns_ = 0.0f;

    const float expected = advised.direction == BeltTurnDirection::TIGHTEN ? 1.0f : -1.0f;
    if (std::abs(gap) <= tolerance_hz_) {
        return finish(BeltTuneOutcome::MATCHED);
    }
    if (std::abs(move) < MIN_MOVE_HZ) {
        return finish(BeltTuneOutcome::NO_MOVEMENT);
    }
    if (move * expected < 0.0f) {
        return finish(BeltTuneOutcome::WRONG_WAY);
    }
    if (gap * prev_gap < 0.0f) {
        return finish(BeltTuneOutcome::OVERSHOT);
    }
    // The peak moved the advised way; if the reference moved with it, the gap
    // did not close and the turn achieved nothing relative to the other belt.
    return finish(std::abs(gap) < std::abs(prev_gap) ? BeltTuneOutcome::CLOSER
                                                     : BeltTuneOutcome::NO_MOVEMENT);
}

bool BeltTuneSession::zone_matched() const {
    return !rounds_.empty() && rounds_.back().outcome == BeltTuneOutcome::MATCHED;
}

bool BeltTuneSession::next_zone() {
    std::vector<float> matched = matched_centres_hz_;
    if (zone_) {
        matched.push_back((zone_->ref_hz + zone_->adj_hz) / 2.0f);
    }
    const auto z = find_zone(matched);
    if (!z) {
        return false;
    }
    matched_centres_hz_ = std::move(matched);
    enter_zone(*z);
    return true;
}

bool BeltTuneSession::has_next_zone() const {
    std::vector<float> matched = matched_centres_hz_;
    if (zone_) {
        matched.push_back((zone_->ref_hz + zone_->adj_hz) / 2.0f);
    }
    return find_zone(std::move(matched)).has_value();
}

std::optional<float> BeltTuneSession::hz_per_turn() const {
    if (rate_samples_.empty()) {
        return std::nullopt;
    }
    std::vector<float> sorted = rate_samples_;
    std::sort(sorted.begin(), sorted.end());
    const size_t mid = sorted.size() / 2;
    return sorted.size() % 2 == 1 ? sorted[mid] : (sorted[mid - 1] + sorted[mid]) / 2.0f;
}

bool BeltTuneSession::show_mechanical_hint() const {
    if (shape_warning_) {
        return true;
    }
    const auto unmatched = std::count_if(rounds_.begin(), rounds_.end(), [](const auto& r) {
        return r.outcome != BeltTuneOutcome::MATCHED;
    });
    const int limit = kind_ == BeltTensionerKind::SPRING ? SPRING_STALL_ROUNDS : STALL_ROUNDS;
    return unmatched >= limit;
}

const std::vector<BeltTuneRound>& BeltTuneSession::rounds() const {
    return rounds_;
}

BeltTensionerKind BeltTuneSession::tensioner() const {
    return kind_;
}

float BeltTuneSession::tolerance_hz() const {
    return tolerance_hz_;
}

float BeltTuneSession::reference_hz() const {
    return ref_hz_;
}

float BeltTuneSession::adjusted_hz() const {
    return adj_hz_;
}

std::optional<float> BeltTuneSession::previous_gap_hz() const {
    return previous_gap_hz_;
}

float BeltTuneSession::total_turns() const {
    return total_turns_;
}

int BeltTuneSession::total_rounds() const {
    return total_rounds_;
}

} // namespace helix::calibration
