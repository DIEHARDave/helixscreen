# Belt Tension (resonance comparison)

Developer guide for the belt tension check: two Klipper resonance sweeps, one per CoreXY
diagonal, compared as curves. The predecessor tool (hand-plucked belts on a live
accelerometer stream) is `BELT_TUNER.md`.

**User guide**: `../user/guide/calibration.md` § Belt Tension

---

## Overview

The check runs Klipper's `TEST_RESONANCES` down each belt path, reads the CSV Klipper
writes, and overlays both frequency responses. The two curves are compared the way
Shake&Tune compares belts: a band-limited Pearson similarity as the headline verdict, plus
each path's peaks paired by frequency with unpaired peaks flagged. A re-test sweeps one
path and keeps the other, ghosting the replaced curve.

Path A is the `AXIS=1,-1` diagonal and Path B the `AXIS=1,1` one. The letters follow the
Voron motor names Shake&Tune uses: a 1,1 move turns only `stepper_x`, which on a Voron is
the B motor, so a user comparing this screen with Shake&Tune or the Voron documentation
sees the same letters on the same belts.

Start is gated, in `src/calibration/belt_gating.cpp#evaluate_belt_gate`:

| Gate leg | Meaning |
|----------|---------|
| `NOT_CONNECTED` | no Moonraker, or klippy not READY (a dead klippy refuses gcode, so a sweep would only stall) |
| `NO_ACCELEROMETER` | no accelerometer in the configfile config |
| `NOT_COREXY` | kinematics is not `corexy`; the two diagonals the comparison needs exist only there |
| `NOT_COLOCATED` | Klipper's unix socket is not reachable: HelixScreen is not running on the printer's own computer, so it cannot read the CSV |
| `PRINTING` | a print is active; the toolhead is not free to move |

The panel computes the gate in one place, `src/ui/ui_panel_belt_tension.cpp#refresh_gate`,
plus a calibrator-idle leg; `bt_can_start` binds the Start button's disabled state, so the
gate holds however the panel was reached (row click, `ctl navigate`, a demo entry, an
accelerometer that drops out after entry).

Start and Re-test also pass through `BeltTensionPanel::run_after_ram_check`: on a host with
less than `RESONANCE_LOW_RAM_WARN_MB` (200 MB) it shows input shaper's shared
`show_low_ram_resonance_warning` first. Klipper analyses each sweep on the printer's own
host, which is this one (the co-location gate), and on a 128 MB board that analysis can be
OOM-killed, leaving klippy wedged until it is restarted.

Z belts are deliberately unsupported; see the header comment on
`include/belt_tension_types.h` for the measurement that closed that door.

## Key files

| File | Role |
|------|------|
| `src/ui/ui_panel_belt_tension.cpp`, `include/ui_panel_belt_tension.h`, `ui_xml/panel_belt_tension.xml` | The panel: five view states (START, RUNNING, RESULTS, ERROR, TUNING), subjects, gate wiring, run queue, re-test and ghost bookkeeping |
| `src/ui/ui_belt_path_sketch.cpp` | The START-screen sketch widget drawing the two diagonals |
| `src/calibration/belt_tune_session.cpp`, `include/belt_tune_session.h` | `BeltTuneSession`: the tuning loop's pure logic (zone choice, peak refinement, advice, round outcomes, rate learning) and the `belt_tune` thresholds |
| `src/ui/ui_belt_gap_gauge.cpp`, `include/ui_belt_gap_gauge.h` | The `<belt_gap_gauge>` widget and its pure helpers `belt_gap_gauge_x` / `belt_gap_severity` |
| `src/printer/printer_detector.cpp#get_belt_tensioner` | The `belt_tensioner` printer-database field as a `BeltTensionerKind` |
| `src/calibration/belt_tension_calibrator.cpp` | Measures one path per call: home if needed, sweep, cancel, `emergency_abort()` |
| `src/calibration/belt_tension_types.cpp` | `compare_belt_paths` (band similarity, paired peaks) with `band_similarity` / `detect_belt_peaks` / `pair_belt_peaks` / `verdict_for_similarity` |
| `src/calibration/resonance_console.cpp` | `[resonance_tester]` config query and console-line parsing shared with input shaper |
| `src/calibration/shaper_csv_parser.cpp` | `parse_resonance_csv`: reads the CSV, returns `(freq, psd_xyz)` pairs or a classified error |
| `src/api/moonraker_advanced_api.cpp` | `BeltResonanceCollector` (console follow + CSV read) and `test_belt_resonance` (entry point); `detect_belt_hardware` for kinematics |
| `src/ui/ui_frequency_response_chart.cpp` | Shared chart: series styles, muted ghost series, sweep cursor, numbered and hollow markers, percent gridline labels |
| `src/calibration/belt_gating.cpp` | The pure Start-gate predicate |
| `src/api/moonraker_client_mock.cpp` | `dispatch_test_resonances_response`: the `--test` simulation of a sweep |

## Flow

```
BeltTensionPanel (LVGL thread)
  |  handle_start_clicked / handle_retest_clicked
  v
BeltTensionCalibrator::measure_path(path)              belt_tension_calibrator.cpp
  |  ensure_homed_then(...)                            toolhead_homing.h
  v
IAdvancedAPI::test_belt_resonance(axis, name, ...)     moonraker_advanced_api.cpp
  |  1. BeltResonanceCollector::start()                (console subscription)
  |  2. query_resonance_tester_config(client)          resonance_console.cpp
  |     -> collector->set_config(cfg)  [progress hold]
  |  3. execute_gcode TEST_RESONANCES AXIS=<1,-1|1,1>
  |                      OUTPUT=resonances NAME=helix_belt_{a,b} SWEEPING_PERIOD=0
  v
Klipper
  |  "Testing frequency %.0f Hz"          -> on_progress (percent + cursor)
  |  "Resonances data written to <path>"  -> read_result(path)
  v
parse_resonance_csv(path)                              shaper_csv_parser.cpp
  |  (freq, psd_xyz) pairs | classified error
  v  on_complete / on_error  (marshalled to the LVGL thread by lifetime_.bg_cb)
BeltTensionPanel::on_sweep_complete -> next path, then finish_run()
  |
  v
compare_belt_paths(curve_a, curve_b, sweep min, sweep max)  belt_tension_types.cpp
  -> BeltComparison -> populate_results(): similarity, verdict chip, peak facts, markers
```

Two threading rules shape the middle of that flow. Every collector callback arrives on the
WebSocket thread; the calibrator wraps each one in `lifetime_.bg_cb(...)` so the panel only
ever hears on the LVGL thread, and a run cancelled mid-flight expires the tokens so late
callbacks die at the guard. And the sweep's G-code rides the long calibration timeout with
`caller_surfaces_errors=true`; a transport loss is ignored (Klipper keeps sweeping and the
console lines keep arriving), everything else completes the collector with an error.

Progress needs the printer's own range: `sweep_percent` scales the current "Testing
frequency" line against the `[resonance_tester]` config queried alongside the sweep, and
the collector holds progress lines until `set_config` lands so the first percent is not
computed against the 5-135 Hz default when the printer sweeps something else. The config
query always answers (defaults on error), so the hold is finite.

The panel owns stall detection: `BeltTensionPanel::STALL_TIMEOUT_MS` (120 s, an
`OperationTimeoutGuard`) fires when no progress line arrived, because Klipper prints one
every second. `BeltResonanceCollector` carries no deadline of its own. The stall message
names the likeliest cause on a small board, Klipper's analysis running out of memory, and
tells the user to power-cycle (or restart Klipper over SSH): an OOM-killed analysis leaves
klippy wedged, the Moonraker firmware-restart RPC cannot reach it, and on the CC1 and AD5M
Moonraker's service manager does not list Klipper at all.

Start refuses outright when `MemAvailable + SwapFree` is under
`RESONANCE_MIN_HEADROOM_MB` (64 MB), and asks first on a board under 200 MB total
(`include/memory_utils.h#resonance_memory_check`, shared with input shaper). On the CC1
the OOM killer takes HelixScreen before the analysis, so the stall error never shows
there: the check has to happen before Start.

**Stop** is not a pause. A sweep cannot be resumed, so `handle_stop_clicked` calls
`BeltTensionCalibrator::emergency_abort`, which is
`include/calibration_abort.h#emergency_stop_and_restart`: M112, then FIRMWARE_RESTART to
clear the movement state the aborted sweep left behind.

## Klipper facts the implementation leans on

- `TEST_RESONANCES AXIS=1,-1` and `AXIS=1,1` excite the two CoreXY diagonals. Which
  physical belt each diagonal loads depends on the printer's geometry, which is why the UI
  reports paths A and B, never belt names.
- `SWEEPING_PERIOD=0` forces the pulse-only excitation. Mainline Klipper's
  `[resonance_tester]` defaults to the sweeping test (1.2 s), Kalico's to pulse-only, and
  firmware older than the sweeping test has no such parameter and ignores it. Shake&Tune's
  own guidance is that the sweep smooths over mechanical faults (loose belts, binding), which
  is what a belt comparison looks for.
- `OUTPUT=resonances` makes Klipper write `resonances_<axis>_<name>.csv`, usually in `/tmp`
  (the Snapmaker U1 writes to `/userdata/gcodes/shaper_calibrate/`). Only the path
  announced on the console in THIS run is read, so a stale file from an earlier run cannot
  be mistaken for a result (`BeltResonanceCollector::on_gcode_response`).
- The CSV's `psd_xyz` column is the combined response. Per-chip columns instead of
  `psd_xyz` mean more than one accelerometer reported; that is `ResonanceCsvError::MULTI_CHIP`,
  surfaced as "More than one accelerometer reported. Belt Tension supports one."
- `TEST_RESONANCES` cannot sweep Z at all, and a toolhead accelerometer barely sees a Z
  belt; see `include/belt_tension_types.h`.
- Klipper defaults for `[resonance_tester]` are 5-135 Hz at 1 Hz/s: two sweeps of about
  130 s each. The START screen shows the printer's actual range as
  "N-M Hz · about K min" from `query_hw_facts`.

## Verdicts, and why provisional

`include/belt_tension_types.h` (`belt_verdict` namespace). Every calculation uses only
bins inside the printer's `[resonance_tester]` sweep range: Klipper and Kalico write bins
past the sweep ceiling, and those carry no excitation.

| Verdict | Condition |
|---------|-----------|
| MATCHED ("Good match") | similarity >= 90% |
| CLOSE ("Fair match") | similarity >= 75% |
| ADJUST ("Poor match") | anything worse |

Similarity is the Pearson correlation of the two in-band PSDs (B interpolated onto A's
bins) x100, clamped to 0..100. Alongside it the comparison pairs local maxima by
frequency, Shake&Tune's `_pair_peaks`: a pair forms when two peaks sit within
min(median + 1.5 * IQR of all peak distances, 10 Hz), greedily closest-first, and a peak
with no partner is reported unpaired rather than differenced against the other path's
tallest peak: the two tallest peaks can be different modes entirely (a frame mode on one
path, a belt hump on the other).

The comparison is invalid when either curve has fewer than three in-band bins. The
constants are provisional (prestonbrown/helixscreen#1737). Six printers captured so far sort
plausibly under them (fixtures in `tests/fixtures/belt_sweeps/`, named by axis):

| Printer | Band (Hz) | Similarity | Verdict |
|---------|-----------|-----------:|---------|
| AD5X | 20-80 | 99.5% | Good match |
| K1C | 5-133.3 | 98.9% | Good match |
| AD5M | 5-100 | 97.4% | Good match |
| K2 Plus | 20-120 | 92.2% | Good match |
| U1 | 5-100 | 81.7% | Fair match (main pair 3 Hz apart) |
| Voron 2.4 (Kalico) | 5-135 | 48.3% | Poor match (uneven belts, gantry due for re-racking) |

A rerun of the same machine moves the number: the AD5M read 98.4% and the Voron 43.7% on a
second capture, so a cutoff a few points from a printer's reading can flip its verdict.

Belt modes sit where the machine puts them (40-60 Hz on the small factory printers,
110-130 Hz on the Voron), so no fixed "belt band" would work across the fleet. No capture
showed anything above 150 Hz stronger than 2% of its peak.

The panel shows no direction ("A tighter" / "B tighter"): a signed difference between two
single peaks read the Voron's 36 Hz frame mode against its 120 Hz belt hump. Direction
returns only if captures show a dependable signal (#1721).

Shake&Tune reports a higher similarity for the same printer (71% against 48% on the Voron):
it correlates 0-200 Hz, where both curves are nearly flat and so agree, while this check
correlates only the swept band.

## The RESULTS screen

- Centre: `bt_similarity` ("48%") and the verdict chip.
- Sides: the path labels with their `bt_note_a` / `bt_note_b` status ("just now", "sweeping",
  "N min ago"); after a single-path re-test the note carries the similarity the comparison
  showed before (" · was 48%").
- Under the chart: `bt_facts`, up to `MAX_LISTED_PEAKS` pairs strongest first as A/B
  ("Peaks 36/35 · 132/133 Hz"), and `bt_unpaired` ("Only on B: 120, 129 Hz"), hidden by
  `bt_has_unpaired` when empty.
- Chart: the listed pairs carry the same number on both curves; the listed unpaired peaks
  are hollow rings. Both curves (and the re-test ghosts) are trimmed to the sweep band and
  scaled by the tallest in-band point on screen, so the y axis reads 0-100%.

## Tuning loop

The RESULTS screen offers **Tune belts** (**Fine-tune** when the verdict is already MATCHED). The button opens the TUNING view
(`BeltTensionPanel::handle_tune_clicked`), which walks the user through adjusting one belt at
a time and re-measuring only the part of the spectrum that moves. The logic is
`BeltTuneSession` (`include/belt_tune_session.h`), which is pure (no LVGL, no I/O): the panel
feeds it the round-0 comparison and each round's two curves and renders what it says.

A higher peak means a tighter belt, and clockwise tightens. The session's gap is always
`adjusted - reference` within one round, so drift between rounds cancels; negative means the
adjusted belt is looser. The advice follows from the gap's sign: a negative gap says tighten
the adjusted path, a positive one loosen it.

### Round flow

```
TunePhase::ADJUST     user turns the tensioner (SCREW: sets the amount on the stepper)
  | Re-measure         handle_tune_measure_clicked: gate re-check, record_turns(), start_tune_round()
  v
TunePhase::SWEEPING   begin_run({A, B}, window): TEST_RESONANCES ... FREQ_START=<lo> FREQ_END=<hi>
  |                   once per path, over the zone's window only
  v
finish_tune_round     BeltTuneSession::add_round(a_window, b_window) -> BeltTuneRound.outcome
  v
ADJUST | MATCHED      MATCHED when the round's outcome is MATCHED
```

Both paths are swept over the same window each round, so the reference peak is re-measured
alongside the adjusted one. Gaps never compare a fresh peak to a stale one. A windowed sweep is
a fraction of the full band, so a round takes roughly `2 * window / hz_per_sec` (the
Re-measure button shows the estimate). Klipper and Kalico both write the CSV across the whole
configured range, but bins outside the window carry no excitation. `unexcited_floor_psd`
reads exactly those bins as the belt's unexcited floor.

Windowing needs `FREQ_START` / `FREQ_END` on `TEST_RESONANCES`. Hardware confirmation that
each firmware honours them is pending (see Thresholds).

**Next zone** and **Confirm** are the other two exits. Confirm runs a full two-path check
(`handle_tune_confirm_clicked`) with the pre-tuning curves ghosted, and the facts line
carries " · Tuned in N rounds" plus, on SCREW printers, the net turns per path
(`BeltTuneSession::total_turns`).

Start is gated at every sweep, not only at Start: `gate_allows_sweep` re-checks the Start
gate on **Re-measure**, **Next zone** and **Confirm**, so a print that began, a klippy that
dropped or an accelerometer that vanished mid-tuning refuses the sweep with the gate's own
message. `begin_run` checks it again, and so does Continue on the low-RAM warning, so a print
that starts while that warning is up still refuses the sweep. On a low-RAM board the warning
is asked once per tuning session; later rounds and Confirm skip it. When a sweep ends the
gate is recomputed, since a gate subject that changed mid-sweep read the calibrator as busy.

The screensaver does not end a tuning session. A `Suspended` deactivation keeps the view, the
run and the session, and the wake's `on_activate` refreshes the gate and the subjects without
returning to START or re-detecting hardware. A confirmed **Stop** acts only while a sweep is
still running; a short round can finish while the dialog is up.

**Next zone** sweeps at once (a baseline round over the next zone's window), so its button
shows that round's duration the way Re-measure does (`peek_next_zone`), except on compact
layouts.

A round recorded with 0 turns on a SCREW printer is a repeatability check (`no_turn`): its
line reads "No turn: the gap moved N Hz" instead of a verdict on a turn, and it never counts
toward the stall. A wake also re-asks what the suspension expired: when no sweep is running
it re-probes the klippy socket and the sweep range, and re-runs hardware detection if it had
not answered. The low-RAM warning closes on any deactivation, since its answer is tied to
the panel's lifetime.

### Zone choice

`pick_tune_zone` turns the round-0 `BeltComparison` into a window (`BeltTuneZone`) around the
peaks to close. Peaks under `ZONE_MIN_AMPLITUDE_FRACTION` (20%) of the stronger path's
in-band maximum never define a zone. Pairs no more than `MATCHED_MIN_HZ` (1 Hz) apart, or
centred within `ZONE_HALF_MIN_HZ` of an already matched zone's centre, are skipped.

Two sources, tried in this order:

| `BeltZoneSource` | Condition |
|------------------|-----------|
| `TALLEST_UNPAIRED` | Either path's tallest peak is unpaired and above the floor (`tallest_peak_unpaired`). The taller of the two steers against the other path's nearest floor-eligible peak, provided it sits within `ZONE_FALLBACK_MAX_HZ` (30 Hz, three times the pairing ceiling) |
| `PAIRED` | Otherwise, or when no partner is that close: the widest remaining pair |

The window is the zone's midpoint +- max(`ZONE_HALF_MIN_HZ`, gap / 2 + `ZONE_MARGIN_HZ`),
clamped to the sweep band.

The shape-warning hint is decoupled from the source. `BeltTuneSession` latches
`tallest_peak_unpaired(round0)` at construction (`shape_warning_`), so the "Curves differ in
shape" hint shows whenever round 0 had an unpaired tallest peak, even if the zone ended up
`PAIRED` because no partner sat within 30 Hz.

The adjusted path starts as the zone's looser one (the lower peak). **Adjust X instead**
(`swap_adjusted_path`) swaps reference and adjusted for this and later zones; the learned
rate and this zone's rounds belong to the other belt, so they clear. `next_zone` keeps a
swapped path; without a swap each new zone adjusts its own looser path.

### Peak refinement

`refine_peak_in_window` finds a path's peak inside the window and refines it between bins by
parabolic interpolation. Candidates are interior local maxima that reach
`NEAR_PEAK_MIN_FRACTION` (50%) of the window's maximum and, when a floor is given,
`PEAK_MIN_PROMINENCE` (1.5x) of it. With several candidates the one nearest the previous
peak wins (so a tall neighbour cannot steal the track), else the tallest.

The floor is `unexcited_floor_psd`: the median of the curve's in-band bins outside the window
(at least `UNEXCITED_MIN_BINS` of them). A windowed sweep drives only its window, so those
bins read what the belt does when nothing excites it. `window_holds_peak` applies the same
1.5x rule to the whole window, falling back to the window's own minimum when there are too few
outside bins. A window failing it is `NO_PEAK`.

### Outcomes

`BeltTuneSession::add_round` classifies each round by the gap's change, never the adjusted
peak's own move. Only the adjusted belt is turned, so drift that moves both peaks together
cancels out of the gap, and a reference that moved (the wrong belt was turned) reads as the
gap moving. The panel's message (`BeltTensionPanel::tune_last_message`) follows from the
outcome:

| `BeltTuneOutcome` | Condition | Message |
|-------------------|-----------|---------|
| `MATCHED` | `|gap|` within the matched tolerance (`matched_tolerance_hz`: the curve's mean bin spacing, at least 1 Hz) | Matched at N Hz |
| `CLOSER` | the gap shrank and kept its sign | "last turn: N Hz closer" |
| `OVERSHOT` | the gap changed sign | "Went past X by N Hz. Back it off a little." |
| `WRONG_WAY` | the gap moved against the advice by `MIN_MOVE_HZ` (0.5 Hz) or more | "The gap grew after tightening X. Check you turned Path X's tensioner." (mirrored for loosening) |
| `NO_MOVEMENT` | the gap moved under `MIN_MOVE_HZ` | "The gap didn't close (moved N Hz). Try a bigger turn." |
| `NO_PEAK` | a path's window holds no clear peak (`window_holds_peak`), or a peak is still past the edge after one widening. `reference_lost` marks the case where only the reference lost its peak | "No clear peak in this zone", or with `reference_lost` "Path R's peak left the zone. Check that only Path X's tensioner was turned." |
| `OUT_OF_WINDOW` | the adjusted peak sits on a window edge; the window re-centres on the reference and widens, once per zone until a peak is measured again, then `NO_PEAK` | "X's peak moved out of the zone. Widening it." |
| `BASELINE` | the first measurement after `next_zone`: no gap history yet, so no outcome against it | (none; the zone's gap shows) |

`NO_PEAK` and `OUT_OF_WINDOW` carry no gap: nothing is measured, the previous gap stays. A
zone's first round after `next_zone` is always a baseline round (`needs_baseline`): the earlier
zone's turns moved this zone's peaks, so the panel measures before advising, and it is
`MATCHED` if the baseline already reads within tolerance.

### Rate learning and advice

On SCREW printers each measured round that follows recorded turns yields a sample,
`|gap move| / turns`, in Hz per turn. `hz_per_turn` is the median of the samples, and the
advised amount is `|gap| / rate`, rounded to `TURN_STEP` (an eighth) and clamped to
`TURN_STEP .. TURN_MAX` (one full turn). A sample is rejected unless the move and the turns
behind it come from the same zone. They always turned one way: only a measured round can
flip the advised direction, and it clears the tally.

- `WRONG_WAY`, `NO_PEAK`, `OUT_OF_WINDOW` and `BASELINE` rounds teach nothing: the first says
  the turn went elsewhere, the others measured no move.
- Turns made in a zone are cleared from the unmeasured tally when the zone or the adjusted
  belt changes, so no sample spans zones.

Without a rate the first advised turn is `FIRST_TURN` (a quarter); after a `NO_MOVEMENT`
round it doubles the turn that did nothing. The user sets the amount actually turned on a
stepper, from 0 to one turn; `record_turns` stores it for the next round, defaulting to the
advice. Zero books no turn: a re-measure to check repeatability doubles nothing and adds
nothing to the Confirm facts. The panel
formats amounts with `format_belt_turns` (the text fonts carry the eighth-fraction glyphs).

### Tensioner kinds

`BeltTensionerKind` comes from the printer database's `belt_tensioner` field
(`PrinterDetector::get_belt_tensioner`, keyed on the detected printer name):

| Kind | Printers | Advice |
|------|----------|--------|
| `SCREW` | Voron 2.4 and Trident (`belt_tensioner: "screw"` in `assets/config/printer_database.json`) | Direction plus an amount in turns, the stepper, the learned Hz per turn |
| `SPRING` | none in the database yet | Direction only: "Release X's tensioner, let the spring settle, lock it". Two rounds without improvement (`SPRING_STALL_ROUNDS`) show the mechanical hint |
| `UNKNOWN` | everything else | Direction only, "Tighten X" / "Loosen X" |

SCREW printers show the mechanical hint after `STALL_ROUNDS` (8) unmatched rounds in a zone.
Add `"belt_tensioner": "screw"` to a database entry only once the printer is known to have a
screw tensioner with a repeatable relation between turns and frequency.

### Gap gauge

`<belt_gap_gauge>` (`ui_belt_gap_gauge_set`) is a horizontal track for the signed gap. It draws
a matched band of +- tolerance around the centre, a filled needle at this round's gap, a
hollow needle at the previous round's and an arrow between them, and no text. The track's
half-span is `tune_gauge_range_hz_`, set at the zone's first measurement
(`reset_tune_gauge_range`) to the larger of `2 * GAUGE_WARN_HZ` and 1.25 x the first gap, so
the needle stays on the track while it closes. `belt_gap_gauge_x` maps a gap to a pixel
(centred at zero, clamped to the ends); `belt_gap_severity` classifies it as `MATCHED`,
`NEAR` (up to `GAUGE_WARN_HZ`, 3 Hz) or `FAR`, and the panel maps that to colour through
`bt_tune_severity`.

### Compact layouts

Below the medium breakpoint, or in portrait (`ui_breakpoint le 2 or ui_is_portrait eq 1` in
`ui_xml/panel_belt_tension.xml`), the TUNING view and RESULTS screen shrink to fit: the hero
becomes one line (the instruction, the amount and a thin `bt_tune_line_gauge`; tapping it
adjusts the other path), the two Re-test buttons on RESULTS fold into one menu, and Confirm
drops its duration when the button row holds three buttons or the layout is compact.
Re-measure and Next zone drop theirs on compact layouts. The wide layout and the compact one are the same subjects
bound to different widgets, so `refresh_tune` feeds both.

Portrait adds two rules of its own, because there width is scarce and height spare. The
button rows wrap: the turn stepper, Tune belts and Confirm each take a line, so the other
buttons keep room for their labels in every locale. The charts grow into the spare height
only up to `belt_chart_max_h` (about the content width of each tier), so a tall screen
leaves space under the controls rather than drawing a chart taller than it is wide. START
stacks the sketch over the text instead of setting them side by side.

### Ultrawide layout

On ultrawide screens (`ui_is_ultrawide eq 1`, wider than 2.5:1) the compact conditions give
way: those screens read a small or medium tier by their short axis, but the problem there is
width, not room. `overlay_content` becomes a row. While a state shows a chart (RUNNING,
RESULTS, TUNING) the chart sits in `chart_host_wide` on the left and grows, and `bt_main`,
holding everything else, becomes a sidebar `belt_sidebar_w` wide (640 small, 720 medium, 800
large, 960 xlarge, 1280 xxlarge, measured so the button labels fit in en and ru). The sidebar
carries the full tuning hero and the wide button variants, the RESULTS hero row and all four
RESULTS buttons, and the run status with Stop. START keeps the full width: the sketch grows
on the left and the text and Start take the sidebar width. The chart is a C++ widget, so
`BeltTensionPanel::chart_host_for` picks the wide host and an observer on `ui_is_ultrawide`
moves it when a rotation crosses the line. On the EMBEDDED tier, which has no chart, the
panel keeps the full width.

### Mock script

`HELIX_MOCK_BELT_TUNE_SCRIPT="B:102.1,107.6,109.8"` scripts one path's peak across successive
windowed sweeps, so the loop can be driven without a printer. Full reference:
`MOCK_ENVIRONMENT_VARIABLES.md`. Windowed sweeps never apply the re-measure walk that full
sweeps do, and an item `flat` writes a curve with no peak to reach `NO_PEAK`.

### Thresholds (provisional)

All in `belt_tune` (`include/belt_tune_session.h`). They are reasoned from the six captures
above and a synthetic model, and are **pending hardware verification** (prestonbrown/helixscreen#1736):
none has been exercised against a real windowed sweep on a real printer.

| Constant | Value | Role |
|----------|-------|------|
| `ZONE_MIN_AMPLITUDE_FRACTION` | 0.20 | peaks below this fraction of the in-band maximum never define a zone |
| `ZONE_HALF_MIN_HZ` / `ZONE_MARGIN_HZ` | 10 / 5 Hz | narrowest window half-width / reach beyond the zone's peaks |
| `ZONE_FALLBACK_MAX_HZ` | 30 Hz | how close the other path's peak must be to steer an unpaired tallest peak |
| `MATCHED_MIN_HZ` | 1 Hz | matched tolerance floor (the real tolerance is also at least the mean bin spacing) |
| `MIN_MOVE_HZ` | 0.5 Hz | smaller moves of the gap are noise |
| `FIRST_TURN` / `TURN_STEP` / `TURN_MAX` | 1/4 / 1/8 / 1 turn | SCREW amounts |
| `STALL_ROUNDS` / `SPRING_STALL_ROUNDS` | 8 / 2 | unmatched rounds before the mechanical hint (screw / spring) |
| `PEAK_MIN_PROMINENCE` | 1.5x | a peak must stand this far over the unexcited floor |
| `UNEXCITED_MIN_BINS` | 8 | in-band bins outside the window needed to measure the floor |
| `NEAR_PEAK_MIN_FRACTION` | 0.5 | a maximum competes for "nearest the previous peak" at this fraction of the window's maximum |
| `GAUGE_WARN_HZ` | 3 Hz | gap gauge: warning colour up to here |

Open hardware questions: that `FREQ_START` / `FREQ_END` take effect on Kalico and mainline
(the first "Testing frequency" line should equal the window start); that a quarter turn moves
the peak at least `MIN_MOVE_HZ`; and that a broad hump (119-133 Hz on the Voron) moves as one
piece under refinement.

## Mock knobs

The mock simulates the whole console sequence: "Testing frequency" lines, the terminal
written-file line, and a CSV with a Lorentzian peak over a noise floor at the requested
frequency (`dispatch_test_resonances_response` in `src/api/moonraker_client_mock.cpp`,
`write_mock_belt_csv` for the file). Full reference: `MOCK_ENVIRONMENT_VARIABLES.md`,
one `###` entry per variable.

| Variable | Effect |
|----------|--------|
| `HELIX_MOCK_BELT_A_HZ` / `HELIX_MOCK_BELT_B_HZ` | The two paths' simulated peaks. Defaults 110/98: 12 Hz apart, deliberately in "Poor match" territory. Re-measuring one path walks its peak toward the other's by up to 4 Hz per run, so the re-test loop converges the way cranking a tensioner does |
| `HELIX_MOCK_BELT_FAIL` | `stall` (no progress lines), `nofile` (CSV missing), `multichip` (per-chip columns), `error` (adxl error line), `kalico` (dialect the parser rejects) |
| `HELIX_MOCK_KINEMATICS` | Overrides the kinematics the mock reports; `cartesian` on the default Voron persona exercises the NOT_COREXY gate leg |
| `HELIX_MOCK_BELT_CSV_A` / `HELIX_MOCK_BELT_CSV_B` | Replay a real capture as that path's result instead of the synthetic curve (e.g. the fixtures in `tests/fixtures/belt_sweeps/`) |
| `HELIX_MOCK_BELT_RANGE` | `<min>-<max>`: the `[resonance_tester]` sweep range the mock reports and sweeps, to match a replayed capture's band |

## Tests

| File | Covers |
|------|--------|
| `tests/unit/test_belt_tension_calibrator.cpp` | The calibrator: homing order, sweep lifecycle, cancel-mid-run generation guard, emergency abort |
| `tests/unit/test_belt_tension_panel_states.cpp` | Panel view states, gate-to-button binding, similarity/facts/markers, ghost series on re-test, low-RAM warning |
| `tests/unit/test_belt_compare.cpp` | `compare_belt_paths` / `pair_belt_peaks` / `band_similarity` against six printers' real captures (`tests/fixtures/belt_sweeps/`) and synthetic curves |
| `tests/unit/test_belt_gating.cpp` | Every gate leg of `evaluate_belt_gate` |
| `tests/unit/test_belt_tune_session.cpp` | `BeltTuneSession`: zone choice and fallback, peak refinement, every outcome, rate learning, tensioner kinds |
| `tests/unit/test_belt_gap_gauge.cpp` | `belt_gap_gauge_x`, `belt_gap_severity` and the widget's set/get |
| `tests/unit/test_mock_test_resonances.cpp` | The mock's sweep simulation, including each `HELIX_MOCK_BELT_FAIL` mode |
| `tests/unit/test_frequency_response_chart_style.cpp` (`[belt]` cases) | Series styling, muted ghost, sweep cursor, markers |

Run them with `make t F='[belt]'` (the `[belt]` tag also collects the pluck-tuner units
kept from `BELT_TUNER.md`; the panel and calibrator suites additionally carry
`[belt_tension]`).
