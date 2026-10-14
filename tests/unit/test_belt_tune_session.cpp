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

/// Both paths swept over the session's current zone window.
const BeltTuneRound& sweep(BeltTuneSession& s, float a_hz, float b_hz) {
    REQUIRE(s.zone().has_value());
    const auto w = s.zone()->window;
    return s.add_round(one_peak(a_hz, w.freq_start_hz, w.freq_end_hz + 0.5f),
                       one_peak(b_hz, w.freq_start_hz, w.freq_end_hz + 0.5f));
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

        BeltTuneSession s(a, b, cmp, 5.0f, 135.0f, BeltTensionerKind::SCREW);
        CHECK(s.show_mechanical_hint());
        CHECK(s.adjusted_path() == BeltPath::PATH_B);
    }

    SECTION("K1C (98.9%): no zone, or one already within tolerance") {
        const auto a = load_fixture("k1c_axis_1_-1.csv");
        const auto b = load_fixture("k1c_axis_1_1.csv");
        const auto cmp = compare_belt_paths(a, b, 5.0f, 133.333f);
        REQUIRE(cmp.valid);
        const auto z = pick_tune_zone(cmp, BeltPath::PATH_A, 5.0f, 133.333f, {});
        if (z.has_value()) {
            INFO("zone A " << z->adj_hz << " B " << z->ref_hz);
            CHECK(std::abs(z->adj_hz - z->ref_hz) <= matched_tolerance_hz(a));
        }
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
        sweep(s, 104.0f, 101.0f);
        s.swap_adjusted_path();
        REQUIRE(s.advice().direction == BeltTurnDirection::LOOSEN);
        s.record_turns(0.125f);
        sweep(s, 103.0f, 101.0f);
        CHECK(s.total_turns(BeltPath::PATH_B) == Catch::Approx(0.25f));
        CHECK(s.total_turns(BeltPath::PATH_A) == Catch::Approx(-0.125f));
    }

    SECTION("swap clears the learned rate; the amount restarts at a quarter turn") {
        sweep(s, 104.0f, 101.0f); // B moved 1 Hz for the default quarter turn
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
        sweep(s, 112.0f, 109.0f); // the suggested turn moved 5 Hz: median 6.5 Hz/turn
        // 3 Hz / 6.5 = 0.46 turn: the nearest eighth is a half, not the 0.375 below it.
        REQUIRE(s.advice().turns.has_value());
        CHECK(*s.advice().turns == Catch::Approx(0.5f));
    }

    SECTION("screw: a turn that moved nothing, before any rate, doubles") {
        auto s = session_for(112.0f, 100.0f, BeltTensionerKind::SCREW);
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

    SECTION("WRONG_WAY: the adjusted peak moved against the advice") {
        const auto& r = sweep(s, 112.0f, 98.0f);
        CHECK(r.outcome == BeltTuneOutcome::WRONG_WAY);
        CHECK(s.advice().direction == BeltTurnDirection::TIGHTEN);
    }

    SECTION("NO_MOVEMENT: under half a hertz either way") {
        CHECK(sweep(s, 112.0f, 100.3f).outcome == BeltTuneOutcome::NO_MOVEMENT);
        CHECK(sweep(s, 112.0f, 99.9f).outcome == BeltTuneOutcome::NO_MOVEMENT);
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
        CHECK(s.gap_hz() == Catch::Approx(-12.0f).margin(0.1f));
        CHECK_FALSE(s.previous_gap_hz().has_value());
        CHECK(s.rounds().size() == 1);
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
    // Default quarter turn: +4 Hz, 16 Hz/turn.
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
                sweep(s, 104.0f, 100.0f);
            }
            CHECK_FALSE(s.show_mechanical_hint());
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
