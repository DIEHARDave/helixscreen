# Live progressive sweep graphs (prestonbrown/helixscreen#1812): design

Status: design only, nothing implemented. Written 2026-10-15 against `feature/belt-tune-loop`.

## Goal

While `TEST_RESONANCES` (Belt Tension) or `SHAPER_CALIBRATE` (Input Shaper) sweeps, draw
the response curve one point per Hz as the sweep advances, on screens that run on the
Klipper host. The preview is cosmetic: when Klipper writes its CSV, the CSV curve replaces
the preview, and every verdict, peak marker, similarity number and shaper recommendation
keeps coming from the CSV exactly as today. Remote screens, the K-Touch (ESP32) and
EMBEDDED-tier boards keep today's cursor-only behaviour.

## The signals

| Input | What it measures | Who publishes it | Who has none |
|---|---|---|---|
| Accelerometer stream (`<chip>/dump_<chip>` on klippy's UDS) | Raw acceleration in mm/s² on x, y, z at the chip's rate (3053 Hz measured on the reference machine, configured 3200). Batches of ~13-15 KB JSON, about ten a second (`include/belt_stream_client.h#BeltStreamClient`) | Every bulk accelerometer module registers the endpoint, keyed on the chip type (`include/belt_stream_client.h#BeltStreamClient::endpoint_for_chip`) | Any screen not on the Klipper host (no socket; `include/belt_gating.h#BeltGate` `NOT_COLOCATED`), the ESP32 K-Touch, and chips whose module does not register `dump_<type>` (assumed: Beacon, which `src/printer/printer_discovery_parse.cpp` detects as chip `beacon`) |
| "Testing frequency N Hz" console lines | Which integer Hz the sweep generator has reached, once per Hz (`src/calibration/resonance_console.cpp#parse_testing_frequency`). Printed when the move is **planned**, not when it executes | Klipper, Kalico, Creality forks alike | Nobody lacks it; the mock emits it too (`src/api/moonraker_client_mock.cpp`) |
| `[resonance_tester]` config | `min_freq`, `max_freq`, `hz_per_sec`, and (to add) `accel_chip`, `accel_chip_x`, `accel_chip_y` | `configfile.settings`, read by `src/calibration/resonance_console.cpp#query_resonance_tester_config` | Printers without the section cannot run either test at all |
| Klipper CSV | Welch PSD of the whole recording, 1.56 Hz bins (K1C CSVs: 0.0, 1.5, 3.1 ... 199.9), columns `psd_x/y/z/xyz` | Klipper at end of run | n/a; the source of truth |

Which column each consumer reads, and the preview must match: Belt reads `psd_xyz`
(`docs/devel/BELT_TENSION.md`), Input Shaper reads `psd_x` or `psd_y` for its axis
(`docs/devel/INPUT_SHAPER.md`, "Selects psd_x or psd_y").

**Klipper behaviour this design relies on, not vendored in-tree (assumptions, verify on
hardware):**

1. The excitation frequency is a linear ramp in time, `f(t) = f_start + hz_per_sec * (t - t0)`.
2. The console line leads physical motion by the toolhead's lookahead buffer, roughly
   1-2 s, so mapping samples by console arrival time is off by 1-2 Hz at the default
   1 Hz/s. That is the "peaks move by several Hz" risk in the issue, before any estimator
   difference.
3. The resonance tester reads the chip through its own internal client, and a webhooks dump
   client is a second consumer of the same batches. The in-tree claim is
   `docs/devel/BELT_TUNER.md` ("Klipper serves a second client alongside Moonraker") and the
   per-client cumulative counters noted in `include/belt_stream_client.h#AccelBatch`. To
   avoid the dump client ever being the one that starts the chip, the preview attaches on
   the **first "Testing frequency" line**, when the tester's own client already runs.
4. `SHAPER_CALIBRATE` on current Klipper overlays a slow "sweeping" motion (period ~1.2 s).
   Belt sweeps pass `SWEEPING_PERIOD=0` and are pulse-only
   (`src/api/moonraker_advanced_api.cpp#MoonrakerAdvancedAPI::test_belt_resonance`, the
   `TEST_RESONANCES` format string). The overlay sits below 1 Hz plus harmonics and only
   leaks into the lowest few preview points.

## Architecture

Two new units in `src/calibration/`, both shared; neither panel holds DSP code.

**`include/sweep_response_estimator.h` (pure, no LVGL, no threads, no sockets)**

```cpp
namespace helix::calibration {
struct SweepPlan { float freq_start_hz; float freq_end_hz; float hz_per_sec; };
struct SweepPoint {
    float freq_hz;
    float psd_x, psd_y, psd_z;            // per-axis power at freq_hz, PSD-normalised
    bool clean;                           // false if any batch in the step had a gap
    [[nodiscard]] float psd_xyz() const;  // psd_x + psd_y + psd_z, as Klipper's column
};
// Hann-windowed Goertzel power at freq_hz, normalised by window energy and rate so it is
// in (mm/s^2)^2/Hz like the CSV. Exposed for tests and the lock search.
[[nodiscard]] float goertzel_psd(const float* x, size_t n, float freq_hz, float rate_hz);

class SweepResponseAccumulator {
  public:
    explicit SweepResponseAccumulator(SweepPlan plan);
    void note_console_frequency(float hz);                        // search hint only
    std::vector<SweepPoint> add_batch(const AccelBatch& batch, float rate_hz); // newly finished points
    [[nodiscard]] bool locked() const;
    [[nodiscard]] float drive_start_s() const;                   // t0 in stream time
};
}
```

Per-step estimator: **three Goertzels (x, y, z) at the step frequency** over the samples the
ramp model assigns to that 1 Hz step, not an axis projection and not a short FFT. Per-axis
powers let each panel pick the same column it reads from the CSV (xyz for Belt, x or y for
Shaper) with no projection convention to get wrong for `AXIS=1,1` belt sweeps. A
Goertzel is ~3 multiply-adds per sample per axis; a 1 s step at 3053 Hz is ~27k operations,
noise next to the 23 ms the CB1 spends on one 2048-point windowed PSD
(`include/belt_dsp_probe.h#MAX_PSD_MS`). A frequency drifting ±0.5 Hz across a 1 s step
loses under 1 dB of coherence, the same for every step, so the curve's shape survives.

**Drive lock** (inside the accumulator): every 0.25 s, on the last 0.5 s of xyz samples,
Goertzel a grid of `[hint - 3, hint + 0.5]` Hz at 0.25 Hz steps (the hint is the latest
console frequency, which leads the motion) and take the argmax as the current drive
frequency. Each pair gives one estimate `t0 = t - (f - f_start) / hz_per_sec`; the lock
is the running median of those estimates once five agree within 0.15 s. The median is
what survives a resonance hijacking the argmax for a second or two. After lock, step
boundaries come from the ramp alone; the tracker keeps running and drops the lock if
it disagrees with the model by more than 1.5 Hz for 3 s. Cost: ~15 bins x 1500 samples
x 4/s, under 2 ms of CPU a second on a CB1-class core. Assumption 1 is what makes a
single `t0` sufficient.

**`include/live_sweep_preview.h` (owns the stream; the only threaded piece)**

```cpp
class LiveSweepPreview {
  public:
    using PointsCallback = std::function<void(std::vector<SweepPoint>)>; // main thread
    // Pure gate: tier != EMBEDDED, cached_dsp_probe().capable, socket reachable, chip resolved.
    [[nodiscard]] static bool available(const LiveSweepGateInputs& in);
    bool start(const std::string& socket_path, const std::string& accel_chip,
               SweepPlan plan, PointsCallback on_points);            // main thread
    void note_console_frequency(float hz);                           // main thread
    void stop();                                                     // main thread, idempotent
};
// X -> accel_chip_x or accel_chip; Y -> accel_chip_y or accel_chip; a diagonal belt sweep
// -> accel_chip only when no per-axis chips are configured, else nullopt (feature absent).
[[nodiscard]] std::optional<std::string> accel_chip_for_sweep(const ResonanceTesterConfig&, SweepAxis);
```

It wraps the existing `BeltStreamClient` unchanged. That class has no production caller
today beyond `BeltStreamClient::socket_reachable` in
`src/ui/ui_panel_belt_tension.cpp#BeltTensionPanel` (the klippy socket probe), so the
preview becomes its first streaming consumer. Its name stays; a rename is separate work.

## Data flow and threading

```
klippy UDS --> BeltStreamClient (own hv::EventLoopThread)
                 on_batch [loop thread]
                   -> SweepResponseAccumulator::add_batch   (only ever touched on loop thread)
                   -> finished points, if any
                   -> tok.defer(...) to main                 (rule 2: never `if expired` then this)
main thread:  PointsCallback -> panel appends to its preview series -> chart set_data
main thread:  collector progress (freq) -> LiveSweepPreview::note_console_frequency
                 -> std::atomic<float> hint read by the loop thread
```

- Nothing on the loop thread touches LVGL or subjects (`.claude/rules/threading.md` rule 1).
- `stop()` invalidates the lifetime token first, then `BeltStreamClient::stop()` joins the loop,
  so a point already queued is dropped, not delivered to a torn-down panel. Each panel calls
  `stop()` from its cleanup and its destructor.
- No new threads: the stream client already owns its libhv loop (no `std::thread`, #724).
- Console frequency: Belt already receives it (`BeltSweepProgressCallback(percent, freq_hz)`,
  `src/ui/ui_panel_belt_tension.cpp#BeltTensionPanel::on_sweep_progress`). Shaper does not:
  `ShaperProgressCallback` is `(percent, phase)` (`include/advanced_panel_types.h`) though
  `InputShaperCollector::parse_sweep_line` has the frequency in hand. Add `float freq_hz`.
- Range and rate: Belt has `sweep_cfg_` plus `run_window_`; Shaper's collector already runs the
  same config query. Add the three `accel_chip*` keys to `ResonanceTesterConfig` and its
  parser, so one query answers both the range and the chip.
- CPU: the cost on the board is JSON-parsing ~150 KB/s of batches, not the DSP. It is
  unmeasured for a sweep. Klipper is moving the toolhead at the same time, so a starved klippy
  ("Timer too close") is the failure that matters. The gate keeps it off EMBEDDED, and the
  loop thread should run at a positive nice (per-thread `setpriority` on the loop's tid).

## Chart change (required)

`ui_frequency_response_chart` places points **by index**, not by frequency
(`src/ui/ui_frequency_response_chart.cpp#plot_point`, `#update_chart_series`), and the
belt panel works around it by setting the axis to the data's first and last bin
(`src/ui/ui_panel_belt_tension.cpp#BeltTensionPanel::push_chart_data`). A partial series
would stretch across the full width. The fix that keeps index placement: the preview always
pushes the **full grid**, one point per integer Hz from `freq_start` to `freq_end`, with unswept
or unclean steps as NaN, and the chart learns that NaN is a gap:

- `update_chart_series`: non-finite amplitude -> `LV_CHART_POINT_NONE`.
- `stroke_polyline` and the muted/styled draw passes: skip a segment with a non-finite end.
- `downsample_data` is point-picking, so on BASIC (50 points) a 131-point grid loses
  peaks and passes NaN through. Acceptable for a preview; note it in the header.

Cost per update is one `set_data` of at most 200 points once a second, far lighter than the
temperature graph that already runs on every tier (`docs/devel/INPUT_SHAPER.md`, tier table).

## UI behaviour

- **Belt Tension** (RUNNING and TUNING views): one preview series per chart, recoloured to
  the path under test, drawn plain (width 2, no glow, no fill) under the sweep cursor. The
  final curves keep their styled look (width 3, glow, fill) and the before-curves stay
  muted, so the three are distinguishable. Normalised to its own running maximum
  (0-100%), since its units are not Klipper's. The curve rescales as a taller peak arrives,
  which is honest. A caption "Live preview" binds to a new subject. On
  `BeltTensionPanel::on_sweep_complete` the preview series is cleared and hidden, then the
  existing `push_chart_data` draws the CSV curve. No peak markers, no similarity, no notes
  from the preview. EMBEDDED already gets no chart (`BeltTensionPanel::ensure_chart`).
- **Input Shaper**: the MEASURING state (`ui_xml/input_shaper_panel.xml`, `state_measuring`)
  has only a progress bar today. Add a chart host under it (XML), created lazily like
  `InputShaperPanel::create_chart_widgets`, one series in the axis colour, x range = the
  sweep plan. During ANALYZING (the ~11 s FFT on a CB1, longer elsewhere) the preview stays
  frozen with the spinner; that is when it is most worth having. RESULTS shows the existing
  charts from the CSV; the preview chart goes with the state change. "Calibrate all" runs a
  fresh preview for Y.
- Wizard (`ui_xml/wizard_input_shaper.xml`) unchanged in the first cut.

## Failure modes

| Case | Behaviour |
|---|---|
| Socket unreachable (remote screen, K-Touch), EMBEDDED tier, DSP probe not capable | `available()` false; today's cursor-only behaviour, no message |
| No chip for this sweep (split x/y chips on a diagonal, Beacon or unknown endpoint, subscribe error) | Preview absent for this run, one `info` log |
| Lock never achieved (accelerometer on the bed for a bed-slinger Y with a weak drive line, very noisy machine) | Nothing drawn; after 10 s without lock, `debug` log; CSV path unaffected |
| Lock lost mid-sweep | Further points NaN (gap), caption unchanged |
| Overflow or errors in a batch (`AccelBatch::contiguous()` false) | That step is NaN; never a fake value |
| Stream stalls (`BeltStreamClient::STALL_TIMEOUT_MS`) | Preview freezes; the run is still owned by the console collector and its own stall guard |
| Cancel, abort, panel teardown, hot reload | `stop()` from cleanup and destructor; queued points dropped by the token |
| CSV read fails | Preview cleared; the existing classified error shows. A preview is never promoted to a result |
| Preview and CSV disagree | Expected; the swap is the correction (see below) |

**Expected disagreement with Klipper's Welch PSD.** The preview measures response *at the
drive frequency* during that second. Klipper's PSD at a bin also collects ring-down
excited while the drive was nearby and harmonics of lower drive frequencies, averaged over
the run with 1.56 Hz bins. On a clean single mode the peaks should coincide within one CSV
bin: the K1C curves peak at 58.4 Hz (60.0 on one run) with half-power 55.4-63.0 Hz, so ±1.6 Hz
is inside the run-to-run scatter already present. Amplitude ratios between peaks will differ,
and the preview's peaks will read sharper. On the Voron's multi-modal 105-140 Hz hump,
peaks already wander 3-5 Hz run to run (`hw-2026-10-15/turns.log`), so the preview's
positions there mean nothing and must not be labelled.

## Test plan

Pure (tag `[sweep_preview]`, no LVGL):
- `goertzel_psd`: a sine at f returns the expected PSD within 5%; f ± 3 Hz is >20 dB down; zero
  input and n < 2 return 0.
- Accumulator on a synthetic sweep: chirp at 1 Hz/s from 5-135 Hz through a second-order
  mode at 58.4 Hz with the K1C half-power width, noise added, `t0` = 1.7 s, console hints
  leading by 1.5 s. Asserts: locks; `drive_start_s()` within 0.1 s of truth; preview peak
  within 1 Hz of 58.4; exactly one point per integer Hz, emitted once, in order.
- Same with a strong off-drive ring at the mode (hijack): lock still within 0.1 s.
- Belt-path chirp with energy on x and y: `psd_xyz()` equals the sum; per-axis columns differ.
- Sweeping overlay (0.83 Hz, 400 mm/s²) added: lock holds; points above 10 Hz unaffected.
- A batch with `overflows=1`: that step `clean=false`; later steps unaffected.
- Shape check against a real CSV: synthesize the drive response from a K1C CSV's
  `psd_xyz`, run it through the accumulator, and require `band_similarity`
  (`include/belt_tension_types.h#band_similarity`, the same Pearson the panel shows) of
  at least 90% against the CSV. This reuses the comparator; it does not fork one.
- `accel_chip_for_sweep` and `LiveSweepPreview::available`: table tests over every input.
- `parse_resonance_tester_config` reads the three chip keys.

Chart (`tests/unit/test_frequency_response_chart.cpp`): NaN becomes `LV_CHART_POINT_NONE`; the
styled and muted draw passes do not stroke across a NaN; downsample passes NaN through.

Session: a fake source drives `on_batch` from a libhv loop; `stop()` while batches are arriving
delivers no callback afterwards. ASAN on the test host once at the end; `make mutate-diff`
once per branch.

Mock and hardware: a mock `AccelBatch` source synthesizing the chirp from the mock's own
curve, gated on `RuntimeConfig::should_mock_*`, so `--test` shows the preview for UI work.
Hardware verify (hw-verify label): K1C and Voron, both panels; record klippy CPU and look
for "Timer too close" during a sweep with the preview on vs off on a CB1.

## Open questions for Preston

1. **Input Shaper MEASURING view gains a chart?** Recommended: yes, a single preview series
   above the progress bar in the panel; the wizard keeps its progress bar for now.
2. **Preview styling.** Recommended: plain 2 px line in the path or axis colour with a
   "Live preview" caption, cleared the instant the CSV curve lands, no transition animation.
3. **Which boards get it.** Recommended: BASIC and STANDARD tiers that pass the existing DSP
   probe; off on EMBEDDED (AD5M class), where klippy shares a weak CPU with us during the
   sweep and the belt panel already has no chart.
4. **When the lock fails, fall back to console-time mapping?** Recommended: no. Show no
   preview rather than one shifted by the 1-2 s lookahead lead; the CSV arrives either way.

## Rulings (Preston, 2026-10-15 17:50)
1. Input Shaper: live chart in the panel AND the wizard.
2. Preview look: drawn exactly like the final curve (fill and all), so the swap to the CSV
   curve is barely visible. No "Live preview" caption is required; the final swap is instant.
3. Boards: every capable board (Klipper host, UDS reachable). No tier gate. Measure the CB1
   CPU cost during implementation anyway and report it.
4. Timing: lock from the accelerometer data; if that fails, show no preview (normal progress
   view). No console-timing fallback.
