# Live Progressive Sweep Graphs Implementation Plan (prestonbrown/helixscreen#1812)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** While `TEST_RESONANCES` (Belt Tension) or `SHAPER_CALIBRATE` (Input Shaper panel and wizard) sweeps, draw the response curve one point per Hz as the sweep advances, on every screen that can reach klippy's socket; the CSV curve replaces it the moment Klipper writes it, and every verdict, peak and recommendation still comes from the CSV.

**Architecture:** A pure estimator (`sweep_response_estimator.h`) turns raw accelerometer samples into per-Hz points: it locks the sweep's start time t0 from the samples themselves (the console line leads the motion by the lookahead buffer) and Goertzels x, y and z at each step's frequency. `LiveSweepPreview` (`live_sweep_preview.h`) is the only threaded piece: it attaches an `IAccelBatchSource` (klippy's dump endpoint through the unchanged `BeltStreamClient`, or the mock's synthetic stream under `--test`) on the sweep's first console line, runs the accumulator on the source's loop thread, and defers finished points to the main thread. The frequency chart learns that NaN is a gap, so a partial curve is a full index-placed grid with NaN where nothing is measured yet. Belt Tension draws each path's preview in a series styled exactly like the path's final curve; the Input Shaper panel and wizard share one `ShaperSweepChart` helper styled like the results' raw PSD.

**Tech Stack:** C++17, LVGL 9.5 + helix-xml, libhv `hv::EventLoopThread` (inside the sources only), Catch2 (`tests/unit`), spdlog.

**Spec:** `docs/devel/plans/2026-10-18-live-sweep-graphs-design.md`, with its "Rulings (Preston, 2026-10-15 17:50)" section overriding the open-question recommendations above it: panel AND wizard; preview drawn exactly like the final curve (fill and all), no caption; every capable board, no tier gate; lock from the data or no preview, never console timing.

---

## Rulings this plan makes (beyond the spec)

1. **No DSP-probe gate either.** Ruling 3 defines "capable" as Klipper host + UDS reachable. The gate is `live_sweep_target()`: socket reachable, one chip, valid plan. Neither `PlatformTier` nor `cached_dsp_probe()` enters it.
2. **Belt Tension on EMBEDDED stays chartless, so no preview there.** `BeltTensionPanel::ensure_chart()` already refuses a chart on EMBEDDED; the preview adds no chart where a surface has none. The Input Shaper charts exist on every tier, so the shaper preview reaches EMBEDDED.
3. **The lock rejects noise with an off-drive reference, not the grid median.** Prototyped in numpy before writing this plan: a 0.5 s window's Hann main lobe (about 4 Hz) is wider than the 3.5 Hz search grid, so max/median over the grid never clears 4 for a real drive, while white noise in a hint-relative grid produced false locks in 5 of 5 seeds. The argmax must stand `LOCK_MIN_CONTRAST` (4) times above the mean power at plus/minus 8 and 12 Hz from it; noise-only then never locked (5 of 5 seeds) and every synthetic case below locked within 0.1 s (0.15 s with the sweeping overlay).
4. **The lock takes the median of the densest cluster, not of all estimates.** With a ring 3x the drive at 52 Hz on a 50-80 Hz sweep, the median of all estimates drifted 0.11 s off truth; the densest cluster stayed within 0.07 s.
5. **The argmax is refined by a parabola through its neighbours.** Without it the 0.25 Hz grid biased t0 by +0.05 s; with it the error is under 0.01 s.
6. **The search window scales with the sweep rate.** `[hint - 3.0 s * hz_per_sec, hint + 0.5 s * hz_per_sec]`, which is the spec's `[hint - 3, hint + 0.5]` Hz at the default 1 Hz/s and stays a lookahead-sized window at 2 Hz/s.
7. **The accumulator takes samples, not an `AccelBatch`.** `add_samples(samples, contiguous, rate_hz)` keeps `sweep_response_estimator.h` free of libhv. And `live_sweep_preview.h` names no libhv type at all (AccelBatch forward-declared, the BeltStreamClient-backed source private to the .cpp), because the three panel headers that hold a preview are parsed by the ESP32 build (`subject_initializer.cpp`, `ui_printer_manager_overlay.cpp`, `ui_wizard.cpp`, `wizard_step_registry.cpp`).
8. **A diagonal sweep with per-axis chips that are equal uses that chip.** Klipper's resonance_tester treats `accel_chip_x == accel_chip_y` as one `xy` chip; only different per-axis chips make the diagonal unavailable.
9. **No Beacon special case.** A chip whose module registers no `dump_<type>` endpoint fails the subscribe; the source error ends the preview with one info log. Generic code names no vendor.
10. **Belt previews stand in for a path's series until that path's CSV curve is drawn.** Full check: at the path's `on_sweep_complete`. Tuning round: at `finish_tune_round` (the chart does not draw a round's curves before then today), so a finished path's preview stays frozen while the other path sweeps.
11. **The Input Shaper wizard swaps each axis's preview for that axis's CSV curve** (`InputShaperResult::freq_response`), so the wizard's chart ends on the Y CSV curve. The panel's MEASURING chart is hidden by the state change at RESULTS, as the spec says.
12. **ANALYZING calls `finish()`, not `stop()`.** The last steps reach the main thread a batch or two after "Calculating the best ..." prints; `finish()` keeps the stream until the last point or `PREVIEW_FINISH_GRACE_S` (2 s of stream), then stops, so klippy's chip is not held through the CB1's 11 s FFT.
13. **The mock's shaper sweep gets a realtime mode instead of a new default.** `MoonrakerClientMock::set_shaper_sweep_realtime(true)` plays whole-Hz lines at the belt sweep's cadence; `MoonrakerManager` turns it on for the app, unit tests keep the compact 20-line transcript their timings depend on.
14. **`HELIX_SWEEP_PREVIEW_OFF=1` disables the preview for one run**, logged once at info. It exists for the CB1 on/off CPU comparison (a temporary override, so an env var, not a setting).
15. **The loop thread logs its own CPU use when the stream stops** (`CLOCK_THREAD_CPUTIME_ID` delta and stream seconds), so the hardware measurement reads the preview's cost from the app log instead of guessing it from `top`.
16. **Belt's klippy socket probe moves onto the shared `query_klippy_uds_address()`/`parse_klippy_uds_address()`,** so the Input Shaper surfaces and Belt read the address one way.

## Global Constraints

- **Read-only research is done; this plan is the contract.** Signatures below are exact and were syntax-checked (see "Header verification"). An implementer who needs a different signature stops and reports instead of improvising.
- **Order.** Task 10 (Belt panel) edits `src/ui/ui_panel_belt_tension.cpp`, `include/ui_panel_belt_tension.h` and the belt panel tests, which the settle-laps batch (#1736, `feature/belt-tune-loop`) is changing now. Do not start Task 10 until that batch has landed its commits; rebase the header diff onto whatever is then in the tree (the declarations, not the line numbers, are the contract).
- **The preview starts at the sweep, never during settling.** Belt arms the preview in `start_next_measurement()` (after the settling laps' ack) and attaches on the first `on_sweep_progress()` line.
- **Threading (`.claude/rules/threading.md`).** Nothing on a source's loop thread touches LVGL, subjects or the owning panel. Finished points cross with `LifetimeToken::defer(tag, fn)` from a token taken on the main thread; never `if (tok.expired()) return;` then touch `this`. `stop()` invalidates the guard BEFORE it joins the source. No `std::thread` in shipped code: the mock source owns an `hv::EventLoopThread`, like `BeltStreamClient`. Test fakes may use threads.
- **The preview never keeps the chip running.** The source is stopped on the last point, on `finish()` + grace, on `stop()`, on a source error, on cancel, error, CSV arrival, deactivate, `cleanup()` and destruction.
- **Never a fake value.** A step with a gap, a step outside the buffer, a step finishing unlocked: NaN, `clean = false`. A preview is never promoted to a result; no peak markers, similarity or notes from it.
- **Declarative UI (`.claude/rules/declarative-ui.md`).** Chart hosts are XML; their visibility binds to a subject (`is_measuring_has_chart`, `wizard_input_shaper_has_chart`). Chart data and series styling stay in C++ (structural exception: chart data). Colors come from tokens or the one shared `shaper_raw_psd_color()`.
- **Vendor abstraction.** No firmware or vendor name in any new generic code (no Beacon branch, no Kalico/Creality branch).
- **ESP32.** Every new `src/**/*.cpp` gets a line in `firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt` with the default `# not in the v1 Core+AMS cut` note, in the task that creates it; `python3 scripts/check_esp32_app_srcs.py` passes after each such task. `resonance_console.cpp` IS compiled for ESP32: its new parsing uses `is_string()`/`get_ref`, never `value()`/`at()`/exceptions.
- **No comment archaeology, no em-dashes, `// SPDX` headers, spdlog only, `#include "hv/json.hpp"`.** Issue cites as `(prestonbrown/helixscreen#1812)`.
- **Build discipline.** Plain `make`, never `-jN`. `make t F='[tag]'` is the inner loop. ASAN, `make mutate-diff` and `make full-test-run` run ONCE, in Task 15, not per task. Every `helix-screen` run is headless: `HELIX_HEADLESS=1 SDL_VIDEODRIVER=dummy`, pinned socket and config dir.
- **Commits.** `git commit -- <paths>` (no `git add` in a shared tree; a new file needs `git add -N <path>` first). Subject + about four lines. `git show --stat` after each commit.

## Review Focus

1. **The estimator's honesty:** no point is ever emitted twice or out of order; NaN for every unclean case; the lock cannot be won by noise (Task 3's noise case) or captured by one resonance (hijack case).
2. **Thread crossings in `LiveSweepPreview`:** accumulator touched only on the loop thread; token invalidated before join; `stop()` from inside a delivered callback; auto-stop on completion deferring to main (the loop thread cannot join itself).
3. **Teardown coverage on all three surfaces:** cancel, error, stall, CSV arrival, deactivate (including `Suspended`, which must keep the run), `Rebuild`, `cleanup()`, destructor. A chart destroyed after its host tree is a use-after-free.
4. **Index placement:** every preview series is laid out on the chart's CURRENT axis (`ui_frequency_response_chart_get_freq_range`) and re-laid whenever a push changes it.
5. **Duplication:** one gate (`live_sweep_target`), one layout (`layout_preview`), one socket parse, one synth for mock and tests, one shaper chart helper for panel and wizard, one mock line-interval rule.

## File Map

| File | Task | Kind |
|---|---|---|
| `include/sweep_response_estimator.h`, `src/calibration/sweep_response_estimator.cpp` | 1, 3, 4 | new, pure |
| `include/sweep_synth.h`, `src/calibration/sweep_synth.cpp` | 2 | new, pure |
| `include/live_sweep_preview.h`, `src/calibration/live_sweep_preview.cpp` | 5, 7, 13 | new |
| `include/resonance_console.h`, `src/calibration/resonance_console.cpp` | 5 | changed |
| `include/ui_frequency_response_chart.h`, `src/ui/ui_frequency_response_chart.cpp` | 6 | changed |
| `include/advanced_panel_types.h`, `include/input_shaper_calibrator.h`, `src/api/moonraker_advanced_api.cpp`, `src/calibration/input_shaper_calibrator.cpp` + every compile-flagged caller | 8 | changed |
| `include/ui_shaper_sweep_chart.h`, `src/ui/ui_shaper_sweep_chart.cpp` | 9 | new |
| `include/ui_panel_belt_tension.h`, `src/ui/ui_panel_belt_tension.cpp` | 10 | changed |
| `include/ui_panel_input_shaper.h`, `src/ui/ui_panel_input_shaper.cpp`, `ui_xml/input_shaper_panel.xml` | 9, 11 | changed |
| `include/ui_wizard_input_shaper.h`, `src/ui/ui_wizard_input_shaper.cpp`, `ui_xml/wizard_input_shaper.xml` | 12 | changed |
| `include/accel_batch_source_mock.h`, `src/api/accel_batch_source_mock.cpp`, `include/moonraker_client_mock.h`, `src/api/moonraker_client_mock.cpp`, `src/api/moonraker_client_mock_objects.cpp`, `src/application/moonraker_manager.cpp` | 5, 13 | new + changed |
| `tests/test_helpers/sweep_preview_fakes.h` | 7 | new |
| `tests/unit/test_sweep_response_estimator.cpp`, `test_sweep_synth.cpp`, `test_live_sweep_target.cpp`, `test_live_sweep_preview.cpp`, `test_live_sweep_header_weight.cpp`, `test_shaper_sweep_chart.cpp`, `test_mock_accel_batch_source.cpp` | 1-13 | new |
| `docs/devel/BELT_TENSION.md`, `INPUT_SHAPER.md`, `BELT_TUNER.md`, `MOCK_ENVIRONMENT_VARIABLES.md`, `ENVIRONMENT_VARIABLES.md` | 14 | changed |

---

## Header verification

Every header below was written out in full (new) or as a diff against the tree at `94f841ceb` (changed), copied with the rest of `include/` into a scratch directory outside the tree, and checked with the flags `compile_commands.json` records for `tests/unit/test_belt_tension_panel_states.cpp` (clang++ `-std=c++17 -fsyntax-only`, `-Iinclude` pointed at the scratch copy):

- A scratch `.cpp` calling every new function, constructor, member and constant (including `MoonrakerClientMock`'s new members, the wizard's new methods, both new callback shapes and `fr_chart_finite_runs`): **exit 0**.
- The same file with `note_console_frequency` misspelled: **exit 1** (`no member named 'note_console_frequncy'`). A scratch header with `cnst` for `const`: **exit 1**. `-H` showed the scratch headers, not the tree's, being parsed.
- A second scratch `.cpp` including only the three panel headers, compiled `-fno-exceptions -fno-rtti` with `#ifdef HV_EVENT_LOOP_THREAD_HPP_ #error`: **exit 0**; with `#include "belt_stream_client.h"` added to `live_sweep_preview.h`: **exit 1** on the `#error`. Task 7 makes that guard a permanent test.

The scratch copies were discarded. Implementers re-run `scripts/syntax_check.py` on each file they touch.

### New: `include/sweep_response_estimator.h` (Task 1 creates it whole; Tasks 3 and 4 implement the rest)

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "belt_tension_types.h"

#include <cstddef>
#include <deque>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

/**
 * @file sweep_response_estimator.h
 * @brief Response of a resonance sweep, one point per Hz, from the raw stream
 *
 * Pure: no LVGL, no threads, no sockets. Callers hand it whatever samples the
 * accelerometer stream delivers and get back the sweep points that finished.
 *
 * The excitation is a linear ramp in time, f(t) = freq_start_hz +
 * hz_per_sec * (t - t0). The console's "Testing frequency" line is printed when
 * a move is planned, a lookahead buffer ahead of the motion, so t0 is found in
 * the samples themselves (DriveLock); the console value only narrows the search.
 */

namespace helix::calibration {

namespace sweep_preview {
/// One point per this many Hz of the ramp.
inline constexpr float STEP_HZ = 1.0f;
/// Stream seconds between drive-frequency readings.
inline constexpr double LOCK_INTERVAL_S = 0.25;
/// Samples each reading looks at, ending at the reading's time.
inline constexpr double LOCK_WINDOW_S = 0.5;
/// The search grid spans [hint - LEAD * hz_per_sec, hint + LAG * hz_per_sec].
inline constexpr double LOCK_SEARCH_LEAD_S = 3.0;
inline constexpr double LOCK_SEARCH_LAG_S = 0.5;
inline constexpr float LOCK_GRID_STEP_HZ = 0.25f;
/// Off-drive reference frequencies, Hz from the grid's argmax. Outside a 0.5 s
/// window's main lobe, so a real drive stands far above them and noise does not.
inline constexpr float kLockReferenceOffsetsHz[] = {-12.0f, -8.0f, 8.0f, 12.0f};
/// The argmax's xyz power over the mean reference power, at least.
inline constexpr float LOCK_MIN_CONTRAST = 4.0f;
/// A lock needs this many retained estimates of t0 within LOCK_AGREE_S of one another.
inline constexpr size_t LOCK_AGREE_COUNT = 5;
inline constexpr double LOCK_AGREE_S = 0.15;
/// Most recent t0 estimates retained while unlocked.
inline constexpr size_t LOCK_HISTORY = 16;
/// A locked model is dropped after readings disagree with it by more than
/// LOCK_LOST_HZ for LOCK_LOST_S of stream time without a single agreeing reading.
inline constexpr float LOCK_LOST_HZ = 1.5f;
inline constexpr double LOCK_LOST_S = 3.0;
/// Stream seconds of samples kept for steps that finish before the lock does.
inline constexpr double BUFFER_S = 8.0;
} // namespace sweep_preview

/// The ramp one sweep drives.
struct SweepPlan {
    float freq_start_hz = 0.0f;
    float freq_end_hz = 0.0f;
    float hz_per_sec = 0.0f;

    /// All finite, freq_start_hz > 0, freq_end_hz > freq_start_hz, hz_per_sec > 0.
    [[nodiscard]] bool valid() const;
    /// floor((end - start) / STEP_HZ) + 1, or 0 when !valid().
    [[nodiscard]] size_t point_count() const;
    /// freq_start_hz + index * STEP_HZ.
    [[nodiscard]] float point_hz(size_t index) const;
};

/// One finished step of the sweep.
struct SweepPoint {
    float freq_hz = 0.0f;
    /// Per-axis power at freq_hz, (mm/s^2)^2/Hz like Klipper's CSV columns.
    /// NaN when !clean.
    float psd_x = 0.0f;
    float psd_y = 0.0f;
    float psd_z = 0.0f;
    /// False when the step's samples had a gap, fell out of the buffer, or
    /// finished while the drive was not locked.
    bool clean = false;

    /// psd_x + psd_y + psd_z, as Klipper's psd_xyz column.
    [[nodiscard]] float psd_xyz() const;
};

/// Which CSV column a consumer reads; the preview must read the same one.
enum class SweepColumn {
    XYZ, ///< belt paths
    X,   ///< input shaper, X axis
    Y,   ///< input shaper, Y axis
};

/// The point's value in `column`; NaN for an unclean point.
[[nodiscard]] float sweep_column_value(const SweepPoint& point, SweepColumn column);

/**
 * @brief One-sided power spectral density of `x` at one frequency
 *
 * Mean removed, symmetric Hann window w[i] = 0.5 - 0.5 cos(2 pi i / (n - 1)),
 * single-bin DFT accumulated in double, scaled 2 |X|^2 / (rate_hz * sum(w^2))
 * so a sine of amplitude A reads A^2 (sum w)^2 / (2 rate_hz sum(w^2)).
 *
 * @return 0 for n < 2, rate_hz <= 0 or freq_hz <= 0
 */
[[nodiscard]] float goertzel_psd(const float* x, size_t n, float freq_hz, float rate_hz);

/**
 * @brief The drive frequency in one window of samples, if one stands out
 *
 * Grid from hint_hz - LOCK_SEARCH_LEAD_S * hz_per_sec to hint_hz +
 * LOCK_SEARCH_LAG_S * hz_per_sec at LOCK_GRID_STEP_HZ, positive frequencies
 * only; power is goertzel_psd summed over x, y and z. The argmax is refined by
 * a parabola through it and its two neighbours.
 *
 * @return nullopt when the grid has fewer than 3 points, the argmax sits on
 *         either end of the grid, or its power is under LOCK_MIN_CONTRAST times
 *         the mean power at kLockReferenceOffsetsHz from it (positive ones only)
 */
[[nodiscard]] std::optional<float> find_drive_hz(const AccelSample* samples, size_t n,
                                                 float rate_hz, float hint_hz, float hz_per_sec);

/**
 * @brief The sweep's start time, from drive-frequency readings
 *
 * Each reading (at_s, drive_hz) estimates t0 = at_s - (drive_hz -
 * freq_start_hz) / hz_per_sec. Locks once LOCK_AGREE_COUNT of the retained
 * estimates sit within LOCK_AGREE_S of one of them; t0 is the median of that
 * densest cluster and stays fixed while locked. A resonance that captures the
 * argmax for a second or two lands outside the cluster.
 */
class DriveLock {
  public:
    explicit DriveLock(SweepPlan plan);

    /// Feed one reading taken over a window centred on stream time `at_s`.
    void observe(double at_s, float drive_hz);

    [[nodiscard]] bool locked() const;
    /// t0 in stream seconds; NaN before the first lock. Keeps the last lock's
    /// value after the lock is lost.
    [[nodiscard]] double drive_start_s() const;
    /// freq_start_hz + hz_per_sec * (at_s - t0); NaN while unlocked.
    [[nodiscard]] float model_hz(double at_s) const;

  private:
    SweepPlan plan_;
    std::deque<double> estimates_;
    bool locked_ = false;
    double t0_s_ = std::numeric_limits<double>::quiet_NaN();
    /// Stream time the current run of disagreeing readings began; NaN when the
    /// last reading agreed.
    double disagree_since_s_ = std::numeric_limits<double>::quiet_NaN();
};

/**
 * @brief Samples in, finished sweep points out
 *
 * Takes a drive-frequency reading every LOCK_INTERVAL_S of stream time once a
 * console hint exists. Once DriveLock holds, step k covers drive frequencies
 * [point_hz(k) - STEP_HZ / 2, point_hz(k) + STEP_HZ / 2] clipped to the plan,
 * and is emitted when the newest sample passes its end time: goertzel_psd of
 * x, y and z at point_hz(k) over the samples in that span. Every step is
 * emitted exactly once and in order. A step finishing while the lock is lost,
 * or whose span overlaps a non-contiguous batch or starts before the buffer,
 * is emitted unclean.
 */
class SweepResponseAccumulator {
  public:
    explicit SweepResponseAccumulator(SweepPlan plan);

    /// The latest "Testing frequency" value; only narrows the drive search.
    void note_console_frequency(float hz);

    /// Append samples (stream time ascending) and return the points that finished.
    /// `contiguous` false marks the batch's span as a gap. rate_hz <= 0 buffers
    /// the samples but takes no reading.
    [[nodiscard]] std::vector<SweepPoint>
    add_samples(const std::vector<AccelSample>& samples, bool contiguous, float rate_hz);

    [[nodiscard]] bool locked() const;
    [[nodiscard]] double drive_start_s() const;
    /// Every one of plan().point_count() points has been emitted.
    [[nodiscard]] bool complete() const;
    /// Time of the newest sample, 0 before any.
    [[nodiscard]] double stream_time_s() const;
    [[nodiscard]] const SweepPlan& plan() const;

  private:
    SweepPlan plan_;
    DriveLock lock_;
    std::deque<AccelSample> buffer_;
    /// [first, last] stream times of non-contiguous batches still in the buffer.
    std::deque<std::pair<double, double>> gaps_;
    float hint_hz_ = std::numeric_limits<float>::quiet_NaN();
    double next_reading_s_ = 0.0;
    size_t next_point_ = 0;
    double stream_time_s_ = 0.0;
};

} // namespace helix::calibration
```

### New: `include/sweep_synth.h` (Task 2)

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "belt_tension_types.h"
#include "sweep_response_estimator.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

/**
 * @file sweep_synth.h
 * @brief A synthetic accelerometer stream of one resonance sweep
 *
 * Feeds the mock printer's live preview and the estimator's tests from one
 * generator, so both see the same ramp the estimator assumes. Pure and
 * deterministic: the hiss comes from a fixed-seed LCG.
 */

namespace helix::calibration {

struct SweepSynthSpec {
    SweepPlan plan;
    /// Stream time the ramp leaves plan.freq_start_hz. Before it, and after the
    /// ramp passes plan.freq_end_hz, the stream carries hiss and gravity only.
    double drive_start_s = 1.5;
    float rate_hz = 3053.0f;
    /// Uniform hiss in [-noise, noise) on every axis.
    float noise_mm_s2 = 30.0f;
    /// Constant on X, where the real captures carry gravity.
    float gravity_mm_s2 = 9810.0f;
    uint32_t seed = 2468u;
};

/// Drive amplitude in mm/s^2 on x, y and z at a drive frequency.
using SweepAxisGain = std::function<std::array<float, 3>(float drive_hz)>;

class SweepSynth {
  public:
    SweepSynth(SweepSynthSpec spec, SweepAxisGain gain);

    /// The next `count` samples, continuing where the previous call stopped.
    /// The drive phase is the integral of the ramp, so it is continuous across calls.
    [[nodiscard]] std::vector<AccelSample> next(size_t count);

    /// Stream time of the next sample.
    [[nodiscard]] double time_s() const;
    /// The ramp's frequency at `t_s`; NaN outside it.
    [[nodiscard]] float drive_hz_at(double t_s) const;

  private:
    SweepSynthSpec spec_;
    SweepAxisGain gain_;
    uint64_t index_ = 0;
    double phase_ = 0.0;
    uint32_t rng_;
};

} // namespace helix::calibration
```

### New: `include/live_sweep_preview.h` (Task 5 creates it whole; Task 7 implements the class and the queries)

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "async_lifetime_guard.h"
#include "belt_tension_types.h"
#include "resonance_console.h"
#include "sweep_response_estimator.h"

#include "hv/json.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

/**
 * @file live_sweep_preview.h
 * @brief The response curve of a running sweep, drawn as it is measured
 *
 * Cosmetic only. Klipper's CSV replaces the preview when it lands, and every
 * verdict, peak and recommendation still comes from the CSV.
 *
 * @par Threading
 * LiveSweepPreview is a main-thread object. Its source delivers batches on the
 * source's own loop thread, where the accumulator runs and nothing else;
 * finished points reach the main thread through a LifetimeToken::defer, and
 * stop() expires the token before it joins the loop, so a point already queued
 * is dropped rather than delivered to a torn-down owner.
 *
 * @par The preview never keeps the chip running
 * A dump client is a consumer of the chip like the resonance tester's own, so
 * the stream is stopped when the last point is emitted, on finish(), stop(),
 * a source error, and every owner teardown.
 *
 * @par Header weight
 * The panel headers that hold a LiveSweepPreview are parsed by the ESP32 build,
 * so this header names no libhv type: AccelBatch is forward-declared and the
 * BeltStreamClient-backed source lives in live_sweep_preview.cpp.
 */

namespace helix {
class IMoonrakerAPI;
}

namespace helix::calibration {

struct AccelBatch;

/// Nice value the stream's loop thread takes on its first batch: klippy moves
/// the toolhead during the sweep, and a starved klippy fails the move.
inline constexpr int PREVIEW_LOOP_NICE = 10;

/// Stream seconds finish() waits for the last point before it stops the stream.
inline constexpr double PREVIEW_FINISH_GRACE_S = 2.0;

/// Stream seconds without a lock after which one debug line says so.
inline constexpr double PREVIEW_NO_LOCK_LOG_S = 10.0;

/// The motion a sweep drives the accelerometer with.
enum class SweepAxis {
    X,        ///< SHAPER_CALIBRATE AXIS=X
    Y,        ///< SHAPER_CALIBRATE AXIS=Y
    DIAGONAL, ///< TEST_RESONANCES AXIS=1,1 or 1,-1 (a belt path)
};

/**
 * @brief The [resonance_tester] chip that records this sweep
 *
 * X: accel_chip_x, else accel_chip. Y: accel_chip_y, else accel_chip.
 * DIAGONAL: accel_chip when neither per-axis chip is set; the per-axis chip
 * when both are set and equal; otherwise nullopt (two chips record one sweep,
 * so no single stream carries it). Values are trimmed; an empty result is nullopt.
 */
[[nodiscard]] std::optional<std::string> accel_chip_for_sweep(const ResonanceTesterConfig& cfg,
                                                              SweepAxis axis);

/// The ramp Klipper sweeps: the window floored and ceiled as
/// test_belt_resonance sends it, else the config's range; the config's rate.
/// nullopt when the result is not SweepPlan::valid().
[[nodiscard]] std::optional<SweepPlan> sweep_plan_for(const ResonanceTesterConfig& cfg,
                                                      std::optional<BeltSweepWindow> window);

/// What one query of the printer says about streaming its sweeps.
struct LiveSweepFacts {
    std::string socket_path;      ///< klippy's UDS, "" when Moonraker did not say
    bool socket_reachable = false; ///< BeltStreamClient::socket_reachable(socket_path)
    ResonanceTesterConfig cfg;
};

/// Everything a preview needs to attach to one sweep.
struct LiveSweepTarget {
    std::string socket_path;
    std::string accel_chip;
    SweepPlan plan;
};

/**
 * @brief The single gate for a live preview
 *
 * nullopt unless the socket path is non-empty and reachable, the sweep has one
 * chip (accel_chip_for_sweep) and a valid plan (sweep_plan_for). No platform
 * tier enters it: every board that can reach klippy's socket gets a preview.
 */
[[nodiscard]] std::optional<LiveSweepTarget>
live_sweep_target(const LiveSweepFacts& facts, SweepAxis axis,
                  std::optional<BeltSweepWindow> window = std::nullopt);

/// Points laid out for an index-placed chart axis.
struct PreviewSeries {
    std::vector<float> freqs;
    std::vector<float> amps; ///< NaN where no clean point falls
    float max_amp = 0.0f;    ///< largest finite amp, 0 when none
};

/**
 * @brief Lay the points out over [axis_lo_hz, axis_hi_hz]
 *
 * n = lround((hi - lo) / STEP_HZ) + 1 slots, slot i at lo + i (hi - lo) / (n - 1),
 * so the first and last slots sit on the axis ends the way the chart places
 * them. A slot takes the value (sweep_column_value) of the point nearest it
 * when that point is within STEP_HZ / 2; otherwise NaN. Empty when hi <= lo or
 * either end is not finite.
 */
[[nodiscard]] PreviewSeries layout_preview(const std::vector<SweepPoint>& points,
                                           SweepColumn column, float axis_lo_hz, float axis_hi_hz);

/// klippy_uds_address from a /server/config response, with or without
/// Moonraker's "result" wrapper; "" when absent or not a string.
[[nodiscard]] std::string parse_klippy_uds_address(const nlohmann::json& data);

/// Ask Moonraker for klippy's socket path. on_done runs on the API's callback
/// thread with "" on any error.
void query_klippy_uds_address(IMoonrakerAPI& api, std::function<void(std::string)> on_done);

/// The socket path, its reachability and [resonance_tester], in one call.
/// on_done runs once, on the API's callback thread; a failed leg leaves its
/// defaults ("" / false / ResonanceTesterConfig{}).
void query_live_sweep_facts(IMoonrakerAPI& api, std::function<void(LiveSweepFacts)> on_done);

/// Where batches come from. start/stop are main-thread calls; the callbacks
/// fire on the source's own thread. The shapes match BeltStreamClient's.
class IAccelBatchSource {
  public:
    using BatchCallback = std::function<void(const AccelBatch&)>;
    using ErrorCallback = std::function<void(const std::string&)>;

    virtual ~IAccelBatchSource() = default;
    virtual bool start(const std::string& socket_path, const std::string& accel_chip,
                       BatchCallback on_batch, ErrorCallback on_error) = 0;
    /// Idempotent; joins the source's thread.
    virtual void stop() = 0;
    [[nodiscard]] virtual float sample_rate_hz() const = 0;
};

/// The mock printer's synthetic stream (MoonrakerClientMock::make_accel_batch_source)
/// when RuntimeConfig::should_mock_moonraker(), else klippy's dump endpoint
/// through an unchanged BeltStreamClient.
[[nodiscard]] std::unique_ptr<IAccelBatchSource> make_accel_batch_source(const SweepPlan& plan);

class LiveSweepPreview {
  public:
    enum class State {
        IDLE,      ///< not armed
        ARMED,     ///< waiting for the sweep's first console line
        STREAMING, ///< the source is running
        DONE,      ///< the stream ended (last point, finish(), source error); points stay
    };

    /// Main thread. Every point delivered so far, in sweep order.
    using PointsCallback = std::function<void(const std::vector<SweepPoint>& points)>;
    using SourceFactory = std::function<std::unique_ptr<IAccelBatchSource>(const SweepPlan& plan)>;

    LiveSweepPreview();
    ~LiveSweepPreview();
    LiveSweepPreview(const LiveSweepPreview&) = delete;
    LiveSweepPreview& operator=(const LiveSweepPreview&) = delete;

    /// Replace make_accel_batch_source. Only in IDLE.
    void set_source_factory(SourceFactory factory);

    /// Arm for one sweep; nothing connects yet. Stops and replaces any earlier
    /// sweep and clears points().
    void arm(LiveSweepTarget target, PointsCallback on_points);

    /// The console's "Testing frequency" value. In ARMED it starts the stream,
    /// so the tester's own client is already running the chip; in STREAMING it
    /// updates the drive search hint. Ignored in IDLE and DONE, and when hz is
    /// not finite and positive.
    void note_console_frequency(float hz);

    /// The sweep's motion is over: keep streaming until the last point or
    /// PREVIEW_FINISH_GRACE_S of further stream, then stop. Points stay.
    void finish();

    /// Back to IDLE; idempotent. Queued points are dropped; points() keeps what
    /// was delivered until the next arm().
    void stop();

    [[nodiscard]] State state() const;
    [[nodiscard]] const std::vector<SweepPoint>& points() const;

  private:
    void attach();
    /// Loop thread.
    void on_batch(const AccelBatch& batch);
    /// Loop thread.
    void on_source_error(const std::string& message);
    /// Main thread.
    void deliver(std::vector<SweepPoint> fresh);
    /// Main thread: expire the token, stop and drop the source and the
    /// accumulator, log the loop thread's CPU use. Keeps points_.
    void stop_source();

    State state_ = State::IDLE;
    SourceFactory factory_;
    std::optional<LiveSweepTarget> target_;
    PointsCallback on_points_;
    std::vector<SweepPoint> points_;

    /// Created by attach() before the source starts, touched only on the
    /// loop thread while streaming, destroyed after the source is stopped.
    std::unique_ptr<SweepResponseAccumulator> accumulator_;
    std::unique_ptr<IAccelBatchSource> source_;

    std::atomic<float> hint_hz_{0.0f};
    std::atomic<bool> finishing_{false};
    /// Loop thread only.
    double finish_deadline_s_ = 0.0;
    bool stop_requested_ = false;
    bool no_lock_logged_ = false;
    bool niced_ = false;
    /// Loop thread CPU time at the first batch (loop thread only) and at the
    /// latest, for the stop log line the CB1 measurement reads.
    int64_t loop_cpu_first_ns_ = -1;
    std::atomic<int64_t> loop_cpu_ns_{0};
    std::atomic<double> stream_time_s_{0.0};

    helix::AsyncLifetimeGuard lifetime_;
};

} // namespace helix::calibration
```

### New: `include/ui_shaper_sweep_chart.h` (Task 9)

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "calibration_types.h"
#include "live_sweep_preview.h"
#include "lvgl/lvgl.h"

#include <functional>
#include <optional>
#include <utility>
#include <vector>

struct ui_frequency_response_chart_t;

namespace helix::ui {

/// The input shaper charts' frequency axis, Hz. The results charts and the
/// live chart share it so a curve sits at the same x on both.
inline constexpr float SHAPER_CHART_MAX_HZ = 200.0f;
/// Amplitude axis ceiling over the tallest point.
inline constexpr float SHAPER_CHART_HEADROOM = 1.1f;

/// The results charts' raw PSD series color; the live curve uses it too.
[[nodiscard]] lv_color_t shaper_raw_psd_color();

/**
 * @brief The input shaper's live sweep chart
 *
 * One series drawn the way the results charts draw the raw PSD, fed by a
 * LiveSweepPreview during the sweep and, for callers that keep it, by the CSV
 * curve after it. Shared by InputShaperPanel's MEASURING view and the wizard
 * step. Main thread only.
 */
class ShaperSweepChart {
  public:
    ShaperSweepChart() = default;
    ~ShaperSweepChart();
    ShaperSweepChart(const ShaperSweepChart&) = delete;
    ShaperSweepChart& operator=(const ShaperSweepChart&) = delete;

    void set_source_factory(helix::calibration::LiveSweepPreview::SourceFactory factory);

    /// Fires with true when the first finite point is drawn and false when the
    /// curve is cleared; callers bind their host's visibility to it.
    void set_on_curve_changed(std::function<void(bool has_curve)> on_changed);

    /**
     * @brief Start a fresh curve for `axis` ('X' or 'Y') in `host`
     *
     * Clears any earlier curve. Creates the chart in `host` on first use (and
     * again when `host` changed). Returns false, arming nothing, when `target`
     * is nullopt or `host` is null.
     */
    bool begin(lv_obj_t* host, char axis,
               const std::optional<helix::calibration::LiveSweepTarget>& target);

    /// A progress report: Sweeping with freq_hz > 0 feeds the console
    /// frequency, Analyzing finishes the stream. Other phases do nothing.
    void on_progress(ShaperCalibrationPhase phase, float freq_hz);

    /// Stop the preview and draw the CSV curve (freq, psd) in its place.
    void show_curve(const std::vector<std::pair<float, float>>& freq_response);

    /// Stop streaming; what is drawn stays.
    void stop();
    /// Stop and free the chart. Call before the host's widget tree goes.
    void destroy();

    [[nodiscard]] bool has_curve() const;
    [[nodiscard]] ui_frequency_response_chart_t* chart() const;
    [[nodiscard]] const helix::calibration::LiveSweepPreview& preview() const;

  private:
    void on_points(const std::vector<helix::calibration::SweepPoint>& points);
    /// Main thread: one set_data, amplitude range 0..max * SHAPER_CHART_HEADROOM.
    void draw(const std::vector<float>& freqs, const std::vector<float>& amps, float max_amp);
    void set_has_curve(bool has_curve);

    helix::calibration::LiveSweepPreview preview_;
    ui_frequency_response_chart_t* chart_ = nullptr;
    lv_obj_t* host_ = nullptr;
    int series_ = -1;
    helix::calibration::SweepColumn column_ = helix::calibration::SweepColumn::X;
    bool has_curve_ = false;
    std::function<void(bool)> on_curve_changed_;
};

} // namespace helix::ui
```

### New: `include/accel_batch_source_mock.h` (Task 13)

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "live_sweep_preview.h"
#include "sweep_synth.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

/**
 * @file accel_batch_source_mock.h
 * @brief The mock printer's accelerometer stream for a running sweep
 *
 * Plays SweepSynth through the same callbacks klippy's dump endpoint feeds, on
 * a private hv::EventLoopThread, so --test exercises the whole preview path
 * from the loop thread to the chart.
 */

namespace hv {
class EventLoopThread;
}

namespace helix::calibration {

/// What the mock's current sweep resonates like. Matches the CSV the mock
/// writes for the same sweep, so the swap to the CSV curve looks like Klipper's.
struct MockSweepResponse {
    float peak_hz = 58.4f;      ///< NaN: no peak, the floor alone
    float half_width_hz = 5.0f; ///< Lorentzian half width at half maximum
    /// Drive amplitude at the peak. A one-second step of amplitude A reads a
    /// PSD near A^2 / 3 on an axis of weight 1, so sqrt(3 * csv_peak_psd) puts
    /// the preview on the CSV's scale.
    float drive_mm_s2 = 300.0f;
    /// Amplitude per axis: the square roots of the CSV's per-axis power shares.
    std::array<float, 3> axis_weight{0.671f, 0.671f, 0.316f};
};

/// Off-peak level, as a fraction of the peak's power.
inline constexpr float MOCK_FLOOR = 0.02f;
/// Hiss amplitude as a fraction of drive_mm_s2.
inline constexpr float MOCK_NOISE_FRACTION = 0.05f;

/// Per-axis drive amplitude in mm/s^2: drive_mm_s2 * sqrt(MOCK_FLOOR +
/// 1 / (1 + ((drive_hz - peak_hz) / half_width_hz)^2)) * axis_weight, the
/// Lorentzian term dropped for a NaN peak.
[[nodiscard]] std::array<float, 3> mock_sweep_axis_gain(const MockSweepResponse& response,
                                                        float drive_hz);

class MockAccelBatchSource final : public IAccelBatchSource {
  public:
    /// Wall milliseconds between batches.
    static constexpr uint32_t BATCH_INTERVAL_MS = 100;
    /// The ramp starts this long after the stream does, as a real one trails
    /// the console line the preview attaches on.
    static constexpr double DRIVE_LEAD_S = 1.5;

    /// @param sim_speed stream seconds per wall second (the mock's --sim-speed)
    MockAccelBatchSource(SweepPlan plan, MockSweepResponse response, double sim_speed);
    ~MockAccelBatchSource() override;
    MockAccelBatchSource(const MockAccelBatchSource&) = delete;
    MockAccelBatchSource& operator=(const MockAccelBatchSource&) = delete;

    bool start(const std::string& socket_path, const std::string& accel_chip,
               BatchCallback on_batch, ErrorCallback on_error) override;
    void stop() override;
    [[nodiscard]] float sample_rate_hz() const override;

  private:
    /// Loop thread: the next batch's worth of SweepSynth samples.
    void emit_batch();

    SweepSynth synth_;
    double sim_speed_;
    BatchCallback on_batch_;
    std::unique_ptr<hv::EventLoopThread> loop_thread_;
    std::atomic<bool> running_{false};
};

} // namespace helix::calibration
```

The changed headers' diffs are in the task that changes them.

---

## Task 1: Sweep plan, points and `goertzel_psd` (pure)

**Files:**
- Create: `include/sweep_response_estimator.h` (the whole header above), `src/calibration/sweep_response_estimator.cpp`
- Create: `tests/unit/test_sweep_response_estimator.cpp`
- Modify: `firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt` (add `src/calibration/sweep_response_estimator.cpp  # not in the v1 Core+AMS cut`, sorted with its neighbours)

**Interfaces:**
- Consumes: `AccelSample` (`include/belt_tension_types.h`)
- Produces: `SweepPlan::{valid, point_count, point_hz}`, `SweepPoint::psd_xyz`, `sweep_column_value`, `goertzel_psd`, the `sweep_preview::` constants

- [ ] **Step 1: Write the failing tests** (tag `[sweep_preview]`)
  - `SweepPlan{5,135,1}`: `valid()`, `point_count() == 131`, `point_hz(0) == 5`, `point_hz(130) == 135`.
  - `SweepPlan{5,133.33f,1}`: `point_count() == 129`, `point_hz(128) == 133`.
  - Invalid, `point_count() == 0`: `{0,135,1}`, `{5,5,1}`, `{5,135,0}`, `{NaN,135,1}`, `{5,INFINITY,1}`.
  - `SweepPoint{50, 1, 2, 3, true}.psd_xyz() == 6`; `sweep_column_value` gives 6 / 1 / 2 for XYZ / X / Y; any column of a `clean = false` point is NaN.
  - `goertzel_psd`: `x[i] = 1000 sin(2 pi 50 i / 3200)`, n = 3200, rate 3200: within 5% of `A^2 (sum w)^2 / (2 rate sum w^2)` computed in the test from the Hann formula (= 333,229.17). The same buffer at 46.7 and 53.3 Hz is more than 20 dB down (prototype: -42.07 dB). A second case at rate 3053, n = 1527, A = 500, 40 Hz is within 5% of its formula value (41,653.0).
  - Mean removal: the 50 Hz buffer plus a 9810 constant reads within 1% of the buffer without it.
  - Returns 0 for: all-zero input, n = 0, n = 1, rate 0, freq 0, freq -5.
- [ ] **Step 2: Run** `make t F='[sweep_preview]'`. Expect FAIL (link errors for the undefined symbols count as the failing state).
- [ ] **Step 3: Implement** in `src/calibration/sweep_response_estimator.cpp`: the three `SweepPlan` members, `psd_xyz`, `sweep_column_value`, `goertzel_psd` (mean removed, symmetric Hann `0.5 - 0.5 cos(2 pi i / (n - 1))` as `compute_psd` uses, single-bin DFT accumulated in `double` with a phasor or the Goertzel recurrence, scaled `2 |X|^2 / (rate * sum w^2)`). No other declaration of the header is implemented yet. Add the ESP32 exclusion line.
- [ ] **Step 4: Run** `make t F='[sweep_preview]'` (PASS) and `python3 scripts/check_esp32_app_srcs.py` (PASS).
- [ ] **Step 5: Commit** `feat(calibration): sweep plan, points and a single-bin PSD for the live preview (prestonbrown/helixscreen#1812)`

## Task 2: `SweepSynth`, one synthetic sweep for the mock and the tests (pure)

**Files:**
- Create: `include/sweep_synth.h`, `src/calibration/sweep_synth.cpp`, `tests/unit/test_sweep_synth.cpp`
- Modify: `app_srcs_excluded.txt` (`src/calibration/sweep_synth.cpp`)

**Interfaces:**
- Consumes: `SweepPlan`, `AccelSample`, `goertzel_psd` (tests only)
- Produces: `SweepSynthSpec`, `SweepAxisGain`, `SweepSynth::{next, time_s, drive_hz_at}`

- [ ] **Step 1: Failing tests** (`[sweep_preview][synth]`)
  - Spec `{plan {5,135,1}, drive_start 1.5, rate 3053, noise 0, gravity 9810}`, gain `{100, 50, 10}` everywhere: `drive_hz_at(1.0)` NaN, `drive_hz_at(1.5) == 5`, `drive_hz_at(61.5) == 65`, `drive_hz_at(131.5) == 135`, `drive_hz_at(131.6)` NaN.
  - `next(3053)` then `next(3053)`: times are `i / 3053` continuing across calls (sample 3053's time is 1.0 within 1e-6); `time_s()` is 2.0 after both.
  - Before the ramp (first 1.5 s), with noise 0: x == 9810 exactly, y == z == 0.
  - Phase continuity: concatenating `next(1000)` + `next(2053)` equals one `next(3053)` sample for sample (same seed).
  - Drive amplitude: 1 s of samples centred on drive 60 Hz, `goertzel_psd` of x at 60 Hz is within 10% of `100^2 * n / (3 * rate)` (the Hann sine formula, approximately); of y, a quarter of x's within 10%.
  - Noise: noise 30, zero gain: every axis minus its constant stays within [-30, 30); two synths with the same seed produce identical samples, different seeds do not.
- [ ] **Step 2: Run** `make t F='[synth]'`; expect FAIL.
- [ ] **Step 3: Implement.** Sample i is at `t = i / rate_hz`. Phase advances by `2 pi f(t) / rate_hz` only while `drive_hz_at(t)` is finite; each axis is `gain(f)[axis] * sin(phase) + hiss`, x adds `gravity_mm_s2`. Hiss is the `belt_test_signals.h` LCG (`state = state * 1664525 + 1013904223`, mapped to [-1, 1)) seeded from `spec.seed`, scaled by `noise_mm_s2`. `AccelSample::time` is `float(t)`.
- [ ] **Step 4: Run** `make t F='[synth]'` (PASS); ESP32 check (PASS).
- [ ] **Step 5: Commit** `feat(calibration): a synthetic resonance sweep stream (prestonbrown/helixscreen#1812)`

## Task 3: Drive lock, `find_drive_hz` and `DriveLock` (pure)

**Files:**
- Modify: `src/calibration/sweep_response_estimator.cpp`, `tests/unit/test_sweep_response_estimator.cpp`

**Interfaces:**
- Consumes: `goertzel_psd`, `sweep_preview::LOCK_*` constants, `SweepPlan`
- Produces: `find_drive_hz`, `DriveLock::{observe, locked, drive_start_s, model_hz}`

- [ ] **Step 1: Failing tests** (`[sweep_preview][lock]`)

  `find_drive_hz` on 0.5 s windows (n = 1527, rate 3053; x carries 9810 plus the tone, y the tone, z nothing unless stated), `hz_per_sec` 1 unless stated:

  | Window | hint | Expected |
  |---|---|---|
  | 40.0 Hz sine, A = 200 | 41.5 | 40.0 within 0.1 |
  | 40.3 Hz sine, A = 200 | 41.5 | 40.3 within 0.1 (the parabola refines past the 0.25 Hz grid) |
  | 40.0 Hz sine | 45.0 | nullopt (argmax on the grid's low end) |
  | hiss only, amplitude 30 on all axes (Hiss from `belt_test_signals.h`) | 41.5 | nullopt (contrast) |
  | 38.0 Hz sine, `hz_per_sec` 2 | 43.0 | 38.0 within 0.1 (grid 37..44) |
  | 40.0 Hz sine | 0.3 | nullopt (fewer than 3 positive grid points) |
  | n = 0 | 41.5 | nullopt |

  `DriveLock`, plan `{5,135,1}` unless stated (an estimate is `at - (hz - 5)`):
  - Five readings (2.00, 5.30), (2.25, 5.55), (2.50, 5.80), (2.75, 6.05), (3.00, 6.30): unlocked after four, locked after five, `drive_start_s()` == 1.70 within 1e-6, `model_hz(10.0)` == 13.3 within 1e-4.
  - Before any lock: `drive_start_s()` and `model_hz(5.0)` are NaN.
  - Outlier: estimates 1.70, 1.70, 3.20 (reading (2.50, 4.30)), 1.70, 1.70 leave it unlocked; a sixth at 1.70 locks it at 1.70.
  - Spread: eight estimates 1.0, 1.2, ... 2.4 never lock.
  - Cluster median: estimates 1.60, 1.65, 1.70, 1.75, 1.80 lock at 1.70.
  - History: one 1.70, then sixteen spread estimates (5.0, 5.2, ... 8.0), then four 1.70: unlocked (the first fell out of `LOCK_HISTORY`); a fifth 1.70 locks.
  - Loss: locked at 1.70, then readings at 10.00, 10.25, ... with `hz = model_hz(at) + 2.0`: still locked at 12.75, unlocked at 13.00; `drive_start_s()` still 1.70, `model_hz` NaN after.
  - Boundary: a reading exactly 1.5 Hz off counts as agreeing (never starts the loss timer).
  - Reset: disagreeing 10.00..12.75, one agreeing reading at 13.00, disagreeing 13.25..16.00: still locked.
  - Relock: after the loss above, five readings estimating 6.70 lock at 6.70.
  - Rate: plan `{5,135,2}`, five readings `(3.0 + 0.25k, 7.6 + 0.5k)` lock at 1.70.
- [ ] **Step 2: Run** `make t F='[lock]'`; expect FAIL.
- [ ] **Step 3: Implement** both per their header comments. `find_drive_hz`: grid points `lo + k * LOCK_GRID_STEP_HZ` up to `hi`, positives only; power = sum of `goertzel_psd` over the three axes (copy each axis into a scratch `std::vector<float>` once per call); argmax interior check; reference = mean power at `argmax + kLockReferenceOffsetsHz` (positive frequencies only; nullopt if none or reference <= 0); parabola `offset = 0.5 (a - c) / (a - 2b + c)` clamped to [-0.5, 0.5] grid steps. `DriveLock::observe`: unlocked, push the estimate (drop the oldest past `LOCK_HISTORY`), find the estimate with the most others within `LOCK_AGREE_S` (inclusive), lock at the median of that cluster when it holds `LOCK_AGREE_COUNT`; locked, compare against `model_hz(at_s)` and run the loss timer; on loss clear the estimates.
- [ ] **Step 4: Run** `make t F='[sweep_preview]'` (PASS).
- [ ] **Step 5: Commit** `feat(calibration): lock a sweep's start time from the accelerometer (prestonbrown/helixscreen#1812)`

## Task 4: `SweepResponseAccumulator` (pure), including the hardware CSV shapes

**Files:**
- Modify: `src/calibration/sweep_response_estimator.cpp`, `tests/unit/test_sweep_response_estimator.cpp`

**Interfaces:**
- Consumes: `find_drive_hz`, `DriveLock`, `goertzel_psd`, `SweepSynth` (tests), `parse_resonance_csv` and `band_similarity` (tests)
- Produces: `SweepResponseAccumulator::{note_console_frequency, add_samples, locked, drive_start_s, complete, stream_time_s, plan}`

The shared test driver (a file-local helper in the test, not a new header): a `SweepSynth` with the case's spec and gain; before each 300-sample batch starting at stream time t, if `t + 1.5 >= drive_start`, `note_console_frequency(floor(min(drive(t + 1.5), plan end)))` (the console leading by 1.5 s, printing whole Hz); then `add_samples(batch, true, 3053)`; run until `drive_start + (end - start) / hz_per_sec + 2` s. Gains are `lorentzian(peak, hwhm)`: `amp * sqrt(0.02 + 1 / (1 + ((f - peak) / hwhm)^2)) * weight[axis]`, amp 800, weights {0.5, 0.5, 0.1}, unless stated.

- [ ] **Step 1: Failing tests** (`[sweep_preview][accumulator]`)
  - Main: plan {5,135,1}, drive_start 1.7, noise 30, peak 58.4, hwhm 3.8 (the K1C 55.4-63.0 Hz half-power band): `locked()`; `|drive_start_s() - 1.7| < 0.1`; exactly 131 points over the run, `freq_hz` 5, 6, ... 135 in order, none repeated (collect every return value); all `clean`; the `psd_xyz` argmax within 1 Hz of 58.4; `complete()` true after the last.
  - Rate 2 Hz/s, same response, lead still 1.5 s: locked within 0.1 s, 131 points, peak within 1 Hz.
  - Hijack: plan {50,80,1}, plus a constant 52.0 Hz ring, amplitude 1000, weights {0.6, 0.6, 0.1}: `|drive_start_s() - 1.7| < 0.1` (prototype: 0.066).
  - Belt-path energy: weights {1.0, 0.5, 0.1}: every clean point's `psd_xyz()` equals `psd_x + psd_y + psd_z` exactly, and at the peak `psd_x > 3 * psd_y`.
  - Sweeping overlay: add `400 sin(2 pi 0.83 t)` to x: `|drive_start_s() - 1.7| < 0.15`; for every point above 10 Hz, `|psd_xyz - main_case_psd_xyz| / main_case_psd_xyz < 0.05` (same seed as the main case; prototype 0.0012).
  - Gap: the batch spanning stream time 1.7 + 25.0 s (drive 30 Hz) is sent `contiguous = false`: the points whose spans overlap that batch's [first, last] sample times are `clean = false` with NaN powers; points 33 Hz and up are clean; still 131 points.
  - Noise only (gain 0), seeds 1-5: never `locked()`, `add_samples` never returns a point.
  - Before any hint: the main case's samples with no `note_console_frequency` call never lock.
  - Late attach: drop the first 4.0 s of samples (the stream started after the drive did): locks; points whose spans begin before the first sample are unclean, later ones clean; still 131 points.
  - Lock loss and recovery: samples from the main case's synth until stream 30.0 s, then from a second synth with drive_start 7.7 (the ramp drops 6 Hz), hints following the second ramp: at some batch after 33 s `locked()` is false; later it is locked again with `|drive_start_s() - 7.7| < 0.1`; at least one point between is unclean; still 131 points, each once, in order.
  - Rate 0: `add_samples(batch, true, 0.0f)` returns nothing and takes no reading; the next batch with a real rate proceeds.
  - Hardware CSV shapes: for each of `tests/fixtures/belt_sweeps/{k1c_axis_1_-1, k1c_axis_1_1, voron24_kalico_axis_1_1, ad5m_axis_1_1}.csv`, read `parse_resonance_csv()`, gain per axis = `20 * sqrt(weight * psd_xyz(f))` with weights {0.45, 0.45, 0.10} and `psd_xyz(f)` linearly interpolated, plan {5,135,1}: locked, and `band_similarity(csv.curve, preview_curve, 5, 135) >= 90` where `preview_curve` is the (freq_hz, psd_xyz) of the clean points. Prototype: 99.9 / 99.9 / 99.9 / 99.96.
- [ ] **Step 2: Run** `make t F='[accumulator]'`; expect FAIL.
- [ ] **Step 3: Implement** per the class comment. Buffer trimmed to `BUFFER_S` behind the newest sample; `gaps_` trimmed with it. Readings: every `LOCK_INTERVAL_S` of stream time from the first sample once a finite hint exists and `rate_hz > 0`, over the samples in `(t - LOCK_WINDOW_S, t]`, observed at `t - LOCK_WINDOW_S / 2`. Step k spans drive `[max(point_hz(k) - 0.5, start), min(point_hz(k) + 0.5, end)]`, i.e. stream `t0 + (f - start) / hz_per_sec` for each end; emitted once the newest sample passes its end time, using the current t0 when locked, else the last t0 (unclean) when one exists; before the first lock nothing is emitted. Powers are `goertzel_psd` at `point_hz(k)` of each axis over the span. Never emit past `point_count()`.
- [ ] **Step 4: Run** `make t F='[sweep_preview]'` (PASS).
- [ ] **Step 5: Commit** `feat(calibration): per-Hz sweep response from the raw stream (prestonbrown/helixscreen#1812)`

## Task 5: Printer facts, the gate and the layout (pure parts of `live_sweep_preview.h`)

**Files:**
- Create: `include/live_sweep_preview.h` (whole header above), `src/calibration/live_sweep_preview.cpp`, `tests/unit/test_live_sweep_target.cpp`
- Modify: `include/resonance_console.h` (diff below), `src/calibration/resonance_console.cpp`, `src/api/moonraker_client_mock_objects.cpp` (or wherever `MoonrakerClientMock::resonance_tester_settings()` is defined: add `"accel_chip": "adxl345"`), `tests/unit/test_resonance_console.cpp`, `app_srcs_excluded.txt` (`src/calibration/live_sweep_preview.cpp`)

```diff
--- a/include/resonance_console.h
+++ b/include/resonance_console.h
@@ -26,4 +26,10 @@
     /// when absent or unparseable; that alone does not reject the range.
     std::optional<std::array<float, 3>> probe_point;
+    /// accel_chip, accel_chip_x and accel_chip_y as configured, trimmed; ""
+    /// when absent or not a string. Names a config section ("adxl345",
+    /// "adxl345 hotend"), not an endpoint.
+    std::string accel_chip;
+    std::string accel_chip_x;
+    std::string accel_chip_y;
 
     /// Seconds one sweep takes: (max - min) / hz_per_sec.
```

**Interfaces:**
- Consumes: `ResonanceTesterConfig`, `BeltSweepWindow`, `SweepPlan`, `SweepPoint`, `sweep_column_value`, `text_io::trim`
- Produces: `accel_chip_for_sweep`, `sweep_plan_for`, `LiveSweepFacts`, `LiveSweepTarget`, `live_sweep_target`, `PreviewSeries`, `layout_preview`, `parse_klippy_uds_address`; the three chip fields; the mock reporting `accel_chip`

- [ ] **Step 1: Failing tests** (`[sweep_preview][target]`, plus `[resonance]` for the parser)
  - `parse_resonance_tester_config`: `{"resonance_tester": {"accel_chip": "adxl345"}}` gives "adxl345", "" and ""; `" adxl345 hotend "` and `"lis2dw bed"` under `accel_chip_x`/`accel_chip_y` come back trimmed; `"accel_chip": 7` gives "" with `from_printer` still true; no section gives all ""; a malformed `min_freq` still returns `ResonanceTesterConfig{}` (chips "").
  - The mock: `MoonrakerClientMock::resonance_tester_settings()["accel_chip"] == "adxl345"`.
  - `accel_chip_for_sweep` (chip, chip_x, chip_y, axis -> result): ("adxl345", "", "", X/Y/DIAGONAL -> "adxl345"); ("", "adxl345 x", "adxl345 y", X -> "adxl345 x", Y -> "adxl345 y", DIAGONAL -> nullopt); ("adxl345", "lis2dw", "", X -> "lis2dw", Y -> "adxl345", DIAGONAL -> nullopt); ("", "adxl345 hotend", "adxl345 hotend", DIAGONAL -> "adxl345 hotend"); ("", "", "", any -> nullopt); ("  adxl345  ", "", "", X -> "adxl345").
  - `sweep_plan_for`: defaults, no window -> {5, 135, 1}; window {58.4, 72.6} -> {58, 73, 1}; config max 133.33 -> {5, 133.33, 1}; `hz_per_sec` 0 -> nullopt; window {80, 70} -> nullopt; window {0.4, 10} -> nullopt (start floors to 0).
  - `live_sweep_target` with facts {"/tmp/k.sock", true, cfg(chip "adxl345", 5-135, 1)}: X -> {"/tmp/k.sock", "adxl345", {5,135,1}}; DIAGONAL + window {58.4, 72.6} -> plan {58,73,1}; nullopt when reachable is false, when the path is "", when the chip is absent, when `hz_per_sec` is 0.
  - `layout_preview`, points k = 0..130 at f = 5 + k with psd_x = f, psd_y = 2f, psd_z = 0, clean:
    - XYZ on [5, 135]: 131 slots, `freqs.front() == 5`, `freqs.back() == 135`, `amps[i] == 3 * (5 + i)`, `max_amp == 405`.
    - X on [0, 200]: 201 slots; amps 0-4 NaN, `amps[5] == 5`, `amps[135] == 135`, 136-200 NaN; `max_amp == 135`.
    - X on [5.2, 134.2]: 130 slots, `amps[i] == 5 + i`. X on [5.6, 134.6]: `amps[0] == 6`.
    - Point 15 Hz unclean: its slot is NaN on [5, 135].
    - Y column: `amps[i] == 2 * (5 + i)`.
    - A tie (slot exactly between two points) takes the lower frequency's point.
    - No points: 131 NaN slots, `max_amp == 0`. `hi <= lo`, or a non-finite end: empty.
  - `parse_klippy_uds_address`: `{"result":{"config":{"server":{"klippy_uds_address":"/tmp/k"}}}}` -> "/tmp/k"; the same without "result" -> "/tmp/k"; missing key, a number, a non-object root -> "".
- [ ] **Step 2: Run** `make t F='[target]'` and `make t F='[resonance]'`; expect FAIL.
- [ ] **Step 3: Implement** the parser fields (`is_string()` + `text_io::trim`; no `value()`/`at()`: the file is ESP32-compiled), the mock key, and the six pure functions in `live_sweep_preview.cpp`. Nothing else in that file yet.
- [ ] **Step 4: Run** both tags (PASS), `[belt]` and `[input_shaper]` (nothing else broke), ESP32 check (PASS).
- [ ] **Step 5: Commit** `feat(calibration): which chip records a sweep, and the live preview's gate (prestonbrown/helixscreen#1812)`

## Task 6: The frequency chart treats NaN as a gap

**Files:**
- Modify: `include/ui_frequency_response_chart.h` (diff below), `src/ui/ui_frequency_response_chart.cpp`, `tests/unit/test_frequency_response_chart.cpp`

```diff
--- a/include/ui_frequency_response_chart.h
+++ b/include/ui_frequency_response_chart.h
@@ -39,4 +39,10 @@
  * When set_data is called with more points than max_points for the current tier,
  * the data is automatically downsampled while preserving frequency range endpoints.
+ * Downsampling picks points, so on BASIC a narrow peak can fall between picks.
+ *
+ * A non-finite amplitude is a gap: the built-in renderer skips it
+ * (LV_CHART_POINT_NONE) and the styled and muted passes stroke and fill only
+ * fr_chart_finite_runs(). Points are still placed by index, so a caller
+ * drawing a partial curve passes the full grid with NaN where it has no value.
  *
  * @see platform_capabilities.h for tier definitions
@@ -316,4 +322,24 @@
 void ui_frequency_response_chart_clear_cursor(ui_frequency_response_chart_t* chart);
 
+/**
+ * @brief A run of consecutive finite amplitudes, [begin, end)
+ */
+// NAMESPACE_OK: joins this header's global ui_frequency_response_chart_* API
+struct FrChartRun {
+    size_t begin = 0;
+    size_t end = 0;
+};
+
+/**
+ * @brief Where a series can be stroked (pure)
+ *
+ * Every maximal run of finite values at least two points long, in order. A
+ * non-finite amplitude is a gap: no segment or fill touches it, and a lone
+ * finite point between two gaps draws nothing.
+ * {1, 2, NaN, 3, 4, 5, NaN} -> {[0,2), [3,6)}.
+ */
+// NAMESPACE_OK: joins this header's global ui_frequency_response_chart_* API
+[[nodiscard]] std::vector<FrChartRun> fr_chart_finite_runs(const float* amps, size_t count);
+
 // ============================================================================
 // Data Management
```

**Interfaces:**
- Produces: `FrChartRun`, `fr_chart_finite_runs`; NaN-safe `set_data`, built-in series, styled, muted and marker passes

- [ ] **Step 1: Failing tests** (`[frequency_response_chart][gaps]`)
  - `fr_chart_finite_runs`: {1, 2, NaN, 3, 4, 5, NaN} -> {[0,2), [3,6)}; {NaN, 1, NaN, 2} -> {}; {1, 2, 3} -> {[0,3)}; count 0 -> {}; {1, INF, 2, 3} -> {[2,4)}; {1} -> {}; {NaN, NaN} -> {}.
  - STANDARD chart, amplitude range 0-100, `set_data` with amps {10, NaN, 30, 40}: the lv_chart (the child of `ui_frequency_response_chart_get_obj(chart)` passing `lv_obj_check_type(child, &lv_chart_class)`) series from `lv_chart_get_series_next(lv_chart, nullptr)` has y values {100, LV_CHART_POINT_NONE, 300, 400}; `ui_frequency_response_chart_get_series_amplitudes` returns NaN at index 1.
  - BASIC (50 points), a 131-point grid with NaN at indices 0-9: the stored amplitudes keep NaN at index 0 and finite values past the picks of index 10.
  - Render smoke: a series styled {3, glow, fill}, one muted series and one marker, all holding NaN gaps and one all-NaN series, `lv_refr_now(nullptr)` completes (ASAN in Task 15 checks it).
- [ ] **Step 2: Run** `make t F='[gaps]'`; expect FAIL.
- [ ] **Step 3: Implement.** `update_chart_series`: a non-finite amplitude sets `LV_CHART_POINT_NONE`. `stroke_polyline` and the fill loop in `draw_styled_series_cb` iterate `fr_chart_finite_runs()` and never draw a segment or triangle touching a gap; `draw_muted_series_cb` likewise (it goes through `stroke_polyline`). `draw_markers_cb` and `draw_peak_dots_cb` skip a point whose amplitude is non-finite. Downsampling stays point-picking.
- [ ] **Step 4: Run** `make t F='[frequency_response_chart]'` (PASS, the existing cases included).
- [ ] **Step 5: Commit** `feat(ui): the frequency chart draws NaN as a gap (prestonbrown/helixscreen#1812)`

## Task 7: `LiveSweepPreview`, the session

**Files:**
- Modify: `src/calibration/live_sweep_preview.cpp`
- Create: `tests/test_helpers/sweep_preview_fakes.h`, `tests/unit/test_live_sweep_preview.cpp`, `tests/unit/test_live_sweep_header_weight.cpp`

**Interfaces:**
- Consumes: `BeltStreamClient` (unchanged; `start`, `stop`, `sample_rate_hz`, its `AccelBatch`), `SweepResponseAccumulator`, `AsyncLifetimeGuard`/`LifetimeToken::defer`, `IMoonrakerAPI::rest().get_server_config`, `IMoonrakerAPI::get_client()`, `query_resonance_tester_config`, `BeltStreamClient::socket_reachable`, `helix::env_flag` (`env_knobs.h`)
- Produces: `IAccelBatchSource`, `make_accel_batch_source` (real source only; Task 13 adds the mock branch), `query_klippy_uds_address`, `query_live_sweep_facts`, `LiveSweepPreview`

Fakes (`tests/test_helpers/sweep_preview_fakes.h`, header-only, `namespace helix::calibration::test`):
- `struct SourceLog { int created = 0; int started = 0; int stopped = 0; std::string socket_path, accel_chip; SweepPlan plan; class ManualBatchSource* last = nullptr; };`
- `class ManualBatchSource final : public IAccelBatchSource` records into a `SourceLog&`, keeps the callbacks, and offers `void push(const AccelBatch&)` and `void fail(const std::string&)`, which call them on the calling thread (the test's main thread stands in for the loop thread).
- `LiveSweepPreview::SourceFactory manual_factory(SourceLog&)`.
- `class LoopBatchSource final : public IAccelBatchSource`: an `hv::EventLoopThread` whose 5 ms timer pushes 300-sample `SweepSynth` batches until stopped (bare test scaffolding).
- `std::vector<AccelBatch> synth_batches(const SweepSynthSpec&, SweepAxisGain, size_t batch_samples, size_t count)`.

- [ ] **Step 1: Failing tests** (`[sweep_preview][session]`, LVGL fixture so `UpdateQueue` drains)
  - `arm()` -> `ARMED`, factory not called. `note_console_frequency(NaN)`, `(0)`, `(-3)` -> still `ARMED`. `note_console_frequency(5)` -> `STREAMING`; `created == started == 1`; socket, chip and plan equal the target's.
  - In `IDLE` and after `stop()`, `note_console_frequency(7)` creates nothing.
  - Deliveries: push the Task 4 main-case batches (plan 5-25 Hz to keep it short) with hints, draining after each: `on_points` receives the cumulative list each time (sizes non-decreasing, the last call holds 21 points, 5..25 Hz in order); after the last point `state() == DONE` and `stopped == 1` with no `stop()` call (auto-stop).
  - Stop drops queued points: push batches that finish points, do not drain, `stop()`, drain: `on_points` never fires after `stop()`; `points()` keeps only what was delivered before.
  - `stop()` from inside `on_points` is safe and final (`IDLE`, `stopped == 1`).
  - Source error: `fail("subscribe failed")` -> after drain `DONE`, `stopped == 1`, points kept, exactly one info line containing "subscribe failed" (`LogCapture`).
  - `finish()`: plan 5-135 while the pushed stream reaches only 40 Hz: after `finish()` and 2.0 s more of pushed stream, `DONE` and `stopped == 1`.
  - Re-arm while streaming: the old source is stopped, `points()` empty, a second source created on the next console line.
  - `HELIX_SWEEP_PREVIEW_OFF=1` (`ScopedEnv`): `arm()` leaves `IDLE`, one info line.
  - Real thread: `LoopBatchSource` streaming, wait (bounded, 2 s) until at least one delivery, `stop()`, then drain every 10 ms for 200 ms: the delivery count does not change. Destroying a `LiveSweepPreview` while a `LoopBatchSource` streams returns (no hang).
  - `query_live_sweep_facts` with `MoonrakerClientMock` + `MoonrakerAPIMock`: `socket_path` is the mock's `/tmp/helix-mock-klippy-<pid>.sock`, `socket_reachable` true, `cfg.accel_chip == "adxl345"`, `cfg.from_printer` true.
  - `test_live_sweep_header_weight.cpp`: includes `ui_panel_belt_tension.h`, `ui_panel_input_shaper.h`, `ui_wizard_input_shaper.h` and `live_sweep_preview.h` first in its TU, then `#ifdef HV_EVENT_LOOP_THREAD_HPP_` `#error`, with a comment stating why (ESP32 parses these headers); one trivial `TEST_CASE` tagged `[sweep_preview][compile]`. Prove it can fail once by temporarily including `belt_stream_client.h` in `live_sweep_preview.h` (do not commit that).
- [ ] **Step 2: Run** `make t F='[session]'`; expect FAIL.
- [ ] **Step 3: Implement** in `live_sweep_preview.cpp`:
  - A file-local `StreamClientBatchSource` wrapping `BeltStreamClient` unchanged; `make_accel_batch_source` returns it.
  - `arm`: `stop()`, clear points, store target and callback, `ARMED`; under `HELIX_SWEEP_PREVIEW_OFF` stay `IDLE` with one info log per process.
  - `note_console_frequency`: store the hint (atomic); in `ARMED`, `attach()`: build the accumulator from the plan, take a token, create the source through the factory, `start()` it with lambdas calling `on_batch`/`on_source_error`; a factory returning null or `start()` returning false -> `DONE` with one info log.
  - `on_batch` (loop thread): once, `setpriority(PRIO_PROCESS, gettid, PREVIEW_LOOP_NICE)` under `#ifdef __linux__`; note the hint; `add_samples(batch.samples, batch.contiguous(), source rate)`; record thread CPU (`clock_gettime(CLOCK_THREAD_CPUTIME_ID)`) and stream time; defer fresh points with `tok.defer("LiveSweepPreview::points", ...)`; one debug line when `PREVIEW_NO_LOCK_LOG_S` passes unlocked; when `complete()`, or `finishing_` and the stream passed the deadline set on the first batch after `finish()`, defer `stop_source()` once.
  - `on_source_error` (loop thread): defer an info log plus `stop_source()`.
  - `deliver` (main): append, call `on_points_(points_)`.
  - `stop_source` (main): invalidate, `source_->stop()`, reset source and accumulator, info log `"[LiveSweepPreview] {chip}: {n} points, loop thread {ms} ms CPU over {s:.0f} s of stream"`, state `DONE` (or `IDLE` when called from `stop()`).
  - `query_klippy_uds_address` / `query_live_sweep_facts`: chained, `on_done` exactly once.
  - The destructor calls `stop()`.
- [ ] **Step 4: Run** `make t F='[sweep_preview]'` (PASS).
- [ ] **Step 5: Commit** `feat(calibration): stream a running sweep into the live preview (prestonbrown/helixscreen#1812)`

## Task 8: Shaper progress carries the console frequency

**Files:**
- Modify: `include/advanced_panel_types.h`, `include/input_shaper_calibrator.h` (diffs below), `src/api/moonraker_advanced_api.cpp` (`InputShaperCollector::emit_progress` and every `on_progress_` call), `src/calibration/input_shaper_calibrator.cpp` (`api_progress` forwards it), `src/ui/ui_panel_input_shaper.cpp` and `src/ui/ui_wizard_input_shaper.cpp` (lambdas gain the parameter, unused for now), and every test the compiler flags (`test_moonraker_api_input_shaper.cpp`, `test_input_shaper_calibrator.cpp`, `test_calibration_transport_loss.cpp`, any `IAdvancedAPI` test double)

```diff
--- a/include/advanced_panel_types.h
+++ b/include/advanced_panel_types.h
@@ -44,4 +44,7 @@
 /// Separate from AdvancedProgressCallback because SHAPER_CALIBRATE is the one
 /// long operation whose UI text depends on a phase the percentage cannot encode.
-using ShaperProgressCallback = std::function<void(int percent, ShaperCalibrationPhase phase)>;
+/// freq_hz is the sweep's latest "Testing frequency" value on Sweeping reports
+/// and 0 on every other phase.
+using ShaperProgressCallback =
+    std::function<void(int percent, ShaperCalibrationPhase phase, float freq_hz)>;
 } // namespace helix
--- a/include/input_shaper_calibrator.h
+++ b/include/input_shaper_calibrator.h
@@ -52,5 +52,7 @@
 /// still sweeping or Klipper has moved on to fitting shapers; it is reported
 /// explicitly because the percentage alone cannot distinguish the two.
-using ProgressCallback = std::function<void(int percent, ShaperCalibrationPhase phase)>;
+/// freq_hz is the sweep's latest "Testing frequency" value while Sweeping, 0 otherwise.
+using ProgressCallback =
+    std::function<void(int percent, ShaperCalibrationPhase phase, float freq_hz)>;
 using ResultCallback = std::function<void(const InputShaperResult& result)>;
 using SuccessCallback = std::function<void()>;
```

**Interfaces:**
- Consumes: `InputShaperCollector::parse_sweep_line` (already holds `freq`)
- Produces: `(percent, phase, freq_hz)` on both callback types

- [ ] **Step 1: Failing test** (`[input_shaper]`, in `test_moonraker_api_input_shaper.cpp`): drive the mock's SHAPER_CALIBRATE transcript (5-135 Hz, compact 20 lines): every `Sweeping` report's `freq_hz` equals that line's "Testing frequency" value (5, 12, ... 135 as the mock prints them); every `Analyzing` and `Complete` report has `freq_hz == 0`. And through `InputShaperCalibrator::run_calibration`, the same values arrive unchanged.
- [ ] **Step 2: Run** `make t F='[input_shaper]'`; expect a compile failure, then FAIL.
- [ ] **Step 3: Implement.** `emit_progress(int percent, ShaperCalibrationPhase phase, float freq_hz, const std::string& status)`; `parse_sweep_line` passes `freq`, every other caller 0. Update callers until it compiles; behaviour of every existing lambda is unchanged.
- [ ] **Step 4: Run** `make t F='[input_shaper]'`, `make t F='[calibrator]'` and `make t F='[wizard]'` (PASS).
- [ ] **Step 5: Commit** `feat(input-shaper): progress reports carry the sweep frequency (prestonbrown/helixscreen#1812)`

## Task 9: `ShaperSweepChart`, the shared Input Shaper live chart

**Files:**
- Create: `include/ui_shaper_sweep_chart.h`, `src/ui/ui_shaper_sweep_chart.cpp`, `tests/unit/test_shaper_sweep_chart.cpp`
- Modify: `src/ui/ui_panel_input_shaper.cpp` (`create_chart_widgets` uses `SHAPER_CHART_MAX_HZ`, `populate_chart` uses `shaper_raw_psd_color()` and `SHAPER_CHART_HEADROOM`; the literals `200.0f`, `0xB0B0B0` and `1.1f` leave the panel), `app_srcs_excluded.txt` (`src/ui/ui_shaper_sweep_chart.cpp`)

**Interfaces:**
- Consumes: `LiveSweepPreview`, `layout_preview`, `ui_frequency_response_chart_*`, `PlatformCapabilities::detect().tier`
- Produces: `SHAPER_CHART_MAX_HZ`, `SHAPER_CHART_HEADROOM`, `shaper_raw_psd_color()`, `ShaperSweepChart`

- [ ] **Step 1: Failing tests** (`[input_shaper][preview]`, LVGL fixture, a host `lv_obj` on the test screen, `manual_factory`)
  - `begin(host, 'X', nullopt)` -> false, no chart, factory not called, `has_curve()` false.
  - `begin(host, 'X', target)` -> true, chart created in `host` with freq range [0, 200] and one series whose color is `shaper_raw_psd_color()` and whose style is the default (the results' raw PSD look); `on_progress(Sweeping, 0)` attaches nothing; `on_progress(Sweeping, 5)` attaches.
  - Points 5..60 (psd_x = f) delivered: the series holds 201 amplitudes, NaN outside 5..60, `amps[60] == 60`; amplitude range [0, 66]; the curve-changed callback fired once with true.
  - Column: for `'Y'` the series reads `psd_y`.
  - `on_progress(Analyzing, 0)` -> preview `finish()` (state still `STREAMING` until the grace passes).
  - `show_curve({{5, 1}, {6, 4}})`: preview stopped (`stopped == 1`), the series holds those two amplitudes, range [0, 4.4].
  - A second `begin` clears the curve (callback false then true on the next point) and arms a fresh preview.
  - `destroy()` then `begin` on a new host recreates the chart there.
  - `stop()` keeps the drawn amplitudes.
- [ ] **Step 2: Run** `make t F='[input_shaper][preview]'`; expect FAIL.
- [ ] **Step 3: Implement.** The chart is configured for `PlatformCapabilities::detect().tier`; on points, `layout_preview(points, column, 0, SHAPER_CHART_MAX_HZ)` then `draw`. `~ShaperSweepChart()` stops the preview and does not touch LVGL; callers `destroy()` while their host tree is alive.
- [ ] **Step 4: Run** `make t F='[input_shaper]'` (PASS, including the results-chart cases); ESP32 check.
- [ ] **Step 5: Commit** `feat(input-shaper): a live sweep chart shared by the panel and the wizard (prestonbrown/helixscreen#1812)`

## Task 10: Belt Tension panel (after the settle-laps batch lands)

**Files:**
- Modify: `include/ui_panel_belt_tension.h` (diff below, re-anchored on the then-current header), `src/ui/ui_panel_belt_tension.cpp`, `tests/unit/test_belt_tension_panel_states.cpp`

```diff
--- a/include/ui_panel_belt_tension.h
+++ b/include/ui_panel_belt_tension.h
@@ -13,4 +13,5 @@
 #include "belt_tension_types.h"
 #include "belt_tune_session.h"
+#include "live_sweep_preview.h"
 #include "memory_utils.h"
 #include "operation_timeout_guard.h"
@@ -300,4 +301,15 @@
     /// One curve into a series as a percentage of `tallest`; hidden when empty.
     void push_series_data(int series_id, const helix::calibration::BeltCurve& curve, float tallest);
+    /// Arm the live preview for `path`'s sweep, which starts now. No-op without
+    /// a chart or without a live_sweep_target().
+    void arm_preview(helix::calibration::BeltPath path);
+    /// Main thread: the preview's points so far for `path`.
+    void on_preview_points(helix::calibration::BeltPath path,
+                           const std::vector<helix::calibration::SweepPoint>& points);
+    /// Lay each live path's preview out on the chart's current axis in place of
+    /// its own series. Runs at the end of every chart push.
+    void refresh_preview_series();
+    /// Stop the stream and drop both previews; each path's own series shows again.
+    void clear_previews();
     void run_after_ram_check(std::function<void()> go);
     /// Re-check the gate before any sweep starts; false (logged) when it is
@@ -418,4 +430,12 @@
     lv_obj_t* chart_host_tuning_ = nullptr;
     lv_obj_t* chart_host_wide_ = nullptr;
+    /// One series per path for its live preview, styled like series_.
+    int preview_series_[2] = {-1, -1};
+    helix::calibration::LiveSweepPreview preview_;
+    /// The points each path's preview shows.
+    std::vector<helix::calibration::SweepPoint> preview_points_[2];
+    /// The path's preview stands in for its series: from its first point until
+    /// the chart draws the CSV curve that preview was measuring.
+    bool preview_live_[2] = {false, false};
 
     /// Render tier override for tests; nullopt reads PlatformCapabilities.
```

**Interfaces:**
- Consumes: `LiveSweepPreview`, `live_sweep_target`, `layout_preview`, `query_klippy_uds_address`/`parse_klippy_uds_address`, `push_series_data`, `path_color`, `ui_frequency_response_chart_get_freq_range`
- Produces: the belt preview behaviour below

Behaviour:
- `ensure_chart()` adds `preview_series_[idx]` per path ("Path A live", "Path B live"), color `path_color`, style `{3, true, true}` (the final curve's), hidden. `apply_path_colors()` and `destroy_chart()` cover them.
- `start_next_measurement()`, after `measure_path(...)`: `arm_preview(path)`, which arms only when `chart_` exists and `live_sweep_target({klippy_socket_path_, klippy_socket_reachable_, sweep_cfg_}, SweepAxis::DIAGONAL, run_window_)` has a value; the callback is `on_preview_points(path, points)`.
- `on_sweep_progress()`: `preview_.note_console_frequency(freq_hz)` before the cursor update.
- `on_preview_points`: store the points, set `preview_live_[idx]` once a clean point exists, `refresh_preview_series()`.
- `refresh_preview_series()`: for each live path, `layout_preview(points, SweepColumn::XYZ, lo, hi)` on the chart's current range, converted to a `BeltCurve` and pushed through `push_series_data(preview_series_[idx], curve, max_amp)` (so it reads 0-100% of its own maximum), and `series_[idx]` hidden; non-live previews hidden. Called at the end of `push_chart_data()` and `push_tune_chart_data()`.
- `on_sweep_complete()`: `preview_.stop()`; full check: `preview_live_[idx] = false`, then `push_chart_data()` (the CSV curve replaces it now); tuning round: the preview stays frozen until `finish_tune_round()`, which clears both before its `push_chart_data()`.
- `clear_previews()` (stop, points cleared, flags false, series hidden): from `cancel_run()`, `on_error()`, `end_tuning()`, `cleanup()`, and the destructor (`preview_.stop()` only there; no LVGL).
- `probe_klippy_socket()` uses `query_klippy_uds_address()`; its inline JSON walk goes.

- [ ] **Step 1: Failing tests** (`[belt][panel][preview]`, `BeltPanelFixture` with `panel().preview_.set_source_factory(manual_factory(log))` through the existing friend)
  - Start: while the run title reads "Settling belts", `log.created == 0` and the preview is `IDLE` (not even armed until the sweep starts); after the first path-A progress line, `created == 1`, `accel_chip == "adxl345"`, plan {5, 135, 1}.
  - Calling `on_preview_points(PATH_A, points)` with clean points 5..60 (psd_x = f, y = z = 0): `preview_series_[0]` visible, its amplitudes 100 * f / 60 at those slots and NaN past 60; `series_[0]` hidden; its style equals `series_[0]`'s.
  - Completing path A's sweep: `preview_series_[0]` hidden, `series_[0]` visible with the CSV curve, `log.stopped >= 1`; path B's sweep arms a second source.
  - Tuning round: after path A's windowed sweep completes, `preview_series_[0]` is still visible (frozen) while B sweeps; after the round ends both preview series are hidden. The preview plan is the floored/ceiled window.
  - Stop (cancel) mid-sweep: previews hidden, `stopped == created`.
  - `set_render_tier_for_test(EMBEDDED)`: no chart, `created == 0` through a whole run.
  - Socket unreachable (`klippy_socket_reachable_ = false` through the fixture): `created == 0`, the run still completes on the CSV.
  - Deactivate with `Suspended` mid-sweep: the source is not stopped; `NavigateAway`: it is.
- [ ] **Step 2: Run** `make t F='[belt][panel]'`; expect FAIL.
- [ ] **Step 3: Implement** the behaviour above.
- [ ] **Step 4: Run** `make t F='[belt]'` (PASS).
- [ ] **Step 5: Commit** `feat(belt): draw each path's response live as it sweeps (prestonbrown/helixscreen#1812)`

## Task 11: Input Shaper panel (MEASURING)

**Files:**
- Modify: `include/ui_panel_input_shaper.h` (diff below), `src/ui/ui_panel_input_shaper.cpp`, `ui_xml/input_shaper_panel.xml`, `tests/test_helpers/input_shaper_panel_test_access.h` (add `set_preview_source_factory(panel, factory)` and `sweep_chart(panel)`), `tests/unit/test_input_shaper_panel_integration.cpp`

```diff
--- a/include/ui_panel_input_shaper.h
+++ b/include/ui_panel_input_shaper.h
@@ -5,4 +5,5 @@
 
 #include "ui_frequency_response_chart.h"
+#include "ui_shaper_sweep_chart.h"
 #include "ui_timer_guard.h"
 
@@ -11,4 +12,5 @@
 #include "input_shaper_calibrator.h"
 #include "klipper_config_editor.h"
+#include "live_sweep_preview.h"
 #include "overlay_base.h"
 #include "platform_capabilities.h"
@@ -210,4 +212,8 @@
     // Calibration commands
     void start_calibration(char axis);
+    /// Ask the printer for klippy's socket and [resonance_tester] (preview_facts_).
+    void query_preview_facts();
+    /// Start `axis`'s live chart in the MEASURING view when live_sweep_target() allows.
+    void begin_axis_preview(char axis);
     void cancel_calibration();
     void apply_recommendation();
@@ -295,4 +301,9 @@
     helix::IMoonrakerAPI* api_ = nullptr;
 
+    // Live sweep chart (MEASURING)
+    lv_subject_t is_measuring_has_chart_{};
+    helix::calibration::LiveSweepFacts preview_facts_;
+    helix::ui::ShaperSweepChart sweep_chart_;
+
     // Private setup helper (called by create())
     void setup_widgets();
```

XML (`state_measuring`, between the status container and `btn_abort`):

```xml
<lv_obj name="chart_container_measuring"
        width="100%" flex_grow="1" style_pad_all="0" style_radius="#border_radius"
        style_bg_color="#screen_bg" style_bg_opa="255" scrollable="false">
  <bind_flag_if_eq subject="is_measuring_has_chart" flag="hidden" ref_value="0"/>
</lv_obj>
```

Behaviour:
- `init_subjects()`: `UI_MANAGED_SUBJECT_INT(is_measuring_has_chart_, 0, "is_measuring_has_chart", subjects_)`; `sweep_chart_.set_on_curve_changed(...)` sets it.
- `on_activate()`: `query_preview_facts()` -> `query_live_sweep_facts(*api_, lifetime_.bg_cb("InputShaperPanel::preview_facts", ...))` storing `preview_facts_`.
- `start_calibration(axis)`: `begin_axis_preview(axis)` before `run_calibration`: `sweep_chart_.begin(find_required(overlay_root_, "chart_container_measuring"), axis, live_sweep_target(preview_facts_, axis == 'X' ? SweepAxis::X : SweepAxis::Y))`. Calibrate All's Y run calls it again (a fresh preview).
- The progress lambda calls `sweep_chart_.on_progress(phase, freq_hz)` after its `cal_tok` check.
- `on_calibration_result()`, `on_calibration_error()`, `cancel_calibration()`, `on_deactivating()` (not `Suspended`): `sweep_chart_.stop()` and `is_measuring_has_chart = 0`.
- `cleanup()` and `on_ui_destroyed()`: `sweep_chart_.destroy()`.

- [ ] **Step 1: Failing tests** (`[input_shaper][panel][preview]`, the existing `InputShaperDeltaFixture` pumping, `manual_factory`)
  - Calibrate X: after the first sweep line, `created == 1`, chip "adxl345", plan {5, 135, 1}; delivering points makes `is_measuring_has_chart == 1` and fills the measuring chart with psd_x values.
  - The "Calculating" line (`Analyzing`): the preview is finishing, the chart keeps its curve.
  - The result: state RESULTS, `stopped == created`, `is_measuring_has_chart == 0`.
  - Calibrate All: the Y run arms a second source with column Y; `created == 2`.
  - Facts with `socket_reachable` false: no source, `is_measuring_has_chart` stays 0, the run reaches RESULTS.
  - Abort mid-sweep: `stopped == created`.
- [ ] **Step 2: Run** `make t F='[input_shaper][panel]'`; expect FAIL.
- [ ] **Step 3: Implement.** Then measure: `ctl navigate` to the panel under `--test` with `HELIX_HEADLESS=1 SDL_VIDEODRIVER=dummy` (pinned socket and config dir), start X, and `ctl geom chart_container_measuring` at 800x480 and at the smallest breakpoint (480x320). Report both heights. If the smallest is under 80 px, stop and report; do not invent a breakpoint rule.
- [ ] **Step 4: Run** `make t F='[input_shaper]'` (PASS).
- [ ] **Step 5: Commit** `feat(input-shaper): live response chart while the panel measures (prestonbrown/helixscreen#1812)`

## Task 12: Input Shaper wizard step

**Files:**
- Modify: `include/ui_wizard_input_shaper.h` (diff below), `src/ui/ui_wizard_input_shaper.cpp`, `ui_xml/wizard_input_shaper.xml`, `tests/unit/test_wizard_input_shaper_step.cpp`

```diff
--- a/include/ui_wizard_input_shaper.h
+++ b/include/ui_wizard_input_shaper.h
@@ -3,8 +3,10 @@
 #pragma once
 
+#include "ui_shaper_sweep_chart.h"
 #include "ui_timer_guard.h"
 
 #include "async_lifetime_guard.h"
 #include "input_shaper_calibrator.h"
+#include "live_sweep_preview.h"
 #include "lvgl/lvgl.h"
 #include "subject_managed_panel.h"
@@ -208,4 +210,29 @@
     }
 
+    lv_subject_t* get_has_chart_subject() {
+        return &has_chart_;
+    }
+
+    // ========================================================================
+    // Live sweep chart (main thread)
+    // ========================================================================
+
+    /// Ask the printer for klippy's socket and [resonance_tester]; the answer
+    /// lands in preview_facts_ before the noise check finishes.
+    void query_preview_facts();
+    /// Start `axis`'s live curve when live_sweep_target() allows one.
+    void begin_axis_preview(char axis);
+    /// A progress report for the running axis.
+    void on_axis_progress(ShaperCalibrationPhase phase, float freq_hz);
+    /// The axis finished: its CSV curve replaces the preview.
+    void on_axis_result(const InputShaperResult& result);
+
+    /// Test hook: the live preview's stream source.
+    void set_preview_source_factory_for_test(
+        helix::calibration::LiveSweepPreview::SourceFactory factory);
+    [[nodiscard]] const helix::ui::ShaperSweepChart& sweep_chart_for_test() const {
+        return sweep_chart_;
+    }
+
     /**
      * @brief Show the analysis-phase treatment: spinner on, elapsed "Ns" label
@@ -298,4 +325,5 @@
     lv_subject_t calibration_indeterminate_{}; ///< 1 during the offline analysis phase (no
                                                ///< percent): hides the bar, shows the spinner
+    lv_subject_t has_chart_{}; ///< 1 while the live chart holds a curve
 
     // String buffers for subjects
@@ -306,4 +334,7 @@
     helix::ui::ElapsedLabelTimer analysis_elapsed_;
 
+    helix::calibration::LiveSweepFacts preview_facts_;
+    helix::ui::ShaperSweepChart sweep_chart_;
+
     // Calibrator instance (owns the calibrator)
     std::unique_ptr<helix::calibration::InputShaperCalibrator> calibrator_;
```

XML (inside the `ui_card`, after the status display, before the spacer):

```xml
<lv_obj name="wizard_input_shaper_chart_host"
        width="100%" flex_grow="1" style_pad_all="0" style_radius="#border_radius"
        style_bg_color="#screen_bg" style_bg_opa="255" scrollable="false">
  <bind_flag_if_eq subject="wizard_input_shaper_has_chart" flag="hidden" ref_value="0"/>
</lv_obj>
```

Behaviour:
- `init_subjects()`: `has_chart_` registered as `wizard_input_shaper_has_chart`; the curve-changed callback sets it.
- `begin_is_calibration_flow()`: `step->query_preview_facts()` (main thread; answer through `get_lifetime_token().defer`).
- Before each `run_calibration('X' / 'Y')`, queue (the noise-check and X-result callbacks are not on the main thread) a token-checked `begin_axis_preview(axis)`; the queue's order puts it ahead of that axis's first progress report.
- Progress lambdas: a new `safe_axis_progress(token, phase, freq_hz)` queues `step->on_axis_progress(...)` like `safe_update_progress`. Result lambdas: `safe_axis_result(token, result)` queues `step->on_axis_result(result)` -> `sweep_chart_.show_curve(result.freq_response)`.
- Errors: `sweep_chart_.stop()`. `abort_in_progress_calibration()`: stop and clear. `cleanup()`: `sweep_chart_.destroy()`, `has_chart_ = 0` unless the calibration completed (then the curve stays for re-entry only if the chart survives; destroy regardless, since the tree goes).

- [ ] **Step 1: Failing tests** (`[wizard][input_shaper][preview]`, `set_preview_source_factory_for_test(manual_factory(log))`, the step's screen created on the test screen, mock client as the global client)
  - Start: X's first sweep line arms and attaches (chip "adxl345", column X); delivered points set `wizard_input_shaper_has_chart` to 1.
  - X's result: the chart shows X's `freq_response` (amplitudes equal the CSV's), source stopped; Y's first line attaches a second source with column Y.
  - Y's result: the chart holds Y's CSV curve.
  - Cancel mid-sweep: `stopped == created`. `cleanup()`: chart destroyed (`sweep_chart_for_test().chart() == nullptr`).
  - Unreachable socket: no source, `has_chart` stays 0, calibration still completes.
- [ ] **Step 2: Run** `make t F='[wizard]'`; expect FAIL.
- [ ] **Step 3: Implement**; then `ctl geom wizard_input_shaper_chart_host` at 800x480 and 480x320 in `--test` (wizard reachable per `docs/devel/HELIXCTL.md`) and report the heights, same stop rule as Task 11.
- [ ] **Step 4: Run** `make t F='[wizard]'` (PASS).
- [ ] **Step 5: Commit** `feat(wizard): live response chart during input shaper calibration (prestonbrown/helixscreen#1812)`

## Task 13: Mock, a synthetic accelerometer stream for `--test`

**Files:**
- Create: `include/accel_batch_source_mock.h`, `src/api/accel_batch_source_mock.cpp` (the name matches the Makefile's `src/api/*_mock*.cpp` filter, so builds without mocks drop it), `tests/unit/test_mock_accel_batch_source.cpp`
- Modify: `include/moonraker_client_mock.h` (diff below), `src/api/moonraker_client_mock.cpp`, `src/application/moonraker_manager.cpp`, `src/calibration/live_sweep_preview.cpp` (`make_accel_batch_source` mock branch), `tests/unit/test_mock_test_resonances.cpp`, `app_srcs_excluded.txt` (`src/api/accel_batch_source_mock.cpp`)

```diff
--- a/include/moonraker_client_mock.h
+++ b/include/moonraker_client_mock.h
@@ -4,4 +4,5 @@
 #pragma once
 
+#include "accel_batch_source_mock.h"
 #include "axis.h"
 #include "mock_persona.h"
@@ -790,4 +791,21 @@
     }
 
+    /// SHAPER_CALIBRATE plays whole-Hz sweep lines at the cadence
+    /// TEST_RESONANCES uses (hz_per_sec and the sim speed) instead of the
+    /// compact 20-line transcript, so the live preview has a sweep to follow.
+    /// MoonrakerManager turns it on for the app; unit tests keep the compact one.
+    void set_shaper_sweep_realtime(bool realtime) {
+        shaper_sweep_realtime_ = realtime;
+    }
+
+    /// What the sweep this mock started last resonates like, matching the CSV
+    /// it will write. Defaults until a sweep starts.
+    [[nodiscard]] helix::calibration::MockSweepResponse active_sweep_response() const;
+
+    /// The live preview's stream for a sweep of this mock: MockAccelBatchSource
+    /// over active_sweep_response() at this mock's sim speed.
+    [[nodiscard]] std::unique_ptr<helix::calibration::IAccelBatchSource>
+    make_accel_batch_source(const helix::calibration::SweepPlan& plan) const;
+
     /**
      * @brief Path the next TEST_RESONANCES for @p name writes
@@ -1604,4 +1622,9 @@
                             std::function<void()> on_done);
 
+    /// Milliseconds between whole-Hz "Testing frequency" lines: the
+    /// set_belt_line_interval_ms() override, else one simulated second per
+    /// hz_per_sec Hz shortened by the sim speed. Shared by both sweeps.
+    [[nodiscard]] uint32_t sweep_line_interval_ms() const;
+
     /// play_console_lines() for ticks that do more than print one line: each
     /// step runs on its own tick, then on_done on the tick after the last.
@@ -2207,4 +2230,8 @@
     /// 0 = derive the line interval from hz_per_sec and the sim speed.
     uint32_t belt_line_interval_ms_{0};
+    bool shaper_sweep_realtime_{false};
+    /// Set where a sweep is dispatched, read where the preview attaches.
+    mutable std::mutex sweep_response_mutex_;
+    helix::calibration::MockSweepResponse sweep_response_;
 
     bool mmu_enabled_{true}; ///< MMU available (default true for existing tests)
```

Values the mock sets as each sweep is dispatched (under `sweep_response_mutex_`), matching the CSV it writes for that sweep:

| Sweep | peak_hz | half_width_hz | axis_weight | drive_mm_s2 |
|---|---|---|---|---|
| TEST_RESONANCES, path A / B | that sweep's `sweep_peak_hz` (NaN for a scripted `flat`) | 10 (`MAIN_HALF_WIDTH_SQ` 100) | {0.671, 0.671, 0.316} (the CSV's 0.45 / 0.45 / 0.10 power shares) | 300 (`sqrt(3 * 3e4)`, the CSV's peak) |
| SHAPER_CALIBRATE X | 53.8 | 8 | {1.0, 0.387, 0.283} (power shares 1 / 0.15 / 0.08) | 0.248 (`sqrt(3 * 0.0205)`) |
| SHAPER_CALIBRATE Y | 48.2 | 8 | {0.387, 1.0, 0.283} | 0.248 |

Behaviour:
- `sweep_line_interval_ms()`: `belt_line_interval_ms_` when nonzero, else `max(1, sim_speed().shorten_wait_ms(1000 / max(1, hz_per_sec)))`; TEST_RESONANCES uses it in place of its inline computation.
- `set_shaper_sweep_realtime(true)`: SHAPER_CALIBRATE plays "Testing frequency N Hz" for N = ceil(min) .. max at `sweep_line_interval_ms()`, then the rest of the transcript at 100 ms (two `play_console_lines` calls chained through `on_done`). Off (default): unchanged.
- `MoonrakerManager`, where it builds the mock client: `mock->set_shaper_sweep_realtime(true)`.
- `make_accel_batch_source(plan)` (mock): `std::make_unique<MockAccelBatchSource>(plan, active_sweep_response(), get_simulation_speedup())`.
- `helix::calibration::make_accel_batch_source`: under `#ifdef HELIX_ENABLE_MOCKS` (the mock sources are filtered out of builds without mocks: `Makefile`, `ENABLE_MOCKS`), when `get_runtime_config()->should_mock_moonraker()`, `get_moonraker_client_mock()`'s source (null when there is no mock: one warn, no preview); otherwise, and always without mocks, the stream client.
- `MockAccelBatchSource`: `SweepSynth` with `drive_start_s = DRIVE_LEAD_S`, rate 3053, `noise_mm_s2 = MOCK_NOISE_FRACTION * drive_mm_s2`, gain `mock_sweep_axis_gain`; every `BATCH_INTERVAL_MS` on its own `hv::EventLoopThread`, one batch of `round(3053 * 0.1 * sim_speed)` samples with `errors = overflows = 0`. `sample_rate_hz()` 3053 once started. `stop()` joins; the destructor calls it.

- [ ] **Step 1: Failing tests** (`[mock][sweep_preview]`)
  - `mock_sweep_axis_gain({58.4, 5, 300, {0.671, 0.671, 0.316}}, 58.4)` == `300 * sqrt(1.02) * weight` per axis (1e-3 relative); at a NaN peak, `300 * sqrt(0.02) * weight`; 10 Hz off a 5 Hz half width, `300 * sqrt(0.02 + 0.2) * weight`.
  - After `TEST_RESONANCES AXIS=1,-1` (defaults): `active_sweep_response()` = {110, 10, 300 within 0.5, {0.671, 0.671, 0.316}}; after `SHAPER_CALIBRATE AXIS=X`: {53.8, 8, 0.248 within 0.001, {1.0, 0.387, 0.283}}; AXIS=Y swaps the first two weights.
  - `set_shaper_sweep_realtime(true)` + `set_belt_line_interval_ms(1)`: the captured SHAPER_CALIBRATE console holds 131 "Testing frequency" lines 5..135 before "Calculating the best"; with realtime off, the 20-line transcript is unchanged.
  - `MockAccelBatchSource` plan {5, 30, 1}, peak 20, hwhm 3, drive 300, sim speed 50: collect batches for up to 2 s wall (mutex-guarded), then run them through a `SweepResponseAccumulator` with hints `floor(drive(t + 1.5))`: locked within 0.1 s of 1.5, 26 points, argmax within 1 Hz of 20, rates 3053. `stop()` returns promptly and no batch arrives after it.
- [ ] **Step 2: Run** `make t F='[mock]'`; expect FAIL.
- [ ] **Step 3: Implement** the above.
- [ ] **Step 4: Run** `make t F='[mock]'`, `make t F='[input_shaper]'`, `make t F='[belt]'` (PASS); ESP32 check. Then in `--test` (`HELIX_HEADLESS=1 SDL_VIDEODRIVER=dummy`, pinned socket, `--sim-speed 6 -vv`), run a belt check and a shaper X calibration through `ctl` and confirm in the log: the `[LiveSweepPreview]` stop line with a point count of 131 for each sweep, no "no lock" debug line.
- [ ] **Step 5: Commit** `feat(mock): a synthetic accelerometer stream so --test shows the live sweep (prestonbrown/helixscreen#1812)`

## Task 14: Docs

**Files:**
- Modify: `docs/devel/BELT_TENSION.md` (Flow: the live preview, when it attaches, when the CSV replaces it, tuning-round freeze; Key files; Tests), `docs/devel/INPUT_SHAPER.md` (Input Shaper Panel: MEASURING chart; the wizard; the chart widget's NaN gaps and `fr_chart_finite_runs`; Downsampling note), `docs/devel/BELT_TUNER.md` (the stream client's consumers now include the live preview), `docs/devel/MOCK_ENVIRONMENT_VARIABLES.md` (the mock's synthetic stream and realtime shaper sweep under `--test`; how `--sim-speed` paces both), `docs/devel/ENVIRONMENT_VARIABLES.md` (`HELIX_SWEEP_PREVIEW_OFF`)
- Cite code as `path#symbol`, never line numbers; `make check-doc-anchors`.

- [ ] **Step 1:** Write the sections, present tense, no history, no em-dashes. Cover: the signals table (stream, console line, config, CSV) in short form; the lock (why console timing is not used); what is NaN and why; that the preview never yields a verdict; the threading crossing; the CB1 measurement procedure (Task 16) and where its result will be recorded.
- [ ] **Step 2:** `make check-doc-anchors` (advisory; fix what it flags in these files).
- [ ] **Step 3: Commit** `docs: live sweep preview (prestonbrown/helixscreen#1812)`

## Task 15: Gates (once, at the end of the branch)

- [ ] `python3 scripts/check_esp32_app_srcs.py`, `scripts/check_l081_anti_pattern.py`, `scripts/check_timer_destructor_cancel.py`, `scripts/check_imperative_ui.py --summary` (count must not rise), `scripts/check_comment_archaeology.py`.
- [ ] `make full-test-run` (unit sweep + bats). Batch it with whatever else is landing (CLAUDE.md "Land in batches").
- [ ] `scripts/test-host-run.sh asan '[sweep_preview]'`, then `'[gaps]'`, `'[belt][panel][preview]'`, `'[input_shaper][preview]'`, `'[wizard][input_shaper][preview]'`, `'[mock][sweep_preview]'` on the test host (never ASAN on thelio).
- [ ] `make mutate-diff` once for the branch; any surviving mutant in `sweep_response_estimator.cpp`, `live_sweep_preview.cpp` or `fr_chart_finite_runs` gets a test or a written reason.
- [ ] Screenshots in `--test` (`HELIX_HEADLESS=1 SDL_VIDEODRIVER=dummy`, `--sim-speed 6`): Belt RUNNING mid path A, Belt TUNING mid round, Input Shaper MEASURING mid sweep and during ANALYZING, the wizard mid Y; show them to Preston with `show_image` (the "drawn like the final curve" judgment is his).

## Task 16: Hardware verification (hw-verify; not a CI gate)

Ask Preston before the first command on a real printer; every sweep moves the machine and is confirmed each time. Claim `device:<name>` around each.

- [ ] **CB1 CPU (Voron 2.4, BTT CB1).** Same belt full check twice, preview on and with `HELIX_SWEEP_PREVIEW_OFF=1`. Record per run: the `[LiveSweepPreview]` stop line (loop thread CPU ms over stream seconds, so percent of one core); klippy's CPU over the sweep from `/proc/<klippy pid>/stat` utime+stime sampled at sweep start and end; helix-screen's the same way; `grep -c "Timer too close\|MCU .* shutdown" klippy.log` over the run. Report the table; no threshold is a gate, but any "Timer too close" with the preview on and not off goes to Preston before merge.
- [ ] **K1C and Voron, both panels:** Belt full check and one tuning round; Input Shaper X and Calibrate All. For each sweep: the preview locked (no "no lock" debug line), its peak against the CSV's (expect within one CSV bin on the K1C's single mode; the Voron's 105-140 Hz hump is not compared), and the swap to the CSV curve looked like a refinement, not a jump (Preston's eye).
- [ ] **Wizard on one of them** (factory-fresh config dir), X and Y.
- [ ] File the results on prestonbrown/helixscreen#1812 (numbers, no device addresses).
