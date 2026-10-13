# Belt Tension Guided Tuning Loop Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the Belt Tension comparison into a guided loop: advise which belt to turn and how, re-measure only the disagreeing zone on both paths with short windowed sweeps, and repeat until the peaks line up.

**Architecture:** A pure `BeltTuneSession` (no LVGL, no I/O) owns zone choice, peak refinement, advice, rate learning and round outcomes. The existing sweep path gains an optional frequency window end to end (API, collector progress, calibrator). The panel gains a fifth view state, TUNING, that drives the session, plus a small gap-gauge widget. A printer-database field says whether the printer has screw tensioners, which gates the turn amount.

**Tech Stack:** C++17, LVGL 9.5 + helix-xml, Catch2, the Moonraker mock (`--test`).

**Spec:** `docs/devel/plans/2026-10-09-belt-tune-loop-design.md` (read it first; it holds the rulings, thresholds and UI). Mockups: https://claude.ai/artifact/AYJMD8kG24LgxnnHts1tNK

Per the project's rule, this plan states interfaces, tests and constraints. Bodies are the implementer's.

## Global Constraints

- Worktree: `scripts/setup-worktree.sh feature/belt-tune-loop` (directory `.worktrees/belt-tune-loop`). Claim `worktree:belt-tune-loop` for its lifetime.
- Inner loop is `make t F='[belt]'` (or the task's narrower tag); `scripts/syntax_check.py <file>` for compile checks. ASAN, `make mutate-diff` and `make full-test-run` run ONCE, at the end (Task 8).
- Every `make`/helix-screen/screenshot run headless: `HELIX_HEADLESS=1 SDL_VIDEODRIVER=dummy`, pinned `ctl` socket and `HELIX_CONFIG_DIR` per the CLAUDE.md box.
- `include/belt_tension_types.h` and `src/api/moonraker_advanced_api.cpp` compile into the ESP32 firmware: no exceptions, no `std::stof`/`stoi`, no `json.value()`, no RTTI there. Every new `src/**/*.cpp` gets a line in `firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt` (belt files are excluded; see lines 111-118) or `scripts/check_esp32_app_srcs.py` fails.
- User-facing strings: `lv_tr("...")` in C++, literal `text=` in XML; then `make translation-sync && make translations`, and commit the YAMLs plus `ui_xml/translations/*.xml`.
- Colors and sizes from tokens (`#belt_path_a`, `#belt_path_b`, `#success`, `#warning`, `#danger`, `#space_*`). Appearance in XML, data in C++ (`.claude/rules/declarative-ui.md`).
- Threading: every collector/calibrator callback reaches the panel through `lifetime_.bg_cb(...)` as today; nothing new touches LVGL off the main thread.
- Comments describe the code as it is: no history, no "used to", no SHAs (issue cites like `#1736` are fine).
- Clockwise = tighten. The screw graphic, turn amount, stepper and Hz/turn show ONLY for `BeltTensionerKind::SCREW`.
- Thresholds (spec values, as named constants in `belt_tune_session.h`): `ZONE_MIN_AMPLITUDE_FRACTION = 0.20f`, `ZONE_HALF_MIN_HZ = 10.0f`, `ZONE_MARGIN_HZ = 5.0f`, `MATCHED_MIN_HZ = 1.0f`, `MIN_MOVE_HZ = 0.5f`, `FIRST_TURN = 0.25f`, `TURN_STEP = 0.125f`, `TURN_MAX = 1.0f`, `STALL_ROUNDS = 8`, `SPRING_STALL_ROUNDS = 2`, `GAUGE_WARN_HZ = 3.0f`.
- Commits: `feat(belt): ... (prestonbrown/helixscreen#1736)`, subject plus a short paragraph.

## Review Focus

1. **The adjusted peak leaves the window** after a big turn (it lands at the window edge, or outside). Expected: the round reports the peak as out of window, the session re-centres the window on the reference peak widened to include the edge, and the next sweep covers it. Never a gap computed from an edge bin. Test in Task 4.
2. **Leaving mid-loop:** Back, Stop or a disconnect while a windowed sweep runs. Expected: the same cancel/abort path as RUNNING, with no late callback touching a destroyed panel; Stop lands on START. Test in Task 6.
3. **Swapping the adjusted path after rounds:** the learned Hz/turn belongs to the other belt. Expected: the rate history clears, the next amount restarts at ¼ turn, and the zone keeps its window. Test in Task 4.
4. **Firmware that ignores `FREQ_START`/`FREQ_END`** runs a full sweep. Expected: progress percent stays clamped to 0-100 against the window, the session uses only in-window bins, and nothing crashes. The sweep just takes longer. Test in Task 1.
5. **A windowed curve with no local maximum** (flat or monotonic: a dead accelerometer, a wrong axis). Expected: `refine_peak_in_window` returns `nullopt`, and the round reports "No clear peak in this zone" with Re-measure available. No NaN gap, no advice. Test in Task 4, panel wiring in Task 6.

---

### Task 1: Windowed sweeps through the API, collector and calibrator

**Files:**
- Modify: `include/belt_tension_types.h` (add `BeltSweepWindow`, `BeltTensionerKind` next to `BeltPath`)
- Modify: `include/i_moonraker_sub_apis.h:429-436`, `include/moonraker_advanced_api.h:528-532`, `src/api/moonraker_advanced_api.cpp:1972-2114` (collector), `:3273-3325` (entry)
- Modify: `include/belt_tension_calibrator.h:113-114`, `src/calibration/belt_tension_calibrator.cpp:86-150`
- Test: `tests/unit/test_moonraker_api_belt_resonance.cpp` (`BeltApiFixture`, `[belt][api]`), `tests/unit/test_belt_tension_calibrator.cpp` (`[belt_tension][calibrator]`)

**Interfaces:**
- Produces:
  ```cpp
  struct BeltSweepWindow { float freq_start_hz = 0.0f; float freq_end_hz = 0.0f; };
  enum class BeltTensionerKind { UNKNOWN, SCREW, SPRING };
  // IAdvancedAPI / MoonrakerAdvancedAPI
  [[nodiscard]] BeltRunCancel test_belt_resonance(const std::string& axis_param,
      const std::string& output_name, BeltSweepProgressCallback on_progress,
      BeltCurveCallback on_complete, ErrorCallback on_error,
      std::optional<calibration::BeltSweepWindow> window = std::nullopt);
  // BeltTensionCalibrator
  void measure_path(BeltPath path, std::function<void(int, float)> on_progress,
      std::function<void(BeltCurve)> on_complete, BeltErrorCallback on_error,
      std::optional<BeltSweepWindow> window = std::nullopt);
  ```
- G-code with a window: `TEST_RESONANCES AXIS=<a> FREQ_START=<lo> FREQ_END=<hi> OUTPUT=resonances NAME=<n> SWEEPING_PERIOD=0`, with lo and hi formatted `{:.0f}` (lo floored, hi ceiled). Without one, the string is byte-identical to today's.
- Collector: when a window is given, `set_config(cfg)` stores `cfg` with `min_freq`/`max_freq` replaced by the window, so `sweep_percent` scales against it. Keep the config hold as it is.

- [ ] **Step 1: Failing tests.** In `test_moonraker_api_belt_resonance.cpp`:
  - "a windowed sweep sends FREQ_START and FREQ_END": window {88.4, 119.6} gives `FREQ_START=88 FREQ_END=120` in `gcode_script_history()`.
  - "a sweep without a window sends today's G-code": exact string equality with the current format.
  - "windowed progress scales against the window": with mock lines at 88, 104 and 120 Hz, progress reads 0, 50 and 100.
  - "progress stays clamped when firmware ignores the window" (Review Focus 4): lines at 5 and 135 Hz against window 88-120 give 0 and 100, never outside.

  In `test_belt_tension_calibrator.cpp`: "measure_path forwards its window", asserting the forwarded G-code carries the window. The mock must accept the parameters first; until Task 2 lands, assert on the G-code history only.
- [ ] **Step 2:** `make t F='[belt][api]'`. Expect FAIL (no overload, no FREQ_START).
- [ ] **Step 3:** Implement. Use `<optional>`; keep `BeltSweepWindow` header-only. `syntax_check.py` both headers.
- [ ] **Step 4:** `make t F='[belt]'`. Expect PASS, including every pre-existing belt test.
- [ ] **Step 5:** Commit: `feat(belt): sweeps can be limited to a frequency window`.

### Task 2: Mock honours windows and plays a scripted tuning run

**Files:**
- Modify: `src/api/moonraker_client_mock.cpp:6184-6290` (`dispatch_test_resonances_response`), constructor env parsing `:327-378`, `include/moonraker_client_mock.h:~2173-2183`
- Modify: `docs/devel/MOCK_ENVIRONMENT_VARIABLES.md` belt section (~1330-1413)
- Test: `tests/unit/test_mock_test_resonances.cpp` (`BeltMockFixture`, `[belt][mock]`)

**Interfaces:**
- Consumes: the G-code from Task 1.
- Produces:
  - The mock parses `FREQ_START=`/`FREQ_END=` (via `text_io::parse_leading`) and emits `Testing frequency` lines only across the window. The CSV still spans the configured range, as Klipper's does.
  - `HELIX_MOCK_BELT_TUNE_SCRIPT="B:102.1,107.6,109.8"`: the path letter, then that path's peak for successive WINDOWED sweeps of it. When exhausted, the last value holds. Windowed sweeps never apply the existing ±4 Hz re-measure walk. Full sweeps keep it unchanged.
  - Setter for tests: `void set_belt_tune_script(char path_letter, std::vector<float> peaks_hz);`
  - Malformed script values (an empty item, a letter other than A/B, a non-number) are skipped with an `spdlog::warn`, never thrown.
  - The script item `flat` writes a featureless curve (floor plus noise, no peak) for that sweep, so tests and `--test` can reach the NO_PEAK round.

- [ ] **Step 1: Failing tests:**
  - "a windowed mock sweep emits lines only inside the window": first line 88 Hz, last 120 Hz.
  - "the tune script moves the scripted path's peak per windowed sweep": three windowed B sweeps peak at 102.1, 107.6 and 109.8 (read back from the CSV via `parse_resonance_csv`, ±0.8 Hz). A windowed A sweep stays at 110.
  - "full sweeps keep the re-measure walk when a script is set": unchanged behaviour.
  - "a malformed script is ignored item by item": `"B:102,abc,,109"` gives 102, then 109.
  - "a flat script item writes a curve with no peak": `"B:flat"` gives a CSV whose in-window maximum is within 10% of its floor.
- [ ] **Step 2:** `make t F='[belt][mock]'`. Expect FAIL.
- [ ] **Step 3:** Implement. Document `HELIX_MOCK_BELT_TUNE_SCRIPT` beside `HELIX_MOCK_BELT_A_HZ`, in that section's style.
- [ ] **Step 4:** `make t F='[belt]'`. Expect PASS.
- [ ] **Step 5:** Commit: `feat(mock): windowed TEST_RESONANCES and a scripted belt tuning run`.

### Task 3: Printer database `belt_tensioner`

**Files:**
- Modify: `assets/config/printer_database.json` (Voron 2.4 ~:859 and Voron Trident ~:1109 get `"belt_tensioner": "screw"`; nothing else)
- Modify: `include/printer_detector.h` (~:585), `src/printer/printer_detector.cpp` (~:1669, beside `get_toolhead_style`)
- Test: `tests/unit/test_printer_detector.cpp` (`[printer][db_shape]` neighbourhood)

**Interfaces:**
- Produces: `static helix::calibration::BeltTensionerKind PrinterDetector::get_belt_tensioner(const std::string& printer_name);`. `"screw"` gives SCREW, `"spring"` gives SPRING, and absent, empty, wrong-typed or any other string gives UNKNOWN. Implement on `printer_field`.
- UI callers get the name from `get_printer_state().profile_state().printer_type()`.

- [ ] **Step 1: Failing tests:** Voron 2.4 and Trident are SCREW; a K1C is UNKNOWN (deliberately unset); an unknown printer name is UNKNOWN; "wrong-typed entry fields read as absent" gains a `belt_tensioner: 3` case that reads UNKNOWN.
- [ ] **Step 2:** `make t F='[printer]'`. Expect FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** `make t F='[printer]'`. Expect PASS.
- [ ] **Step 5:** Commit: `feat(printers): belt_tensioner names screw-tensioned printers`.

### Task 4: `BeltTuneSession`, the pure tuning logic

**Files:**
- Create: `include/belt_tune_session.h`, `src/calibration/belt_tune_session.cpp`
- Modify: `firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt` (add the new cpp beside the belt entries)
- Create: `tests/unit/test_belt_tune_session.cpp` (`[belt][tune]`)

**Interfaces:**
- Consumes: `BeltComparison`, `BeltCurve`, `BeltPeak`, `detect_belt_peaks`, `pair_belt_peaks` (`belt_tension_types.h`); `BeltSweepWindow` and `BeltTensionerKind` from Task 1.
- Produces: exactly the spec's `helix::calibration` block (`BeltZoneSource`, `BeltTuneZone`, `pick_tune_zone`, `refine_peak_in_window`, `matched_tolerance_hz`, `BeltTuneOutcome`, `BeltTurnDirection`, `BeltTuneAdvice`, `BeltTuneRound`, `BeltTuneSession`), with these additions:
  ```cpp
  enum class BeltTuneOutcome { CLOSER, OVERSHOT, WRONG_WAY, NO_MOVEMENT, MATCHED, NO_PEAK, OUT_OF_WINDOW };
  struct BeltTuneAdvice {
      BeltPath path; BeltTurnDirection direction;
      std::optional<float> turns;   // SCREW only, multiple of TURN_STEP
      bool spring_reseat = false;   // SPRING: release, settle, lock
  };
  // BeltTuneSession
  [[nodiscard]] BeltTensionerKind tensioner() const;
  [[nodiscard]] float reference_hz() const;     // latest round's reference peak
  [[nodiscard]] float adjusted_hz() const;
  [[nodiscard]] std::optional<float> previous_gap_hz() const;  // hollow gauge needle
  [[nodiscard]] float total_turns() const;      // signed sum, SCREW only, for the RESULTS facts
  [[nodiscard]] int total_rounds() const;       // across zones
  ```
  `refine_peak_in_window` returns `nullopt` when the window holds no interior local maximum. A maximum on the first or last in-window bin is reported as `OUT_OF_WINDOW` by `add_round`, which re-centres the zone window on the reference peak, widened to include that edge (still clamped to the band).

- [ ] **Step 1: Failing tests.** Add a shared header `tests/unit/belt_test_curves.h` with `BeltCurve lorentzian_curve(std::vector<std::pair<float,float>> peaks_hz_height, float lo, float hi, float bin_hz = 3200.0f/4096.0f, float floor = 0.01f)`, and move `test_belt_compare.cpp`'s file-local `curve_with_peak` onto it (one helper, two callers). Cases, one TEST_CASE each:
  - zone: a paired gap picks the widest pair above the amplitude floor; a pair under 20% amplitude is ignored; tallest-unpaired fallback when both tallest peaks are unpaired (`source == TALLEST_UNPAIRED`); no pairs and no peaks give `nullopt`; the window is the midpoint ± max(10, gap/2+5), clamped to the band at both ends; already-matched centres are skipped.
  - real captures: `voron24_kalico` picks TALLEST_UNPAIRED; `k1c` (98.9%) yields either `nullopt` or a zone whose gap is within tolerance (load through the existing `parse_resonance_csv` fixture path).
  - refine: an off-bin peak at 104.3 Hz refines to within 0.2 Hz; `near_hz` picks the nearer of two maxima; a monotonic curve gives `nullopt` (Review Focus 5).
  - tolerance: `max(1.0, bin width)`.
  - default adjusted path is the lower-peak path; `swap_adjusted_path` flips advice direction and clears the rate history (Review Focus 3).
  - advice: SCREW round 1 is ¼ turn; after a round moving 4.1 Hz for ¼ turn, an 8 Hz gap gives ½; rounding to ⅛ with clamps at ⅛ and 1; UNKNOWN and SPRING never carry `turns`; SPRING sets `spring_reseat`.
  - outcomes: CLOSER, OVERSHOT (sign change; advice direction flips), WRONG_WAY (moved ≥ 0.5 Hz against the advice), NO_MOVEMENT (< 0.5 Hz), MATCHED (|gap| ≤ tolerance), NO_PEAK, OUT_OF_WINDOW (Review Focus 1: window re-centred and widened, gap not updated).
  - rate: the median over rounds with moves ≥ 0.5 Hz; sub-threshold rounds excluded; `record_turns` overrides the suggestion.
  - hints: `show_mechanical_hint()` for TALLEST_UNPAIRED, after 8 non-matched rounds in one zone (SCREW/UNKNOWN), and after 2 for SPRING.
  - `next_zone` advances to the next unmatched pair, then returns false.
- [ ] **Step 2:** `make t F='[belt][tune]'`. Expect FAIL (no header).
- [ ] **Step 3:** Implement. `syntax_check.py include/belt_tune_session.h src/calibration/belt_tune_session.cpp`.
- [ ] **Step 4:** `make t F='[belt]'`. Expect PASS.
- [ ] **Step 5:** Commit: `feat(belt): the tuning loop's zone, peak and advice logic`.

### Task 5: Gap gauge widget

**Files:**
- Create: `include/ui_belt_gap_gauge.h`, `src/ui/ui_belt_gap_gauge.cpp` (model on `src/ui/ui_belt_path_sketch.cpp`: registration, XML element, draw)
- Modify: the ESP32 exclusion list; component registration where `ui_belt_path_sketch` registers
- Test: `tests/unit/test_belt_gap_gauge.cpp` (`[belt][gauge]`)

**Interfaces:**
- Produces: XML element `<belt_gap_gauge name=... />` drawing a track, a matched band of ±tolerance around zero, a filled needle at the gap, a hollow needle at the previous gap, and an arrow from previous to current. It draws no text and no end labels (spec ruling). C++ API:
  ```cpp
  void ui_belt_gap_gauge_set(lv_obj_t* gauge, float gap_hz, std::optional<float> previous_gap_hz,
                             float tolerance_hz, float range_hz);
  // pure, for tests:
  int belt_gap_gauge_x(float gap_hz, float range_hz, int track_px);  // clamped to the track
  enum class GapSeverity { MATCHED, NEAR, FAR };
  GapSeverity belt_gap_severity(float gap_hz, float tolerance_hz);  // MATCHED <= tol, NEAR <= 3 Hz
  ```
  Colors come from tokens (`#success`/`#warning`/`#danger`, `#text_muted`); a theme change re-reads them, as the chart does.
- [ ] **Step 1: Failing tests:** x mapping (0 centred, ± range at the ends, beyond the range clamped); severity bands at tolerance and 3 Hz; set() with and without a previous gap creates no children and does not crash when called before layout (zero size).
- [ ] **Step 2:** `make t F='[belt][gauge]'`. Expect FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** PASS.
- [ ] **Step 5:** Commit: `feat(belt): a gap gauge for the tuning loop`.

### Task 6: TUNING view in the Belt Tension panel

**Files:**
- Modify: `include/ui_panel_belt_tension.h` (`ViewState::TUNING = 4`, session member, subjects), `src/ui/ui_panel_belt_tension.cpp` (flow, callbacks, chart), `ui_xml/panel_belt_tension.xml` (state 4 block, Tune button)
- Test: `tests/unit/test_belt_tension_panel_states.cpp` (`BeltPanelFixture`, `[belt][panel]`)

**Interfaces:**
- Consumes: Tasks 1-5.
- New subjects, all registered with the others at cpp:131-161:

  | Subject | Type | Meaning |
  |---|---|---|
  | `bt_tune_phase` | int | 0 ADJUST, 1 SWEEPING, 2 MATCHED |
  | `bt_tune_kind` | int | 0 UNKNOWN, 1 SCREW, 2 SPRING |
  | `bt_tune_dir` | int | 0 tighten, 1 loosen |
  | `bt_tune_path` | int | adjusted path, 0 A / 1 B (colors the instruction) |
  | `bt_tune_instruction` | string | "Tighten B", "Loosen A", "Release B's tensioner, let the spring settle, lock it" |
  | `bt_tune_amount` | string | "¼ turn" (SCREW) |
  | `bt_tune_gap` | string | "2.4 Hz" |
  | `bt_tune_gap_caption` | string | "B looser than A" / "within one bin" |
  | `bt_tune_round` | string | "Round 3" |
  | `bt_tune_zone` | string | "Zone 88-120 Hz" |
  | `bt_tune_rate` | string | "~14 Hz / turn", empty when unknown or not SCREW |
  | `bt_tune_last` | string | "last turn: 5.5 Hz closer", plus the WRONG_WAY / NO_MOVEMENT / NO_PEAK lines from the spec table |
  | `bt_tune_shape_warning` | int | show the "Curves differ in shape" line |
  | `bt_tune_has_next_zone` | int | |
  | `bt_tune_swap_label` | string | "Adjust A instead" |
  | `bt_tune_cta` | string | "Tune belts" / "Fine-tune" by verdict |

- New XML callbacks: `belt_tension_tune_cb`, `belt_tension_tune_measure_cb`, `belt_tension_tune_done_cb` (Good enough), `belt_tension_tune_swap_cb`, `belt_tension_tune_amount_dec_cb`, `belt_tension_tune_amount_inc_cb`, `belt_tension_tune_next_zone_cb`, `belt_tension_tune_confirm_cb`, `belt_tension_tune_keep_cb`, `belt_tension_tune_why_cb`.
- Flow:
  - Tune: build `BeltTuneSession` from `runs_[0/1].curve`, the last comparison, the sweep band and `PrinterDetector::get_belt_tensioner(printer_type)`, then go to TUNING/ADJUST. No zone means straight to the confirm offer.
  - Re-measure: `run_after_ram_check`, then `begin_run({A, B})` with the zone window, through the existing queue, stall guard and Stop; phase SWEEPING; on both complete, `session.add_round`, then ADJUST or MATCHED.
  - Confirm: the existing full `begin_run({A,B}, true)` with `was_similarity_percent_` set to the round-0 similarity, landing on RESULTS. The facts line adds "Tuned in N rounds" plus, for SCREW, "B tightened about ⅞ turn".
  - Good enough: RESULTS as last shown, no sweep.
  - Stop during a tuning sweep: the existing `handle_stop_clicked` (abort to START).
- Chart in TUNING: x range = zone window; reference and adjusted solid with hollow markers at their refined peaks; up to two previous adjusted-path rounds as muted series (allocate two extra ghost ids per path in `ensure_chart`; `MAX_SERIES` is 13); sweep cursor while SWEEPING. RESULTS restores the full band. Leave out the spec's dashed reference line: the chart has no API for one, and the hollow marker carries that peak.
- Shape warning "Why? ›": `helix::ui::modal_alert` with the causes list (same pattern as `_help_cb`).
- Tune button: added to the RESULTS row, `bind_text="bt_tune_cta"`, primary variant.

- [ ] **Step 1: Failing tests,** driving the mock (`set_belt_peaks_hz`, `set_belt_tune_script('B', {102.1f,107.6f,109.8f})`):
  - the Tune button reads "Tune belts" on Poor/Fair and "Fine-tune" on Good;
  - Tune enters state 4/ADJUST with the instruction "Tighten B", the zone around 98-110 and the shape warning shown (the default mock peaks are unpaired);
  - three Re-measures walk CLOSER, CLOSER, MATCHED; the G-code history shows windowed sweeps of both paths each round;
  - with no tensioner kind (mock printer UNKNOWN) the amount string is empty and the stepper hidden; with SCREW, the amount steps by ⅛ and clamps;
  - Good enough lands on RESULTS with no new G-code;
  - Confirm runs one full A+B pair and lands on RESULTS with notes ending "· was N%";
  - Stop during a tuning sweep lands on START; destroying the panel mid-sweep drops the late callbacks without a crash (Review Focus 2);
  - a NO_PEAK round (script `B:flat`) shows "No clear peak in this zone" and keeps Re-measure enabled (Review Focus 5).
- [ ] **Step 2:** `make t F='[belt][panel]'`. Expect FAIL.
- [ ] **Step 3:** Implement C++ and XML (XML hot-reloads; C++ needs `make`).
- [ ] **Step 4:** `make t F='[belt]'`. Expect PASS.
- [ ] **Step 5:** Drive it: launch the mock headless at 800x480 with `HELIX_MOCK_BELT_TUNE_SCRIPT="B:102.1,107.6,109.8" --sim-speed 50`, then `ctl` through Tune, three Re-measures and Confirm, with `ctl geom` on the hero blocks and chart (no overlaps; every element inside 676x404) and a screenshot of each phase.
- [ ] **Step 6:** Commit: `feat(belt): guided tuning loop on the Belt Tension panel`.

### Task 7: Smallest breakpoint and the RESULTS re-test menu

**Files:**
- Modify: `ui_xml/panel_belt_tension.xml`, `src/ui/ui_panel_belt_tension.cpp`
- Test: `tests/unit/test_belt_tension_panel_states.cpp`

**Interfaces:**
- At `ui_breakpoint == 0`:
  - the TUNING hero is one line ("↻ Tighten B ¼ · 2.4 Hz · R3", glyph from `rotate_right`/`rotate_left`, `include/ui_icon_codepoints.h:251-252`);
  - the gauge is an 8 px bar under it;
  - the amount stepper sits in the button row;
  - the swap control is the tappable hero line.
- RESULTS at breakpoint 0 hides `btn_retest_a`/`btn_retest_b` and shows `btn_retest_menu`, which opens a context menu with "Re-test A" and "Re-test B" (follow the existing `HELIX_CONTEXT_MENU_KIND` pattern, e.g. `include/ui_ams_selector_menu.h`).

- [ ] **Step 1: Failing test:** at the micro breakpoint, the RESULTS row shows three buttons and the menu's two items call the existing re-test handlers.
- [ ] **Step 2:** FAIL. **Step 3:** implement. **Step 4:** PASS.
- [ ] **Step 5:** Headless run at `-s 480x272`, `ctl geom` of every TUNING and RESULTS element: all inside 414 wide, nothing overlapping. Screenshot both.
- [ ] **Step 6:** Commit: `feat(belt): tuning loop and results fit the smallest screens`.

### Task 8: Docs, translations, gates

**Files:**
- Modify: `docs/devel/BELT_TENSION.md` (new "Tuning loop" section: flow, session, thresholds, tensioner kinds, mock script); `docs/user/guide/calibration.md` § Belt Tension (how to tune, what the instruction and gauge mean, spring/unknown printers); `docs/devel/CHANGELOG_1_1_DRAFT.md` (one entry)
- Delete: `docs/devel/plans/2026-10-09-belt-tune-loop-design.md` and this plan, in the change that ships the work
- Translations: `make translation-sync && make translations`

- [ ] **Step 1:** Write the docs; cite code as `path#symbol`, never line numbers.
- [ ] **Step 2:** Translations synced; commit the YAMLs and `ui_xml/translations/*.xml`.
- [ ] **Step 3:** `make mutate-diff` once for the branch; any surviving mutant in `belt_tune_session.cpp` gets a test.
- [ ] **Step 4:** `scripts/test-host-run.sh asan '[belt]'`.
- [ ] **Step 5:** `make full-test-run`. Green, read from the run's own summary.
- [ ] **Step 6:** Commit: `docs(belt): the guided tuning loop`.

### After the branch (not agent tasks)

- Independent whole-branch review (Opus), then land via the batch protocol in CLAUDE.md, tear down the worktree, comment on #1736.
- Hardware, with Preston's OK per session:
  1. On the Voron (.112, Kalico), confirm `FREQ_START`/`FREQ_END` take effect (the console's first "Testing frequency" line equals the window start). Same on a mainline printer (K1C or AD5M).
  2. Check that a ¼ turn moves the peak ≥ 0.5 Hz and the 119-133 Hz hump moves as one piece.
  3. Run a full loop. Then ask the #1721 reporter for a run on their 350.
