// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "belt_tension_types.h"
#include "belt_test_curves.h"
#include "belt_tune_session.h"
#include "shaper_csv_parser.h"

#include <cmath>

#include "../catch_amalgamated.hpp"

using namespace helix::calibration;
using helix::calibration::test::lorentzian_curve;
using helix::calibration::test::response_from;
using helix::calibration::test::windowed;

namespace {

constexpr float LO = 5.0f;
constexpr float HI = 135.0f;

std::string fixture_dir() {
    std::string src = __FILE__;
    auto pos = src.rfind("/tests/unit/");
    if (pos != std::string::npos) {
        return src.substr(0, pos) + "/tests/fixtures/";
    }
    return "tests/fixtures/";
}

BeltCurve load_fixture(const std::string& name) {
    const std::string path = fixture_dir() + "belt_sweeps/" + name;
    auto data = parse_resonance_csv(path);
    INFO("fixture missing or unparsable: " << path);
    REQUIRE(data.error == ResonanceCsvError::NONE);
    REQUIRE_FALSE(data.curve.empty());
    return data.curve;
}

/// One path of a hardware capture pair, through the parser: path 'A' reads
/// `<name>_axis_1_-1.csv`, 'B' reads `<name>_axis_1_1.csv`.
BeltResponse load_fixture_response(const std::string& name, char path) {
    const std::string file =
        fixture_dir() + "belt_sweeps/" + name + (path == 'A' ? "_axis_1_-1.csv" : "_axis_1_1.csv");
    auto data = parse_resonance_csv(file);
    INFO("fixture missing or unparsable: " << file);
    REQUIRE(data.error == ResonanceCsvError::NONE);
    REQUIRE_FALSE(data.in_plane.empty());
    return BeltResponse{data.curve, data.in_plane};
}

BeltComparison compare_fixture(const std::string& printer, float lo, float hi) {
    return compare_belt_paths(load_fixture(printer + "_axis_1_-1.csv"),
                              load_fixture(printer + "_axis_1_1.csv"), lo, hi);
}

BeltCurve one_peak(float hz, float lo = LO, float hi = HI) {
    return lorentzian_curve({{hz, 1e4f}}, lo, hi);
}

/// Round 0 from one peak per path over the full band.
BeltTuneSession session_for(float a_hz, float b_hz, BeltTensionerKind kind) {
    const auto a = one_peak(a_hz);
    const auto b = one_peak(b_hz);
    return BeltTuneSession(a, b, compare_belt_paths(a, b, LO, HI), LO, HI, kind);
}

/// The unexcited floor of a synthetic windowed sweep.
constexpr float FLOOR = 0.01f;

/// Both paths swept over the session's current zone window: driven bins
/// inside it, the unexcited floor across the rest of the band.
const BeltTuneRound& sweep_curves(BeltTuneSession& s, const BeltCurve& a, const BeltCurve& b) {
    REQUIRE(s.zone().has_value());
    const auto w = s.zone()->window;
    return s.add_round(windowed(a, w, FLOOR), windowed(b, w, FLOOR));
}

const BeltTuneRound& sweep(BeltTuneSession& s, float a_hz, float b_hz) {
    return sweep_curves(s, one_peak(a_hz), one_peak(b_hz));
}

/// The user turned the suggested amount and recorded it on the stepper.
void turn_as_advised(BeltTuneSession& s) {
    if (const auto turns = s.advice().turns) {
        s.record_turns(*turns);
    }
}

BeltPeak pk(float hz, float amp) {
    return BeltPeak{hz, amp};
}

BeltComparison cmp_with(std::vector<BeltPeakPair> pairs, std::vector<BeltPeak> unpaired_a = {},
                        std::vector<BeltPeak> unpaired_b = {}) {
    BeltComparison c;
    c.valid = true;
    c.peaks.pairs = std::move(pairs);
    c.peaks.unpaired_a = std::move(unpaired_a);
    c.peaks.unpaired_b = std::move(unpaired_b);
    c.peaks.threshold_hz = belt_verdict::PAIR_MAX_HZ;
    return c;
}

} // namespace

// ============================================================================
// pick_tune_zone()
// ============================================================================

TEST_CASE("tune zone: widest pair, amplitude floor, fallback, window, matched", "[belt][tune]") {
    // Sorted by amplitude sum, as pair_belt_peaks returns them. The 110/118
    // pair is the widest but sits under 20% of the strongest peak.
    const auto paired = cmp_with({{pk(50.0f, 100.0f), pk(52.0f, 100.0f)},
                                  {pk(80.0f, 60.0f), pk(86.0f, 60.0f)},
                                  {pk(110.0f, 15.0f), pk(118.0f, 15.0f)}});

    SECTION("the widest pair above the amplitude floor") {
        const auto z = pick_tune_zone(paired, BeltPath::PATH_A, LO, HI, {});
        REQUIRE(z.has_value());
        CHECK(z->source == BeltZoneSource::PAIRED);
        CHECK(z->adj_hz == Catch::Approx(80.0f));
        CHECK(z->ref_hz == Catch::Approx(86.0f));
        // Midpoint 83 +- max(10, 6/2 + 5).
        CHECK(z->window.freq_start_hz == Catch::Approx(73.0f));
        CHECK(z->window.freq_end_hz == Catch::Approx(93.0f));
    }

    SECTION("the adjusted path decides which peak is the reference") {
        const auto z = pick_tune_zone(paired, BeltPath::PATH_B, LO, HI, {});
        REQUIRE(z.has_value());
        CHECK(z->adj_hz == Catch::Approx(86.0f));
        CHECK(z->ref_hz == Catch::Approx(80.0f));
    }

    SECTION("a pair with one peak under the floor is ignored") {
        const auto c = cmp_with(
            {{pk(50.0f, 100.0f), pk(52.0f, 100.0f)}, {pk(80.0f, 60.0f), pk(90.0f, 19.0f)}});
        const auto z = pick_tune_zone(c, BeltPath::PATH_A, LO, HI, {});
        REQUIRE(z.has_value());
        CHECK(z->adj_hz == Catch::Approx(50.0f));
    }

    SECTION("already-matched centres are skipped") {
        auto z = pick_tune_zone(paired, BeltPath::PATH_A, LO, HI, {83.0f});
        REQUIRE(z.has_value());
        CHECK(z->adj_hz == Catch::Approx(50.0f));
        CHECK(z->ref_hz == Catch::Approx(52.0f));
        // Midpoint 51 +- 10: the half-width floor.
        CHECK(z->window.freq_start_hz == Catch::Approx(41.0f));
        CHECK(z->window.freq_end_hz == Catch::Approx(61.0f));

        CHECK_FALSE(pick_tune_zone(paired, BeltPath::PATH_A, LO, HI, {83.0f, 51.0f}).has_value());
    }

    SECTION("a pair already within the matched floor is not a zone") {
        const auto c = cmp_with({{pk(50.0f, 100.0f), pk(50.5f, 100.0f)}});
        CHECK_FALSE(pick_tune_zone(c, BeltPath::PATH_A, LO, HI, {}).has_value());
    }

    SECTION("both tallest peaks unpaired: steer by them") {
        const auto c = cmp_with({{pk(60.0f, 30.0f), pk(61.0f, 30.0f)}}, {pk(98.0f, 100.0f)},
                                {pk(110.0f, 90.0f)});
        const auto z = pick_tune_zone(c, BeltPath::PATH_A, LO, HI, {});
        REQUIRE(z.has_value());
        CHECK(z->source == BeltZoneSource::TALLEST_UNPAIRED);
        CHECK(z->adj_hz == Catch::Approx(98.0f));
        CHECK(z->ref_hz == Catch::Approx(110.0f));
        // Midpoint 104 +- max(10, 12/2 + 5).
        CHECK(z->window.freq_start_hz == Catch::Approx(93.0f));
        CHECK(z->window.freq_end_hz == Catch::Approx(115.0f));
    }

    SECTION("either tallest peak unpaired: steer it against the nearest eligible peak") {
        // A's tallest (36) is paired; B's tallest (119.5) is not. A's 118 is
        // nearest but under the floor, so 131 partners it.
        const auto c = cmp_with(
            {{pk(36.0f, 100.0f), pk(35.0f, 95.0f)}, {pk(131.0f, 50.0f), pk(133.0f, 45.0f)}},
            {pk(118.0f, 10.0f)}, {pk(119.5f, 98.0f)});
        CHECK(tallest_peak_unpaired(c));
        const auto z = pick_tune_zone(c, BeltPath::PATH_A, LO, HI, {});
        REQUIRE(z.has_value());
        CHECK(z->source == BeltZoneSource::TALLEST_UNPAIRED);
        CHECK(z->adj_hz == Catch::Approx(131.0f));
        CHECK(z->ref_hz == Catch::Approx(119.5f));
    }

    SECTION("nothing within 30 Hz of the unpaired peak: the widest pair instead") {
        const auto c =
            cmp_with({{pk(36.0f, 100.0f), pk(35.0f, 95.0f)}, {pk(60.0f, 50.0f), pk(63.0f, 50.0f)}},
                     {}, {pk(119.5f, 98.0f)});
        CHECK(tallest_peak_unpaired(c));
        const auto z = pick_tune_zone(c, BeltPath::PATH_A, LO, HI, {});
        REQUIRE(z.has_value());
        CHECK(z->source == BeltZoneSource::PAIRED);
        CHECK(z->adj_hz == Catch::Approx(60.0f));
        CHECK(z->ref_hz == Catch::Approx(63.0f));
    }

    SECTION("both tallest peaks unpaired: the taller one steers") {
        // B's 110 is taller, so its nearest A peak (120) partners it, not the
        // 90 that A's own tallest (98) would have chosen.
        const auto c = cmp_with({}, {pk(98.0f, 80.0f), pk(120.0f, 25.0f)},
                                {pk(110.0f, 100.0f), pk(90.0f, 30.0f)});
        const auto z = pick_tune_zone(c, BeltPath::PATH_A, LO, HI, {});
        REQUIRE(z.has_value());
        CHECK(z->source == BeltZoneSource::TALLEST_UNPAIRED);
        CHECK(z->adj_hz == Catch::Approx(120.0f));
        CHECK(z->ref_hz == Catch::Approx(110.0f));
    }

    SECTION("paired tallest peaks raise no shape warning") {
        CHECK_FALSE(tallest_peak_unpaired(paired));
        // B's tallest is unpaired but under 20% of A's: a weak path, not a shape.
        const auto weak_b =
            cmp_with({{pk(98.0f, 100.0f), pk(99.0f, 10.0f)}}, {}, {pk(50.0f, 15.0f)});
        CHECK_FALSE(tallest_peak_unpaired(weak_b));
    }

    SECTION("no pairs at all: steer by the tallest peaks") {
        const auto c = cmp_with({}, {pk(40.0f, 10.0f), pk(98.0f, 100.0f)}, {pk(110.0f, 90.0f)});
        const auto z = pick_tune_zone(c, BeltPath::PATH_A, LO, HI, {});
        REQUIRE(z.has_value());
        CHECK(z->source == BeltZoneSource::TALLEST_UNPAIRED);
        CHECK(z->adj_hz == Catch::Approx(98.0f));
    }

    SECTION("the window is clamped to the band at both ends") {
        const auto c = cmp_with({}, {pk(98.0f, 100.0f)}, {pk(110.0f, 90.0f)});
        const auto z = pick_tune_zone(c, BeltPath::PATH_A, 96.0f, 112.0f, {});
        REQUIRE(z.has_value());
        CHECK(z->window.freq_start_hz == Catch::Approx(96.0f));
        CHECK(z->window.freq_end_hz == Catch::Approx(112.0f));
    }

    SECTION("no pairs and no peaks: no zone") {
        CHECK_FALSE(pick_tune_zone(cmp_with({}), BeltPath::PATH_A, LO, HI, {}).has_value());
        CHECK_FALSE(pick_tune_zone(cmp_with({}, {pk(98.0f, 100.0f)}), BeltPath::PATH_A, LO, HI, {})
                        .has_value());
        CHECK_FALSE(pick_tune_zone(BeltComparison{}, BeltPath::PATH_A, LO, HI, {}).has_value());
    }
}

TEST_CASE("tune zone on real captures", "[belt][tune]") {
    SECTION("Voron 2.4: B's unpaired belt hump steers against A's nearest peak") {
        const auto a = load_fixture("voron24_kalico_axis_1_-1.csv");
        const auto b = load_fixture("voron24_kalico_axis_1_1.csv");
        const auto cmp = compare_belt_paths(a, b, 5.0f, 135.0f);
        REQUIRE(cmp.valid);
        CHECK(tallest_peak_unpaired(cmp));
        const auto z = pick_tune_zone(cmp, BeltPath::PATH_A, 5.0f, 135.0f, {});
        REQUIRE(z.has_value());
        INFO("zone A " << z->adj_hz << " B " << z->ref_hz << " window " << z->window.freq_start_hz
                       << "-" << z->window.freq_end_hz);
        CHECK(z->source == BeltZoneSource::TALLEST_UNPAIRED);
        CHECK(z->adj_hz == Catch::Approx(131.5f).margin(0.1f));
        CHECK(z->ref_hz == Catch::Approx(119.5f).margin(0.1f));
        CHECK(z->window.freq_end_hz - z->window.freq_start_hz <=
              2.0f * (belt_tune::ZONE_FALLBACK_MAX_HZ / 2.0f + belt_tune::ZONE_MARGIN_HZ));
        // Midpoint 125.5 +- (12/2 + 5), clamped at the band top.
        CHECK(z->window.freq_start_hz == Catch::Approx(114.5f).margin(0.5f));
        CHECK(z->window.freq_end_hz == Catch::Approx(135.0f).margin(0.5f));

        BeltTuneSession s(a, b, cmp, 5.0f, 135.0f, BeltTensionerKind::SCREW);
        CHECK(s.show_mechanical_hint());
        CHECK(s.adjusted_path() == BeltPath::PATH_B);
    }

    SECTION("K1C (98.9%): no zone, because every strong pair is within a bin") {
        const auto a = load_fixture("k1c_axis_1_-1.csv");
        const auto b = load_fixture("k1c_axis_1_1.csv");
        const auto cmp = compare_belt_paths(a, b, 5.0f, 133.333f);
        REQUIRE(cmp.valid);
        CHECK_FALSE(pick_tune_zone(cmp, BeltPath::PATH_A, 5.0f, 133.333f, {}).has_value());
        CHECK_FALSE(tallest_peak_unpaired(cmp));
        const float tolerance = std::max(matched_tolerance_hz(a), matched_tolerance_hz(b));
        const float floor = belt_tune::ZONE_MIN_AMPLITUDE_FRACTION * cmp.band_max_psd;
        REQUIRE_FALSE(cmp.peaks.pairs.empty());
        for (const auto& p : cmp.peaks.pairs) {
            INFO("pair " << p.a.freq_hz << " / " << p.b.freq_hz << " amps " << p.a.amplitude
                         << " / " << p.b.amplitude << " floor " << floor);
            CHECK(std::abs(p.a.freq_hz - p.b.freq_hz) <= tolerance);
        }
        BeltTuneSession s(a, b, cmp, 5.0f, 133.333f, BeltTensionerKind::SCREW);
        CHECK_FALSE(s.zone().has_value());
    }
}

// ============================================================================
// refine_peak_in_window() / matched_tolerance_hz()
// ============================================================================

TEST_CASE("refine peak: parabolic, nearest maximum, none in a monotonic window", "[belt][tune]") {
    SECTION("an off-bin peak refines between bins") {
        // 1.5 Hz bins from 80 put the nearest bin at 104.0, 0.3 Hz off.
        const auto c = lorentzian_curve({{104.3f, 1e4f}}, 80.0f, 130.0f, 1.5f);
        const auto f = refine_peak_in_window(c, {90.0f, 120.0f}, std::nullopt);
        REQUIRE(f.has_value());
        CHECK(*f == Catch::Approx(104.3f).margin(0.2f));
        CHECK(std::abs(*f - 104.0f) > 0.1f);
    }

    SECTION("two maxima: the one nearest near_hz, else the tallest") {
        const auto c = lorentzian_curve({{95.0f, 1e4f}, {115.0f, 8e3f}}, 80.0f, 130.0f);
        const BeltSweepWindow w{85.0f, 125.0f};
        const auto near_high = refine_peak_in_window(c, w, 112.0f);
        REQUIRE(near_high.has_value());
        CHECK(*near_high == Catch::Approx(115.0f).margin(0.2f));
        const auto near_low = refine_peak_in_window(c, w, 97.0f);
        REQUIRE(near_low.has_value());
        CHECK(*near_low == Catch::Approx(95.0f).margin(0.2f));
        const auto tallest = refine_peak_in_window(c, w, std::nullopt);
        REQUIRE(tallest.has_value());
        CHECK(*tallest == Catch::Approx(95.0f).margin(0.2f));
    }

    SECTION("a monotonic or flat window has no peak") {
        BeltCurve rising, flat;
        for (int i = 0; i < 40; ++i) {
            const float f = 90.0f + 0.78125f * static_cast<float>(i);
            rising.emplace_back(f, static_cast<float>(i));
            flat.emplace_back(f, 5.0f);
        }
        CHECK_FALSE(refine_peak_in_window(rising, {90.0f, 120.0f}, 100.0f).has_value());
        CHECK_FALSE(refine_peak_in_window(flat, {90.0f, 120.0f}, 100.0f).has_value());
        CHECK_FALSE(refine_peak_in_window({}, {90.0f, 120.0f}, std::nullopt).has_value());
    }

    SECTION("a maximum outside the window is not reported") {
        const auto c = lorentzian_curve({{104.0f, 1e4f}}, 80.0f, 130.0f);
        CHECK_FALSE(refine_peak_in_window(c, {108.0f, 125.0f}, 104.0f).has_value());
    }
}

TEST_CASE("matched tolerance is one bin, at least 1 Hz", "[belt][tune]") {
    CHECK(matched_tolerance_hz(lorentzian_curve({{100.0f, 1.0f}}, 80.0f, 120.0f)) ==
          Catch::Approx(1.0f));
    CHECK(matched_tolerance_hz(lorentzian_curve({{100.0f, 1.0f}}, 80.0f, 120.0f, 1.5f)) ==
          Catch::Approx(1.5f).margin(0.01f));
    CHECK(matched_tolerance_hz({}) == Catch::Approx(belt_tune::MATCHED_MIN_HZ));
    CHECK(matched_tolerance_hz({{100.0f, 1.0f}}) == Catch::Approx(belt_tune::MATCHED_MIN_HZ));
}

// ============================================================================
// BeltTuneSession
// ============================================================================

TEST_CASE("tune session: the looser path is adjusted, and swapping flips it", "[belt][tune]") {
    // A rings at 104, B at 100: paired 4 Hz apart, B looser.
    auto s = session_for(104.0f, 100.0f, BeltTensionerKind::SCREW);
    REQUIRE(s.zone().has_value());
    CHECK(s.zone()->source == BeltZoneSource::PAIRED);
    CHECK(s.adjusted_path() == BeltPath::PATH_B);
    CHECK(s.gap_hz() == Catch::Approx(-4.0f).margin(0.1f));
    CHECK(s.reference_hz() == Catch::Approx(104.0f).margin(0.1f));
    CHECK(s.adjusted_hz() == Catch::Approx(100.0f).margin(0.1f));
    CHECK(s.advice().path == BeltPath::PATH_B);
    CHECK(s.advice().direction == BeltTurnDirection::TIGHTEN);
    CHECK_FALSE(s.previous_gap_hz().has_value());

    SECTION("the mirror image adjusts A") {
        auto m = session_for(100.0f, 104.0f, BeltTensionerKind::SCREW);
        CHECK(m.adjusted_path() == BeltPath::PATH_A);
        CHECK(m.gap_hz() == Catch::Approx(-4.0f).margin(0.1f));
    }

    SECTION("swap flips the advice and keeps the window") {
        const auto window = s.zone()->window;
        s.swap_adjusted_path();
        CHECK(s.adjusted_path() == BeltPath::PATH_A);
        CHECK(s.gap_hz() == Catch::Approx(4.0f).margin(0.1f));
        CHECK(s.advice().path == BeltPath::PATH_A);
        CHECK(s.advice().direction == BeltTurnDirection::LOOSEN);
        CHECK(s.zone()->window.freq_start_hz == Catch::Approx(window.freq_start_hz));
        CHECK(s.zone()->window.freq_end_hz == Catch::Approx(window.freq_end_hz));
    }

    SECTION("turns are totalled per belt across a swap") {
        // B tightened a quarter, then A, now adjusted and higher, loosened an eighth.
        turn_as_advised(s);
        sweep(s, 104.0f, 101.0f);
        s.swap_adjusted_path();
        REQUIRE(s.advice().direction == BeltTurnDirection::LOOSEN);
        s.record_turns(0.125f);
        sweep(s, 103.0f, 101.0f);
        CHECK(s.total_turns(BeltPath::PATH_B) == Catch::Approx(0.25f));
        CHECK(s.total_turns(BeltPath::PATH_A) == Catch::Approx(-0.125f));
    }

    SECTION("swap clears the learned rate; the amount restarts at a quarter turn") {
        turn_as_advised(s);
        sweep(s, 104.0f, 101.0f); // B moved 1 Hz for the suggested quarter turn
        REQUIRE(s.hz_per_turn().has_value());
        CHECK(*s.hz_per_turn() == Catch::Approx(4.0f).margin(0.4f));
        const auto window = s.zone()->window;
        s.swap_adjusted_path();
        CHECK_FALSE(s.hz_per_turn().has_value());
        REQUIRE(s.advice().turns.has_value());
        CHECK(*s.advice().turns == Catch::Approx(belt_tune::FIRST_TURN));
        CHECK(s.zone()->window.freq_start_hz == Catch::Approx(window.freq_start_hz));
        CHECK(s.zone()->window.freq_end_hz == Catch::Approx(window.freq_end_hz));
    }
}

TEST_CASE("tune advice: amounts for screws only, spring re-seat", "[belt][tune]") {
    SECTION("screw: a quarter turn first, then the gap over the learned rate") {
        auto s = session_for(112.0f, 100.0f, BeltTensionerKind::SCREW);
        REQUIRE(s.advice().turns.has_value());
        CHECK(*s.advice().turns == Catch::Approx(0.25f));
        CHECK_FALSE(s.advice().spring_reseat);
        // 4.1 Hz for a quarter turn is 16.4 Hz/turn; 7.9 Hz left rounds to a half.
        turn_as_advised(s);
        sweep(s, 112.0f, 104.1f);
        REQUIRE(s.hz_per_turn().has_value());
        CHECK(*s.hz_per_turn() == Catch::Approx(16.4f).margin(0.5f));
        REQUIRE(s.advice().turns.has_value());
        CHECK(*s.advice().turns == Catch::Approx(0.5f));
    }

    SECTION("screw: amounts clamp to an eighth and to one turn") {
        auto s = session_for(112.0f, 100.0f, BeltTensionerKind::SCREW);
        s.record_turns(0.125f);
        sweep(s, 112.0f, 108.0f); // 8 Hz for an eighth: 64 Hz/turn
        turn_as_advised(s);
        sweep(s, 112.0f, 110.0f); // the suggested eighth moved 2 Hz: median 40 Hz/turn
        // 2 Hz / 40 = 0.05 turn, which rounds to zero eighths.
        REQUIRE(s.advice().turns.has_value());
        CHECK(*s.advice().turns == Catch::Approx(belt_tune::TURN_STEP));

        auto slow = session_for(112.0f, 100.0f, BeltTensionerKind::SCREW);
        slow.record_turns(1.0f);
        sweep(slow, 112.0f, 100.6f); // 0.6 Hz for a whole turn
        REQUIRE(slow.advice().turns.has_value());
        CHECK(*slow.advice().turns == Catch::Approx(belt_tune::TURN_MAX));
    }

    SECTION("screw: an amount is a multiple of an eighth") {
        auto s = session_for(112.0f, 100.0f, BeltTensionerKind::SCREW);
        s.record_turns(0.5f);
        sweep(s, 112.0f, 104.0f); // 4 Hz for half a turn, 8 Hz left: one turn
        REQUIRE(s.advice().turns.has_value());
        CHECK(*s.advice().turns == Catch::Approx(1.0f));
        turn_as_advised(s);
        sweep(s, 112.0f, 109.0f); // the suggested turn moved 5 Hz: median 6.5 Hz/turn
        // 3 Hz / 6.5 = 0.46 turn: the nearest eighth is a half, not the 0.375 below it.
        REQUIRE(s.advice().turns.has_value());
        CHECK(*s.advice().turns == Catch::Approx(0.5f));
    }

    SECTION("screw: a turn that moved nothing, before any rate, doubles") {
        auto s = session_for(112.0f, 100.0f, BeltTensionerKind::SCREW);
        turn_as_advised(s);
        CHECK(sweep(s, 112.0f, 100.2f).outcome == BeltTuneOutcome::NO_MOVEMENT);
        CHECK_FALSE(s.hz_per_turn().has_value());
        REQUIRE(s.advice().turns.has_value());
        CHECK(*s.advice().turns == Catch::Approx(0.5f));
    }

    SECTION("unknown and spring tensioners never carry turns") {
        for (auto kind : {BeltTensionerKind::UNKNOWN, BeltTensionerKind::SPRING}) {
            auto s = session_for(112.0f, 100.0f, kind);
            CHECK_FALSE(s.advice().turns.has_value());
            s.record_turns(0.5f);
            const auto& r = sweep(s, 112.0f, 104.0f);
            CHECK_FALSE(r.turns_made.has_value());
            CHECK_FALSE(s.advice().turns.has_value());
            CHECK_FALSE(s.hz_per_turn().has_value());
            CHECK(s.total_turns(BeltPath::PATH_A) == Catch::Approx(0.0f));
            CHECK(s.total_turns(BeltPath::PATH_B) == Catch::Approx(0.0f));
            CHECK(s.advice().spring_reseat == (kind == BeltTensionerKind::SPRING));
        }
    }
}

TEST_CASE("tune round outcomes", "[belt][tune]") {
    // A at 112 is the reference; B at 100 is adjusted. Window 95-117.
    auto s = session_for(112.0f, 100.0f, BeltTensionerKind::UNKNOWN);
    REQUIRE(s.adjusted_path() == BeltPath::PATH_B);
    REQUIRE(s.zone()->window.freq_start_hz == Catch::Approx(95.0f).margin(0.5f));
    REQUIRE(s.zone()->window.freq_end_hz == Catch::Approx(117.0f).margin(0.5f));

    SECTION("CLOSER: the gap shrank, same sign") {
        const auto& r = sweep(s, 112.0f, 104.0f);
        CHECK(r.outcome == BeltTuneOutcome::CLOSER);
        CHECK(r.gap_hz == Catch::Approx(-8.0f).margin(0.1f));
        CHECK(s.gap_hz() == Catch::Approx(-8.0f).margin(0.1f));
        REQUIRE(s.previous_gap_hz().has_value());
        CHECK(*s.previous_gap_hz() == Catch::Approx(-12.0f).margin(0.1f));
        CHECK(s.adjusted_hz() == Catch::Approx(104.0f).margin(0.1f));
        CHECK(s.reference_hz() == Catch::Approx(112.0f).margin(0.1f));
        CHECK(s.advice().direction == BeltTurnDirection::TIGHTEN);
        CHECK_FALSE(s.zone_matched());
    }

    SECTION("OVERSHOT: the gap changed sign and the advice flips") {
        const auto& r = sweep(s, 112.0f, 114.0f);
        CHECK(r.outcome == BeltTuneOutcome::OVERSHOT);
        CHECK(s.gap_hz() == Catch::Approx(2.0f).margin(0.1f));
        CHECK(s.advice().direction == BeltTurnDirection::LOOSEN);
    }

    SECTION("WRONG_WAY: the gap moved against the advice") {
        const auto& r = sweep(s, 112.0f, 98.0f);
        CHECK(r.outcome == BeltTuneOutcome::WRONG_WAY);
        CHECK(s.advice().direction == BeltTurnDirection::TIGHTEN);
    }

    SECTION("NO_MOVEMENT: under half a hertz either way") {
        CHECK(sweep(s, 112.0f, 100.3f).outcome == BeltTuneOutcome::NO_MOVEMENT);
        CHECK(sweep(s, 112.0f, 99.9f).outcome == BeltTuneOutcome::NO_MOVEMENT);
    }

    SECTION("both peaks drifting together is not a move") {
        // 1.5 Hz of common drift, no turn: the gap held at -12.
        const auto& r = sweep(s, 113.5f, 101.5f);
        CHECK(r.outcome == BeltTuneOutcome::NO_MOVEMENT);
        CHECK(s.gap_hz() == Catch::Approx(-12.0f).margin(0.1f));
    }

    SECTION("both peaks drifting down together is not a wrong way") {
        // B fell 1.5 Hz under TIGHTEN advice, but A fell with it.
        CHECK(sweep(s, 110.5f, 98.5f).outcome == BeltTuneOutcome::NO_MOVEMENT);
    }

    SECTION("the reference moving away is the gap moving the wrong way") {
        // B held at 100 while A rose 2 Hz: tightening B cannot widen the gap.
        CHECK(sweep(s, 114.0f, 100.0f).outcome == BeltTuneOutcome::WRONG_WAY);
    }

    SECTION("the reference coming closer is the gap closing") {
        CHECK(sweep(s, 108.0f, 100.0f).outcome == BeltTuneOutcome::CLOSER);
    }

    SECTION("MATCHED: the gap is within tolerance") {
        const auto& r = sweep(s, 112.0f, 111.4f);
        CHECK(r.outcome == BeltTuneOutcome::MATCHED);
        CHECK(s.zone_matched());
    }

    SECTION("NO_PEAK: a flat window leaves the gap alone") {
        const auto w = s.zone()->window;
        BeltCurve flat;
        for (float f = w.freq_start_hz; f <= w.freq_end_hz; f += 0.78125f) {
            flat.emplace_back(f, 5.0f);
        }
        const auto& r = s.add_round(one_peak(112.0f, w.freq_start_hz, w.freq_end_hz + 0.5f), flat);
        CHECK(r.outcome == BeltTuneOutcome::NO_PEAK);
        // The adjusted path lost its peak, not the reference.
        CHECK_FALSE(r.reference_lost);
        CHECK(s.gap_hz() == Catch::Approx(-12.0f).margin(0.1f));
        CHECK_FALSE(s.previous_gap_hz().has_value());
        CHECK(s.rounds().size() == 1);
    }

    SECTION("NO_PEAK: the reference's peak left the window") {
        const auto w = s.zone()->window;
        SECTION("its window is flat") {
            BeltCurve flat;
            for (float f = w.freq_start_hz; f <= w.freq_end_hz; f += 0.78125f) {
                flat.emplace_back(f, 5.0f);
            }
            const auto& r =
                s.add_round(flat, one_peak(104.0f, w.freq_start_hz, w.freq_end_hz + 0.5f));
            CHECK(r.outcome == BeltTuneOutcome::NO_PEAK);
            CHECK(r.reference_lost);
        }
        SECTION("it runs off the window's low edge") {
            // A, the reference, slid down past 95 Hz; B still peaks inside.
            const auto& r = sweep(s, 88.0f, 104.0f);
            CHECK(r.outcome == BeltTuneOutcome::NO_PEAK);
            CHECK(r.reference_lost);
        }
        SECTION("both windows flat: no path is singled out") {
            BeltCurve flat;
            for (float f = w.freq_start_hz; f <= w.freq_end_hz; f += 0.78125f) {
                flat.emplace_back(f, 5.0f);
            }
            const auto& r = s.add_round(flat, flat);
            CHECK(r.outcome == BeltTuneOutcome::NO_PEAK);
            CHECK_FALSE(r.reference_lost);
        }
    }

    SECTION("OUT_OF_WINDOW: re-centred on the reference, widened, gap kept") {
        // B jumped to 120: past the window's 117 Hz edge.
        const auto& r = sweep(s, 112.0f, 120.0f);
        CHECK(r.outcome == BeltTuneOutcome::OUT_OF_WINDOW);
        CHECK(s.gap_hz() == Catch::Approx(-12.0f).margin(0.1f));
        CHECK_FALSE(s.previous_gap_hz().has_value());
        const auto w = s.zone()->window;
        CHECK((w.freq_start_hz + w.freq_end_hz) / 2.0f == Catch::Approx(112.0f).margin(0.2f));
        CHECK(w.freq_end_hz > 117.0f + belt_tune::ZONE_MARGIN_HZ - 0.5f);

        // The next sweep covers the peak and measures the overshoot.
        const auto& next = sweep(s, 112.0f, 120.0f);
        CHECK(next.outcome == BeltTuneOutcome::OVERSHOT);
        CHECK(s.gap_hz() == Catch::Approx(8.0f).margin(0.1f));
        REQUIRE(s.previous_gap_hz().has_value());
        CHECK(*s.previous_gap_hz() == Catch::Approx(-12.0f).margin(0.1f));
    }

    SECTION("a gentle slope to the edge is NO_PEAK, a steep one OUT_OF_WINDOW") {
        const auto w = s.zone()->window;
        const auto ramp = [&](float top) {
            BeltCurve c;
            for (float f = w.freq_start_hz; f <= w.freq_end_hz; f += 0.78125f) {
                const float t = (f - w.freq_start_hz) / (w.freq_end_hz - w.freq_start_hz);
                c.emplace_back(f, 10.0f + (top - 10.0f) * t);
            }
            return c;
        };
        const auto ref = one_peak(112.0f, w.freq_start_hz, w.freq_end_hz + 0.5f);
        // Rises to 1.4x its minimum at the edge: under PEAK_MIN_PROMINENCE.
        CHECK(s.add_round(ref, ramp(14.0f)).outcome == BeltTuneOutcome::NO_PEAK);
        CHECK(s.zone()->window.freq_start_hz == Catch::Approx(w.freq_start_hz));
        CHECK(s.zone()->window.freq_end_hz == Catch::Approx(w.freq_end_hz));
        CHECK(s.add_round(ref, ramp(20.0f)).outcome == BeltTuneOutcome::OUT_OF_WINDOW);
    }

    SECTION("a second OUT_OF_WINDOW in a row is NO_PEAK and the window stays") {
        // B at 125 is past both the first window and the widened one.
        CHECK(sweep(s, 112.0f, 125.0f).outcome == BeltTuneOutcome::OUT_OF_WINDOW);
        const auto w = s.zone()->window;
        REQUIRE(w.freq_end_hz < 125.0f);
        CHECK(sweep(s, 112.0f, 125.0f).outcome == BeltTuneOutcome::NO_PEAK);
        CHECK(s.zone()->window.freq_start_hz == Catch::Approx(w.freq_start_hz));
        CHECK(s.zone()->window.freq_end_hz == Catch::Approx(w.freq_end_hz));
        CHECK(s.gap_hz() == Catch::Approx(-12.0f).margin(0.1f));
    }

    SECTION("OUT_OF_WINDOW stays inside the band") {
        auto edge = session_for(130.0f, 118.0f, BeltTensionerKind::UNKNOWN);
        REQUIRE(edge.zone().has_value());
        const auto& r = sweep(edge, 130.0f, 140.0f);
        CHECK(r.outcome == BeltTuneOutcome::OUT_OF_WINDOW);
        CHECK(edge.zone()->window.freq_end_hz <= HI);
    }
}

TEST_CASE("tune rate: median of real moves, recorded turns win", "[belt][tune]") {
    auto s = session_for(112.0f, 100.0f, BeltTensionerKind::SCREW);
    // The suggested quarter turn: +4 Hz, 16 Hz/turn.
    turn_as_advised(s);
    CHECK(sweep(s, 112.0f, 104.0f).turns_made == std::optional<float>(0.25f));
    // Half a turn barely moved it: excluded from the rate.
    s.record_turns(0.5f);
    const auto& still = sweep(s, 112.0f, 104.2f);
    CHECK(still.outcome == BeltTuneOutcome::NO_MOVEMENT);
    CHECK(still.turns_made == std::optional<float>(0.5f));
    // A quarter turn for 1.5 Hz: 6 Hz/turn.
    s.record_turns(0.25f);
    sweep(s, 112.0f, 105.7f);
    // An eighth for 1 Hz: 8 Hz/turn.
    s.record_turns(0.125f);
    sweep(s, 112.0f, 106.7f);
    // Median of 16, 6 and 8; counting the 0.4 Hz/turn round would give 7.
    REQUIRE(s.hz_per_turn().has_value());
    CHECK(*s.hz_per_turn() == Catch::Approx(8.0f).margin(0.4f));
    CHECK(s.total_turns(BeltPath::PATH_B) == Catch::Approx(1.125f));
    CHECK(s.total_turns(BeltPath::PATH_A) == Catch::Approx(0.0f));
    CHECK(s.total_rounds() == 4);
    CHECK(s.rounds().size() == 4);
}

TEST_CASE("tune rate: common drift teaches nothing", "[belt][tune]") {
    auto s = session_for(112.0f, 100.0f, BeltTensionerKind::SCREW);
    // A quarter turn booked, but both peaks rose 1.5 Hz: the gap held, so the
    // turn did nothing and no rate is learned.
    turn_as_advised(s);
    const auto& r = sweep(s, 113.5f, 101.5f);
    REQUIRE(r.turns_made == std::optional<float>(0.25f));
    CHECK(r.outcome == BeltTuneOutcome::NO_MOVEMENT);
    CHECK_FALSE(s.hz_per_turn().has_value());
    // A quarter turn that closed the gap 4 Hz while both drifted 1 Hz: 16 Hz/turn.
    s.record_turns(0.25f);
    REQUIRE(sweep(s, 114.5f, 106.5f).outcome == BeltTuneOutcome::CLOSER);
    REQUIRE(s.hz_per_turn().has_value());
    CHECK(*s.hz_per_turn() == Catch::Approx(16.0f).margin(0.4f));
}

TEST_CASE("tune session: a recorded zero is a round with no turn", "[belt][tune]") {
    auto s = session_for(112.0f, 100.0f, BeltTensionerKind::SCREW);
    s.record_turns(0.0f);
    const auto& r = sweep(s, 112.0f, 100.1f);
    CHECK(r.outcome == BeltTuneOutcome::NO_MOVEMENT);
    CHECK_FALSE(r.turns_made.has_value());
    CHECK(s.total_turns(BeltPath::PATH_B) == Catch::Approx(0.0f));
    // Nothing was turned, so nothing doubles: the first quarter turn stands.
    REQUIRE(s.advice().turns.has_value());
    CHECK(*s.advice().turns == Catch::Approx(belt_tune::FIRST_TURN));
    // The zero turn adds nothing behind the next sample: a quarter turn for 4 Hz.
    s.record_turns(0.25f);
    REQUIRE(sweep(s, 112.0f, 104.1f).outcome == BeltTuneOutcome::CLOSER);
    REQUIRE(s.hz_per_turn().has_value());
    CHECK(*s.hz_per_turn() == Catch::Approx(16.0f).margin(0.4f));
}

TEST_CASE("tune hints: zero-turn rounds never stall", "[belt][tune]") {
    auto s = session_for(104.0f, 100.0f, BeltTensionerKind::SCREW);
    REQUIRE_FALSE(s.show_mechanical_hint());
    for (int i = 0; i < belt_tune::STALL_ROUNDS; ++i) {
        s.record_turns(0.0f);
        const auto& r = sweep(s, 104.0f, 100.0f);
        REQUIRE(r.no_turn);
        REQUIRE(r.outcome == BeltTuneOutcome::NO_MOVEMENT);
    }
    CHECK_FALSE(s.show_mechanical_hint());
    // A recorded turn is not a no-turn round.
    s.record_turns(0.25f);
    CHECK_FALSE(sweep(s, 104.0f, 100.0f).no_turn);
}

TEST_CASE("tune rate: a wrong-way round teaches nothing", "[belt][tune]") {
    auto s = session_for(112.0f, 100.0f, BeltTensionerKind::SCREW);
    // A quarter turn moved B 2 Hz the wrong way: no rate, so the next
    // amount is the default, not |gap| over 8 Hz/turn.
    turn_as_advised(s);
    const auto& wrong = sweep(s, 112.0f, 98.0f);
    REQUIRE(wrong.outcome == BeltTuneOutcome::WRONG_WAY);
    CHECK_FALSE(s.hz_per_turn().has_value());
    REQUIRE(s.advice().turns.has_value());
    CHECK(*s.advice().turns == Catch::Approx(0.25f));
    // The next real move is the only sample: a quarter turn for 4 Hz.
    turn_as_advised(s);
    REQUIRE(sweep(s, 112.0f, 102.0f).outcome == BeltTuneOutcome::CLOSER);
    REQUIRE(s.hz_per_turn().has_value());
    CHECK(*s.hz_per_turn() == Catch::Approx(16.0f).margin(0.4f));
}

TEST_CASE("tune hints: unpaired zones and stalled rounds", "[belt][tune]") {
    SECTION("an unpaired zone shows the mechanical causes at once") {
        auto s = session_for(112.0f, 100.0f, BeltTensionerKind::SCREW);
        REQUIRE(s.zone()->source == BeltZoneSource::TALLEST_UNPAIRED);
        CHECK(s.show_mechanical_hint());
    }

    SECTION("an unpaired tallest peak shows them even when a pair is steered") {
        // B's 119.5 has no A peak within 30 Hz: the 60/63 pair is tuned, and
        // the shape warning stays on.
        const auto a = lorentzian_curve({{36.0f, 1e4f}, {60.0f, 5e3f}}, LO, HI);
        const auto b = lorentzian_curve({{35.0f, 9.5e3f}, {63.0f, 5e3f}, {119.5f, 1.2e4f}}, LO, HI);
        const auto cmp = compare_belt_paths(a, b, LO, HI);
        REQUIRE(cmp.peaks.pairs.size() == 2);
        BeltTuneSession s(a, b, cmp, LO, HI, BeltTensionerKind::SCREW);
        REQUIRE(s.zone().has_value());
        CHECK(s.zone()->source == BeltZoneSource::PAIRED);
        CHECK(s.reference_hz() == Catch::Approx(63.0f).margin(0.1f));
        CHECK(s.show_mechanical_hint());
    }

    SECTION("screw and unknown stall after eight rounds") {
        for (auto kind : {BeltTensionerKind::SCREW, BeltTensionerKind::UNKNOWN}) {
            auto s = session_for(104.0f, 100.0f, kind);
            REQUIRE(s.zone()->source == BeltZoneSource::PAIRED);
            for (int i = 0; i < 7; ++i) {
                turn_as_advised(s);
                sweep(s, 104.0f, 100.0f);
            }
            CHECK_FALSE(s.show_mechanical_hint());
            turn_as_advised(s);
            sweep(s, 104.0f, 100.0f);
            CHECK(s.show_mechanical_hint());
        }
    }

    SECTION("spring stalls after two rounds") {
        auto s = session_for(104.0f, 100.0f, BeltTensionerKind::SPRING);
        CHECK_FALSE(s.show_mechanical_hint());
        sweep(s, 104.0f, 100.0f);
        CHECK_FALSE(s.show_mechanical_hint());
        sweep(s, 104.0f, 100.0f);
        CHECK(s.show_mechanical_hint());
    }
}

TEST_CASE("tune next_zone walks the unmatched pairs, then stops", "[belt][tune]") {
    // Pairs 100/104 and 50/47: the 4 Hz gap first, A looser there.
    const auto a = lorentzian_curve({{50.0f, 1e4f}, {100.0f, 1e4f}}, LO, HI);
    const auto b = lorentzian_curve({{47.0f, 1e4f}, {104.0f, 1e4f}}, LO, HI);
    const auto cmp = compare_belt_paths(a, b, LO, HI);
    REQUIRE(cmp.peaks.pairs.size() == 2);
    BeltTuneSession s(a, b, cmp, LO, HI, BeltTensionerKind::SCREW);
    REQUIRE(s.zone().has_value());
    CHECK(s.adjusted_path() == BeltPath::PATH_A);
    CHECK(s.reference_hz() == Catch::Approx(104.0f).margin(0.1f));
    CHECK(s.has_next_zone());

    const auto w = s.zone()->window;
    turn_as_advised(s);
    s.add_round(one_peak(104.0f, w.freq_start_hz, w.freq_end_hz + 0.5f),
                one_peak(104.0f, w.freq_start_hz, w.freq_end_hz + 0.5f));
    REQUIRE(s.zone_matched());
    REQUIRE(s.hz_per_turn().has_value());

    REQUIRE(s.next_zone());
    // B is adjusted now: A's learned rate does not carry over.
    CHECK_FALSE(s.hz_per_turn().has_value());
    CHECK(s.rounds().empty());
    CHECK(s.total_rounds() == 1);
    CHECK_FALSE(s.zone_matched());
    CHECK(s.adjusted_path() == BeltPath::PATH_B);
    CHECK(s.reference_hz() == Catch::Approx(50.0f).margin(0.1f));
    CHECK(s.adjusted_hz() == Catch::Approx(47.0f).margin(0.1f));
    CHECK_FALSE(s.has_next_zone());

    CHECK_FALSE(s.next_zone());
}

TEST_CASE("tune session: a pair within one bin is already matched", "[belt][tune]") {
    // 1.5 Hz bins: the pair is 1.5 Hz apart, over the 1 Hz floor but within a bin.
    const auto a = lorentzian_curve({{99.5f, 1e4f}}, LO, HI, 1.5f);
    const auto b = lorentzian_curve({{101.0f, 1e4f}}, LO, HI, 1.5f);
    const auto cmp = compare_belt_paths(a, b, LO, HI);
    REQUIRE(pick_tune_zone(cmp, BeltPath::PATH_A, LO, HI, {}).has_value());
    BeltTuneSession s(a, b, cmp, LO, HI, BeltTensionerKind::SCREW);
    CHECK(s.tolerance_hz() == Catch::Approx(1.5f).margin(0.01f));
    CHECK_FALSE(s.zone().has_value());
    CHECK_FALSE(s.has_next_zone());
}

// ============================================================================
// Floors, prominence, LOOSEN outcomes and zones across rounds
// ============================================================================

TEST_CASE("tune zone: the amplitude floor follows the in-band maximum", "[belt][tune]") {
    // Peaks alone put the floor at 20; a 400 band maximum puts it at 80.
    auto c =
        cmp_with({{pk(50.0f, 100.0f), pk(52.0f, 100.0f)}, {pk(80.0f, 60.0f), pk(86.0f, 60.0f)}});
    auto z = pick_tune_zone(c, BeltPath::PATH_A, LO, HI, {});
    REQUIRE(z.has_value());
    REQUIRE(z->adj_hz == Catch::Approx(80.0f));
    c.band_max_psd = 400.0f;
    z = pick_tune_zone(c, BeltPath::PATH_A, LO, HI, {});
    REQUIRE(z.has_value());
    CHECK(z->adj_hz == Catch::Approx(50.0f));

    // compare_belt_paths records the stronger curve's in-band maximum.
    const auto a = lorentzian_curve({{60.0f, 5e3f}}, LO, HI);
    const auto b = lorentzian_curve({{60.0f, 7e3f}}, LO, HI);
    float top = 0.0f;
    for (const auto& [f, v] : b) {
        top = std::max(top, v);
    }
    CHECK(compare_belt_paths(a, b, LO, HI).band_max_psd == Catch::Approx(top));
}

TEST_CASE("tune peak: prominence over the unexcited floor", "[belt][tune]") {
    SECTION("the real Voron B hump in a centred 20 Hz window holds a peak") {
        const auto full = load_fixture("voron24_kalico_axis_1_1.csv");
        const BeltSweepWindow w{109.5f, 129.5f};
        const auto in_window = curve_in_band(full, w.freq_start_hz, w.freq_end_hz);
        float lo = in_window.front().second, hi = lo;
        for (const auto& [f, v] : in_window) {
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        // The hump is broad: inside the window it never reaches 1.5x its own minimum.
        REQUIRE(hi < belt_tune::PEAK_MIN_PROMINENCE * lo);
        float floor = hi;
        for (const auto& [f, v] : curve_in_band(full, 5.0f, 135.0f)) {
            floor = std::min(floor, v);
        }
        const auto swept = windowed(full, w, floor);
        CHECK(window_holds_peak(swept, w, w, 5.0f, 135.0f));
        const auto f =
            refine_peak_in_window(swept, w, 119.5f, unexcited_floor_psd(swept, w, 5.0f, 135.0f));
        REQUIRE(f.has_value());
        CHECK(*f == Catch::Approx(119.5f).margin(1.0f));
    }

    SECTION("a window at the floor holds no peak") {
        const BeltSweepWindow w{95.0f, 117.0f};
        const auto flat = windowed(lorentzian_curve({}, LO, HI, 3200.0f / 4096.0f, 5.0f), w, 5.0f);
        REQUIRE(unexcited_floor_psd(flat, w, LO, HI).has_value());
        CHECK_FALSE(window_holds_peak(flat, w, w, LO, HI));
    }

    SECTION("with too few unexcited bins, the window's own minimum is the floor") {
        const BeltSweepWindow w{95.0f, 117.0f};
        const auto only_window = lorentzian_curve({{106.0f, 1e4f}}, 95.0f, 117.5f);
        CHECK_FALSE(unexcited_floor_psd(only_window, w, LO, HI).has_value());
        CHECK(window_holds_peak(only_window, w, w, LO, HI));
    }

    SECTION("candidates need the near-peak fraction and the floor prominence") {
        // A sharp peak moved from 100 to 104, and a 15% ripple left at 100.
        auto c = lorentzian_curve({{104.0f, 1e4f}}, 80.0f, 130.0f, 3200.0f / 4096.0f, 0.01f, 1.5f);
        auto nearest = c.begin();
        for (auto it = c.begin(); it != c.end(); ++it) {
            if (std::abs(it->first - 100.0f) < std::abs(nearest->first - 100.0f)) {
                nearest = it;
            }
        }
        nearest->second += 1.5e3f;
        const BeltSweepWindow w{90.0f, 120.0f};
        REQUIRE(detect_belt_peaks(c, w.freq_start_hz, w.freq_end_hz).size() == 2);
        const auto f = refine_peak_in_window(c, w, 100.0f);
        REQUIRE(f.has_value());
        CHECK(*f == Catch::Approx(104.0f).margin(0.2f));

        // A 60% second peak competes, unless the floor puts it under 1.5x.
        const auto two = lorentzian_curve({{95.0f, 1e4f}, {115.0f, 6e3f}}, 80.0f, 130.0f);
        const BeltSweepWindow w2{85.0f, 125.0f};
        const auto near_high = refine_peak_in_window(two, w2, 114.0f);
        REQUIRE(near_high.has_value());
        CHECK(*near_high == Catch::Approx(115.0f).margin(0.3f));
        const auto floored = refine_peak_in_window(two, w2, 114.0f, 4.5e3f);
        REQUIRE(floored.has_value());
        CHECK(*floored == Catch::Approx(95.0f).margin(0.3f));
    }
}

TEST_CASE("tune outcomes under LOOSEN advice", "[belt][tune]") {
    // After the swap A, at 112, is adjusted against B at 100: loosen A.
    auto s = session_for(112.0f, 100.0f, BeltTensionerKind::UNKNOWN);
    s.swap_adjusted_path();
    REQUIRE(s.adjusted_path() == BeltPath::PATH_A);
    REQUIRE(s.advice().direction == BeltTurnDirection::LOOSEN);

    SECTION("CLOSER") {
        CHECK(sweep(s, 108.0f, 100.0f).outcome == BeltTuneOutcome::CLOSER);
    }
    SECTION("OVERSHOT") {
        CHECK(sweep(s, 98.0f, 100.0f).outcome == BeltTuneOutcome::OVERSHOT);
        CHECK(s.advice().direction == BeltTurnDirection::TIGHTEN);
    }
    SECTION("WRONG_WAY") {
        CHECK(sweep(s, 114.0f, 100.0f).outcome == BeltTuneOutcome::WRONG_WAY);
    }
    SECTION("NO_MOVEMENT") {
        CHECK(sweep(s, 111.7f, 100.0f).outcome == BeltTuneOutcome::NO_MOVEMENT);
    }
}

TEST_CASE("tune OUT_OF_WINDOW: an edge-dominant window beside a ripple, once per zone",
          "[belt][tune]") {
    auto s = session_for(112.0f, 100.0f, BeltTensionerKind::UNKNOWN);

    SECTION("a ripple left behind does not hide a peak past the edge") {
        // B's peak ran to 120, past 117; a sharp 15% ripple stays at 100.
        auto b = one_peak(120.0f);
        for (auto& [f, v] : b) {
            if (std::abs(f - 100.0f) < 0.4f) {
                v += 1.5e3f;
            }
        }
        CHECK(sweep_curves(s, one_peak(112.0f), b).outcome == BeltTuneOutcome::OUT_OF_WINDOW);
    }

    SECTION("O, then NO_PEAK until a peak is measured, then O again") {
        CHECK(sweep(s, 112.0f, 125.0f).outcome == BeltTuneOutcome::OUT_OF_WINDOW);
        const auto w = s.zone()->window;
        CHECK(sweep(s, 112.0f, 125.0f).outcome == BeltTuneOutcome::NO_PEAK);
        CHECK(sweep(s, 112.0f, 125.0f).outcome == BeltTuneOutcome::NO_PEAK);
        CHECK(s.zone()->window.freq_end_hz == Catch::Approx(w.freq_end_hz));
        CHECK(sweep(s, 112.0f, 115.0f).outcome == BeltTuneOutcome::OVERSHOT);
        CHECK(sweep(s, 112.0f, 125.0f).outcome == BeltTuneOutcome::OUT_OF_WINDOW);
    }
}

TEST_CASE("tune zones: a later zone starts from its own measurement", "[belt][tune]") {
    // A is looser in both zones: 100 vs 104, then 47 vs 50.
    const auto a = lorentzian_curve({{47.0f, 1e4f}, {100.0f, 1e4f}}, LO, HI);
    const auto b = lorentzian_curve({{50.0f, 1e4f}, {104.0f, 1e4f}}, LO, HI);
    const auto cmp = compare_belt_paths(a, b, LO, HI);
    REQUIRE(cmp.peaks.pairs.size() == 2);
    BeltTuneSession s(a, b, cmp, LO, HI, BeltTensionerKind::SCREW);
    REQUIRE(s.adjusted_path() == BeltPath::PATH_A);
    REQUIRE_FALSE(s.needs_baseline());

    // A quarter turn takes A's 100 Hz mode to 104: 16 Hz/turn, matched.
    turn_as_advised(s);
    REQUIRE(sweep_curves(s, lorentzian_curve({{48.5f, 1e4f}, {104.0f, 1e4f}}, LO, HI), b).outcome ==
            BeltTuneOutcome::MATCHED);
    REQUIRE(s.hz_per_turn() == Catch::Approx(16.0f).margin(0.5f));

    REQUIRE(s.next_zone());
    CHECK(s.adjusted_path() == BeltPath::PATH_A);
    CHECK(s.needs_baseline());
    // The first zone's turn already moved A's 47 Hz mode to 48.5: a baseline,
    // not a 1.5 Hz move for the suggested quarter turn.
    const auto& base =
        sweep_curves(s, lorentzian_curve({{48.5f, 1e4f}, {104.0f, 1e4f}}, LO, HI), b);
    CHECK(base.outcome == BeltTuneOutcome::BASELINE);
    CHECK_FALSE(base.turns_made.has_value());
    CHECK_FALSE(s.needs_baseline());
    CHECK_FALSE(s.previous_gap_hz().has_value());
    CHECK(s.adjusted_hz() == Catch::Approx(48.5f).margin(0.1f));
    CHECK(s.hz_per_turn() == Catch::Approx(16.0f).margin(0.5f));
    CHECK(s.total_turns(BeltPath::PATH_A) == Catch::Approx(0.25f));

    // The next round measures this zone alone: the suggested eighth moved A
    // 1 Hz, 8 Hz/turn; the median of 16 and 8 is 12.
    turn_as_advised(s);
    const auto& r = sweep_curves(s, lorentzian_curve({{49.5f, 1e4f}, {104.0f, 1e4f}}, LO, HI), b);
    CHECK(r.outcome == BeltTuneOutcome::MATCHED);
    REQUIRE(s.previous_gap_hz().has_value());
    CHECK(*s.previous_gap_hz() == Catch::Approx(-1.5f).margin(0.1f));
    CHECK(r.turns_made == std::optional<float>(0.125f));
    CHECK(s.hz_per_turn() == Catch::Approx(12.0f).margin(0.5f));
}

TEST_CASE("tune zones: a swap sticks across zones", "[belt][tune]") {
    // A is the looser path in both zones; swapping to B in the first keeps B.
    const auto a = lorentzian_curve({{47.0f, 1e4f}, {100.0f, 1e4f}}, LO, HI);
    const auto b = lorentzian_curve({{50.0f, 1e4f}, {104.0f, 1e4f}}, LO, HI);
    BeltTuneSession s(a, b, compare_belt_paths(a, b, LO, HI), LO, HI, BeltTensionerKind::UNKNOWN);
    REQUIRE(s.adjusted_path() == BeltPath::PATH_A);
    s.swap_adjusted_path();
    sweep(s, 100.0f, 100.0f);
    REQUIRE(s.zone_matched());
    REQUIRE(s.next_zone());
    CHECK(s.adjusted_path() == BeltPath::PATH_B);

    BeltTuneSession t(a, b, compare_belt_paths(a, b, LO, HI), LO, HI, BeltTensionerKind::UNKNOWN);
    sweep(t, 104.0f, 104.0f);
    REQUIRE(t.zone_matched());
    REQUIRE(t.next_zone());
    CHECK(t.adjusted_path() == BeltPath::PATH_A);
}

TEST_CASE("tune hints: spring rounds that improve do not stall", "[belt][tune]") {
    auto s = session_for(104.0f, 100.0f, BeltTensionerKind::SPRING);
    REQUIRE_FALSE(s.show_mechanical_hint());
    REQUIRE(sweep(s, 104.0f, 101.0f).outcome == BeltTuneOutcome::CLOSER);
    REQUIRE(sweep(s, 104.0f, 102.0f).outcome == BeltTuneOutcome::CLOSER);
    CHECK_FALSE(s.show_mechanical_hint());
    REQUIRE(sweep(s, 104.0f, 102.1f).outcome == BeltTuneOutcome::NO_MOVEMENT);
    CHECK_FALSE(s.show_mechanical_hint());
    REQUIRE(sweep(s, 104.0f, 102.2f).outcome == BeltTuneOutcome::NO_MOVEMENT);
    CHECK(s.show_mechanical_hint());
}

TEST_CASE("tune session: a round with no zone books no turns", "[belt][tune]") {
    const auto a = lorentzian_curve({{99.5f, 1e4f}}, LO, HI, 1.5f);
    const auto b = lorentzian_curve({{101.0f, 1e4f}}, LO, HI, 1.5f);
    BeltTuneSession s(a, b, compare_belt_paths(a, b, LO, HI), LO, HI, BeltTensionerKind::SCREW);
    REQUIRE_FALSE(s.zone().has_value());
    s.record_turns(0.5f);
    CHECK(s.add_round(a, b).outcome == BeltTuneOutcome::NO_PEAK);
    CHECK(s.total_turns(BeltPath::PATH_A) == Catch::Approx(0.0f));
    CHECK(s.total_turns(BeltPath::PATH_B) == Catch::Approx(0.0f));
}

TEST_CASE("turn counts read as eighth-fraction glyphs", "[belt][tune]") {
    using helix::calibration::format_belt_turns;
    CHECK(format_belt_turns(0.125f) == "⅛");
    CHECK(format_belt_turns(0.25f) == "¼");
    CHECK(format_belt_turns(0.375f) == "⅜");
    CHECK(format_belt_turns(0.5f) == "½");
    CHECK(format_belt_turns(0.625f) == "⅝");
    CHECK(format_belt_turns(0.75f) == "¾");
    CHECK(format_belt_turns(0.875f) == "⅞");
    CHECK(format_belt_turns(1.0f) == "1");
    CHECK(format_belt_turns(1.25f) == "1¼");
    CHECK(format_belt_turns(2.0f) == "2");
    // Loosening totals are negative; the words carry the direction.
    CHECK(format_belt_turns(-0.875f) == "⅞");
    // Rounds to the nearest eighth.
    CHECK(format_belt_turns(0.3f) == "¼");
    CHECK(format_belt_turns(0.33f) == "⅜");
    CHECK(format_belt_turns(0.04f) == "0");
}

TEST_CASE("tune session: an unrecorded stepper is a round with no turn", "[belt][tune]") {
    auto s = session_for(112.0f, 100.0f, BeltTensionerKind::SCREW);
    REQUIRE(s.advice().turns == std::optional<float>(belt_tune::FIRST_TURN));

    SECTION("a small move the wrong way is no movement, not a wrong way") {
        // The gap moved 0.6 Hz against the TIGHTEN advice with nothing turned.
        const auto& r = sweep(s, 112.0f, 99.4f);
        CHECK(r.no_turn);
        CHECK_FALSE(r.turns_made.has_value());
        CHECK(r.outcome == BeltTuneOutcome::NO_MOVEMENT);
        // The reading is adopted.
        CHECK(s.gap_hz() == Catch::Approx(-12.6f).margin(0.1f));
        CHECK(s.total_turns(BeltPath::PATH_B) == Catch::Approx(0.0f));
    }
    SECTION("a large move either way is still no movement and teaches no rate") {
        CHECK(sweep(s, 112.0f, 98.0f).outcome == BeltTuneOutcome::NO_MOVEMENT);
        CHECK(sweep(s, 112.0f, 102.0f).outcome == BeltTuneOutcome::NO_MOVEMENT);
        CHECK_FALSE(s.hz_per_turn().has_value());
        // Nothing was turned, so nothing doubles.
        CHECK(s.advice().turns == std::optional<float>(belt_tune::FIRST_TURN));
    }
    SECTION("a match is still a match") {
        CHECK(sweep(s, 112.0f, 111.9f).outcome == BeltTuneOutcome::MATCHED);
    }
    SECTION("a recorded half turn is booked") {
        s.record_turns(0.5f);
        const auto& r = sweep(s, 112.0f, 104.0f);
        CHECK_FALSE(r.no_turn);
        CHECK(r.turns_made == std::optional<float>(0.5f));
        CHECK(r.outcome == BeltTuneOutcome::CLOSER);
        CHECK(s.total_turns(BeltPath::PATH_B) == Catch::Approx(0.5f));
    }
}

TEST_CASE("tune session: a turn carried past a peakless round is still a turn", "[belt][tune]") {
    auto s = session_for(112.0f, 100.0f, BeltTensionerKind::SCREW);

    SECTION("OUT_OF_WINDOW, then an untouched round that closed the gap") {
        s.record_turns(0.25f);
        REQUIRE(sweep(s, 112.0f, 125.0f).outcome == BeltTuneOutcome::OUT_OF_WINDOW);
        const auto& r = sweep(s, 112.0f, 105.0f); // stepper left untouched
        CHECK_FALSE(r.no_turn);
        CHECK(r.outcome == BeltTuneOutcome::CLOSER);
        // The quarter turn moved the gap 5 Hz: 20 Hz/turn.
        REQUIRE(s.hz_per_turn().has_value());
        CHECK(*s.hz_per_turn() == Catch::Approx(20.0f).margin(0.5f));
    }
    SECTION("OUT_OF_WINDOW, then an untouched round that widened the gap") {
        s.record_turns(0.25f);
        REQUIRE(sweep(s, 112.0f, 125.0f).outcome == BeltTuneOutcome::OUT_OF_WINDOW);
        // The widened window starts above 100, so the gap widens from A.
        const auto& r = sweep(s, 117.0f, 104.0f);
        CHECK_FALSE(r.no_turn);
        CHECK(r.outcome == BeltTuneOutcome::WRONG_WAY);
        CHECK_FALSE(s.hz_per_turn().has_value());
    }
    SECTION("NO_PEAK, then an untouched round that closed the gap") {
        s.record_turns(0.25f);
        const auto w = s.zone()->window;
        BeltCurve flat;
        for (float f = w.freq_start_hz; f <= w.freq_end_hz; f += 0.78125f) {
            flat.emplace_back(f, 5.0f);
        }
        REQUIRE(
            s.add_round(one_peak(112.0f, w.freq_start_hz, w.freq_end_hz + 0.5f), flat).outcome ==
            BeltTuneOutcome::NO_PEAK);
        const auto& r = sweep(s, 112.0f, 104.0f);
        CHECK_FALSE(r.no_turn);
        CHECK(r.outcome == BeltTuneOutcome::CLOSER);
        REQUIRE(s.hz_per_turn().has_value());
        CHECK(*s.hz_per_turn() == Catch::Approx(16.0f).margin(0.5f));
    }
    SECTION("the round after a measured one is a repeat check again") {
        s.record_turns(0.25f);
        REQUIRE(sweep(s, 112.0f, 104.0f).outcome == BeltTuneOutcome::CLOSER);
        CHECK(sweep(s, 112.0f, 104.1f).no_turn);
    }
}

TEST_CASE("tune advice: a repeat check keeps the doubled turn", "[belt][tune]") {
    auto s = session_for(112.0f, 100.0f, BeltTensionerKind::SCREW);
    s.record_turns(0.25f);
    REQUIRE(sweep(s, 112.0f, 100.2f).outcome == BeltTuneOutcome::NO_MOVEMENT);
    REQUIRE(s.advice().turns == std::optional<float>(0.5f));
    // A repeat check in between changes nothing.
    REQUIRE(sweep(s, 112.0f, 100.1f).no_turn);
    CHECK(s.advice().turns == std::optional<float>(0.5f));
}

TEST_CASE("tune advice: a turn carried past a peakless round still doubles", "[belt][tune]") {
    auto s = session_for(112.0f, 100.0f, BeltTensionerKind::SCREW);
    s.record_turns(0.25f);
    const auto w = s.zone()->window;
    BeltCurve flat;
    for (float f = w.freq_start_hz; f <= w.freq_end_hz; f += 0.78125f) {
        flat.emplace_back(f, 5.0f);
    }
    REQUIRE(s.add_round(one_peak(112.0f, w.freq_start_hz, w.freq_end_hz + 0.5f), flat).outcome ==
            BeltTuneOutcome::NO_PEAK);
    // The stepper is left untouched; the quarter turn moved nothing.
    const auto& r = sweep(s, 112.0f, 100.2f);
    REQUIRE_FALSE(r.no_turn);
    REQUIRE(r.outcome == BeltTuneOutcome::NO_MOVEMENT);
    CHECK(r.turns_measured == Catch::Approx(0.25f));
    CHECK(s.advice().turns == std::optional<float>(0.5f));
}

// ============================================================================
// In-plane zone candidates, centroids, power and the sweep window
// ============================================================================

namespace {

constexpr float BIN_HZ = 3200.0f / 4096.0f;

/// The frequency of the tallest bin of `curve` inside the zone (inclusive).
float tallest_bin_hz(const BeltCurve& curve, BeltSweepWindow zone) {
    const auto x = curve_in_band(curve, zone.freq_start_hz, zone.freq_end_hz);
    REQUIRE_FALSE(x.empty());
    return std::max_element(x.begin(), x.end(),
                            [](const auto& l, const auto& r) { return l.second < r.second; })
        ->first;
}

/// The curve with the bin nearest `hz` raised to `value`.
BeltCurve with_spike(BeltCurve curve, float hz, float value) {
    auto it = std::min_element(curve.begin(), curve.end(), [hz](const auto& l, const auto& r) {
        return std::abs(l.first - hz) < std::abs(r.first - hz);
    });
    it->second = value;
    return curve;
}

/// Each path's peaks, the same on both, as a response with 0.9 in-plane share.
BeltResponse peaks_response(std::vector<std::pair<float, float>> peaks) {
    return response_from(lorentzian_curve(std::move(peaks), LO, HI, BIN_HZ, 0.01f, 1.0f));
}

} // namespace

TEST_CASE("zone candidates: the Voron's settled capture has two, strongest first", "[belt][tune]") {
    const auto a = load_fixture_response("voron24_settled", 'A');
    const auto b = load_fixture_response("voron24_settled", 'B');
    const auto c = tune_zone_candidates(a, b, 5.0f, 135.0f);
    REQUIRE(c.size() == 2);
    CHECK(c[0].zone.source == BeltZoneSource::PAIRED);
    CHECK(c[0].zone.adj_hz == Catch::Approx(128.6f).margin(0.1f));
    CHECK(c[0].zone.ref_hz == Catch::Approx(128.6f).margin(0.1f));
    CHECK(c[0].zone.window.freq_start_hz == Catch::Approx(118.6f).margin(0.1f));
    CHECK(c[0].zone.window.freq_end_hz == Catch::Approx(135.0f).margin(0.1f));
    CHECK(c[1].zone.source == BeltZoneSource::PAIRED);
    CHECK(c[1].zone.adj_hz == Catch::Approx(37.8f).margin(0.1f));
    CHECK(c[1].zone.ref_hz == Catch::Approx(37.8f).margin(0.1f));
    CHECK(c[1].zone.window.freq_start_hz == Catch::Approx(27.8f).margin(0.1f));
    CHECK(c[1].zone.window.freq_end_hz == Catch::Approx(47.8f).margin(0.1f));
    CHECK(c[0].strength > c[1].strength);
}

TEST_CASE("zone candidates: the K1C's settled capture has one", "[belt][tune]") {
    const auto a = load_fixture_response("k1c_settled", 'A');
    const auto b = load_fixture_response("k1c_settled", 'B');
    const auto c = tune_zone_candidates(a, b, 5.0f, 133.0f);
    REQUIRE(c.size() == 1);
    CHECK(c[0].zone.adj_hz == Catch::Approx(58.4f).margin(0.1f));
    CHECK(c[0].zone.ref_hz == Catch::Approx(58.4f).margin(0.1f));
    CHECK(c[0].zone.window.freq_start_hz == Catch::Approx(48.4f).margin(0.1f));
    CHECK(c[0].zone.window.freq_end_hz == Catch::Approx(68.4f).margin(0.1f));
}

TEST_CASE("zone centroid: the in-plane centroids of the hardware captures", "[belt][tune]") {
    struct Row {
        const char* name;
        BeltSweepWindow zone;
        float a_hz;
        float b_hz;
    };
    const Row rows[] = {
        {"voron24_settled", {118.6f, 135.0f}, 129.12f, 127.17f},
        {"voron24_turn1", {118.6f, 135.0f}, 130.04f, 126.92f},
        {"voron24_turn2", {118.6f, 135.0f}, 128.31f, 129.79f},
        {"voron24_settled_repeat", {118.6f, 135.0f}, 130.28f, 126.37f},
        {"k1c_settled", {48.4f, 68.4f}, 59.19f, 59.34f},
    };
    for (const auto& row : rows) {
        INFO(row.name);
        const auto a =
            zone_centroid_hz(tuning_curve(load_fixture_response(row.name, 'A')), row.zone);
        const auto b =
            zone_centroid_hz(tuning_curve(load_fixture_response(row.name, 'B')), row.zone);
        REQUIRE(a.has_value());
        REQUIRE(b.has_value());
        CHECK(*a == Catch::Approx(row.a_hz).margin(0.05f));
        CHECK(*b == Catch::Approx(row.b_hz).margin(0.05f));
    }
}

TEST_CASE("zone centroid: the zone centroid wanders less than the tallest zone bin",
          "[belt][tune]") {
    const BeltSweepWindow zone{118.6f, 135.0f};
    for (const char path : {'A', 'B'}) {
        INFO("path " << path);
        const auto settled = tuning_curve(load_fixture_response("voron24_settled", path));
        const auto repeat = tuning_curve(load_fixture_response("voron24_settled_repeat", path));
        const float centroid_move =
            std::abs(*zone_centroid_hz(repeat, zone) - *zone_centroid_hz(settled, zone));
        const float tallest_move =
            std::abs(tallest_bin_hz(repeat, zone) - tallest_bin_hz(settled, zone));
        CHECK(centroid_move == Catch::Approx(path == 'A' ? 1.16f : 0.80f).margin(0.05f));
        CHECK(tallest_move == Catch::Approx(1.5f).margin(0.1f));
        CHECK(centroid_move < tallest_move);
    }
}

TEST_CASE("zone centroid: too few zone bins or no power is no centroid", "[belt][tune]") {
    const BeltCurve two_bins = {{100.0f, 1.0f}, {101.0f, 2.0f}, {120.0f, 3.0f}};
    CHECK_FALSE(zone_centroid_hz(two_bins, {99.0f, 110.0f}).has_value());
    const BeltCurve zeros = {{100.0f, 0.0f}, {101.0f, 0.0f}, {102.0f, 0.0f}};
    CHECK_FALSE(zone_centroid_hz(zeros, {99.0f, 110.0f}).has_value());
    const BeltCurve three = {{100.0f, 1.0f}, {101.0f, 2.0f}, {102.0f, 1.0f}};
    REQUIRE(zone_centroid_hz(three, {100.0f, 102.0f}).has_value());
    CHECK(*zone_centroid_hz(three, {100.0f, 102.0f}) == Catch::Approx(101.0f));
}

TEST_CASE("zone power hint: the Voron's B path resonates far harder", "[belt][tune]") {
    const BeltSweepWindow zone{118.6f, 135.0f};
    SECTION("settled") {
        const auto h =
            zone_power_hint(tuning_curve(load_fixture_response("voron24_settled", 'A')),
                            tuning_curve(load_fixture_response("voron24_settled", 'B')), zone);
        REQUIRE(h.has_value());
        CHECK(h->stronger == BeltPath::PATH_B);
        CHECK(h->ratio == Catch::Approx(3.65f).margin(0.05f));
        CHECK(h->freq_hz == Catch::Approx(127.17f).margin(0.05f));
    }
    SECTION("two turns") {
        const auto h =
            zone_power_hint(tuning_curve(load_fixture_response("voron24_turn2", 'A')),
                            tuning_curve(load_fixture_response("voron24_turn2", 'B')), zone);
        REQUIRE(h.has_value());
        CHECK(h->stronger == BeltPath::PATH_B);
        CHECK(h->ratio == Catch::Approx(15.3f).margin(0.3f));
    }
    SECTION("the K1C's paths are within 3x") {
        CHECK_FALSE(zone_power_hint(tuning_curve(load_fixture_response("k1c_settled", 'A')),
                                    tuning_curve(load_fixture_response("k1c_settled", 'B')),
                                    {48.4f, 68.4f})
                        .has_value());
    }
}

TEST_CASE("zone power hint: equal, exactly 3x and zero power give none", "[belt][tune]") {
    const BeltSweepWindow zone{100.0f, 104.0f};
    const auto flat = [](float v) {
        BeltCurve c;
        for (float f = 100.0f; f <= 104.0f; f += 1.0f) {
            c.emplace_back(f, v);
        }
        return c;
    };
    CHECK(zone_power(flat(2.0f), zone) == Catch::Approx(10.0f));
    CHECK_FALSE(zone_power_hint(flat(1.0f), flat(1.0f), zone).has_value());
    CHECK_FALSE(zone_power_hint(flat(1.0f), flat(3.0f), zone).has_value());
    CHECK_FALSE(zone_power_hint(flat(0.0f), flat(3.0f), zone).has_value());
    const auto h = zone_power_hint(flat(4.0f), flat(1.0f), zone);
    REQUIRE(h.has_value());
    CHECK(h->stronger == BeltPath::PATH_A);
    CHECK(h->ratio == Catch::Approx(4.0f));
    CHECK(h->freq_hz == Catch::Approx(102.0f));
}

TEST_CASE("zone candidates: a peak needs half its motion in the XY plane", "[belt][tune]") {
    const auto peak = one_peak(100.0f);
    const auto b = response_from(peak, 0.9f);
    CHECK(tune_zone_candidates(response_from(peak, 0.4f), b, LO, HI).empty());
    CHECK(tune_zone_candidates(response_from(peak, 0.5f), b, LO, HI).size() == 1);
}

TEST_CASE("in-plane fraction: no per-axis columns reads 1, no xyz power reads 0", "[belt][tune]") {
    CHECK(in_plane_fraction(BeltResponse{one_peak(100.0f), {}}, 100.0f) == 1.0f);
    const BeltCurve zeros = {{99.0f, 0.0f}, {100.0f, 0.0f}, {101.0f, 0.0f}};
    CHECK(in_plane_fraction(BeltResponse{zeros, zeros}, 100.0f) == 0.0f);
    const BeltResponse r{{{99.0f, 4.0f}, {100.0f, 8.0f}}, {{99.0f, 1.0f}, {100.0f, 6.0f}}};
    CHECK(in_plane_fraction(r, 99.9f) == Catch::Approx(0.75f));
    CHECK(&tuning_curve(r) == &r.in_plane);
    const BeltResponse xyz_only{{{99.0f, 4.0f}}, {}};
    CHECK(&tuning_curve(xyz_only) == &xyz_only.xyz);
}

TEST_CASE("zone candidates: without per-axis columns, psd_xyz is tuned", "[belt][tune]") {
    const auto a = load_fixture_response("voron24_settled", 'A');
    const auto b = load_fixture_response("voron24_settled", 'B');
    const auto bare =
        tune_zone_candidates(BeltResponse{a.xyz, {}}, BeltResponse{b.xyz, {}}, LO, HI);
    const auto same =
        tune_zone_candidates(BeltResponse{a.xyz, a.xyz}, BeltResponse{b.xyz, b.xyz}, LO, HI);
    REQUIRE_FALSE(bare.empty());
    REQUIRE(bare.size() == same.size());
    for (size_t i = 0; i < bare.size(); ++i) {
        CHECK(bare[i].zone.adj_hz == same[i].zone.adj_hz);
        CHECK(bare[i].zone.ref_hz == same[i].zone.ref_hz);
        CHECK(bare[i].zone.window.freq_start_hz == same[i].zone.window.freq_start_hz);
        CHECK(bare[i].zone.window.freq_end_hz == same[i].zone.window.freq_end_hz);
        CHECK(bare[i].strength == same[i].strength);
    }
}

TEST_CASE("zone candidates: a weaker candidate within 10 Hz of a stronger one is dropped",
          "[belt][tune]") {
    SECTION("30 and 37 Hz keep only 37") {
        const auto r = peaks_response({{30.0f, 1e4f}, {37.0f, 1.5e4f}});
        const auto c = tune_zone_candidates(r, r, LO, HI);
        REQUIRE(c.size() == 1);
        CHECK(c[0].zone.adj_hz == Catch::Approx(37.0f).margin(0.5f));
    }
    SECTION("30 and 45 Hz keep both") {
        const auto r = peaks_response({{30.0f, 1e4f}, {45.0f, 1.5e4f}});
        const auto c = tune_zone_candidates(r, r, LO, HI);
        REQUIRE(c.size() == 2);
        CHECK(c[0].zone.adj_hz == Catch::Approx(45.0f).margin(0.5f));
        CHECK(c[1].zone.adj_hz == Catch::Approx(30.0f).margin(0.5f));
    }
    SECTION("six well-separated pairs leave the strongest four") {
        const auto r = peaks_response({{20.0f, 3e3f},
                                       {40.0f, 4e3f},
                                       {60.0f, 5e3f},
                                       {80.0f, 6e3f},
                                       {100.0f, 7e3f},
                                       {120.0f, 8e3f}});
        const auto c = tune_zone_candidates(r, r, LO, HI);
        REQUIRE(c.size() == static_cast<size_t>(belt_tune::MAX_ZONE_CANDIDATES));
        const float want[] = {120.0f, 100.0f, 80.0f, 60.0f};
        for (size_t i = 0; i < c.size(); ++i) {
            CHECK(c[i].zone.adj_hz == Catch::Approx(want[i]).margin(0.5f));
        }
    }
}

TEST_CASE("zone candidates: a matched pair is a candidate", "[belt][tune]") {
    const auto r = response_from(one_peak(100.0f));
    const auto c = tune_zone_candidates(r, r, LO, HI);
    REQUIRE(c.size() == 1);
    CHECK(c[0].zone.source == BeltZoneSource::PAIRED);
    CHECK(c[0].zone.adj_hz == c[0].zone.ref_hz);
}

TEST_CASE("zone candidates: an unpaired tallest peak steers against the nearest", "[belt][tune]") {
    const auto c = tune_zone_candidates(response_from(one_peak(110.0f)),
                                        response_from(one_peak(98.0f)), LO, HI);
    REQUIRE(c.size() == 1);
    CHECK(c[0].zone.source == BeltZoneSource::TALLEST_UNPAIRED);
    CHECK(c[0].zone.adj_hz == Catch::Approx(110.0f).margin(0.5f));
    CHECK(c[0].zone.ref_hz == Catch::Approx(98.0f).margin(0.5f));
    CHECK(c[0].zone.window.freq_start_hz == Catch::Approx(93.0f).margin(0.5f));
    CHECK(c[0].zone.window.freq_end_hz == Catch::Approx(115.0f).margin(0.5f));
}

TEST_CASE("zone sweep window: whole Hz, inside the printer's range", "[belt][tune]") {
    struct Row {
        BeltSweepWindow zone;
        float band_min;
        float band_max;
        float start;
        float end;
    };
    const Row rows[] = {
        {{118.6f, 135.0f}, 5.0f, 135.0f, 109.0f, 135.0f},
        {{20.0f, 40.0f}, 5.0f, 135.0f, 10.0f, 60.0f},
        {{8.0f, 30.0f}, 5.0f, 135.0f, 5.0f, 50.0f},
        {{48.4f, 68.4f}, 5.0f, 133.33f, 39.0f, 88.0f},
        {{100.0f, 120.0f}, 5.0f, 133.33f, 90.0f, 133.0f},
        {{8.0f, 30.0f}, 5.5f, 135.0f, 6.0f, 50.0f},
    };
    for (const auto& row : rows) {
        INFO(row.zone.freq_start_hz << "-" << row.zone.freq_end_hz << " in " << row.band_min << "-"
                                    << row.band_max);
        const auto w = zone_sweep_window(row.zone, row.band_min, row.band_max);
        CHECK(w.freq_start_hz == row.start);
        CHECK(w.freq_end_hz == row.end);
        CHECK(w.freq_start_hz == std::floor(w.freq_start_hz));
        CHECK(w.freq_end_hz == std::floor(w.freq_end_hz));
        CHECK(w.freq_start_hz >= row.band_min);
        CHECK(w.freq_end_hz <= row.band_max);
    }
}

TEST_CASE("zone analysis: the sweep margins never reach a centroid, power or floor",
          "[belt][tune]") {
    const BeltSweepWindow zone{90.0f, 110.0f};
    const auto clean = lorentzian_curve({{100.0f, 1e4f}}, LO, HI, BIN_HZ, 0.01f, 1.0f);
    const float spike = 10.0f * 1e4f;
    for (const float at : {85.0f, 125.0f}) {
        INFO("spike at " << at);
        const auto spiked = with_spike(clean, at, spike);
        REQUIRE(zone_centroid_hz(clean, zone).has_value());
        CHECK(zone_centroid_hz(spiked, zone) == zone_centroid_hz(clean, zone));
        CHECK(zone_power(spiked, zone) == zone_power(clean, zone));
    }

    SECTION("the unexcited floor reads bins outside the swept range") {
        // Margin bins at the floor, everything outside the sweep well above
        // the zone's peak: only the bins outside `swept` say "no peak".
        const BeltSweepWindow swept{80.0f, 130.0f};
        BeltCurve c = lorentzian_curve({{100.0f, 10.0f}}, 70.0f, 140.0f, BIN_HZ, 0.01f, 1.0f);
        for (auto& [f, v] : c) {
            if (f < swept.freq_start_hz || f > swept.freq_end_hz) {
                v = 10.0f;
            }
        }
        CHECK_FALSE(window_holds_peak(c, zone, swept, 70.0f, 140.0f));
        CHECK(window_holds_peak(c, zone, zone, 70.0f, 140.0f));
    }
}
