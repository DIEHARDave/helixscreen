# Belt Tension Tuning Loop Revision Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. This project's Verification Cadence (below, and `CLAUDE.md`) overrides those skills' per-task review and verification steps.

**Goal:** Finish the guided belt-tuning loop's revision after the Voron and K1C hardware sessions: analyse in-plane curves, notice belts that already match, choose the zone with a calibration turn, open every zone with a narrow baseline that stays inside the printer's sweep range, steer on an in-plane centroid gap, take bigger turns, drop implausible readings, and flag a path that resonates far harder than the other.

**Architecture:** The pure pieces come first (`BeltResponse` and the in-plane zone functions in `belt_tune_session.h`), then the sweep path carries a `BeltResponse` from the CSV parser to the panel, then the session's stage machine (ALREADY_MATCHED, CALIBRATE, BASELINE, TUNE, NO_ZONE), then the mock's scripted calibration, then the panel's run kinds, calibration and already-matched phases.

**Tech Stack:** C++17, LVGL 9.5 + helix-xml, Catch2, the Moonraker mock (`--test`).

**Spec:** `docs/devel/plans/2026-10-15-belt-tune-loop-revision-design.md` (binding, including §2a; it wins over the original design). Preston ruled on 2026-10-18: build the full design (calibration turn, in-plane centroid tracker, narrow baseline with `ZONE_SWEEP_MARGIN_HZ`, turn sizes, the 3x power hint, already-matched entry). Context: `docs/devel/plans/2026-10-09-belt-tune-loop-design.md`, `docs/devel/BELT_TENSION.md`. Hardware data: `.superpowers/sdd/2026-10-13-belt-tune-loop-plans/` (`hardware-2026-10-14.md`, `hw-2026-10-15/turns.log`, `hw-2026-10-15/narrow/`, `hw-2026-10-15/k1c/`).

This plan states interfaces, tests and constraints; bodies are the implementer's. Every header block below was compiled on 2026-10-18 against HEAD `b3a6061b7`: a scratch copy of the tree outside the worktree with these declarations in `include/`, and a probe translation unit calling every new or changed signature (the panel's privates through the `BeltPanelFixture` friend), through `scripts/syntax_check.py`. A deliberate typo in `belt_tune_session.h` and one in `ui_panel_belt_tension.h` each turned it red; the scratch copy is deleted.

## Status at HEAD `b3a6061b7`

Done (base `460d3d487`; reports `settle-report.md` and `s2-report.md` in the SDD workspace). Nothing below re-does them:

| Old task | Commits | What landed |
|---|---|---|
| 3 `BeltSettle` + probe point | `5cd656f7a`, `94f841ceb` | `include/belt_settle.h`, `plan_belt_settle()`; `ResonanceTesterConfig::probe_point`; `BeltSettleInputs::tool_count` (multi-tool inset 25%, fed from `ToolState::has_multiple_toolheads()`, which now asks `physical_heads()`); no `NOT_HOMED_XY` (settle always homes) |
| 4, settle half | `48fe879b0` | `BeltTensionCalibrator::settle()`, `State::SETTLING` |
| 5, probe-point half | `3d14ee0ec` | mock `probe_points`, `set_resonance_probe_points()`, `resonance_tester_settings()`; the mock already runs a multi-line script line by line |
| 6, settle half | `623fb1569` | every run settles first; refusal strings; `settle_generation_`, `settle_query_pending_`; `refresh_hw_sweep()`; durations include the laps. No `RunKind` and no `settle_first` yet: Task 5 adds them |
| 2, the no-turn rule | `b2d247738`, `1ea7ba03a`, `7202cad95` | the stepper starts at "Not turned"; a SCREW round with nothing turned since the last measured round is `no_turn` (adopts the reading, teaches no rate, NO_MOVEMENT unless MATCHED/NO_PEAK/OUT_OF_WINDOW); turns carried past NO_PEAK/OUT_OF_WINDOW still count (`BeltTuneRound::turns_measured`); `advice()` doubles from `turns_measured` past repeat checks; the instruction carries the amount ("Tighten {0} {1}") |
| 8, UI fixes + Title Case | `15d743f11` | "done" notes, age refresh, `bt_similarity_was`, ghosts on their own scale, Good Enough / Next Zone / Keep Tuning / Tune Belts |
| (layout) | `2085646ef`, `625da840d`, `a2213e6ec`, `a20f9c775`, `b3a6061b7` | compact rows, Re-measure keeps its label, RESULTS buttons in every locale, Stop icon |

Still to build, in order: Task 1 (pure zone functions), Task 2 (the sweep path carries `BeltResponse`), Task 3 (the session), Task 4 (mock), Task 5 (panel), Task 6 (docs), then the controller's finish.

## Global Constraints

### Verification Cadence (overrides every skill's per-task verify step)

- **Per task:** only `make t F='[tags the task touches]'`, once after the change. Every new or rewritten test is proven red once: break the line it pins (or run it before the implementation exists), see it fail, restore. That is one extra `make t` cycle, not a habit.
- **Never inside a task:** `make mutate-diff`, `make unit-sweep`, `make full-test-run`, `make land-gate`, ASAN, screenshots, re-reviews, per-task reviews.
- **At the finish, once, by the controller:** one review of the whole batch (a second round only for a crash, lifetime, data-loss or wrong-hardware-command finding); one `make mutate-diff MUTATE_ARGS="--tests '[belt],[shaper_csv],[settle]'"` scoped to the batch's hunks; one headless screenshot per changed screen (CALIBRATE, ALREADY_MATCHED, the power-hint row); ONE `make land-gate` on the combined result.
- `scripts/syntax_check.py <file>...` is the compile check while editing; it proves nothing about behaviour.

### Everything else

- Worktree `.worktrees/belt-tune-loop`, branch `feature/belt-tune-loop`. Hold `worktree:belt-tune-loop` from the first edit to the last commit.
- Headless always: `HELIX_HEADLESS=1 SDL_VIDEODRIVER=dummy`, pinned `ctl` socket and `HELIX_CONFIG_DIR` per CLAUDE.md.
- `src/calibration/shaper_csv_parser.cpp`, `src/calibration/resonance_console.cpp`, `src/api/moonraker_advanced_api.cpp` and `include/belt_tension_types.h` compile into the ESP32 firmware: no exceptions, no `std::stof`/`stoi`, no `json.value()`/`at()`, no RTTI. Every other belt source is already in `app_srcs_excluded.txt`.
- User-facing strings: `lv_tr("...")` in C++, literal `text=` in XML, then `make translation-sync && make translations`; commit the YAMLs and `ui_xml/translations/*.xml`, all nine locales filled. A renamed English key is renamed in all nine `translations/*.yml` with its translated value kept. A new subject bound in XML gets its line in `scripts/esp32_xml_binding_baseline.txt`.
- Appearance in XML, data in C++ (`.claude/rules/declarative-ui.md`); visibility through `bind_flag_if*`, never `lv_obj_add_flag`. Layout facts read the `ui_breakpoint` / `ui_is_portrait` / `ui_is_ultrawide` subjects in XML; C++ that needs the layout calls `app_layout_type()` / `app_layout_is_portrait()` (`include/layout_manager.h`), never a `LayoutManager` field.
- Threading: every API and calibrator callback reaches the panel through `lifetime_.bg_cb(...)`, as today.
- Comments describe the code as it is. No history, no SHAs, no "used to". Issue cites (`#1736`) are fine. No em-dashes anywhere.
- Clockwise = tighten. Turn amounts, the stepper and Hz/turn show only for `BeltTensionerKind::SCREW`.
- Tests that change meaning are REWRITTEN to the new rule under a name that says the new rule, never deleted silently. A test deleted because its function is gone is named in the commit body with the replacement that covers its intent.
- Commits: `feat(belt): ... (prestonbrown/helixscreen#1736)`, subject plus a short paragraph. `git commit -- <paths>`; new files `git add -N` first. `git show --stat` after each.

### Thresholds (named constants; values are the spec's)

`belt_tune::` (full list in Task 3's header): `ZONE_MIN_AMPLITUDE_FRACTION 0.20`, `IN_PLANE_MIN_FRACTION 0.5`, `ZONE_HALF_MIN_HZ 10`, `ZONE_MARGIN_HZ 5`, `ZONE_FALLBACK_MAX_HZ 30`, `MAX_ZONE_CANDIDATES 4`, `ZONE_SWEEP_MARGIN_HZ 10`, `ZONE_SWEEP_TOP_MARGIN_HZ 20`, `CALIBRATION_TURNS 1`, `CALIBRATION_MAX_STEPS 2`, `CALIBRATION_MIN_MOVE_HZ 1.5`, `MATCHED_MIN_HZ 1.0`, `MIN_MOVE_HZ 0.8`, `FIRST_TURN 1.0`, `TURN_STEP 0.25`, `TURN_MIN 0.5`, `TURN_MIN_NEAR 0.25`, `NEAR_MATCH_FACTOR 1.5`, `TURN_MAX 2.0`, `IMPLAUSIBLE_MIN_HZ 6`, `IMPLAUSIBLE_RATE_FACTOR 3`, `POWER_HINT_RATIO 3`, `STALL_ROUNDS 8`, `SPRING_STALL_ROUNDS 2`, `PEAK_MIN_PROMINENCE 1.5`, `UNEXCITED_MIN_BINS 8`, `GAUGE_WARN_HZ 3`. Removed: `NEAR_PEAK_MIN_FRACTION` (its only user, `refine_peak_in_window`, goes). Changed from today's tree: `MIN_MOVE_HZ` 0.5 to 0.8, `FIRST_TURN` 0.25 to 1, `TURN_STEP` 0.125 to 0.25, `TURN_MAX` 1 to 2.

`belt_settle::` is done and unchanged.

### Rulings (where the design is silent)

1. **Sweep windows never leave the printer's range.** `zone_sweep_window()` returns whole-Hz bounds inside `[min_freq, max_freq]`: start rounded UP, end rounded DOWN, so the API's floor/ceil of `FREQ_START`/`FREQ_END` changes nothing. Klipper accepts a `FREQ_END` up to 300 Hz whatever `max_freq` says, and a K1C (max_freq 133) shut its MCU down ("Move queue overflow") on a sweep to 200 Hz, so this clamp is the only guard. Full sweeps send no `FREQ_*` and run the printer's own range (`a sweep without a window sends today's G-code` pins that).
2. **No windowed sweep on a guessed range.** `sweep_cfg_` holds Klipper's defaults (max 135) until a `[resonance_tester]` query answers from the printer. Tune Belts refuses, with an ERROR naming why, when `sweep_cfg_.from_printer` is false: on a K1C (133) the default would send `FREQ_END=135`.
3. **Asymmetric sweep margins.** 10 Hz below the zone (the start transient: the 10-15 "fixed" narrow sweeps bump at 97-104 Hz on a 95 Hz start) and 20 Hz above it. In all 22 narrow CSVs (window 95-150), path A's in-plane curve rises from 136-139 Hz, 11-14 Hz below the window end, to a maximum at 143-148 Hz of up to 15x A's real 125-128 Hz hump. A 20 Hz top margin keeps that rise 6 Hz clear of the zone's top bin. Those sweeps also ran past the Voron's max_freq (135), which ruling 1 now forbids. Where the band clamps the top, the margin is whatever remains: the 10-14 narrow sweeps that ended at 134 Hz on the same Voron show no rise (A falls to 12% of its peak by 134 Hz), so the zone keeps its top bins rather than being cut away.
4. **Already matched (§2a).** At construction, the strongest candidate's in-plane centroid gap is compared with `max(tolerance_hz(), CALIBRATION_MIN_MOVE_HZ)`: the calibration's own definition of run-to-run wander, so a gap below it is indistinguishable from a match. Within it, the stage is ALREADY_MATCHED for every tensioner kind (a SPRING printer would otherwise be told to re-seat a matched belt). `calibrate_anyway()` continues as an unmatched session would. On the data: K1C gaps 0.14 / 0.23 Hz (threshold 1.54) are matched; the Voron's detuned settled state, 1.95 Hz, is not.
5. **The secondary button reads "Calibrate Anyway"**, Title Case like every other button label on the panel.
6. **Calibration is cumulative and directional.** Each calibration sweep compares every candidate's in-plane centroid gap with round 0's, over all calibration turns so far. Only a move the advised way (the turned belt's side of the gap rising) counts. After `CALIBRATION_MAX_STEPS` steps without one, the stage is NO_ZONE and the user is told to turn the belt back.
7. **Candidates.** Matched pairs are candidates (the turn tests movement, not the gap). An unpaired tallest peak still steers against the other path's nearest eligible peak within 30 Hz (TALLEST_UNPAIRED); the mock's default printer needs it. A candidate centred within 10 Hz of a stronger one is dropped; at most four.
8. **Next Zone** walks only the candidates the calibration registered, best move first. SPRING skips calibration: its zone is the strongest candidate and Next Zone walks all candidates. UNKNOWN calibrates with words only ("Tighten B noticeably").
9. **No-turn rounds keep today's semantics** (nothing turned since the last measured round), and IMPLAUSIBLE joins NO_PEAK and OUT_OF_WINDOW as a round whose turns carry forward. A no-turn round is never CLOSER, OVERSHOT or WRONG_WAY and teaches no rate.
10. **The sanity bound without a rate** is `IMPLAUSIBLE_MIN_HZ x max(1, turns)`.
11. **Every run settles** except the BASELINE round chained directly onto a calibration sweep (no belt changed between them).
12. **The power hint** shows in TUNING as a second warning row under the shape warning and opens the same "Why?" modal. RESULTS does not show it.
13. **RESULTS keeps psd_xyz** (similarity, peaks, chart). TUNING draws and reads the in-plane curves.
14. Hz/turn shows one decimal (`~1.7 Hz / turn`): learned rates are now 1.5-3.
15. `format_belt_turns` rounds to the new `TURN_STEP` (¼): every recorded amount, and every total, is a multiple of ¼.
16. The BASELINE takes one round. The 10-15 notes propose two sweeps that refuse to steer when they disagree; that is not in the binding design and stays out.
17. The printer database is not changed here: the K1C's spring tensioner is seen on one printer only. Tests construct sessions with an explicit kind.

## Review Focus

1. **The range clamp.** No `FREQ_END` above `floor(max_freq)` and no `FREQ_START` below `ceil(min_freq)`, for a candidate window, a widened OUT_OF_WINDOW zone and a K1C band of 5-133.33. Tune refuses on a default range. Tests in Tasks 1, 3 and 5.
2. **Already matched.** K1C round 0 lands on ALREADY_MATCHED for SCREW, UNKNOWN and SPRING; Calibrate Anyway reaches CALIBRATE (BASELINE for SPRING); Good Enough returns to RESULTS with no sweep. Tests in Tasks 3 and 5.
3. **The calibration verdict.** A no-turn repeat, a turn of the wrong belt, and a first real turn on the Voron data must NOT register; two cumulative turns must; a K1C-shaped turn of +2.5 Hz registers on the first step. A wrong-sign move never registers. After two failed steps the session is NO_ZONE and the UI says to turn the belt back. Tests in Tasks 3 and 5.
4. **Only zone bins are analysed.** The margin bins never reach a centroid, an edge test, a zone maximum or a power sum; the unexcited floor reads bins outside the SWEPT range. A test feeds a margin spike 10x the zone peak and expects the same centroid.
5. **The run kinds.** A calibration run never lands on RESULTS and never writes `runs_`; a chained BASELINE sends no settle script; Stop, Back and a rebuild during a calibration run behave as they do in a tuning round. Tests in Task 5.
6. **IMPLAUSIBLE.** The reading is dropped: gap, previous gap, gauge and rate unchanged, and the round's turns stay pending into the next round's rate sample. Test in Task 3.
7. **In-plane fallback.** A CSV without psd_x/psd_y must tune on psd_xyz with an in-plane share of 1, not fail or divide by zero. Test in Task 1.
8. **Translations.** Renamed keys keep their translations; the new strings fit their buffers in every locale.

## What the hardware data says about these rules

Read before tuning any threshold. Computed offline on 2026-10-18 from the CSVs with exactly the rules below (in-plane curves, the candidate rule, inclusive zone bins; a Python replica of `detect_belt_peaks`/`pair_belt_peaks`).

| Voron 2.4 (Kalico, max 135), round 0 = settled A0 (121903 A, 122119 B), band 5-135 | value |
|---|---|
| Candidates, strongest first | A 128.6 / B 128.6, zone 118.6-135.0; A 37.8 / B 37.8, zone 27.8-47.8 |
| Zone 118.6-135.0 centroids | A 129.12, B 127.17: gap 1.95 > 1.55, so not already matched; B looser, "Tighten B one full turn" |
| Calibration step 1 (123448 A, 123704 B: one turn) | 118.6-135 gap B-A -1.95 to -3.12 (1.17 the WRONG way); 27.8-47.8 +0.18 to +0.24. Not registered |
| Calibration step 2 (124527 A, 124742 B: two turns) | 118.6-135 gap -1.95 to +1.48, +3.43 the advised way: registered, 1.715 Hz/turn; 27.8-47.8 moved -0.25 |
| No-turn repeat as a calibration sweep (122732 A, 122948 B) | 118.6-135 moved -1.96 (wrong way), 27.8-47.8 -0.37. Not registered |
| Zone 118.6-135 power, round 0 / step 2 | B over A 3.65x / 15.3x: the power hint fires for B |
| 10-14 narrow r1 / r2 over 118.6-135 | gap B-A +0.05 / -1.22 with no turn |
| Tuned state (130022 A, 130237 B; and the 13:06 repeat) | one candidate, 26.3-46.3 (A 36.3 / B 36.3), gap +0.23 / +0.35: ALREADY_MATCHED, on the frame-mode band: it is the only candidate there |

| K1C (Creality, max_freq 133, rigid toolhead), band 5-133 | value |
|---|---|
| Bins | 131, mean spacing 1.538 Hz: tolerance 1.54 |
| base r1 (161412 A, 161920 B) | one candidate, A 58.4 / B 58.4, zone 48.4-68.4, centroids A 59.19 / B 59.34, gap 0.14: ALREADY_MATCHED, A adjusted (lower centroid); power B/A 1.11 (no hint) |
| base r2 (162451 A, 163003 B) | A 58.4 / B 60.0, zone 49.2-69.2, gap 0.23: ALREADY_MATCHED |
| r1 with B's bins shifted -4 Hz | candidate A 58.4 / B 54.4, zone 46.4-66.4, centroids A 58.71 / B 55.75, gap -2.96: CALIBRATE, B adjusted |
| then r2 with B shifted -4 / -1.5 Hz as the calibration sweep | move +0.16 (not registered) / +2.20 (registered, 2.20 Hz/turn) |

One turn did not register on the Voron, and a no-turn repeat moved the same zone's in-plane gap 1.96 Hz. The loop as specified will often say the Voron's first calibration turn did not register. That is the spec working, not a bug; do not tune thresholds to make step 1 register without Preston's ruling. After the Voron's mechanical fix (15:34-16:02), a full turn of A moved nothing in 120-133 Hz: NO_ZONE after two steps is the honest answer there. A rigid-toolhead K1C gives one clean, repeatable peak per belt.

---

### Task 1: `BeltResponse`, in-plane curves, zone candidates, centroid, power and the sweep window (pure)

**Files:**
- Modify: `include/belt_tension_types.h` (add `BeltResponse` after `BeltCurve`)
- Modify: `include/shaper_csv_parser.h`, `src/calibration/shaper_csv_parser.cpp` (`ResonanceCsvData::in_plane`)
- Modify: `include/belt_tune_session.h`, `src/calibration/belt_tune_session.cpp` (the free functions only; the class changes in Task 3)
- Modify: `tests/unit/belt_test_curves.h` (three helpers)
- Create fixtures by copying, unchanged, from `.superpowers/sdd/2026-10-13-belt-tune-loop-plans/` into `tests/fixtures/belt_sweeps/` (path A is `_axis_1_-1`, path B `_axis_1_1`):

  | Destination | Source | State |
  |---|---|---|
  | `voron24_settled_axis_1_-1.csv` / `_axis_1_1.csv` | `hw-2026-10-15/121903-belt_a.csv` / `122119-belt_b.csv` | 12:21, A 0, after laps |
  | `voron24_settled_repeat_axis_1_-1.csv` / `_axis_1_1.csv` | `122732-belt_a.csv` / `122948-belt_b.csv` | 12:30, no-turn repeat |
  | `voron24_turn1_axis_1_-1.csv` / `_axis_1_1.csv` | `123448-belt_a.csv` / `123704-belt_b.csv` | 12:37, one turn (A loosened) |
  | `voron24_turn2_axis_1_-1.csv` / `_axis_1_1.csv` | `124527-belt_a.csv` / `124742-belt_b.csv` | 12:47, two turns |
  | `voron24_narrow_r1_axis_1_-1.csv` / `_axis_1_1.csv` | `hw-2026-10-14/cb1-csv/r1/resonances_axis=1.000,-1.000_helix_belt_a.csv` / `r1/resonances_axis=1.000,1.000_helix_belt_b.csv` | 10-14 narrow round, 121-134 Hz, no turn |
  | `voron24_narrow_r2_axis_1_-1.csv` / `_axis_1_1.csv` | the same two files under `r2/` | repeat, no turn |
  | `k1c_settled_axis_1_-1.csv` / `_axis_1_1.csv` | `hw-2026-10-15/k1c/161412-base-r1-a.csv` / `161920-base-r1-b.csv` | laps + full sweep, hands off |
  | `k1c_settled_repeat_axis_1_-1.csv` / `_axis_1_1.csv` | `k1c/162451-base-r2-a.csv` / `163003-base-r2-b.csv` | repeat, hands off |

  Loosening A by a turn moves the A-B gap the same way tightening B does, so with B adjusted the Voron turn files stand in for "Tighten B one turn". The existing `k1c_axis_*` fixtures stay; they are a different capture.
- Test: `tests/unit/test_shaper_csv_parser.cpp` (`[shaper_csv][belt]`), `tests/unit/test_belt_tune_session.cpp` (`[belt][tune]`)

**Interfaces:**
- Consumes: `compare_belt_paths`, `detect_belt_peaks`, `pair_belt_peaks`, `curve_in_band` (`include/belt_tension_types.h`); `BeltSweepWindow`.
- Produces, `belt_tension_types.h` (compiled):
  ```cpp
  /// One sweep's response on one frequency grid: psd_xyz for the similarity
  /// summary, psd_x + psd_y for tuning. Belt modes move the toolhead in the XY
  /// plane; Z motion in a band is the frame, not the belt.
  struct BeltResponse {
      BeltCurve xyz;
      BeltCurve in_plane; ///< empty when the CSV carried no psd_x and psd_y columns
  };
  ```
- Produces, `shaper_csv_parser.h` (compiled):
  ```cpp
  struct ResonanceCsvData {
      BeltCurve curve; ///< (freq, psd_xyz), empty unless error == NONE
      /// (freq, psd_x + psd_y) on curve's bins; empty unless both columns exist
      BeltCurve in_plane;
      ResonanceCsvError error = ResonanceCsvError::NONE;
  };
  ```
  Column lookup by header name, as for `psd_xyz`. A row dropped from `curve` is dropped from `in_plane` too, so the two stay bin-aligned.
- Produces, `belt_tune_session.h`: the free functions and constants exactly as in Task 3's header (`tuning_curve`, `in_plane_fraction`, `tune_zone_candidates`, `zone_sweep_window`, `zone_centroid_hz`, `zone_power`, `zone_power_hint`, the `swept`-taking `unexcited_floor_psd` and `window_holds_peak`, `BeltZoneCandidate`, `BeltPowerHint`, and the `belt_tune::` constants). `tune_zone_candidates` runs `compare_belt_paths(tuning_curve(a), tuning_curve(b), band)` and filters its pairing; it reuses `detect_belt_peaks`/`pair_belt_peaks` through that call, never a second peak finder. The session class keeps compiling in this task: where it calls `unexcited_floor_psd`/`window_holds_peak`, pass the zone window as `swept` (today's behaviour). `pick_tune_zone` and `refine_peak_in_window` stay until Task 3 removes their last callers.
- Produces, `tests/unit/belt_test_curves.h`:
  ```cpp
  /// A response whose in-plane curve is `in_plane` and whose psd_xyz is
  /// in_plane / in_plane_share at every bin.
  inline BeltResponse response_from(const BeltCurve& in_plane, float in_plane_share = 0.9f);
  /// windowed() applied to both curves of a response.
  inline BeltResponse windowed(const BeltResponse& full, BeltSweepWindow window, float floor);
  /// Every bin of both curves moved by `hz`: a capture's shape at another tension.
  inline BeltResponse shifted(const BeltResponse& r, float hz);
  ```
  A loader for the new fixtures beside the existing one (`load_fixture_response(name, 'A')` reading through `parse_resonance_csv`) so the tests read the parser's real output.

- [ ] **Step 1: Failing tests.**
  - Parser: "resonance CSV: the in-plane curve is psd_x + psd_y on the same bins": a 3-row CSV `freq,psd_x,psd_y,psd_z,psd_xyz` with rows (10, 1, 2, 3, 6), (11, 4, 5, 1, 10), (12, 0.5, 0.5, 9, 10) gives `in_plane` {(10,3),(11,9),(12,1)} and `curve` {(10,6),(11,10),(12,10)}. "a CSV with psd_xyz but no psd_x or psd_y has an empty in-plane curve" (header `freq,psd_xyz`). Extend `a truncated last row is dropped, the rest kept` so `in_plane` has the same size and frequencies as `curve`.
  - `voron24_settled` (band 5-135): `tune_zone_candidates` returns exactly two, in this order: (A 128.6, B 128.6, window 118.6-135.0, PAIRED) and (A 37.8, B 37.8, window 27.8-47.8, PAIRED), frequencies within 0.1 Hz. A C++ difference is investigated (pairing threshold quantiles first) before any expectation changes.
  - `k1c_settled` (band 5-133): one candidate, A 58.4 / B 58.4, window 48.4-68.4 within 0.1.
  - `zone_centroid_hz` on `tuning_curve` over 118.6-135.0: settled A 129.12, B 127.17; turn1 A 130.04, B 126.92; turn2 A 128.31, B 129.79; repeat A 130.28, B 126.37 (all within 0.05). K1C settled over 48.4-68.4: A 59.19, B 59.34.
  - "the zone centroid wanders less than the tallest zone bin": settled to repeat, A's centroid moves 1.16 Hz while its tallest in-zone bin moves 128.6 to 130.1; B's moves 0.80 while its tallest bin moves 128.6 to 127.1. Assert centroid move < tallest-bin move on both paths.
  - `zone_power_hint` over 118.6-135.0: settled gives B, ratio 3.65 (within 0.05), freq 127.17; turn2 gives B, ratio 15.3 (within 0.3); K1C settled over 48.4-68.4 gives nullopt (1.11). Synthetic: equal powers give nullopt; ratio exactly 3.0 gives nullopt; a zero-power path gives nullopt.
  - In-plane share (synthetic, `response_from`): a pair whose A peak has share 0.4 is not a candidate; at 0.5 it is. `in_plane_fraction` of a response with empty `in_plane` is 1; with psd_xyz 0 at that bin it is 0.
  - Fallback (Review Focus 7): responses with empty `in_plane` give the same candidates as the same curves passed as in-plane.
  - Dedupe: synthetic pairs at 30 Hz (strength 1.0) and 37 Hz (strength 1.5) leave only the 37 Hz candidate; pairs at 30 and 45 Hz leave both. Six well-separated pairs leave four, strongest first.
  - Matched pairs are candidates: one pair at 100.0/100.0 Hz gives one candidate.
  - TALLEST_UNPAIRED: A 110 and B 98 Lorentzians (the mock's default printer) give a TALLEST_UNPAIRED candidate with window 93-115.
  - `zone_sweep_window` (Review Focus 1): `({118.6,135},5,135)` is {109,135}; `({20,40},5,135)` is {10,60}; `({8,30},5,135)` is {5,50}; `({48.4,68.4},5,133.33)` is {39,88}; `({100,120},5,133.33)` is {90,133} (never 134); `({8,30},5.5,135)` is {6,50} (never 5). Every result has integral bounds inside the band.
  - Margin isolation (Review Focus 4): a narrow Lorentzian at 100 Hz plus a spike 10x taller at 85 Hz (inside the low sweep margin of zone 90-110) gives the same `zone_centroid_hz` and `zone_power` as without the spike, and the same with the spike at 125 Hz (inside the top margin); `window_holds_peak(curve, zone {90,110}, swept {80,130}, 5, 135)` reads its floor from bins outside 80-130.
  - `zone_centroid_hz` with fewer than 3 zone bins, or all-zero power, is nullopt.
- [ ] **Step 2:** `make t F='[shaper_csv]'` and `make t F='[belt][tune]'`. Expect FAIL (no `BeltResponse`, no candidates).
- [ ] **Step 3:** Implement. Copy the fixtures. `scripts/syntax_check.py src/calibration/shaper_csv_parser.cpp src/calibration/belt_tune_session.cpp`; `python3 scripts/check_esp32_app_srcs.py`.
- [ ] **Step 4:** `make t F='[shaper_csv]'` and `make t F='[belt][tune]'`. Expect PASS.
- [ ] **Step 5:** Commit: `feat(belt): in-plane curves, zone candidates and a sweep window inside the printer's range`.

### Task 2: The sweep path carries `BeltResponse`

**Files:**
- Modify: `include/i_moonraker_sub_apis.h` (`BeltCurveCallback` becomes `BeltResponseCallback`), `include/moonraker_advanced_api.h`, `src/api/moonraker_advanced_api.cpp` (`BeltResonanceCollector` delivers `BeltResponse{data.curve, data.in_plane}`)
- Modify: `include/belt_tension_calibrator.h`, `src/calibration/belt_tension_calibrator.cpp` (`measure_path` only; `settle()` is done)
- Modify: `include/ui_panel_belt_tension.h`, `src/ui/ui_panel_belt_tension.cpp` (`PathRun::in_plane`, `on_sweep_complete` takes a `BeltResponse`; nothing else)
- Test: `tests/unit/test_moonraker_api_belt_resonance.cpp` (`[belt][api]`), `tests/unit/test_belt_tension_calibrator.cpp` (`[belt_tension][calibrator]`), `tests/unit/test_mock_test_resonances.cpp` (lambda types only), `tests/unit/test_belt_tension_panel_states.cpp` (one case)

**Interfaces:**
- Consumes: `BeltResponse`, `ResonanceCsvData::in_plane` (Task 1).
- Produces, `IAdvancedAPI` (compiled):
  ```cpp
  /// Fires off the LVGL thread with the parsed curves.
  using BeltResponseCallback =
      std::function<void(const helix::calibration::BeltResponse& response)>;
  [[nodiscard]] virtual BeltRunCancel test_belt_resonance(
      const std::string& axis_param, const std::string& output_name,
      BeltSweepProgressCallback on_progress, BeltResponseCallback on_complete,
      ErrorCallback on_error,
      std::optional<helix::calibration::BeltSweepWindow> window = std::nullopt) = 0;
  ```
  `MoonrakerAdvancedAPI` mirrors it (`using BeltResponseCallback = IAdvancedAPI::BeltResponseCallback;`). No name for the old alias survives (`grep -rn BeltCurveCallback include src tests` is empty).
- Produces, `BeltTensionCalibrator` (compiled):
  ```cpp
  /// @param on_complete The parsed curves: psd_xyz and psd_x + psd_y, ascending
  void measure_path(BeltPath path, std::function<void(int percent, float freq_hz)> on_progress,
                    std::function<void(BeltResponse)> on_complete, BeltErrorCallback on_error,
                    std::optional<BeltSweepWindow> window = std::nullopt);
  ```
- Produces, panel (compiled):
  ```cpp
  struct PathRun {
      helix::calibration::BeltCurve curve;    ///< psd_xyz
      helix::calibration::BeltCurve in_plane; ///< psd_x + psd_y; empty when the CSV had neither
      helix::calibration::BeltCurve previous;
      bool has = false;
      bool has_previous = false;
      uint32_t measured_at_ms = 0;
  };
  void on_sweep_complete(helix::calibration::BeltPath path,
                         helix::calibration::BeltResponse response);
  ```
  A full sweep stores `.xyz` into `curve` and `.in_plane` into `in_plane`; a windowed sweep stores `.xyz` into today's `tune_curves_` (Task 5 turns that member into a `BeltResponse`).

- [ ] **Step 1: Failing tests.**
  - API: "test_belt_resonance delivers both curves" (rewrite of `test_belt_resonance delivers the parsed curve`): the mock's belt CSV writes psd_x = psd_y = 0.45 x psd_xyz, so every `in_plane` bin is 0.9 x its `xyz` bin (relative 1e-3, the CSV's `%.3e` rounding) and the two share frequencies. Every other `[belt][api]` case takes `const BeltResponse&` and reads `.xyz` where it read the curve.
  - Calibrator: "measure_path reports both curves" (rewrite of `measure_path sweeps its path and reports the peak`, same peak assertion on `.xyz`, plus `in_plane` non-empty and bin-aligned).
  - Panel: "a full sweep keeps both curves": after Start, `runs_[0].in_plane` and `runs_[1].in_plane` are non-empty with `runs_[i].curve`'s frequencies.
- [ ] **Step 2:** `make t F='[belt][api]'`, `make t F='[belt_tension][calibrator]'`. Expect FAIL.
- [ ] **Step 3:** Implement. `scripts/syntax_check.py` every touched file; `python3 scripts/check_esp32_app_srcs.py`.
- [ ] **Step 4:** `make t F='[belt][api]'`, `make t F='[belt_tension]'`, `make t F='[belt][mock]'`, `make t F='[belt][panel]'`. Expect PASS.
- [ ] **Step 5:** Commit: `feat(belt): sweeps report in-plane curves`.

### Task 3: `BeltTuneSession`: already matched, calibration, centroid tracking, IMPLAUSIBLE and turn sizes (pure)

**Files:**
- Modify: `include/belt_tune_session.h`, `src/calibration/belt_tune_session.cpp`
- Modify (bridging only): `src/ui/ui_panel_belt_tension.cpp`
- Test: `tests/unit/test_belt_tune_session.cpp` (`[belt][tune]`)

**Interfaces:**
- Consumes: Task 1's free functions and fixtures; `BeltResponse`.
- Produces: `include/belt_tune_session.h` exactly as below (compiled; the private section may change, the public one may not without a plan update).

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "belt_tension_types.h"

#include <optional>
#include <string>
#include <vector>

/**
 * @file belt_tune_session.h
 * @brief The guided belt-tuning loop's logic: zones, calibration, tracking and advice
 *
 * Pure: no LVGL, no I/O. The Belt Tension panel feeds it the round-0 sweeps,
 * the calibration sweeps and each round's two narrow sweeps, and renders what
 * it says. Tuning reads the in-plane curves (psd_x + psd_y). A higher
 * frequency means a tighter belt, and clockwise tightens (#1736).
 */

namespace helix::calibration {

namespace belt_tune {
/// Peaks under this fraction of the stronger path's in-band in-plane maximum
/// never define a zone.
inline constexpr float ZONE_MIN_AMPLITUDE_FRACTION = 0.20f;
/// A peak defines a zone only where psd_x + psd_y is at least this share of psd_xyz.
inline constexpr float IN_PLANE_MIN_FRACTION = 0.5f;
inline constexpr float ZONE_HALF_MIN_HZ = 10.0f; ///< narrowest half-width of a zone
inline constexpr float ZONE_MARGIN_HZ = 5.0f;    ///< zone reach beyond each defining peak
/// An unpaired tallest peak steers only against a peak this close on the other path.
inline constexpr float ZONE_FALLBACK_MAX_HZ = 3.0f * belt_verdict::PAIR_MAX_HZ;
inline constexpr int MAX_ZONE_CANDIDATES = 4;
/// A narrow sweep starts this far below the zone, so its start transient
/// lands in bins the analysis never reads.
inline constexpr float ZONE_SWEEP_MARGIN_HZ = 10.0f;
/// ...and ends this far above it: the last 15 Hz of a narrow sweep can read
/// many times the zone's own power.
inline constexpr float ZONE_SWEEP_TOP_MARGIN_HZ = 20.0f;
inline constexpr float CALIBRATION_TURNS = 1.0f;       ///< one full turn per calibration step
inline constexpr int CALIBRATION_MAX_STEPS = 2;        ///< steps before the loop gives up
inline constexpr float CALIBRATION_MIN_MOVE_HZ = 1.5f; ///< a smaller gap move is run-to-run wander
inline constexpr float MATCHED_MIN_HZ = 1.0f;          ///< matched tolerance floor
inline constexpr float MIN_MOVE_HZ = 0.8f;             ///< smaller gap moves are noise
inline constexpr float FIRST_TURN = 1.0f;              ///< the suggestion with no rate learned
inline constexpr float TURN_STEP = 0.25f;
inline constexpr float TURN_MIN = 0.5f;
inline constexpr float TURN_MIN_NEAR = 0.25f; ///< the smallest turn when nearly matched
/// "Nearly matched": |gap| under this multiple of the matched tolerance.
inline constexpr float NEAR_MATCH_FACTOR = 1.5f;
inline constexpr float TURN_MAX = 2.0f;
/// A round's gap move above max(IMPLAUSIBLE_MIN_HZ, IMPLAUSIBLE_RATE_FACTOR x
/// turns x Hz/turn) is a measurement fault. Without a rate the bound is
/// IMPLAUSIBLE_MIN_HZ x max(1, turns).
inline constexpr float IMPLAUSIBLE_MIN_HZ = 6.0f;
inline constexpr float IMPLAUSIBLE_RATE_FACTOR = 3.0f;
/// Zone in-plane power differing by more than this between paths points at
/// the mechanics, not tension.
inline constexpr float POWER_HINT_RATIO = 3.0f;
inline constexpr int STALL_ROUNDS = 8;        ///< unmatched rounds before the mechanical hint
inline constexpr int SPRING_STALL_ROUNDS = 2; ///< spring rounds in a row without improvement
/// A zone holds a peak when its maximum reaches this multiple of the unexcited
/// floor (or, without one, of the zone's minimum).
inline constexpr float PEAK_MIN_PROMINENCE = 1.5f;
/// In-band bins outside the swept range needed to measure the unexcited floor.
inline constexpr int UNEXCITED_MIN_BINS = 8;
inline constexpr float GAUGE_WARN_HZ = 3.0f; ///< gap gauge: warning colour up to here
} // namespace belt_tune

enum class BeltZoneSource {
    PAIRED,           ///< a peak pair
    TALLEST_UNPAIRED, ///< an unpaired tallest peak and the other path's nearest peak
};

struct BeltTuneZone {
    BeltSweepWindow window; ///< the analysed bins; a sweep reaches past it (zone_sweep_window)
    BeltZoneSource source = BeltZoneSource::PAIRED;
    float ref_hz = 0.0f; ///< reference path's peak that defined the zone
    float adj_hz = 0.0f; ///< adjusted path's peak that defined the zone
};

/// A zone the calibration turn tests. Its zone is seen from PATH_A: adj_hz is
/// A's peak, ref_hz B's.
struct BeltZoneCandidate {
    BeltTuneZone zone;
    float strength = 0.0f; ///< the two defining peaks' in-plane amplitudes, summed
};

/// What the zone's power says when it differs too much between the paths.
struct BeltPowerHint {
    BeltPath stronger = BeltPath::PATH_A;
    float freq_hz = 0.0f; ///< the stronger path's zone centroid
    float ratio = 1.0f;   ///< stronger power over weaker, > POWER_HINT_RATIO
};

/// The curve tuning reads: in_plane, or xyz when the sweep had no per-axis columns.
[[nodiscard]] const BeltCurve& tuning_curve(const BeltResponse& r);

/// psd_x + psd_y over psd_xyz at the bin nearest freq_hz. 1 when in_plane is
/// empty (nothing says otherwise); 0 when psd_xyz there is not positive.
[[nodiscard]] float in_plane_fraction(const BeltResponse& r, float freq_hz);

/// True when either path's tallest peak is unpaired and at or above the zone
/// amplitude floor: the curves differ in shape, and the mechanical-causes
/// hint shows.
[[nodiscard]] bool tallest_peak_unpaired(const BeltComparison& cmp);

/// The zones a calibration turn tests, strongest first, at most
/// MAX_ZONE_CANDIDATES. Built from the in-plane comparison over the band:
/// every pair whose two peaks reach ZONE_MIN_AMPLITUDE_FRACTION of the
/// in-band in-plane maximum and IN_PLANE_MIN_FRACTION in-plane share
/// (PAIRED), and an unpaired tallest peak meeting both, steered against the
/// other path's nearest such peak within ZONE_FALLBACK_MAX_HZ
/// (TALLEST_UNPAIRED). A matched pair is a candidate: the turn tests movement,
/// not the gap. A candidate whose centre lies within ZONE_HALF_MIN_HZ of a
/// stronger one's is dropped. Each window is the two peaks' midpoint
/// +- max(ZONE_HALF_MIN_HZ, gap / 2 + ZONE_MARGIN_HZ), clamped to the band.
[[nodiscard]] std::vector<BeltZoneCandidate>
tune_zone_candidates(const BeltResponse& a, const BeltResponse& b, float band_min_hz,
                     float band_max_hz);

/// What a narrow sweep of the zone sends: ZONE_SWEEP_MARGIN_HZ below it and
/// ZONE_SWEEP_TOP_MARGIN_HZ above it, in whole Hz inside the band (start
/// rounded up, end rounded down), so FREQ_START/FREQ_END never leave the
/// printer's [resonance_tester] range.
[[nodiscard]] BeltSweepWindow zone_sweep_window(BeltSweepWindow zone, float band_min_hz,
                                                float band_max_hz);

/// Power-weighted mean frequency of the curve's bins inside the zone
/// (inclusive). nullopt with fewer than 3 such bins or no positive power.
[[nodiscard]] std::optional<float> zone_centroid_hz(const BeltCurve& curve, BeltSweepWindow zone);

/// Sum of the curve's values over the zone's bins (inclusive).
[[nodiscard]] float zone_power(const BeltCurve& curve, BeltSweepWindow zone);

/// The hint when one path's zone power exceeds the other's by more than
/// POWER_HINT_RATIO; nullopt otherwise, or when either power is not positive.
[[nodiscard]] std::optional<BeltPowerHint> zone_power_hint(const BeltCurve& a,
                                                           const BeltCurve& b,
                                                           BeltSweepWindow zone);

/// What the belt reads unexcited: the median of the curve's in-band bins
/// outside `swept`, which the sweep did not drive. nullopt with fewer than
/// UNEXCITED_MIN_BINS such bins.
[[nodiscard]] std::optional<float> unexcited_floor_psd(const BeltCurve& curve,
                                                       BeltSweepWindow swept, float band_min_hz,
                                                       float band_max_hz);

/// Whether the zone's maximum reaches PEAK_MIN_PROMINENCE times the unexcited
/// floor outside `swept`, or, without one, times the zone's minimum.
[[nodiscard]] bool window_holds_peak(const BeltCurve& curve, BeltSweepWindow zone,
                                     BeltSweepWindow swept, float band_min_hz, float band_max_hz);

/// A turn count to the nearest belt_tune::TURN_STEP, in fraction glyphs:
/// "¼", "¾", "1", "1¼". The sign is dropped.
[[nodiscard]] std::string format_belt_turns(float turns);

/// max(MATCHED_MIN_HZ, the curve's mean bin spacing).
[[nodiscard]] float matched_tolerance_hz(const BeltCurve& curve);

enum class BeltTuneStage {
    /// Round 0's strongest candidate already matches: its centroid gap is
    /// within max(tolerance_hz(), CALIBRATION_MIN_MOVE_HZ). Nothing is asked
    /// of the user until calibrate_anyway().
    ALREADY_MATCHED,
    CALIBRATE, ///< waiting on a calibration turn and its full sweep of both paths
    BASELINE,  ///< the zone is known; its first narrow round gives no verdict
    TUNE,      ///< each round's verdict reads the zone's gap move
    NO_ZONE,   ///< no candidate, or no candidate moved after CALIBRATION_MAX_STEPS turns
};

enum class BeltTuneOutcome {
    CLOSER,    ///< the gap shrank and kept its sign
    OVERSHOT,  ///< the gap changed sign
    WRONG_WAY, ///< the gap moved against the advice by MIN_MOVE_HZ or more
    /// The gap moved less than MIN_MOVE_HZ, or the round turned nothing.
    /// Outcomes read the gap's change, not one path's centroid: only the
    /// adjusted belt was turned, so drift common to both paths cancels.
    NO_MOVEMENT,
    MATCHED,       ///< |gap| within the matched tolerance
    NO_PEAK,       ///< a path's zone holds no clear peak; nothing measured
    OUT_OF_WINDOW, ///< the adjusted path's maximum sits on the zone edge; the zone moved
                   ///< (once per zone until a round is measured again; then NO_PEAK)
    BASELINE,      ///< a zone's first narrow round: no gap history yet
    /// The gap moved past the sanity bound: a measurement fault. The reading
    /// is dropped, the turns stay pending, and nothing is learned.
    IMPLAUSIBLE,
};

enum class BeltTurnDirection { TIGHTEN, LOOSEN };

struct BeltTuneAdvice {
    BeltPath path = BeltPath::PATH_A;
    BeltTurnDirection direction = BeltTurnDirection::TIGHTEN;
    std::optional<float> turns; ///< SCREW only, a multiple of TURN_STEP
    bool spring_reseat = false; ///< SPRING: release, settle, lock
};

/// One calibration sweep's verdict.
struct BeltCalibrationResult {
    bool registered = false; ///< a candidate's gap moved CALIBRATION_MIN_MOVE_HZ the advised way
    float move_hz = 0.0f;    ///< the best candidate's gap move, positive = the advised way
    float turns = 0.0f;      ///< calibration turns so far; the move is measured over all of them
    bool gave_up = false;    ///< CALIBRATION_MAX_STEPS reached without a move: stage is NO_ZONE
};

struct BeltTuneRound {
    float ref_hz = 0.0f;             ///< reference path's zone centroid
    float adj_hz = 0.0f;             ///< adjusted path's zone centroid
    float gap_hz = 0.0f;             ///< adjusted - reference
    std::optional<float> turns_made; ///< what the stepper recorded before this round
    /// The turns this round's reading measured: its own plus any carried from
    /// NO_PEAK, OUT_OF_WINDOW or IMPLAUSIBLE rounds since the last measured
    /// one. 0 on a round that measured nothing.
    float turns_measured = 0.0f;
    BeltTuneOutcome outcome = BeltTuneOutcome::NO_PEAK;
    /// NO_PEAK with a peak on the adjusted path only: the reference's peak
    /// left the zone, which a turn of the wrong belt does.
    bool reference_lost = false;
    /// SCREW: nothing was turned since the last measured round, so the round
    /// checks repeatability rather than a turn. It never counts toward the
    /// stall and teaches nothing. Never set on a BASELINE round.
    bool no_turn = false;
};

/// One tuning run. Round 0's candidates are tested by a calibration turn; the
/// zone that moved is tuned a round at a time, each round a pair of narrow
/// sweeps. The gap is always adjusted - reference from the same round, so
/// drift common to both paths cancels; negative means the adjusted belt is
/// looser.
class BeltTuneSession {
  public:
    /// `round0` is the psd_xyz comparison RESULTS showed; candidates come from
    /// the responses' in-plane curves. Any kind may start ALREADY_MATCHED.
    /// Otherwise SCREW and UNKNOWN start at CALIBRATE, and SPRING skips
    /// calibration: the strongest candidate becomes the zone, adjusting its
    /// looser path, at BASELINE.
    BeltTuneSession(const BeltResponse& a, const BeltResponse& b, const BeltComparison& round0,
                    float band_min_hz, float band_max_hz, BeltTensionerKind kind);

    [[nodiscard]] BeltTuneStage stage() const;
    [[nodiscard]] const std::vector<BeltZoneCandidate>& candidates() const;
    /// ALREADY_MATCHED only: go on as an unmatched session would (CALIBRATE,
    /// or BASELINE for SPRING). Elsewhere it changes nothing.
    void calibrate_anyway();
    /// The zone being tuned, seen from the adjusted path; nullopt until
    /// calibration registers (and at NO_ZONE). SPRING has one from the start.
    [[nodiscard]] const std::optional<BeltTuneZone>& zone() const;
    /// The range a round sweeps: zone_sweep_window() of the zone. nullopt without a zone.
    [[nodiscard]] std::optional<BeltSweepWindow> sweep_window() const;
    /// ALREADY_MATCHED and CALIBRATE: the looser path by the strongest
    /// candidate's centroid gap, the one the calibration turn tightens.
    /// Later: the path being tuned.
    [[nodiscard]] BeltPath adjusted_path() const;
    /// Swaps reference and adjusted, for this and later zones. The zone stays;
    /// the learned rate and this zone's rounds belong to the other belt, so
    /// they clear. At CALIBRATE it changes which belt the turn tightens.
    void swap_adjusted_path();
    /// Signed, adjusted - reference. ALREADY_MATCHED and CALIBRATE: round 0's
    /// strongest candidate's centroid gap.
    [[nodiscard]] float gap_hz() const;
    /// CALIBRATE: tighten the adjusted path, CALIBRATION_TURNS for SCREW.
    /// TUNE: from the gap and the learned rate. Meaningless at
    /// ALREADY_MATCHED, BASELINE and NO_ZONE.
    [[nodiscard]] BeltTuneAdvice advice() const;
    /// The stepper's value for the next round; SCREW only. A SCREW round with
    /// nothing turned since the last measured round (no recorded turn, or a
    /// recorded zero, and none carried from a NO_PEAK, OUT_OF_WINDOW or
    /// IMPLAUSIBLE round) is a no-turn round: it adopts the reading, teaches
    /// no rate and reads NO_MOVEMENT unless MATCHED, NO_PEAK, OUT_OF_WINDOW or
    /// IMPLAUSIBLE.
    void record_turns(float turns);
    /// The full sweep of both paths after a calibration turn. Each candidate's
    /// centroid gap is compared with round 0's, so a second step measures both
    /// turns together. Outside CALIBRATE it changes nothing and returns a
    /// result with registered false.
    BeltCalibrationResult add_calibration_sweep(const BeltResponse& a_full,
                                                const BeltResponse& b_full);
    /// Both paths swept over sweep_window().
    const BeltTuneRound& add_round(const BeltResponse& a_swept, const BeltResponse& b_swept);
    [[nodiscard]] bool zone_matched() const;
    /// Marks this zone matched and moves to the next candidate the calibration
    /// registered (every candidate, for SPRING), keeping the adjusted path.
    /// The new zone needs a BASELINE round. False, changing nothing, when none
    /// remains.
    bool next_zone();
    [[nodiscard]] bool has_next_zone() const;
    /// The zone next_zone() would move to, seen from PATH_A; nullopt when none.
    [[nodiscard]] std::optional<BeltTuneZone> peek_next_zone() const;
    /// stage() == BASELINE.
    [[nodiscard]] bool needs_baseline() const;
    [[nodiscard]] std::optional<float> hz_per_turn() const;
    /// tallest_peak_unpaired() for round 0, or this zone has stalled.
    [[nodiscard]] bool show_mechanical_hint() const;
    /// zone_power_hint() over the zone for the latest curves measured across
    /// it; nullopt without a zone.
    [[nodiscard]] std::optional<BeltPowerHint> power_hint() const;
    [[nodiscard]] const std::vector<BeltTuneRound>& rounds() const; ///< current zone

    [[nodiscard]] BeltTensionerKind tensioner() const;
    [[nodiscard]] float tolerance_hz() const; ///< the matched band, +- around zero
    [[nodiscard]] float reference_hz() const; ///< latest measured reference centroid
    [[nodiscard]] float adjusted_hz() const;
    [[nodiscard]] std::optional<float> previous_gap_hz() const; ///< hollow gauge needle
    /// Signed sum, tighten positive, of the turns recorded while `path` was
    /// the adjusted one, calibration turns included. SCREW only.
    [[nodiscard]] float total_turns(BeltPath path) const;
    [[nodiscard]] int total_rounds() const; ///< across zones, calibration sweeps excluded

  private:
    void enter_zone(const BeltZoneCandidate& candidate, BeltPath path);
    void clear_belt_history();
    void book_turns(float turns);

    BeltResponse a0_;
    BeltResponse b0_;
    float band_min_hz_;
    float band_max_hz_;
    BeltTensionerKind kind_;
    float tolerance_hz_;
    bool shape_warning_;

    BeltTuneStage stage_ = BeltTuneStage::NO_ZONE;
    std::vector<BeltZoneCandidate> candidates_;
    /// Candidates the calibration registered, best move first (all of them for SPRING).
    std::vector<BeltZoneCandidate> registered_;
    size_t zone_index_ = 0; ///< into registered_
    std::optional<BeltTuneZone> zone_;
    int calibration_steps_ = 0;

    BeltPath adjusted_ = BeltPath::PATH_A;
    bool window_widened_ = false; ///< OUT_OF_WINDOW since the last measured round
    float ref_hz_ = 0.0f;
    float adj_hz_ = 0.0f;
    std::optional<float> previous_gap_hz_;
    std::vector<BeltTuneRound> rounds_;
    int total_rounds_ = 0;
    /// The latest in-plane curves measured across the zone, for power_hint().
    BeltCurve last_a_;
    BeltCurve last_b_;

    std::optional<float> pending_turns_;
    /// Turns since the last measured round, as magnitudes. Only a measured
    /// round can flip the advised direction, so they all went one way.
    float unmeasured_turns_ = 0.0f;
    std::vector<float> rate_samples_; ///< |gap move| / turns, Hz per turn
    float total_turns_a_ = 0.0f;
    float total_turns_b_ = 0.0f;
};

} // namespace helix::calibration
```

- Behaviour the header comments do not spell out:
  - **Constructor:** candidates from `tune_zone_candidates`; none gives NO_ZONE. `tolerance_hz_` = max of both xyz curves' `matched_tolerance_hz`. `shape_warning_` = `tallest_peak_unpaired(round0)`. `adjusted_` = the lower-centroid path over the strongest candidate's zone (PATH_A on a tie); `ref_hz_`/`adj_hz_` are those centroids. Then ALREADY_MATCHED when `|gap| <= max(tolerance_hz_, CALIBRATION_MIN_MOVE_HZ)`; else SCREW/UNKNOWN CALIBRATE; SPRING `registered_` = all candidates, enter the first, BASELINE.
  - **`calibrate_anyway()`:** from ALREADY_MATCHED, SCREW/UNKNOWN go to CALIBRATE with the same adjusted path, SPRING does what its constructor would have done. Elsewhere a no-op.
  - **Calibration step:** books `CALIBRATION_TURNS` (SCREW: into `total_turns`, sign tighten). For each candidate, move = (adj - ref centroid gap now) - (the same over round 0). Registered = the moves >= `CALIBRATION_MIN_MOVE_HZ`, best first. On registration: enter the best, its centroids from this sweep as `ref_hz_`/`adj_hz_`, BASELINE, and (SCREW) one rate sample = move / total calibration turns. Otherwise `calibration_steps_` grows; at `CALIBRATION_MAX_STEPS` the stage is NO_ZONE and `gave_up` is true.
  - **Round, BASELINE:** measure both centroids, outcome MATCHED if |gap| <= tolerance else BASELINE, stage TUNE, no rate sample, previous gap cleared.
  - **Round, TUNE:** NO_PEAK when either path's zone fails `window_holds_peak` (floor from outside `sweep_window()`). OUT_OF_WINDOW as today, from the adjusted path's zone bins only, re-centring the zone on the reference centroid and widening it to the edge plus `ZONE_MARGIN_HZ`, clamped to the band, once per zone until a measured round. Then the sanity bound (IMPLAUSIBLE drops the reading and keeps `unmeasured_turns_`), then outcomes in today's order with ruling 9 for no-turn rounds. A rate sample needs: SCREW, not no_turn, not WRONG_WAY, not IMPLAUSIBLE, |move| >= `MIN_MOVE_HZ`, `turns_measured` > 0; sample = |move| / `turns_measured`.
  - **Advice, TUNE, SCREW:** with a rate, `turns = clamp(round_to(|gap| / rate, TURN_STEP), min, TURN_MAX)` where `min = TURN_MIN_NEAR` when `|gap| < NEAR_MATCH_FACTOR x tolerance`, else `TURN_MIN`. Without a rate: after a NO_MOVEMENT round that measured turns, double its `turns_measured` (same clamp; repeat checks are skipped looking back, as today); else `FIRST_TURN`.
  - `book_turns` keeps `total_turns` and `unmeasured_turns_` in step for calibration and round turns alike.
- **Bridging** in `ui_panel_belt_tension.cpp`, behaviour left to Task 5: construct the session from `BeltResponse{runs_[i].curve, runs_[i].in_plane}`; `add_round` gets `BeltResponse{tune_curves_[i], {}}`; `start_tune_round` sends `tune_->sweep_window()`; `refresh_tune`/`push_tune_chart_data` read `zone()` where they did. Panel tuning cases that assume a zone at entry fail until Task 5; list them in the commit body.

- [ ] **Step 1: Failing tests.** Synthetic peaks use `lorentzian_curve(..., half_width_hz = 1.0f)` through `response_from` so a centroid sits within 0.2 Hz of its peak. Name every case for the rule it pins.
  - Stages: SCREW and UNKNOWN start at CALIBRATE on an unmatched pair; SPRING starts at BASELINE on the strongest candidate with the looser path adjusted; no candidates give NO_ZONE with `zone()` nullopt.
  - **Already matched (Review Focus 2):** `k1c_settled` (band 5-133) for SCREW, UNKNOWN and SPRING each: ALREADY_MATCHED, `zone()` nullopt, `adjusted_path()` A, `gap_hz()` -0.14 within 0.05, `candidates()` one with window 48.4-68.4. `calibrate_anyway()` gives CALIBRATE (SCREW, UNKNOWN; advice tighten A, `turns == 1` for SCREW) or BASELINE with `sweep_window()` {39, 88} (SPRING). `k1c_settled_repeat` is ALREADY_MATCHED too (gap 0.23). `voron24_settled` (gap 1.95, threshold 1.55) is CALIBRATE: "a 2 Hz gap is not already matched". `calibrate_anyway()` at CALIBRATE changes nothing.
  - Calibration advice: A 104 / B 100 narrow peaks give adjusted B, TIGHTEN, `turns == 1` for SCREW, nullopt for UNKNOWN. `swap_adjusted_path()` at CALIBRATE flips to A and keeps TIGHTEN.
  - **Clean steering on a real shape (Review Focus 3):** `k1c_settled` with B `shifted` by -4 Hz: CALIBRATE, adjusted B, gap -2.96 within 0.05, candidate window 46.4-66.4. `add_calibration_sweep(k1c_settled_repeat` A, B shifted -1.5`)` registers, `move_hz` 2.20 within 0.05, `hz_per_turn()` 2.20 within 0.05, stage BASELINE, `sweep_window()` {37, 86}. A second session given B shifted -4 as both calibration sweeps: step 1 move 0.16 within 0.05, not registered; step 2 `gave_up`, NO_ZONE.
  - Calibration on Voron data (Review Focus 3): session from `voron24_settled` (band 5-135, SCREW); adjusted is B. `add_calibration_sweep(voron24_turn1)` gives registered false, `turns == 1`, stage still CALIBRATE, `move_hz` +0.06 within 0.05 (the 27.8-47.8 candidate's move; the 118.6-135 zone moved 1.17 the wrong way). Then `add_calibration_sweep(voron24_turn2)` gives registered true, `move_hz` 3.43 within 0.05, stage BASELINE, zone window 118.6-135.0, `hz_per_turn()` 1.715 within 0.03, `total_turns(PATH_B) == 2`, `sweep_window()` {109, 135}, `has_next_zone()` false, `power_hint()` stronger B ratio 15.3 within 0.3.
  - Calibration with no turn: from `voron24_settled`, `add_calibration_sweep(voron24_settled_repeat)` does not register (the zone's gap moved 1.96 Hz the wrong way); a second identical call gives `gave_up` true and NO_ZONE.
  - A wrong-sign move never registers: synthetic B 100 to 97 (loosened) with A at 104 does not register however large.
  - Two candidates both moving give `registered_` order by move: `next_zone()` goes to the second and needs BASELINE; then returns false.
  - `add_calibration_sweep` outside CALIBRATE changes nothing (stage, zone, turns).
  - Baseline: the first round after registration is BASELINE (or MATCHED within tolerance), no rate sample added, stage TUNE.
  - Range (Review Focus 1): a TALLEST_UNPAIRED zone at the top of a 5-133.33 band, then an OUT_OF_WINDOW round that widens it: `sweep_window()->freq_end_hz` stays 133.
  - Turn sizes (SCREW), each reached by a calibration whose zone gap moves `rate x 1` turn: rate 2, gap -2.0 gives 1; rate 4, gap -1.3 (tolerance 1.0, under 1.5x) gives ¼; rate 8, gap -1.6 gives ½ (clamped up from ¼); rate 2, gap -10 gives 2 (clamped); a swap clears the rate and the next suggestion is 1; a NO_MOVEMENT round after 1 turn suggests 2.
  - No-turn rounds on real data (hardware verification 3): register the 118.6-135 zone from `voron24_settled` with `voron24_turn1` then `voron24_turn2` (SCREW), then `add_round(voron24_narrow_r1)` is BASELINE and `add_round(voron24_narrow_r2)` with no turn recorded is NO_MOVEMENT, though its zone gap (B-A) moved from +0.05 to -1.22 Hz.
  - Outcomes with a recorded turn and MIN_MOVE 0.8: CLOSER, OVERSHOT (advice flips), WRONG_WAY, NO_MOVEMENT (0.7 Hz), MATCHED; both paths drifting together 3 Hz is NO_MOVEMENT.
  - IMPLAUSIBLE (Review Focus 6): rate 2, one turn recorded, gap moves 7 Hz (bound max(6, 3x1x2) = 6): outcome IMPLAUSIBLE, `gap_hz()`, `previous_gap_hz()`, `reference_hz()`, `adjusted_hz()` and `hz_per_turn()` unchanged. The next round, one more turn, gap moves 2 Hz the advised way: CLOSER, `turns_measured == 2`, and the rate sample is 2 / 2 turns = 1. Without a rate (UNKNOWN), a 6.5 Hz move with no stepper is IMPLAUSIBLE; 5.5 is not.
  - Zone bins only (Review Focus 4): a round whose margin bins hold a spike 10x the zone peak gives the same gap as without it; a spike at the zone edge itself (not the margin) is OUT_OF_WINDOW.
  - NO_PEAK and `reference_lost` as today, with the floor read outside the sweep window.
  - Hints: unchanged stall rules (8 non-matched turn rounds; SPRING 2); no-turn and IMPLAUSIBLE rounds never count toward the stall.
  - **Rewritten, not deleted** (old name: new rule): `tune zone: widest pair, amplitude floor, fallback, window, matched` and `tune zone: the amplitude floor follows the in-band maximum`: Task 1's candidate cases; `tune zone on real captures`: the Voron Kalico capture gives a TALLEST_UNPAIRED candidate, window 114.5-135, and the old K1C capture now has candidates and starts at ALREADY_MATCHED or CALIBRATE by its gap; `tune session: a pair within one bin is already matched`: the ALREADY_MATCHED stage; `tune advice: amounts for screws only, spring re-seat`: the new sizes; `tune rate: median of real moves, recorded turns win`: the calibration sample is the first; `tune next_zone walks the unmatched pairs, then stops`: walks registered candidates; `tune zones: a later zone starts from its own measurement` and `tune zones: a swap sticks across zones`: stay with BASELINE; `tune peak: prominence over the unexcited floor` and `tune OUT_OF_WINDOW: ...`: on centroids and the swept floor; `turn counts read as eighth-fraction glyphs`: quarter glyphs (ruling 15); the five no-turn cases from the S2 batch keep their rules with the new constants. **Deleted with their function:** `refine peak: parabolic, nearest maximum, none in a monotonic window` (centroid and zone-bin cases replace it). Say so in the commit body.
- [ ] **Step 2:** `make t F='[belt][tune]'`. Expect FAIL.
- [ ] **Step 3:** Implement; remove `pick_tune_zone`, `refine_peak_in_window` and `NEAR_PEAK_MIN_FRACTION`. `scripts/syntax_check.py src/calibration/belt_tune_session.cpp src/ui/ui_panel_belt_tension.cpp`.
- [ ] **Step 4:** `make t F='[belt][tune]'`. Expect PASS for the session cases; panel tuning cases may fail until Task 5 (list them in the commit body).
- [ ] **Step 5:** Commit: `feat(belt): already-matched entry, calibration turn, narrow baseline and centroid tracking in the tuning session`.

### Task 4: Mock: the script plays on full sweeps, so a calibration can be scripted

**Files:**
- Modify: `src/api/moonraker_client_mock.cpp` (`dispatch_test_resonances_response` script semantics), `include/moonraker_client_mock.h` (doc comment)
- Modify: `docs/devel/MOCK_ENVIRONMENT_VARIABLES.md` (`HELIX_MOCK_BELT_TUNE_SCRIPT` paragraph)
- Test: `tests/unit/test_mock_test_resonances.cpp` (`[belt][mock]`)

**Interfaces:**
- Produces, `MoonrakerClientMock` (compiled):
  ```cpp
  /// Script one path's peak across its sweeps after the first, full or
  /// windowed; the last entry holds once the list runs out. A NaN entry
  /// writes a featureless curve (floor plus noise, no peak). While either
  /// path has a script, no sweep applies the re-measure walk.
  void set_belt_tune_script(char path_letter, std::vector<float> peaks_hz);
  ```
  With no script, behaviour is unchanged: the walk runs on full re-measures, so `--test` without a script still registers a calibration (A 110 to 106, B 98 to 102).
- [ ] **Step 1: Failing tests:**
  - **Rewritten** `full sweeps keep the re-measure walk when a script is set` as "a script turns the walk off and plays on every sweep after the first": first sweeps of A and B read 110 and 98; with script `B:98.3,102.1`, the next full B sweep reads 98.3, a windowed one 102.1, and A stays 110 across two more full sweeps.
  - `windowed sweeps never apply the re-measure walk` and the tune-script cases keep their numbers, now counted from the second sweep of the path.
- [ ] **Step 2:** `make t F='[belt][mock]'`. Expect FAIL.
- [ ] **Step 3:** Implement; update the doc paragraph (state the rule, and the no-script default calibration).
- [ ] **Step 4:** `make t F='[belt][mock]'`. Expect PASS.
- [ ] **Step 5:** Commit: `feat(mock): the belt tune script plays on full sweeps too`.

### Task 5: Panel: run kinds, already-matched and calibration phases, the narrow baseline and the revised round

**Files:**
- Modify: `include/ui_panel_belt_tension.h`, `src/ui/ui_panel_belt_tension.cpp`, `ui_xml/panel_belt_tension.xml`
- Modify: `translations/*.yml` (nine), regenerated `ui_xml/translations/*.xml`, `scripts/esp32_xml_binding_baseline.txt`
- Test: `tests/unit/test_belt_tension_panel_states.cpp` (`[belt][panel][tune]`, `[belt][panel][settle]`)

**Interfaces:**
- Consumes: Task 3's session, Task 2's `PathRun::in_plane` and `BeltResponse` callbacks, Task 4's script, the done settle path (`start_settle`, `on_settle_done`).
- Produces (header excerpts, compiled):
  ```cpp
  enum class TunePhase {
      ADJUST = 0,          ///< the user turns a tensioner, then re-measures
      SWEEPING = 1,        ///< settling, then a calibration sweep or a narrow round
      MATCHED = 2,         ///< the zone is matched (or none is left)
      CALIBRATE = 3,       ///< the user makes the calibration turn, then measures
      ALREADY_MATCHED = 4, ///< round 0 already matches: Good Enough or Calibrate Anyway
  };
  /// What a run feeds and where it lands.
  enum class RunKind {
      CHECK,       ///< Start, Re-test or Confirm: full sweeps into runs_, RUNNING then RESULTS
      CALIBRATION, ///< full sweeps of both paths for the session's calibration step
      TUNE_ROUND,  ///< both paths over the session's sweep_window()
  };
  /// ALREADY_MATCHED's secondary action: go on to the calibration turn.
  void handle_tune_calibrate_anyway_clicked();
  void begin_run(const std::vector<helix::calibration::BeltPath>& queue, bool clear_previous,
                 RunKind kind = RunKind::CHECK, bool settle_first = true);
  void on_sweep_complete(helix::calibration::BeltPath path,
                         helix::calibration::BeltResponse response);
  /// Sweep both paths over the session's sweep_window(). Only the BASELINE
  /// chained onto a calibration sweep skips the laps: no belt changed between.
  void start_tune_round(bool settle_first = true);
  /// The calibration run's two full sweeps are in: feed the session, then
  /// chain the BASELINE, ask for another turn, or give up.
  void finish_calibration();
  lv_subject_t tune_power_hint_subject_{};
  char tune_power_hint_buf_[256] = {};
  lv_subject_t tune_has_power_hint_subject_{};
  RunKind run_kind_ = RunKind::CHECK;
  /// The latest calibration or round sweeps; valid once tune_has_curves_.
  helix::calibration::BeltResponse tune_curves_[2];
  /// The latest calibration step's verdict; cleared when a zone is entered.
  std::optional<helix::calibration::BeltCalibrationResult> tune_last_calibration_;
  ```
  `begin_run` loses its `window` parameter: `run_window_` = `tune_->sweep_window()` for TUNE_ROUND, nullopt otherwise, so the session is the one source of a window. `settle_first = false` skips `start_settle()` and goes straight to `start_next_measurement()`.
- New subjects: `bt_tune_power_hint` (string), `bt_tune_has_power_hint` (int). `bt_tune_phase` gains 3 = CALIBRATE and 4 = ALREADY_MATCHED.
- Flow:
  - **Routing by `run_kind_`:** `on_sweep_complete` writes `runs_[i]` (curve + in_plane, `has`, `measured_at_ms`) for CHECK and `tune_curves_[i]` otherwise; `finish_run` goes to RESULTS (CHECK), `finish_calibration()` (CALIBRATION) or `finish_tune_round()` (TUNE_ROUND). CALIBRATION and TUNE_ROUND keep the view on TUNING, phase SWEEPING. Stop, Back, a rebuild, the gate and the stall guard treat a calibration run as they treat a tuning round.
  - **Tune:** refuse with `on_error(lv_tr("The printer has not reported its [resonance_tester] range, so a narrow sweep cannot be kept inside it."))` when `!sweep_cfg_.from_printer` (ruling 2). Session from `BeltResponse{runs_[i].curve, runs_[i].in_plane}`. Stage ALREADY_MATCHED: phase 4. CALIBRATE: phase 3. BASELINE (SPRING): run the baseline round at once (settling). NO_ZONE: phase MATCHED, instruction "Nothing to tune here".
  - **ALREADY_MATCHED:** instruction "Belts already match"; last line "Within {0:.1f} Hz of each other at {1:.0f} Hz" (|gap|, the adjusted centroid); the gauge shows the gap; buttons Good Enough (today's `btn_tune_done`, back to RESULTS with no sweep) and a new `btn_tune_calibrate_anyway` "Calibrate Anyway" (`belt_tension_tune_calibrate_anyway_cb`), both shown only at phase 4; stepper, swap and Re-measure hidden. Calibrate Anyway: `tune_->calibrate_anyway()`, then phase 3 (or the SPRING baseline round).
  - **CALIBRATE:** instruction "Tighten {0} one full turn" (SCREW) or "Tighten {0} noticeably" (UNKNOWN); stepper hidden (`bt_tune_phase eq 3`); swap allowed (`handle_tune_swap_clicked` accepts phase 3); the measure buttons read "Measure (~{} min)" (`full_check_minutes()`). Measure: `begin_run({A, B}, false, RunKind::CALIBRATION)`. `finish_calibration()`: registered, then `start_tune_round(false)` (the BASELINE); not registered, stay CALIBRATE with last line "That turn didn't register yet. Tighten {0} one more full turn." (UNKNOWN: "That turn didn't register yet. Tighten {0} a little more."); gave up, phase MATCHED, last line "Nothing followed {1} turns of {0}. Turn {0} back {1} turns, then check the mechanics." (UNKNOWN: "Nothing followed the turns on {0}. Undo them, then check the mechanics.") and the mechanical hint row shown.
  - Rounds sweep `tune_->sweep_window()`; `round_minutes` takes that window; the zone label and the chart read `zone()->window`.
  - Stepper: unchanged rules (starts at "Not turned" after every round, a swap, Next Zone and Calibrate Anyway), now ¼ steps clamped to [0, 2].
  - Last-line texts: add IMPLAUSIBLE "That reading jumped too far to trust. Re-measure without turning."; keep "No turn: the gap moved {:.1f} Hz".
  - Power hint: `bt_tune_power_hint` = "Path {0} resonates much more than {1} at {2:.0f} Hz. That points at a mechanical difference (pulley grub screws, idler bearings, belt tracking), not tension." when `power_hint()` holds, `bt_tune_has_power_hint` 1; a second warning row under `bt_tune_hint_row`, same `belt_tension_tune_why_cb`.
  - Shape warning text becomes "Curves differ in shape. Check the mechanics before chasing a perfect match."; the Why modal's last sentence becomes "Tuning still follows the zone that moved with the turn, but check these before chasing a perfect match."
  - Rate line: `~{:.1f} Hz / turn` (renamed key from `~{:.0f} Hz / turn`, values kept with the precision changed).
  - Chart in TUNING: `tuning_curve()` of the round-0 runs (ALREADY_MATCHED and CALIBRATE: over the band) or the latest sweeps (later: over the zone, not the sweep window); hollow rings at `reference_hz()`/`adjusted_hz()` (centroids). The adjusted path's earlier rounds stay as muted ghosts on their own scale.
- [ ] **Step 1: Failing tests** (mock script `B:98.3,102.1,102.1,105.0,108.5,109.9`, A at 110, B at 98; SCREW printer; if the mock's broad peaks compress the centroid moves so a step below misses its outcome, adjust the script values and keep the outcome sequence):
  - "Tune opens on the calibration turn" (rewrite of `Tune opens on the looser belt's instruction`): phase 3, instruction "Tighten B one full turn", stepper hidden.
  - **"matched belts open on Good Enough" (Review Focus 2):** `set_belt_peaks_hz(100, 100)`, Start, Tune: phase 4, instruction "Belts already match", `btn_tune_calibrate_anyway` visible, the stepper hidden; Good Enough lands on RESULTS with no new `TEST_RESONANCES` in history; a second Tune then Calibrate Anyway gives phase 3 "Tighten A one full turn" (or B, by the centroids).
  - **"Tune refuses a range the printer never reported" (Review Focus 1):** with the `[resonance_tester]` query failing (mock knob or `sweep_cfg_.from_printer = false` through the fixture), Tune lands on ERROR with the range sentence and sends nothing.
  - "a calibration turn that doesn't register asks for another": Measure gives the not-registered line and stays in phase 3; history shows one settle script and two full sweeps (no `FREQ_START`); `runs_` unchanged.
  - "the second turn registers and chains a narrow baseline without settling": Measure again; history then holds one more settle, two full sweeps, then two windowed sweeps with no settle script between them; `FREQ_START`/`FREQ_END` equal the session's `sweep_window()` exactly and lie inside the mock's `[min_freq, max_freq]`; phase ADJUST with no verdict line.
  - "three Re-measures walk CLOSER, CLOSER, MATCHED" (rewrite): each Re-measure after stepping the stepper to the advised amount; every round settles first.
  - "an untouched stepper is a repeat check" (keep, new numbers): Re-measure without stepping gives the "No turn" line, never the wrong-way line; the stepper reads "Not turned" after every outcome, a swap, Next Zone and Calibrate Anyway.
  - "the instruction carries the advised amount": "Tighten B 1 turn" style text for SCREW; no amount for UNKNOWN.
  - "the power hint names the stronger path": a run where B's zone power exceeds A's 3x (scale one path's mock PSD, or construct through the fixture's session hooks) shows `bt_tune_has_power_hint == 1` and the B sentence.
  - "a calibration that gives up says to turn back": script `B:98.2,98.4` gives phase MATCHED with the "Turn B back 2 turns" line.
  - "a spring printer skips calibration" (rewrite of `a spring tensioner gets the re-seat instruction and no screw controls`): SPRING runs its baseline round straight from Tune.
  - "Next Zone walks only registered zones" (rewrite of `Next zone measures the new zone before advising`).
  - "Stop during a calibration run lands on START" and "a rebuild mid calibration run drops the late callbacks" (Review Focus 5), beside today's tuning-round twins.
  - Rewrites for wording or units: `each outcome has its own last-turn line` (adds IMPLAUSIBLE, the calibration lines and the already-matched line), `the turn stepper shows only for screw tensioners` (¼ steps to 2), `Next zone names how long its sweep takes` (sweep window, laps included), `a wrong-way line names the turn the user made` (steps the stepper first), `a round with no peak says so and can be re-measured` (the flat item now counts from the second sweep), `Confirm runs one full pair and reports the tuning` (the summary in quarters, calibration turns included), `Confirm settles once before its pair`, `every locale's tuning strings fit their buffers` (every new string, every locale; grow a buffer to the next power of two if one overflows and say so in the commit).
- [ ] **Step 2:** `make t F='[belt][panel]'`. Expect FAIL.
- [ ] **Step 3:** Implement C++ and XML; `make translation-sync && make translations`; the binding-baseline lines.
- [ ] **Step 4:** `make t F='[belt]'` and `make t F='[i18n]'`. Expect PASS, every belt case.
- [ ] **Step 5:** Commit: `feat(belt): the tuning loop notices matched belts and picks its zone with a calibration turn`.

### Task 6: Docs

**Files:**
- Modify: `docs/devel/BELT_TENSION.md`: "Tuning loop" rewritten for in-plane curves, candidates, already-matched entry, the calibration turn, the narrow baseline and its asymmetric margins, the range clamp (and why: Klipper takes a `FREQ_END` up to 300 Hz), centroid tracking, the sanity bound, turn sizes, the power hint; "Peak refinement" replaced by "Centroid tracking"; the thresholds table regenerated from the constants; the settling-laps section checked against the code (S1 wrote none); "What the hardware data says" from this plan, cited to the fixtures. Code cited as `path#symbol`.
- Modify: `docs/user/guide/calibration.md` § Belt Tension (already-matched, the calibration turn, the power hint); `docs/devel/CHANGELOG_1_1_DRAFT.md` (one entry).
- Delete: `docs/devel/plans/2026-10-15-belt-tune-loop-revision-design.md` and this plan, with `2026-10-09-belt-tune-loop-design.md` if the branch lands in one piece.

- [ ] **Step 1:** Write the docs. `make check-doc-anchors` (advisory) for the new citations.
- [ ] **Step 2:** Commit: `docs(belt): the revised tuning loop`.

### Finish (the controller, once, not an agent task)

- [ ] One review of the whole batch (`docs/devel/REVIEW_RUBRIC.md`, the Review Focus above). A second round only for a crash, lifetime, data-loss or wrong-hardware-command finding.
- [ ] `make mutate-diff MUTATE_ARGS="--tests '[belt],[shaper_csv],[settle]'"`; each surviving mutant in `belt_tune_session.cpp`, the parser or the panel's run routing gets a test (tags only to verify).
- [ ] Headless mock at 800x480, `--sim-speed 50`: Test Both then Tune with matched peaks (phase 4), Calibrate Anyway (phase 3), Measure, the chained baseline, a power-hint round; `ctl text` on the instruction, last line and rate; `ctl geom` of the hero, both hint rows, the stepper and the phase-4 buttons inside 676x404; one screenshot per phase.
- [ ] ONE `make land-gate` on the combined result.
- [ ] Batch landing per CLAUDE.md, worktree teardown, a comment on #1736.
- [ ] Hardware, with Preston's OK: on the K1C, Tune Belts from the hands-off state shows "Belts already match", and no windowed sweep ever sends `FREQ_END` above 133. On the Voron, from A +1 off the tuned position, the design's four checks; expect the first calibration turn to report "didn't register" (see the data table) and record whether the second does.


## Addendum (2026-10-18): repeatability before advice (design §3a)

Owned by **Task 3** (session, pure) and **Task 5** (panel). Implementers of those tasks
include it; the reviewer checks it.

- Session: `inline constexpr float REPEAT_MAX_GAP_HZ = 1.5f;` and
  `inline constexpr float REPEAT_MAX_POWER_RATIO = 2.0f;` in `belt_tune`. A BASELINE takes two
  rounds of both paths (`add_round` twice, no turn between). New stage `UNREPEATABLE` (or an
  equivalent outcome on the second baseline round) when the in-plane centroid gap of the two
  rounds differs by more than REPEAT_MAX_GAP_HZ, or either path's in-plane zone power ratio
  (max/min of the two rounds) exceeds REPEAT_MAX_POWER_RATIO. In UNREPEATABLE, `advice()` gives
  no direction or amount; a fresh BASELINE pair (Re-measure) may clear it. Tests: the Voron
  10-15 narrow no-turn CSVs (hw-2026-10-15/narrow/*tuned*, *nolaps*) land UNREPEATABLE; the
  K1C pair (hw-2026-10-15/k1c base r1/r2 trimmed to the zone) does not; boundary cases at
  exactly 1.5 Hz and 2.0x.
- Panel: the BASELINE run sweeps the pair twice (four narrow sweeps, one settle before the
  first); UNREPEATABLE shows the line "These readings aren't repeatable, so tuning can't be
  steered. Check the mechanics first." with the Why modal, hides the turn instruction and the
  stepper, and offers Re-measure and Good Enough. Duration estimates count both sweeps.
  Translations synced.
