# Job Queue and Print Stats Tiles Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rebuild the job queue and print stats home tiles as sized, declarative widgets: a "glance + start" queue tile driven by one pure state function, and a "this week" print stats tile with lifetime totals and seven day bars.

**Architecture:** Each tile follows the sized-widget pattern in `docs/devel/PANEL_WIDGET_GUIDE.md` § "The reference pattern": a pure decision header (state and layout), a widget that measures the minimum box per `SizeRung` and publishes module-level subjects through a `LiveTileOwner`, and one XML tree that binds every part's visibility and text to those subjects. Data comes from existing owners: `JobQueueState` (queue, plus the next job's metadata through the existing `get_file_metadata()` call and the shared `ThumbnailCache`) and `PrintHistoryManager` plus `server.history.totals`.

**Tech Stack:** C++17, LVGL 9.5, helix-xml (`lib/helix-xml/`), Catch2 (`tests/unit/`), Moonraker JSON-RPC mocks.

**Spec:** `docs/devel/plans/2026-10-14-queue-stats-tiles-design.md` (binding; this plan does not redesign it). Read both before starting a task.

## Global Constraints

- Spans are in grid tracks (half a cell each). The spec's "2x1 / 2x2 / 4x2 / 4x4" are track spans and map to `SizeRung::Strip / Cell / Wide / Full` (`src/ui/panel_widgets/size_rung.h`).
- Both tiles: minimum span **2x1**; default span stays 4x4; max spans unchanged (job_queue 8x6, print_stats 6x4); `default_enabled` stays false (the no-fillers ruling).
- Job queue gate: `hardware_gate_subject = "printer_has_job_queue"`, hint `"Job queue not enabled in Moonraker"` (wrapped in `TR_NOOP`).
- The tile's start button calls Moonraker's `server.job_queue.start` through `IMoonrakerQueueAPI::start_queue()` (`include/i_moonraker_sub_apis.h`, implemented by `MoonrakerQueueAPI`), wrapped once as `helix::start_job_queue()`. It never goes through `handle_up_next_tap()` or print-select. The print-status "Up next" row and the print-complete "Start next" keep `handle_up_next_tap()` unchanged.
- WaitingForStart button label: **"Bed clear · Start next"**. Other copy, verbatim: "Queue empty", "Next: {} (starts automatically)", "Starting {}…", "Next: {}", "Could not start the next job", "{} queued", "+{} more", "this week", "No prints this week", "No prints yet".
- Stats hero sums `print_duration`, never `total_duration`. Lifetime line comes only from `server.history.totals`; if totals are unknown or failed, the lifetime line is hidden, never computed from the job cache.
- Filament from 4x2 up, as a length in meters ("412 m"). No mass conversion, no diameter or density assumption.
- XML: no `lv_obj_create`, `lv_label_set_text`, `lv_obj_add_flag`/`remove_flag` for visibility, `lv_obj_set_style_*` in either widget (`.claude/rules/declarative-ui.md`). Repeated parts use `<repeat>` with `${i}` subjects. Pressed feedback is `<style name="styles.press_wash" selector="pressed"/>`, never C++ opacity.
- Design tokens only: `#space_*`, `#font_*` via `text_*` components, theme colors; `scripts/check_hardcoded_pixels.py` must not rise.
- At most one accent color per tile (the action button, the bars).
- Every user-facing string goes through `lv_tr()` with a whole-format key, and every task that adds strings fills all nine locales in `translations/*.yml` and regenerates `ui_xml/translations/*.xml` (the commit hook's `qc_translation_coverage` and `qc_base_locale` fail otherwise). Reuse renderings from `translations/GLOSSARY.md` and the existing locale; if `qc_cjk_fonts` names a missing codepoint, choose an already-covered rendering rather than regenerating fonts.
- Threading (`.claude/rules/threading.md`): no LVGL off the main thread; background callbacks go through `lifetime_.bg_cb()` or `tok.defer()`; every `observe<V>()` gets the owner's `SubjectLifetime`.
- New module-level `lv_subject_t` and `char` buffers are `HELIX_PSRAM_BSS` (`include/helix_psram_attr.h`). New `.cpp` files are added to `firmware/helixscreen-esp32/components/helixapp/app_srcs.txt`. Firmware-compiled code: no exceptions, no RTTI, `<spdlog/fmt/fmt.h>` not `<fmt/format.h>`.
- No raw `lv_obj_t*` data members beyond what already exists (`scripts/check_cached_widget_pointers.py` ratchet).
- Comments describe the code as it is: no archaeology (`CLAUDE.md` § "Comments describe the code, not its past").
- Builds: plain `make` / `make test`; per-task loop is `make t F='<tag>'`. No `make full-test-run` and no mutation run per task. Every `helix-screen` run is headless (`HELIX_HEADLESS=1 SDL_VIDEODRIVER=dummy`) with a pinned `--remote-socket` and `HELIX_CONFIG_DIR`.
- Claim the tree before the first edit: `scripts/helix-claim take worktree:queue-stats-tiles "queue/stats tiles"`; release when the branch is handed off.
- Commits: explicit pathspecs. New files: `git add -N <new> && git commit -m "..." -- <paths>`. Check every commit with `git show --stat HEAD`.

## Review Focus

1. **A very long filename with no spaces at the 2x1 tile on the smallest panel.** A reasonable person expects the hero to end in an ellipsis on one line and the start button to stay fully visible. Pinned in Task 3 (`"a 90-character filename stays on one line beside the icon button"`) and Task 7 (the seeded long name in every rung row).
2. **The queue changes from another client while the tile shows the next job.** A metadata answer for the old first job arriving after the queue moved on must not paint the old thumbnail or estimate under the new name. Pinned in Task 2 (`"a metadata answer for a job no longer first is dropped"`).
3. **History rows with odd timestamps:** an in-progress row (`end_time` 0), a print spanning midnight, a 23-hour DST day, an `end_time` in the future (clock skew). Each lands in the day of `job_timestamp()` or is skipped, never double-counted or thrown into today. Pinned in Task 4.
4. **The totals call fails, or answers after the history cache.** The week figures show at once; the lifetime line stays hidden until totals land, then appears without a resize. Pinned in Task 5 (`"lifetime line appears when totals land after the week"`).
5. **The Moonraker socket drops while jobs are queued.** `JobQueueState::invalidate()` clears `is_loaded()`; the tile must keep the last known queue instead of falling back to its NotLoaded look. Pinned in Task 3 (`"a dropped socket keeps the last known queue on the tile"`).

---

## Spec ambiguities resolved here

These are recorded so a reviewer can overturn them; each is a one-line change in the named task.

1. **An idle printer with jobs always offers Start (Preston's ruling).** Moonraker's `job_queue.py` sets `queue_state` to `"paused"` after every job it loads unless automatic transition is on, and also starts in `"paused"`, so a post-job wait and a user pause cannot be told apart. They need not be: idle with jobs is WaitingForStart whatever `automatic_transition` and `queue_state` say. There is no UserPaused state and no "Resume queue".
2. **"Starts automatically" only while printing (Preston's ruling).** Moonraker transitions only when a job completes, so AutoNext is a print running with automatic transition on and the queue `"ready"`. A running print with the queue paused, or with auto off, is Printing.
3. **The button starts the job on the printer (Preston's ruling).** `start_job_queue()` sends `server.job_queue.start`; Moonraker loads the first queued file itself and, with auto off, returns the queue to `"paused"` after that one job. Open point for Preston: HelixScreen's stored per-job pre-print options (`include/queued_job_options.h`) are applied only by the print-select path (`src/ui/ui_panel_print_select.cpp#start_queued_job`), so a job started from the tile prints the file as queued, without them. The plan does not build an option-applying path; it records this in the user guide.
4. **A NotLoaded state.** The spec removes the old "Ready / Queue empty" shown before data. Before the first queue response the tile shows only its icon and "Job Queue" label (state `NotLoaded`). After that, a dropped socket keeps the last known queue (`JobQueueState::has_ever_loaded()`).
5. **No shared metadata cache exists.** Print-select keeps metadata per directory listing; the print-status media manager fetches per active print. "Reuse that cache" is read as: the same `api->files().get_file_metadata()` call and the shared `ThumbnailCache` (`get_thumbnail_cache().fetch(req, ctx, ...)`), owned by `JobQueueState` and fetched once per change of first job. On `HELIX_PLATFORM_ESP32` the thumbnail fetch is skipped (that path uses a PSRAM-specific loader) and the placeholder shows; the estimate still shows.
6. **Filament as length (Preston's ruling).** `server.history.totals.total_filament_used` is millimetres; the tile shows meters, one decimal under 10 m, whole meters from there ("412 m", "12346 m"). `HistoryDashboardPanel::format_filament()` (`src/ui/ui_panel_history_dashboard.cpp`) prints mm/m/km untranslated with no space, a different output for a different screen, so the tile's formatter is its own.
7. **"Last 7 days."** The hero is the sum of the seven local-day bars (today plus the six days before), so hero and bars always agree. The history cache is asked to cover from the oldest bar's local midnight.
8. **Hours format.** "0 h", whole minutes under an hour ("45 min"), one decimal under ten hours with a trailing ".0" dropped ("2.5 h", "2 h"), whole hours from ten ("23 h").
9. **Cell button label.** A 1x1 tile cannot hold "Bed clear · Start next" at body size; Cell shows `action_short` ("Start next"), Wide and Full show the full label. Strip shows an icon-only button. All three call `start_job_queue()`.
10. **1920x1080 in the content-fits sweep.** `kShipping` on main lacks it; the unmerged `feature/default-home-layouts` (b6d86b0b4) adds `{"1920x1080", 1920, 1080, 1720, 1056, 10}`. Task 7 adds that identical row if it is still missing, so the two branches merge clean.

---

## File Structure

| Path | Change | Responsibility |
|------|--------|----------------|
| `include/job_queue_tile_state.h` | create | Pure queue-tile state and action decision; `QueueRunState` parse |
| `include/job_queue_state.h`, `src/ui/job_queue_state.cpp` | modify | Publish run state and next-job media; compose tile text; retire `job_queue_state_text` / `job_queue_summary_text` (Task 3) |
| `include/job_queue_start.h`, `src/ui/job_queue_start.cpp` | modify | `start_job_queue()`, `open_job_queue_modal()` |
| `src/ui/panel_widgets/job_queue_tile_layout.h` | create | Pure rung and part decision for the queue tile |
| `src/ui/panel_widgets/job_queue_widget.h/.cpp` | rewrite | Measure, decide, publish; no imperative UI |
| `ui_xml/components/panel_widget_job_queue.xml` | rewrite | One tree bound to `jq_tile_*` subjects |
| `ui_xml/components/job_queue_row.xml` | modify | Read-only form: `hide_delete`, `row_clickable`, `name_subject` props |
| `include/print_activity.h`, `src/print/print_activity.cpp` | create | Day bucketing, bar scaling, hours/count/length formatting |
| `include/locale_formats.h`, `src/ui/locale_formats.cpp` | modify | Export the weekday abbreviation |
| `src/ui/panel_widgets/print_stats_tile_layout.h` | create | Pure content, rung and part decision for the stats tile |
| `src/ui/panel_widgets/print_stats_widget.h/.cpp` | rewrite | Measure, decide, publish; tap opens the history dashboard |
| `ui_xml/components/panel_widget_print_stats.xml` | rewrite | One tree bound to `print_stats_*` subjects |
| `src/ui/panel_widget_registry.cpp`, `src/ui/panel_widget_manager.cpp` | modify | Min spans, gate, description, gate owner row |
| `firmware/helixscreen-esp32/components/helixapp/app_srcs.txt` | modify | `src/print/print_activity.cpp` |
| tests (see each task) | create/modify/delete | |
| `docs/user/guide/home-panel.md`, `docs/devel/PANEL_WIDGET_GUIDE.md`, `docs/devel/CHANGELOG_1_1_DRAFT.md` | modify | |

---

### Task 1: Queue tile state decision and its text

**Files:**
- Create: `include/job_queue_tile_state.h`
- Modify: `include/job_queue_state.h`, `src/ui/job_queue_state.cpp`
- Modify: `tests/test_helpers/job_queue_subjects_fixture.h` (unregister the new subject), `tests/test_helpers/job_queue_state_test_access.h` (comment names the published subjects)
- Test: `tests/unit/test_job_queue_tile_state.cpp` (new), `tests/unit/test_job_queue_count_subject.cpp` (extend)
- Translations: `translations/*.yml`, `ui_xml/translations/*.xml`

**Interfaces:**
- Consumes: `JobQueueState` (`include/job_queue_state.h`), `helix::gcode::get_display_filename()` (`include/ui_filename_utils.h`), `helix::format::duration()` (`include/format_utils.h`).
- Produces (exact, syntax-checked):

```cpp
// include/job_queue_tile_state.h  (bodies are constexpr, written inline by the implementer)
namespace helix {
enum class QueueRunState : int { Ready = 0, Loading = 1, Starting = 2, Paused = 3 };
[[nodiscard]] constexpr QueueRunState parse_queue_run_state(std::string_view state);
enum class JobQueueTileState : int {
    Unavailable = 0, NotLoaded = 1, Empty = 2, WaitingForStart = 3,
    AutoNext = 4, Printing = 5, Starting = 6,
};
enum class JobQueueTileAction : int { None = 0, StartNext = 1 };
struct JobQueueTileInputs {
    bool component_available = false;
    bool loaded = false;
    int job_count = 0;
    QueueRunState queue_state = QueueRunState::Paused;
    bool printer_busy = false; ///< print_active: printing or paused
    bool automatic_transition = false;
};
[[nodiscard]] constexpr JobQueueTileState job_queue_tile_state(const JobQueueTileInputs& in);
[[nodiscard]] constexpr JobQueueTileAction job_queue_tile_action(JobQueueTileState state);
} // namespace helix

// include/job_queue_state.h, namespace helix, beside format_start_next_text()
struct JobQueueTileText {
    std::string hero;
    std::string detail;
    std::string more;
    std::string action;
    std::string action_short;
};
JobQueueTileText compose_job_queue_tile_text(JobQueueTileState state,
                                             const std::string& next_display_name,
                                             int queued_count, int next_estimated_s);

// class JobQueueState, public
[[nodiscard]] bool has_ever_loaded() const { return ever_loaded_; }
[[nodiscard]] helix::QueueRunState run_state() const { return helix::parse_queue_run_state(queue_state_); }
// private
bool ever_loaded_ = false;                    // set in on_queue_fetched(), never cleared
lv_subject_t job_queue_run_state_subject_{};  // XML name "job_queue_run_state", int QueueRunState
```

Decision rules, first match wins (this order is the contract the tests pin):

1. `!component_available` → `Unavailable`
2. `!loaded` → `NotLoaded`
3. `job_count <= 0` → `Empty`
4. `queue_state` is `Loading` or `Starting` → `Starting`
5. `printer_busy && automatic_transition && queue_state == Ready` → `AutoNext`
6. `printer_busy` → `Printing`
7. otherwise → `WaitingForStart` (idle with jobs, whatever `automatic_transition` and `queue_state` say)

`parse_queue_run_state`: `"ready"` → Ready, `"loading"` → Loading, `"starting"` → Starting, anything else (including `"paused"` and `""`) → Paused.

`job_queue_tile_action`: WaitingForStart → StartNext; every other state → None.

`compose_job_queue_tile_text` (N = `queued_count`, name = `next_display_name`):

| State | hero | detail | more | action | action_short |
|---|---|---|---|---|---|
| Unavailable | "Job queue not enabled in Moonraker" | "" | "" | "" | "" |
| NotLoaded | "" | "" | "" | "" | "" |
| Empty | "Queue empty" | "" | "" | "" | "" |
| WaitingForStart | name | `format::duration(est)` or "" when est is 0 | "+{N-1} more" when N ≥ 2 | "Bed clear · Start next" | "Start next" |
| AutoNext | "{N} queued" | "Next: {name} (starts automatically)" | "" | "" | "" |
| Printing | "{N} queued" | "Next: {name}" | "" | "" | "" |
| Starting | "Starting {name}…" | "" | "" | "" | "" |

Each format string is one `lv_tr()` key: `"{} queued"`, `"+{} more"`, `"Next: {}"`, `"Next: {} (starts automatically)"`, `"Starting {}…"` (U+2026).

`JobQueueState::update_subjects()` publishes `job_queue_run_state` **before** `job_queue_count` (the count stays last, as its comment requires). `init_subjects()` registers it with `SubjectDebugRegistry` like its siblings. `job_queue_state_text` and `job_queue_summary_text` stay in this task (the old tile still binds them); Task 3 removes them.

- [ ] **Step 1: Claim the tree and write the failing tests**

`tests/unit/test_job_queue_tile_state.cpp`, tag `[job_queue_tile]`, plain `TEST_CASE` (no LVGL) for the decision, `LVGLTestFixture` for the text (it calls `lv_tr`). One `TEST_CASE` per table row, each building `JobQueueTileInputs` that would match only that row if the rule above it were deleted:

- Unavailable wins over everything: `{component_available=false, loaded=true, job_count=3, queue_state=Loading, printer_busy=true}` → Unavailable.
- NotLoaded: `{true, loaded=false, 3, Ready, false, false}` → NotLoaded.
- Empty beats Starting: `{true, true, 0, Loading, false, false}` → Empty.
- Starting from Loading and from Starting, while busy: `{true, true, 2, Loading, true, false}` → Starting; same with `Starting` → Starting.
- AutoNext only while printing: `{true, true, 2, Ready, true, true}` → AutoNext.
- Printing when the queue will not start the next job by itself: `{true, true, 1, Ready, true, false}` (auto off) and `{true, true, 3, Paused, true, true}` (auto on, queue paused) → Printing.
- WaitingForStart for every idle combination (Preston's ruling: idle with jobs always offers Start): `{true, true, 1, Paused, false, false}`, `{true, true, 1, Ready, false, false}`, `{true, true, 2, Paused, false, true}`, `{true, true, 2, Ready, false, true}` → WaitingForStart.
- Edge cases the spec names: one job (`job_count=1`) in WaitingForStart; "a job added while printing": inputs go from `{.., 0, Ready, busy=true, ..}` (Empty) to `{.., 1, Ready, busy=true, ..}` (Printing).
- `parse_queue_run_state`: the four Moonraker strings, `""` and `"bogus"` → Paused.
- `job_queue_tile_action`: StartNext only for WaitingForStart, None for the other six (loop over all seven enumerators).
- Text, one case per state row, with name `"Benchy"`, N = 3, est = 4500 (expect detail `"1h 15m"` in WaitingForStart), and these extra cases: N = 1 → `more` empty in WaitingForStart; est = 0 → WaitingForStart detail empty; `action_short` is `"Start next"` while `action` is `"Bed clear · Start next"`.

Extend `tests/unit/test_job_queue_count_subject.cpp`: a case that delivers `{queue_state="paused", 2 jobs}` then `{queue_state="ready", 2 jobs}` through `JobQueueStateTestAccess::deliver_status()` and asserts `job_queue_run_state` reads `3` then `0` while `job_queue_count` stays `2`; and a case that an observer of `job_queue_count` (synchronous `observe<int>`) already reads the new `job_queue_run_state` when it fires. Add a case: after `deliver_status`, `invalidate()` → `is_loaded()` is false and `has_ever_loaded()` is still true.

- [ ] **Step 2: Run them and watch them fail**

Run: `make t F='[job_queue_tile]'` then `make t F='[job_queue][subjects]'`.
Expected: compile failure (`job_queue_tile_state.h` not found), then failures on the new subject.

- [ ] **Step 3: Implement**

Write the header bodies, `compose_job_queue_tile_text()` in `src/ui/job_queue_state.cpp` next to `format_up_next_text()` (same whole-format `lv_tr` style), the subject and the `ever_loaded_` latch. Do not touch the old tile.

- [ ] **Step 4: Run the tests until green**

Run: `make t F='[job_queue_tile]'` and `make t F='[job_queue][subjects]'`. Expected: all pass.

- [ ] **Step 5: Prove one test can fail**

Drop the `queue_state == Ready` term from rule 5; rerun `[job_queue_tile]`; the auto-on-paused Printing case must go red. Then make rule 7 require `!automatic_transition`; the idle auto-on cases must go red. Restore.

- [ ] **Step 6: Translations**

`make translation-sync`, fill the new keys in all nine `translations/*.yml` (en identical to the key), `make translations`.

- [ ] **Step 7: Commit**

```bash
git add -N include/job_queue_tile_state.h tests/unit/test_job_queue_tile_state.cpp
git commit -m "feat(job-queue): one pure decision for the queue tile's state, action and text" -- include/job_queue_tile_state.h include/job_queue_state.h src/ui/job_queue_state.cpp tests/unit/test_job_queue_tile_state.cpp tests/unit/test_job_queue_count_subject.cpp tests/test_helpers/job_queue_subjects_fixture.h tests/test_helpers/job_queue_state_test_access.h translations ui_xml/translations
git show --stat HEAD
```

---

### Task 2: Next-job estimate and thumbnail in JobQueueState

**Files:**
- Modify: `include/job_queue_state.h`, `src/ui/job_queue_state.cpp`
- Modify: `tests/test_helpers/job_queue_state_test_access.h` (add `deliver_next_metadata`)
- Modify: `tests/test_helpers/job_queue_subjects_fixture.h`
- Test: `tests/unit/test_job_queue_next_media.cpp` (new)

**Interfaces:**
- Consumes: `api_->files().get_file_metadata(path, on_ok, on_err)` (`include/i_moonraker_sub_apis.h`), `FileMetadata` (`include/moonraker_types.h`), `helix::select_and_resolve_thumbnail()` and `helix::gcode_dir_of()` (`include/moonraker_types.h`), `ThumbnailRequest` / `get_thumbnail_cache().fetch(req, ctx, ok, err)` (`include/thumbnail_cache.h`), `ThumbnailLoadContext::create(lifetime_, &gen)` (`include/thumbnail_load_context.h`), `ThumbnailProcessor::get_target_for_display(ThumbnailSize::Card)` (`include/thumbnail_processor.h`), `PrinterPrintState::no_thumbnail_placeholder()` (`include/printer_print_state.h`).
- Produces (exact, syntax-checked):

```cpp
// class JobQueueState, public
[[nodiscard]] int next_estimated_seconds() const { return next_estimated_s_; }
// private
void refresh_next_job_media();
void on_next_job_metadata(const std::string& filename, const FileMetadata& metadata);
void clear_next_job_media();
std::string next_media_filename_;
int next_estimated_s_ = 0;
std::atomic<uint32_t> next_thumbnail_generation_{0};
lv_subject_t job_queue_next_estimate_subject_{};   // XML "job_queue_next_estimate", int seconds
lv_subject_t job_queue_next_thumbnail_subject_{};  // XML "job_queue_next_thumbnail", string
char next_thumbnail_buf_[256];
// header gains: struct FileMetadata; (forward declaration)

// tests/test_helpers/job_queue_state_test_access.h
static void deliver_next_metadata(JobQueueState& state, const std::string& filename,
                                  const FileMetadata& metadata);   // calls on_next_job_metadata
```

Behaviour:
- `on_queue_fetched()` calls `refresh_next_job_media()` before `update_subjects()`.
- `refresh_next_job_media()`: empty queue → `clear_next_job_media()`. First job's filename equal to `next_media_filename_` → nothing (one fetch per change of first job). Otherwise set `next_media_filename_`, clear to unknown, and issue `get_file_metadata(first.filename, ...)`. The success callback runs on the WebSocket thread: copy the `FileMetadata`, then `token.defer(...)` into `on_next_job_metadata(filename, md)`. The error callback defers a log at debug and leaves the unknown state.
- `on_next_job_metadata(filename, md)`: if `filename != next_media_filename_`, drop it (the queue moved on). Otherwise `next_estimated_s_ = max(0, (int)md.estimated_time)`, publish `job_queue_next_estimate`, and outside `HELIX_PLATFORM_ESP32` resolve the thumbnail with `select_and_resolve_thumbnail(md.thumbnails, gcode_dir_of(filename), target.width, target.height)` for the Card target and fetch it through `get_thumbnail_cache().fetch(req, ThumbnailLoadContext::create(lifetime_, &next_thumbnail_generation_), ...)` with `req.source_modified = md.modified`. Its success callback publishes the path into `job_queue_next_thumbnail` (it is marshalled to the main thread by the cache, and the context drops superseded loads).
- `clear_next_job_media()`: estimate 0, thumbnail subject = `PrinterPrintState::no_thumbnail_placeholder()`. The thumbnail subject is never `""`; its init value is the placeholder.
- Both subjects publish before `job_queue_count` when they change in `update_subjects()`; a metadata answer that arrives later publishes them on its own (the tile observes `job_queue_next_estimate`).

- [ ] **Step 1: Write the failing tests** (`[job_queue][next_media]`, fixture deriving from `XMLTestFixture` so a mock `MoonrakerAPI` is present; construct `JobQueueState(api, client)` from the fixture and `init_subjects()`)

  - "first queued job's estimate comes from its file metadata": deliver a status whose first job is a file the mock serves metadata for (one from `scan_test_gcode_files()`; read its expected `estimated_time` from the same mock call in the test), drain the UpdateQueue, expect `job_queue_next_estimate` equals that value and is > 0.
  - "a metadata answer for a job no longer first is dropped", through the real path: `client.defer_next("server.files.metadata")`, deliver status A (first = file A), deliver status B (first = file B, whose metadata answers normally), drain, record B's estimate, then `client.fire_deferred("server.files.metadata")` (A's answer lands late) and drain; expect `job_queue_next_estimate` still equals B's estimate. A second case does the same through `deliver_next_metadata(state, "<A>", md_with_estimate_999)` and expects no change.
  - "an empty queue clears the next job's media": after a populated status, deliver an empty one; expect estimate 0 and thumbnail subject equals `PrinterPrintState::no_thumbnail_placeholder()`.
  - "the thumbnail subject is never empty": right after `init_subjects()`, and after a metadata answer with no thumbnails, the subject string is non-empty and equals the placeholder.
  - "one fetch per change of first job": deliver the same status twice; `MoonrakerClientMock::call_count("server.files.metadata")` rises by exactly 1. Deliver a status whose first job differs; it rises by 1 more.
  - "a failed metadata read leaves the job unknown": `client.fail_next("server.files.metadata")` before delivering; estimate 0, thumbnail is the placeholder.

- [ ] **Step 2: Run and watch them fail**: `make t F='[next_media]'`. Expected: compile errors on the missing members.

- [ ] **Step 3: Implement** per the behaviour list. Include `printer_print_state.h`, `thumbnail_cache.h`, `thumbnail_processor.h` in the `.cpp` only.

- [ ] **Step 4: Run until green**: `make t F='[next_media]'`, then `make t F='[job_queue]'`.

- [ ] **Step 5: Prove the stale-drop test can fail**: delete the `filename != next_media_filename_` early return; the second case must go red. Restore.

- [ ] **Step 6: Commit**

```bash
git add -N tests/unit/test_job_queue_next_media.cpp
git commit -m "feat(job-queue): JobQueueState publishes the next job's estimate and thumbnail" -- include/job_queue_state.h src/ui/job_queue_state.cpp tests/unit/test_job_queue_next_media.cpp tests/test_helpers/job_queue_state_test_access.h tests/test_helpers/job_queue_subjects_fixture.h
git show --stat HEAD
```

---

### Task 3: The job queue tile

**Files:**
- Create: `src/ui/panel_widgets/job_queue_tile_layout.h`
- Rewrite: `src/ui/panel_widgets/job_queue_widget.h`, `src/ui/panel_widgets/job_queue_widget.cpp`, `ui_xml/components/panel_widget_job_queue.xml`
- Modify: `ui_xml/components/job_queue_row.xml` (read-only props), `include/job_queue_start.h`, `src/ui/job_queue_start.cpp`
- Modify: `include/job_queue_state.h`, `src/ui/job_queue_state.cpp` (remove `job_queue_state_text`, `job_queue_summary_text`, their buffers and the "Ready"/"Queue empty" defaults)
- Modify: `tests/unit/test_job_holds_machine.cpp` (move `panel_widget_job_queue.xml` from `kNoMachineControlFiles` to `kGuardedFiles` with its guard count), `tests/unit/test_label_one_line_call_sites.cpp` (rewrite the job queue case), `tests/unit/test_job_queue_start_guard.cpp` (the action-button case), `tests/unit/test_job_queue_count_subject.cpp` and `tests/test_helpers/job_queue_subjects_fixture.h` (drop the retired subject names)
- Delete: `tests/unit/test_widget_size_job_queue.cpp` (it pins the retired 3-mode list rebuild)
- Test: `tests/unit/test_job_queue_tile_layout.cpp` (new, pure), `tests/unit/test_widget_size_job_queue_tile.cpp` (new, harness)
- Translations

**Interfaces:**
- Consumes: Task 1 (`job_queue_tile_state()`, `job_queue_tile_action()`, `compose_job_queue_tile_text()`, `JobQueueState::has_ever_loaded()`, `run_state()`, subject `job_queue_run_state`), Task 2 (`job_queue_next_estimate`, `job_queue_next_thumbnail`, `next_estimated_seconds()`), `api->queue().start_queue(on_ok, on_err)` (`include/i_moonraker_sub_apis.h`, already present on `IMoonrakerQueueAPI` and `MoonrakerQueueAPI`; no API change needed), `choose_size_rung()` / `holds_any_rung()` / `rung_box()` (`src/ui/panel_widgets/size_rung.h`), `measure_content_chrome()` (`src/ui/panel_widgets/content_chrome.h`), `measure_text_px()` (`src/ui/panel_widgets/text_measure.h`), `LiveTileOwner<W>` (`src/ui/panel_widgets/live_tile_owner.h`), `ui::observe_language_change()` (`include/observe_language.h`), `get_printer_state().print_state().get_print_active_subject()` with `get_printer_state().get_subjects_lifetime()`, subject `printer_has_job_queue`.
- Produces (exact, syntax-checked):

```cpp
// src/ui/panel_widgets/job_queue_tile_layout.h
namespace helix {
inline constexpr int kJobQueueTileListRows = 3;
struct JobQueueTileLayoutInput {
    int content_w = 0;
    int content_h = 0;
    JobQueueTileState state = JobQueueTileState::NotLoaded;
    int job_count = 0;
    RungNeeds needs; ///< offers strip, cell, wide and full only
};
struct JobQueueTileVerdict {
    SizeRung rung = SizeRung::Strip;
    bool fits = false;
    bool show_thumbnail = false;
    bool show_detail = false;
    bool show_more = false;
    int list_rows = 0;
    bool button_icon_only = false;
};
[[nodiscard]] constexpr JobQueueTileVerdict decide_job_queue_tile_layout(const JobQueueTileLayoutInput& in);
}

// include/job_queue_start.h
void start_job_queue();       // server.job_queue.start: Moonraker loads the first queued job itself.
                              // Error callback marshals to the main thread and shows
                              // NOTIFY_WARNING(lv_tr("Could not start the next job")); success needs
                              // nothing, notify_job_queue_changed refreshes JobQueueState.
void open_job_queue_modal();  // the existing process-wide queue_modal().show(lv_screen_active())

// src/ui/panel_widgets/job_queue_widget.h
class JobQueueWidget : public PanelWidget {
  public:
    JobQueueWidget();
    ~JobQueueWidget() override;
    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    void on_activate() override;
    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override;
    bool fits_at(int width_px, int height_px) const override;
    const char* id() const override { return "job_queue"; }
    static void on_tile_clicked(lv_event_t* e);    // XML "on_job_queue_widget_clicked"
    static void on_action_clicked(lv_event_t* e);  // XML "on_job_queue_tile_action"
  protected:
    void on_hooked_root_deleted() override;
  private:
    [[nodiscard]] JobQueueTileInputs read_inputs() const;
    [[nodiscard]] JobQueueTileVerdict decide(int width_px, int height_px, const JobQueueTileInputs& in) const;
    void schedule_refresh();
    void refresh();
    void publish(const JobQueueTileInputs& in);
    int granted_w_ = 0;
    int granted_h_ = 0;
    JobQueueTileVerdict verdict_{};
    bool refresh_pending_ = false;
    ObserverGuard count_observer_, run_state_observer_, automatic_observer_,
                  estimate_observer_, print_active_observer_, language_observer_;  // one per line in code
    helix::AsyncLifetimeGuard lifetime_;
};
void register_job_queue_widget();
```

The widget no longer owns a `JobQueueModal`, a `job_list_container_`, a `widget_obj_` or a `parent_screen_`; the tile tap calls `open_job_queue_modal()`.

Layout rules (`decide_job_queue_tile_layout`), with `r = choose_size_rung(w, h, needs)` and `fits = holds_any_rung(w, h, needs)`:
- `show_thumbnail` = `r >= Cell` and state ∈ {WaitingForStart, Starting}.
- `show_detail` = `r >= Cell`, except WaitingForStart (its detail is the estimate) needs `r >= Wide`.
- `show_more` = `r >= Cell` and `job_count >= 2`.
- `list_rows` = `r == Full` ? `min(kJobQueueTileListRows, max(0, job_count - 1))` : 0, and 0 in Empty, NotLoaded, Unavailable.
- `button_icon_only` = `r == Strip`.

Published subjects (module level, `HELIX_PSRAM_BSS`, registered by `register_widget_subjects("job_queue", ...)`, written only by the `LiveTileOwner` owner):

| Subject | Type | Value |
|---|---|---|
| `jq_tile_state` | int | `JobQueueTileState` |
| `jq_tile_action` | int | `JobQueueTileAction` |
| `jq_tile_rung` | int | `SizeRung` |
| `jq_tile_show_thumb`, `jq_tile_show_detail`, `jq_tile_show_more` | int 0/1 | verdict |
| `jq_tile_list_rows` | int 0..3 | verdict |
| `jq_tile_hero`, `jq_tile_detail`, `jq_tile_more` | string | `compose_job_queue_tile_text()` |
| `jq_tile_action_text` | string | `action_short` at Cell, `action` at Wide and Full |
| `jq_tile_after_0` .. `jq_tile_after_2` | string | display names of queued jobs 2, 3, 4 ("" past the end) |

`jq_size_mode` is retired.

Measured needs (`decide()`; the per-rung boxes the widget hands the layout, after `measure_content_chrome(root(), 0)`, falling back to the root's `#space_*` padding before `root()` exists, as `src/ui/panel_widgets/speed_flow_widget.cpp#decide` does). Budget text widths against fixed worst cases so the rung holds while names change: hero sample = the wider of `"88 queued"` and the translated "Queue empty" in `font_heading`; names ellipsize, so they are never measured.
- Strip: `w = hero_sample_w + col_gap + button_height_sm`, `h = max(line(font_heading), button_height_sm)`.
- Cell: `w = max(hero_sample_w, action_short_w(font_body) + 2*space_sm)`, `h = line(heading) + line(small) + button_height_sm + 2*row_gap`.
- Wide: `w = thumb_w + col_gap + max(hero_sample_w, action_w(font_body) + 2*space_sm)` with `thumb_w = line(heading) + line(small)`, `h = line(heading) + 2*line(small) + button_height_sm + 3*row_gap`.
- Full: Wide's box plus `kJobQueueTileListRows * line(font_small) + kJobQueueTileListRows*row_gap` in height, raised to the 2x2 size class with `widget_size::w_wide()` and `widget_size::h_taller()` (the `full_min_h` rule in `docs/devel/PANEL_WIDGET_GUIDE.md` § "The sized-widget family").
- Glance and Column are not offered. `fits_at()` returns `decide(...).fits` for the NotLoaded inputs and for WaitingForStart (the widest strip content): a size is refused unless the Strip box fits in both.

Refresh: observers on `job_queue_count`, `job_queue_run_state`, `job_queue_automatic_transition`, `job_queue_next_estimate` (lifetime `jqs->get_subjects_lifetime()`), `print_active` (lifetime `get_printer_state().get_subjects_lifetime()`), and the language change all call `schedule_refresh()`, which coalesces through `helix::ui::run_next_tick(lifetime_.token(), ...)`. `attach()` claims the tile (`s_tile.claim(this)`), installs the delete hook, wires the observers and calls `refresh()`, because instances are recycled across grid rebuilds and the size callback may not follow. `on_size_changed()` stores the granted size and calls `refresh()`. `refresh()` reads inputs, decides at the granted size, stores `verdict_`, and publishes only when `s_tile.is_owner(this)`. `detach()` invalidates `lifetime_`, clears `refresh_pending_`, resets the observers, and releases the tile; a returned previous owner republishes. `on_activate()` calls `jqs->fetch()` as before.

`read_inputs()`: `component_available` = `printer_has_job_queue` subject value != 0; `loaded` = `jqs && jqs->has_ever_loaded()`; `job_count` = `jqs->get_jobs().size()`; `queue_state` = `jqs->run_state()`; `printer_busy` = `print_active != 0`; `automatic_transition` = `jqs->automatic_transition()`.

`on_action_clicked`: `lv_event_stop_bubbling(e)`; read `jq_tile_action`; StartNext → `start_job_queue()`; None → nothing. The doc comment on `start_next_queued_job()` in `include/job_queue_start.h` ("no caller starts a queued job any other way") becomes: it is the file-view path the "Up next" row and the completion dialog use, and `start_job_queue()` is the tile's direct path.

XML (`panel_widget_job_queue.xml`), one tree. Names are the contract the tests read:
- Root: same root padding and gap as `ui_xml/components/panel_widget_speed_flow.xml`'s root; `<event_cb trigger="clicked" callback="on_job_queue_widget_clicked"/>`; `<style name="styles.press_wash" selector="pressed"/>`.
- `jq_label_row`: `progress_clock` icon + `text_small` "Job Queue" (`translation_tag`), muted; shown only in NotLoaded (`bind_flag_if_not_eq subject="jq_tile_state" ref_value="1"`).
- `jq_body` (row, `flex_track_place="center"`): `jq_thumb` (`lv_image bind_src="job_queue_next_thumbnail"`, `inner_align="contain"`, hidden unless `jq_tile_show_thumb` is 1); `jq_text` column holding `jq_hero` (`text_heading`, `bind_text="jq_tile_hero"`, `long_mode="dots"`), `jq_detail` (`text_small`, muted, hidden unless `jq_tile_show_detail` is 1), `jq_more` (`text_small`, muted, hidden unless `jq_tile_show_more` is 1), `jq_action_wide` (`ui_button`, `bind_text="jq_tile_action_text"`, shown at rung Wide or Full and action ≠ 0); then `jq_action_icon` (icon-only `ui_button`, `play` icon for StartNext, shown at Strip and action ≠ 0).
- `jq_action_cell`: full-width `ui_button` under `jq_body`, shown at rung Cell and action ≠ 0.
- All three action buttons: `callback="on_job_queue_tile_action"`, `moves_machine="true"` (they start a print through Moonraker), the accent color is theirs alone.
- `jq_list`: a column holding `<repeat count="3">` of `<job_queue_row hide_delete="true" row_clickable="false" name_subject="jq_tile_after_${i}">`, each hidden by `bind_flag_if` with `cond="jq_tile_list_rows le ${i}"`. Visibility conditions on several subjects use `cond=` expressions (`docs/devel/LVGL9_XML_GUIDE.md` § "Expression conditionals"), not C++ observers.
- `job_queue_row.xml` gains props `hide_delete` (boolean, default false) on `job_delete`'s `hidden`, `row_clickable` (boolean, default true) on the row's `clickable`, and `name_subject` (string, default "") on `job_filename`'s `bind_text`; the modal's existing use is unchanged and still sets the name from C++.

`tests/unit/test_job_holds_machine.cpp`: `panel_widget_job_queue.xml` → `kGuardedFiles` with `3`.

- [ ] **Step 1: Pure layout tests** (`tests/unit/test_job_queue_tile_layout.cpp`, `[job_queue_tile][layout]`), with synthetic needs `strip {100,40}`, `cell {100,100}`, `wide {220,100}`, `full {220,220}`:
  - Rung boundaries are exact: `(99,40)` → not fits; `(100,40)` → Strip; `(100,99)` → Strip; `(100,100)` → Cell; `(219,100)` → Cell; `(220,100)` → Wide; `(220,219)` → Wide; `(220,220)` → Full.
  - Flags per rung for WaitingForStart with 4 jobs: Strip → icon-only, no thumb/detail/more, 0 rows; Cell → thumb, more, no detail; Wide → thumb, detail, more; Full → also `list_rows == 3`.
  - `list_rows` = `job_count - 1` for 2 and 3 jobs at Full, capped at 3 for 9 jobs; 0 at Wide; 0 for Empty at Full.
  - Printing at Cell: no thumbnail, detail shown.
  - `fits` is false below Strip and monotonic: for every `w` in 90..240 and `h` in 30..240 step 10, `fits(w,h)` implies `fits(w+10,h)` and `fits(w,h+10)`.

- [ ] **Step 2: Widget tests** (`tests/unit/test_widget_size_job_queue_tile.cpp`, `[widget_size][job_queue]`), fixture from `LVGLUITestFixture` with `PanelWidgetManager::instance().init_widget_subjects()`, a `JobQueueState` installed through `set_job_queue_state()` in a scoped guard (copy `ScopedJobQueueState` from the deleted `test_widget_size_job_queue.cpp`), statuses delivered with `JobQueueStateTestAccess::deliver_status()`, `printer_has_job_queue` set to 1, and `PanelWidgetHarness<JobQueueWidget> h(test_screen())`. Medium-tier boxes from `docs/devel/PANEL_WIDGET_GUIDE.md` § "Rules every sized widget follows": 114x54, 114x113, 234x113, 234x232.
  - "medium-tier budgets land on their rung": the four boxes give `jq_tile_rung` Strip, Cell, Wide, Full.
  - "each state draws its parts at each rung" (subjects, visibility via a `shown()` helper as in `test_widget_size_speed_flow.cpp`, and button presence): WaitingForStart (3 jobs, auto off) at all four boxes; AutoNext; Printing (`print_active` = 1); Starting (`queue_state` "loading"); idle with auto on and `queue_state` "paused" and "ready" (both WaitingForStart with the Start button: add `JobQueueStateTestAccess::set_automatic_transition(JobQueueState&, bool)`, which sets `automatic_transition_` and the `job_queue_automatic_transition` subject the way the `server.config` callback in `src/ui/job_queue_state.cpp#fetch_automatic_transition` does); AutoNext (auto on, `print_active` = 1, "ready"); Empty; NotLoaded (fresh state, nothing delivered).
  - "a queue state change redraws without a count change": auto on, printing, deliver `"paused"` (Printing, detail "Next: …") then the same jobs with `"ready"` → AutoNext, detail ends in "(starts automatically)".
  - "print end turns AutoNext into the Start button": AutoNext, then `print_active` = 0 → WaitingForStart, `jq_tile_action` 0 → 1.
  - "the start button sends server.job_queue.start and nothing else": put this case in `tests/unit/test_job_queue_start_guard.cpp` on its `QueuedStartFixture` (mock client, `StaticPanelRegistry` handling, `PrintSelectPanelTestAccess`). Seed the mock's default queue as "start next routes the queue's front job through the panel entry point" does, build the tile in a `PanelWidgetHarness<JobQueueWidget>` at 114x113 (Cell), read `mock_client_.call_count("server.job_queue.start")`, `lv_obj_send_event(<jq_action_cell's button>, LV_EVENT_CLICKED)`, drain. Expect the count up by exactly 1, `PrintSelectPanelTestAccess::pending_queued_job_id(*get_print_select_panel(get_printer_state(), api_.get()))` to be null (print-select was not involved), and `ModalStack` to hold no dialog (the tile's own tap did not also open the queue modal). Repeat at 114x54 through `jq_action_icon` and at 234x113 through `jq_action_wide`. Tag it `[job_queue][queue_start][widget_size]`.
  - "a refused start says so": `mock_client_.fail_next("server.job_queue.start")`, click, drain; a warning is posted: capture it with the file's `WarningLog` as "a queued job refuses to start while the printer is busy" does, and expect it to contain the translated "Could not start the next job".
  - "Up next keeps the file-view path": `handle_up_next_tap()` with the printer idle still leaves `pending_queued_job_id` at `"0001"` and does not send `server.job_queue.start`.
  - "a 90-character filename stays on one line beside the icon button": at 114x54, a name of 90 characters with no spaces; `jq_hero` holds one line (`held_to_one_line()` from `tests/test_helpers/one_line_label.h`) and `jq_action_icon`'s right edge is inside the root's content box (`lv_obj_get_coords`).
  - "a dropped socket keeps the last known queue on the tile": WaitingForStart, then `jqs.invalidate()`, drain; `jq_tile_state` still 3 and `jq_tile_hero` unchanged.
  - "the following jobs list shows jobs 2 to 4 at Full": 5 jobs; `jq_tile_after_0..2` are jobs 2, 3, 4's display names; the three rows are shown; with 2 jobs only row 0 is shown.
  - "a language switch recomposes the text": Printing with 3 jobs, `SystemSettingsManager::instance().set_language("de")` (the switch `tests/unit/test_widget_language_switch.cpp#LanguageSwitchFixture` uses), drain, expect `jq_tile_hero` equal to `fmt::format(lv_tr("{} queued"), 3)` read under `de` and different from the English text; set `"en"` back. `HelixTestFixture` also resets the language between tests.
  - "fits_at refuses below the strip": `fits_at(60, 30)` false, `fits_at(114, 54)` true.

Rewrite the job queue case in `tests/unit/test_label_one_line_call_sites.cpp` to assert `jq_hero` is held to one line at the Strip box with the long filename.

- [ ] **Step 3: Run and watch them fail**: `make t F='[job_queue_tile]'`, `make t F='[widget_size][job_queue]'`. Expected: compile errors.

- [ ] **Step 4: Implement** the layout header, the widget, the XML, the row props, `start_job_queue()` and `open_job_queue_modal()` (move the existing `open_job_queue_modal()` out of the anonymous namespace and declare it), and remove the retired subjects from `JobQueueState`. Delete `tests/unit/test_widget_size_job_queue.cpp`.

- [ ] **Step 5: Run until green**: both tags, then `make t F='[job_queue]'`, `make t F='[one_line]'`, `make t F='[job_holds_machine]'`, `make t F='[queue_start]'`, `make t F='[content_fits][sweep]'` (the old minimum still applies; it must stay green).

- [ ] **Step 6: Prove three tests can fail**: (a) make `show_detail` ignore the WaitingForStart exception; the Cell flags case must go red. (b) remove `lv_event_stop_bubbling` from `on_action_clicked`; the "sends server.job_queue.start and nothing else" case must go red on the modal check. (c) route StartNext to `handle_up_next_tap()`; that case goes red on the call count. Restore all three.

- [ ] **Step 7: Lint and translations**: `python3 scripts/check_imperative_ui.py --summary` count must not rise; `python3 scripts/check_hardcoded_pixels.py` clean for the touched files; `python3 scripts/format-xml.py --check ui_xml/components/panel_widget_job_queue.xml ui_xml/components/job_queue_row.xml`; translation-sync, fill nine locales, `make translations`.

- [ ] **Step 8: Commit**

```bash
git add -N src/ui/panel_widgets/job_queue_tile_layout.h tests/unit/test_job_queue_tile_layout.cpp tests/unit/test_widget_size_job_queue_tile.cpp
git commit -m "feat(home): the job queue tile shows the next job and starts it from 2x1 up" -- src/ui/panel_widgets/job_queue_tile_layout.h src/ui/panel_widgets/job_queue_widget.h src/ui/panel_widgets/job_queue_widget.cpp ui_xml/components/panel_widget_job_queue.xml ui_xml/components/job_queue_row.xml include/job_queue_start.h src/ui/job_queue_start.cpp include/job_queue_state.h src/ui/job_queue_state.cpp tests/unit/test_job_queue_tile_layout.cpp tests/unit/test_widget_size_job_queue_tile.cpp tests/unit/test_widget_size_job_queue.cpp tests/unit/test_job_holds_machine.cpp tests/unit/test_label_one_line_call_sites.cpp tests/unit/test_job_queue_start_guard.cpp tests/unit/test_job_queue_count_subject.cpp tests/test_helpers/job_queue_subjects_fixture.h tests/test_helpers/job_queue_state_test_access.h translations ui_xml/translations
git show --stat HEAD
```

---

### Task 4: Print activity math

**Files:**
- Create: `include/print_activity.h`, `src/print/print_activity.cpp`
- Modify: `include/locale_formats.h`, `src/ui/locale_formats.cpp`
- Modify: `firmware/helixscreen-esp32/components/helixapp/app_srcs.txt` (add `src/print/print_activity.cpp` beside `src/print/print_history_manager.cpp`)
- Test: `tests/unit/test_print_activity.cpp` (new)
- Translations

**Interfaces:**
- Consumes: `PrintHistoryJob`, `helix::job_timestamp()` (`include/print_history_data.h`).
- Produces (exact, syntax-checked):

```cpp
// include/print_activity.h
namespace helix {
inline constexpr int kActivityDays = 7;
inline constexpr int kMinVisibleBarPct = 6;
using ActivityDayStarts = std::array<double, kActivityDays + 1>;
[[nodiscard]] ActivityDayStarts local_day_starts(std::time_t now);
struct WeekActivity {
    std::array<double, kActivityDays> hours_by_day{};
    double week_hours = 0.0;
    int week_prints = 0;
};
[[nodiscard]] WeekActivity summarize_week(const std::vector<PrintHistoryJob>& jobs, const ActivityDayStarts& starts);
[[nodiscard]] std::array<int, kActivityDays> scale_day_bars(const std::array<double, kActivityDays>& hours);
[[nodiscard]] std::string format_print_hours(double hours);
[[nodiscard]] std::string format_print_count(uint64_t prints);
[[nodiscard]] std::string format_filament_length(double mm);
struct LifetimeSummary { uint64_t prints = 0; double hours = 0.0; double filament_mm = 0.0; };
[[nodiscard]] std::string format_lifetime_line(const LifetimeSummary& summary, bool with_filament);
[[nodiscard]] std::string weekday_initial(std::string_view abbreviation);
}

// include/locale_formats.h, namespace helix::ui
std::string format_localized_weekday_abbr(const struct tm* tm_info);  // wraps the file's get_day_name()
```

Why not `count_trend_buckets()` (`include/print_history_data.h#count_trend_buckets`): it counts jobs into rolling 24-hour windows ending now for the dashboard's chart; this sums `print_duration` into local calendar days. Different rule, different caller; say so in a one-line comment on `summarize_week`.

Rules:
- `local_day_starts(now)`: `localtime_r(now)`, zero h/m/s, `tm_isdst = -1`, then for offsets −6..+1 days set `tm_mday += offset` and `mktime()`. Strictly increasing.
- `summarize_week`: for each job, `t = job_timestamp(job)`; skip `t <= 0`; find `i` with `starts[i] <= t < starts[i+1]`; skip if none; add `max(0, print_duration) / 3600` to day `i`, `week_prints++`.
- `scale_day_bars`: max = largest hour value; max ≤ 0 → all 0; else `round(100 * h / max)`, and any `h > 0` that rounds below `kMinVisibleBarPct` becomes `kMinVisibleBarPct`.
- `format_print_hours(h)`: `h <= 0` or not finite → "0 h"; `h < 1` → `"{} min"` with minutes = `max(1, round(h*60))` (and 60 → "1 h"); `h < 10` → one decimal, rounded, ".0" dropped, and a value that rounds to 10.0 goes to the whole-hour branch; else `round(h)` whole. Keys: `"{} h"`, `"{} min"`.
- `format_print_count`: `"1 print"` for 1, `"{} prints"` otherwise (0 included).
- `format_filament_length(mm)`: `mm <= 0` or not finite → ""; `m = mm / 1000`; `m < 10` → one decimal, rounded, ".0" dropped, never below "0.1 m" for a positive length, and a value that rounds to 10.0 goes to the whole branch; else `round(m)` whole. Key `"{} m"`.
- `format_lifetime_line`: `format_print_count(prints) + " · " + format_print_hours(hours)`, plus `" · " + format_filament_length(mm)` when `with_filament` and the length string is non-empty.
- `weekday_initial`: empty → ""; take the first UTF-8 code point by its lead byte (1 to 4 bytes); uppercase it if ASCII `a`..`z`.

- [ ] **Step 1: Write the failing tests** (`[print_activity]`). Day-boundary cases set `TZ` with an RAII guard that saves and restores `TZ` and calls `tzset()` (see `tests/unit/test_timezone_env.cpp#EnvGuard`), using the POSIX rule string `"EST5EDT,M3.2.0,M11.1.0"` so no zoneinfo files are needed:
  - `local_day_starts` at 2026-10-14 15:00 local: `[6]` is 2026-10-14 00:00 local, `[7]` 2026-10-15 00:00, `[0]` 2026-10-08 00:00, each a whole local midnight (`localtime_r` reads h:m:s 0:0:0).
  - DST: `now` = 2026-11-03 12:00 local; the day holding 2026-11-01 is 25 hours long (`starts[i+1] - starts[i] == 90000`).
  - `summarize_week` with synthetic starts `{0, 86400, ..., 7*86400}` (integers in seconds):
    - a job with `end_time` in day 3 and `print_duration` 7200, `total_duration` 9000 → `hours_by_day[3] == 2.0` (not 2.5), `week_prints == 1`.
    - an in-progress row: `end_time 0`, `start_time` in day 6 → counted in day 6.
    - a print spanning midnight: `start_time` in day 2, `end_time` in day 3 → day 3 only.
    - `end_time` before `starts[0]`, `end_time >= starts[7]` (future), and both timestamps 0 → skipped; `week_prints` counts only the counted ones.
    - negative `print_duration` contributes 0.
    - `week_hours` equals the sum of `hours_by_day`.
  - `scale_day_bars`: `{0,0,0,0,0,0,0}` → all 0; `{1,2,4,0,0,0,8}` → `{13,25,50,0,0,0,100}`; `{0.01,0,0,0,0,0,10}` → `{6,0,0,0,0,0,100}`.
  - `format_print_hours`: `0 → "0 h"`, `-3 → "0 h"`, `0.004 → "1 min"`, `0.75 → "45 min"`, `0.999 → "1 h"`, `1.0 → "1 h"`, `2.54 → "2.5 h"`, `2.04 → "2 h"`, `9.96 → "10 h"`, `23.4 → "23 h"`, `1204.6 → "1205 h"`.
  - `format_print_count`: `0 → "0 prints"`, `1 → "1 print"`, `128 → "128 prints"`.
  - `format_filament_length`: `0 → ""`, `-5 → ""`, `30 → "0.1 m"`, `2540 → "2.5 m"`, `2040 → "2 m"`, `9960 → "10 m"`, `412300 → "412 m"`, `12345678 → "12346 m"`.
  - `format_lifetime_line({128, 412.0, 412300.0}, true)` → `"128 prints · 412 h · 412 m"`; same with `false` → `"128 prints · 412 h"`; with filament 0 and `true` → `"128 prints · 412 h"`.
  - `weekday_initial`: `"Mon" → "M"`, `"mié." → "M"`, `"dim." → "D"`, `"пн" → "п"`, `"月" → "月"`, `"" → ""`.
  - `format_localized_weekday_abbr`: with the language set to `en` through `locale_set_language("en")`, a `tm` with `tm_wday = 1` gives a string whose `weekday_initial` is `"M"`.

- [ ] **Step 2: Run and watch them fail**: `make t F='[print_activity]'`.
- [ ] **Step 3: Implement.** `src/print/print_activity.cpp` uses `lv_tr()` for the unit keys (as `src/ui/job_queue_state.cpp#format_up_next_text` does) and `<spdlog/fmt/fmt.h>` for formatting.
- [ ] **Step 4: Run until green**: `make t F='[print_activity]'`.
- [ ] **Step 5: Prove a test can fail**: bucket by `start_time` instead of `job_timestamp()`; the spanning-midnight case must go red. Restore.
- [ ] **Step 6: Translations**: the keys `"{} h"`, `"{} min"`, `"1 print"`, `"{} prints"`, `"{} m"`.
- [ ] **Step 7: Commit**

```bash
git add -N include/print_activity.h src/print/print_activity.cpp tests/unit/test_print_activity.cpp
git commit -m "feat(history): hours per local day and the stats tile's number formats" -- include/print_activity.h src/print/print_activity.cpp include/locale_formats.h src/ui/locale_formats.cpp firmware/helixscreen-esp32/components/helixapp/app_srcs.txt tests/unit/test_print_activity.cpp translations ui_xml/translations
git show --stat HEAD
```

---

### Task 5: The print stats tile

**Files:**
- Create: `src/ui/panel_widgets/print_stats_tile_layout.h`
- Rewrite: `src/ui/panel_widgets/print_stats_widget.h`, `src/ui/panel_widgets/print_stats_widget.cpp`, `ui_xml/components/panel_widget_print_stats.xml`
- Modify: `tests/unit/test_print_stats_widget_lifetime_totals.cpp` (new subject names; delete the weekly-toggle case "print_stats weekly mode still aggregates the cached job list"), `tests/test_helpers/print_stats_test_access.h` (unchanged members; keep it compiling)
- Delete: `tests/unit/test_widget_size_print_stats.cpp` (pins the retired `mode_for_size`)
- Test: `tests/unit/test_print_stats_tile_layout.cpp` (new, pure), `tests/unit/test_widget_size_print_stats_tile.cpp` (new, harness)
- Translations

**Interfaces:**
- Consumes: Task 4 (all of `print_activity.h`, `format_localized_weekday_abbr`), `PrintHistoryManager::get_jobs()`, `is_loaded(HistoryScope::RECENT)`, `ensure_covers_since()`, `add_observer()` / `remove_observer()` (`include/print_history_manager.h`), `api->history().get_history_totals()` (as today), `get_global_history_dashboard_panel().show(lv_obj_t*)` (`include/ui_panel_history_dashboard.h`), the size-ladder helpers listed in Task 3.
- Produces (exact, syntax-checked):

```cpp
// src/ui/panel_widgets/print_stats_tile_layout.h
namespace helix {
enum class PrintStatsContent : int { Loading = 0, NoHistory = 1, Activity = 2 };
[[nodiscard]] constexpr PrintStatsContent print_stats_content(bool history_loaded, int cached_jobs, int64_t lifetime_prints);
enum class PrintStatsSupport : int { None = 0, PrintsOnly = 1, Full = 2 };
struct PrintStatsTileLayoutInput {
    int content_w = 0;
    int content_h = 0;
    PrintStatsContent content = PrintStatsContent::Loading;
    bool lifetime_known = false;
    RungNeeds needs; ///< offers strip, cell, wide and full only
};
struct PrintStatsTileVerdict {
    SizeRung rung = SizeRung::Strip;
    bool fits = false;
    bool show_bars = false;
    bool show_day_labels = false;
    bool side_by_side = false;
    PrintStatsSupport support = PrintStatsSupport::None;
};
[[nodiscard]] constexpr PrintStatsTileVerdict decide_print_stats_tile_layout(const PrintStatsTileLayoutInput& in);
}

// src/ui/panel_widgets/print_stats_widget.h
class PrintStatsWidget : public PanelWidget {
  public:
    PrintStatsWidget();
    ~PrintStatsWidget() override;
    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    void on_activate() override;
    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override;
    bool fits_at(int width_px, int height_px) const override;
    const char* id() const override { return "print_stats"; }
    static void print_stats_clicked_cb(lv_event_t* e);
  protected:
    void on_hooked_root_deleted() override;
  private:
    void fetch_lifetime_totals();
    void refresh();
    [[nodiscard]] PrintStatsTileVerdict decide(int width_px, int height_px,
                                               PrintStatsContent content, bool lifetime_known) const;
    void handle_clicked();
    friend class PrintStatsTestAccess;
    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;
    int granted_w_ = 0;
    int granted_h_ = 0;
    PrintHistoryTotals lifetime_totals_{};
    bool lifetime_totals_valid_ = false;
    bool lifetime_totals_in_flight_ = false;
    helix::HistoryChangedCallback history_observer_;
    ObserverGuard language_observer_;
    helix::AsyncLifetimeGuard lifetime_;
};
void register_print_stats_widget();
```

`mode_for_size()` (both overloads) is removed.

Rules:
- `print_stats_content`: `!history_loaded` → Loading; `cached_jobs == 0 && lifetime_prints <= 0` → NoHistory (a negative `lifetime_prints` means unknown and counts as none here); else Activity.
- `decide_print_stats_tile_layout`: `r = choose_size_rung(...)`, `fits = holds_any_rung(...)`; `show_bars = r >= Cell && content == Activity`; `show_day_labels = show_bars && r == Full`; `side_by_side = r == Wide`; `support` = None when `r == Strip` or `!lifetime_known` or `content != Activity`, PrintsOnly at Cell, Full at Wide and up.

Published subjects (module level, `HELIX_PSRAM_BSS`, owner-only writes through a `LiveTileOwner<PrintStatsWidget>`):

| Subject | Type | Value |
|---|---|---|
| `print_stats_rung` | int | `SizeRung` |
| `print_stats_content` | int | `PrintStatsContent` |
| `print_stats_support` | int | `PrintStatsSupport` |
| `print_stats_show_bars`, `print_stats_show_day_labels`, `print_stats_side_by_side` | int 0/1 | verdict |
| `print_stats_hero` | string | `format_print_hours(week_hours)`; "--" while Loading |
| `print_stats_hero_label` | string | "this week"; "No prints this week" when `week_prints == 0`; "No prints yet" in NoHistory |
| `print_stats_support_text` | string | PrintsOnly: `format_print_count(totals.total_jobs)`; Full: `format_lifetime_line({total_jobs, total_time/3600.0, total_filament_used}, true)`; None: "" |
| `print_stats_day_0` .. `print_stats_day_6` | int 0..100 | `scale_day_bars()`, oldest first |
| `print_stats_day_label_0` .. `print_stats_day_label_6` | string | `weekday_initial(format_localized_weekday_abbr(localtime of starts[i]))` |

The retired subjects (`print_stats_size_mode`, `_view_mode`, `_title`, `_total_prints`, `_total_time`, `_total_time_short`, `_success_rate`, `_weekly`, `_last_print`) are deleted, with the view-mode toggle and `lv_obj_set_style_opa` pressed feedback.


Lifecycle: `attach()` claims the `LiveTileOwner`, installs the delete hook, adds the history and language observers, and calls `refresh()` (direct subject writes only; it runs inside a ScopedFreeze, so nothing is queued and nothing is fetched there). `detach()` and `on_hooked_root_deleted()` keep today's teardown (`lifetime_.invalidate()`, the in-flight flag cleared, pointers dropped) and release the tile; a returned previous owner republishes. Coverage window: `on_activate()` asks `ensure_covers_since(local_day_starts(time(nullptr))[0])` on the next tick (keep the existing `run_next_tick` and its ScopedFreeze comment); the history observer and the totals callback call `refresh()` and `fetch_lifetime_totals()` as today; the language observer calls `refresh()`.

Measured needs, budgeted against the hero sample `format_print_hours(888.0)` in `font_heading` and the label "this week" in `font_small`; `bar_w = space_xs`, `bars_w = 7*bar_w + 6*space_xxs`:
- Strip: `w = max(hero_w, label_w)`, `h = line(heading) + line(small) + row_gap`.
- Cell: `w = max(hero_w, label_w, bars_w, count_sample_w)` with `count_sample_w` = width of `format_print_count(88888)` in `font_small`; `h = line(heading) + line(small) + 2*line(small) /*bars*/ + line(small) /*support*/ + 3*row_gap`.
- Wide: `w = max(hero_w, label_w) + col_gap + bars_w`, plus room for the full line: `w = max(that, full_line_sample_w)` with `format_lifetime_line({88888, 8888, 88888000.0}, true)` in `font_small`; `h = line(heading) + line(small) + line(small) + 2*row_gap`.
- Full: Cell's box plus `line(font_xs)` for day labels, bars minimum `3*line(small)`, raised to the 2x2 size class with `widget_size::w_wide()` / `widget_size::h_taller()`.
- `fits_at()` refuses unless the Strip box fits for Activity.

XML (`panel_widget_print_stats.xml`), one tree:
- Root: padding and gap as `panel_widget_speed_flow.xml`'s root; `<event_cb trigger="clicked" callback="print_stats_clicked_cb"/>`; `<style name="styles.press_wash" selector="pressed"/>`; flex direction switches with `print_stats_side_by_side` through two `bind_style` styles (row vs column) defined in the component's `<styles>` with `layout="flex"` set (`PANEL_WIDGET_GUIDE.md` rule on style-path flex).
- `ps_hero_block` (column): `ps_hero` (`text_heading`, `bind_text="print_stats_hero"`, hidden in NoHistory), `ps_hero_label` (`text_small`, muted, `bind_text="print_stats_hero_label"`).
- `ps_bars` (row, bottom-aligned, `flex_grow="1"`, hidden unless `print_stats_show_bars` is 1): `<repeat count="7">` of a column holding an `lv_bar` (`orientation="vertical"`, `bind_value="print_stats_day_${i}"`, `min_value="0"`, `max_value="100"`, `flex_grow="1"`, accent indicator, the track at a low-opacity token as the baseline) and a `text_tiny` `bind_text="print_stats_day_label_${i}"` hidden unless `print_stats_show_day_labels` is 1.
- `ps_support` (`text_small`, muted, `bind_text="print_stats_support_text"`, long_mode dots, hidden when `print_stats_support` is 0).

`handle_clicked()`: `get_global_history_dashboard_panel().show(parent_screen_)` (the call `src/ui/ui_panel_advanced.cpp` uses).

- [ ] **Step 1: Pure layout tests** (`tests/unit/test_print_stats_tile_layout.cpp`, `[print_stats_tile][layout]`): `print_stats_content` truth table (`(false,5,10)` → Loading; `(true,0,-1)` → NoHistory; `(true,0,0)` → NoHistory; `(true,0,12)` → Activity; `(true,3,-1)` → Activity); rung boundaries with synthetic needs as in Task 3; flags per rung for Activity with lifetime known (Strip: no bars, support None; Cell: bars, PrintsOnly; Wide: bars, side_by_side, Full; Full: bars, day labels, Full); lifetime unknown → support None at every rung; NoHistory → no bars at Full; `fits` monotonic.

- [ ] **Step 2: Widget tests** (`tests/unit/test_widget_size_print_stats_tile.cpp`, `[widget_size][print_stats]`). Seed history the way `tests/unit/test_print_stats_widget_lifetime_totals.cpp#PrintStatsLifetimeFixture` does (mock API, `PrintHistoryManager`, `set_print_history_manager`), with jobs placed relative to `local_day_starts(time(nullptr))`.
  - "medium-tier budgets land on their rung": 114x54 Strip, 114x113 Cell, 234x113 Wide, 234x232 Full.
  - "the hero sums print_duration over the seven bars": two jobs today (`print_duration` 3600 and 5400, `total_duration` 99999) → `print_stats_hero == format_print_hours(2.5)`; `print_stats_day_6 == 100`.
  - "no prints this week keeps the lifetime line": jobs only 10 days ago, totals valid → hero "0 h", label "No prints this week", support text non-empty at Wide.
  - "no history at all": empty cache, totals `total_jobs` 0 → content NoHistory, label "No prints yet", `ps_hero` hidden, bars hidden at Full.
  - "lifetime line appears when totals land after the week": `client.defer_next("server.history.totals")` before activation, load the history → hero shows, support text "" and `ps_support` hidden at Wide; then `client.fire_deferred("server.history.totals")` and drain → support text equals `format_lifetime_line(...)` for the mock's totals, with no `h.resize()` in between.
  - "the lifetime line never comes from the job cache": `client.fail_next("server.history.totals")`, cache holds 3 jobs → support text stays "" at Cell and Wide.
  - "filament shows from Wide, in meters": totals `total_filament_used` 412300 mm → at Wide the support text ends in `" · 412 m"`; at Cell it equals `format_print_count(total_jobs)`.
  - "day labels only at Full": `print_stats_show_day_labels` 0 at Wide, 1 at Full; `print_stats_day_label_6` equals today's initial.
  - "tap opens the history dashboard": send `LV_EVENT_CLICKED` to the tile root and assert the dashboard overlay is on the `NavigationManager` stack (register it as tests/CLAUDE.md requires).
  - "a language switch recomposes the label": `SystemSettingsManager::instance().set_language("de")`, drain; `print_stats_hero_label` equals `lv_tr("this week")` read under `de` and differs from "this week"; set `"en"` back.

Update `test_print_stats_widget_lifetime_totals.cpp`: the server-totals cases read `print_stats_support_text` at a Wide-sized harness instead of `print_stats_total_prints` / `_total_time`; the escalation case's window start becomes `local_day_starts(now)[0]`; delete the weekly-mode case (the toggle no longer exists).

- [ ] **Step 3: Run and watch them fail**: `make t F='[print_stats_tile]'`, `make t F='[widget_size][print_stats]'`, `make t F='[print_stats]'`.
- [ ] **Step 4: Implement** the layout header, widget, XML. Delete `tests/unit/test_widget_size_print_stats.cpp`.
- [ ] **Step 5: Run until green**: the three tags above, `make t F='[teardown][uaf]'` (the print_stats teardown case must still pass), `make t F='[content_fits][sweep]'`.
- [ ] **Step 6: Prove two tests can fail**: (a) sum `total_duration` instead of `print_duration`; the hero case goes red. (b) fall back to `jobs.size()` when totals are invalid; "never comes from the job cache" goes red. Restore.
- [ ] **Step 7: Lint and translations** as Task 3 Step 7, for this tile's files.
- [ ] **Step 8: Commit**

```bash
git add -N src/ui/panel_widgets/print_stats_tile_layout.h tests/unit/test_print_stats_tile_layout.cpp tests/unit/test_widget_size_print_stats_tile.cpp
git commit -m "feat(home): the print stats tile shows this week's print hours with lifetime totals" -- src/ui/panel_widgets/print_stats_tile_layout.h src/ui/panel_widgets/print_stats_widget.h src/ui/panel_widgets/print_stats_widget.cpp ui_xml/components/panel_widget_print_stats.xml tests/unit/test_print_stats_tile_layout.cpp tests/unit/test_widget_size_print_stats_tile.cpp tests/unit/test_widget_size_print_stats.cpp tests/unit/test_print_stats_widget_lifetime_totals.cpp tests/test_helpers/print_stats_test_access.h translations ui_xml/translations
git show --stat HEAD
```

---

### Task 6: Registry: minimum spans, gate, description

**Files:**
- Modify: `src/ui/panel_widget_registry.cpp` (the `job_queue` and `print_stats` rows)
- Modify: `src/ui/panel_widget_manager.cpp#gate_subject_lifetime` (add `printer_has_job_queue` to the `get_printer_state().get_subjects_lifetime()` group)
- Modify: `tests/unit/test_gate_subject_lifetime.cpp` (add `printer_has_job_queue` to the `printer` owner list in `owners()`)
- Test: `tests/unit/test_queue_stats_registry.cpp` (new)
- Translations (the hint and description are `TR_NOOP` keys)

**Interfaces:**
- Consumes: `find_widget_def()` (`include/panel_widget_registry.h`), Tasks 3 and 5 (`fits_at()` that can refuse a sub-cell box).
- Produces: registry rows, column order as the table header comment in `src/ui/panel_widget_registry.cpp`:
  - `{"job_queue", TR_NOOP("Job Queue"), "progress_clock", TR_NOOP("Print jobs waiting to run"), "printer_has_job_queue", TR_NOOP("Job queue not enabled in Moonraker"), CAT_PRINT, false, 4, 4, 2, 1, 8, 6, false, true, true}`
  - `{"print_stats", TR_NOOP("Print Stats"), "printer_3d", TR_NOOP("Print hours this week and lifetime totals"), nullptr, nullptr, CAT_PRINT, false, 4, 4, 2, 1, 6, 4, false, true, true}`

- [ ] **Step 1: Write the failing tests** (`[widget_def][queue_stats]`): both defs have `effective_min_colspan() == 2`, `effective_min_rowspan() == 1`, `colspan == 4 && rowspan == 4`, `default_enabled == false`, `supports_half_col && supports_half_row`; `job_queue`'s gate subject is `"printer_has_job_queue"` and its hint is `"Job queue not enabled in Moonraker"`; `print_stats` has no gate and its description does not contain "success".
- [ ] **Step 2: Run and watch them fail**: `make t F='[queue_stats]'`.
- [ ] **Step 3: Implement** the two rows and the gate owner rows.
- [ ] **Step 4: Run until green**: `make t F='[queue_stats]'`, `make t F='[span_bands]'`, `make t F='[panel_widget][manager][observer]'`, `make t F='[content_fits]'` (the sweep now measures both tiles at their Strip minimum on every shipping geometry; any new clipping fails it and is fixed in the tile, never added to `kKnownClipping`), `make t F='[grid_edit]'`.
- [ ] **Step 5: Prove the gate test can fail**: drop the gate subject from the row; the hint/gate case goes red, and `test_gate_subject_lifetime.cpp`'s "every widget gate subject has an owner row" goes red if the owner row is removed instead. Restore.
- [ ] **Step 6: Commit**

```bash
git add -N tests/unit/test_queue_stats_registry.cpp
git commit -m "feat(home): queue and stats tiles go down to 2x1, and the queue tile needs Moonraker's job_queue" -- src/ui/panel_widget_registry.cpp src/ui/panel_widget_manager.cpp tests/unit/test_gate_subject_lifetime.cpp tests/unit/test_queue_stats_registry.cpp translations ui_xml/translations
git show --stat HEAD
```

---

### Task 7: Content-fits rows for every rung and geometry

**Files:**
- Modify: `tests/unit/test_widget_content_fits.cpp`
- Possibly modify: `tests/test_helpers/` (a seed helper, if the queue and history seeds are reused by more than this file)

**Interfaces:**
- Consumes: `render_at()`, `kShipping`, `exceptions()` and `ContentFitsFixture` in `tests/unit/test_widget_content_fits.cpp`; `JobQueueStateTestAccess`; the history seeding from Task 5.

- [ ] **Step 1: Add the 1920x1080 geometry** if `kShipping` still lacks it: `{"1920x1080", 1920, 1080,  1720, 1056, 10},` after the 1280x720 row, the exact row `feature/default-home-layouts` adds (b6d86b0b4). If that branch has landed by now, skip this step.

- [ ] **Step 2: Seed both tiles for the existing minimum sweep.** Add `JobQueueSeed` and `PrintHistorySeed` RAII seeds beside `BufferReadingSeed` / `ClogReadingSeed` and enable them for `def.id == "job_queue"` and `"print_stats"` in "every home widget renders its content at its authored minimum": the queue seed installs a `JobQueueState` with 4 jobs whose first name is 90 characters with no spaces, auto off, idle, `printer_has_job_queue` = 1; the history seed loads a week of jobs and valid totals with `total_filament_used` 12'345'678 mm and `total_jobs` 88888 (the widest numbers the tiles budget for).

- [ ] **Step 3: Write the rung sweep** as a new case, `"queue and stats tiles render every rung on every shipping geometry"`, `[content_fits][queue_stats]`: for each `kShipping` geometry, for spans 2x1, 2x2, 4x2, 4x4 (each capped by the grid; skip a span the widget's `fits_at` refuses at that geometry, recording it in an INFO list), for each queue state (WaitingForStart, AutoNext, Printing, Empty) and the stats tile with Activity, render with `render_at()` and `CHECK(report.clean())`. No `kKnownClipping` entries may be added for these two widgets; a clip is fixed in the tile.

- [ ] **Step 4: Run**: `make t F='[content_fits]'`. Expected: the new case passes after any fixes the sweep forces in Tasks 3 and 5 files; the existing sweep stays green.

- [ ] **Step 5: Prove the rung sweep can fail**: give `jq_hero` a fixed width wider than the Strip box; the new case goes red at the small geometries. Restore.

- [ ] **Step 6: Commit**

```bash
git commit -m "test(home): queue and stats tiles fit at every rung on every shipping panel" -- tests/unit/test_widget_content_fits.cpp
git show --stat HEAD
```

---

### Task 8: Renders for Preston's review

**Files:** none committed. Screenshots go to the session scratchpad.

- [ ] **Step 1: Build**: `make`.
- [ ] **Step 2: Launch headless at 800x480** with a pinned socket and config dir:

```bash
TREE=queue-stats-tiles
export HELIX_SOCK="/tmp/helix-$TREE.sock" HELIX_CONFIG_DIR="/tmp/helix-config-$TREE"
mkdir -p "$HELIX_CONFIG_DIR"
HELIX_HEADLESS=1 SDL_VIDEODRIVER=dummy ./build/bin/helix-screen --test -s 800x480 -vv \
  --remote-socket "$HELIX_SOCK" > /tmp/helix-$TREE.log 2>&1 &
```

- [ ] **Step 3: Place and capture.** `ctl -s "$HELIX_SOCK" navigate home`; clear the plugin modal if it is up (`ctl click btn_done`); free space with `ctl home remove <id>` as needed; then for each size in cells `1 0.5`, `1 1`, `2 1`, `2 2`: `ctl home place job_queue <col> <row> <w> <h>`, `ctl home place print_stats ...`, `ctl screenshot <scratch>/<state>-<size>-800x480.png`. Read exact text with `ctl text jq_hero`, `ctl text ps_hero` alongside each shot.
  - States: WaitingForStart (default mock: 3 jobs, auto off); Printing (relaunch with `HELIX_MOCK_AUTO_PRINT=1 --sim-speed 6`, capture once the log shows Printing); AutoNext (the same with `HELIX_MOCK_JOB_QUEUE_AUTOMATIC_TRANSITION=1`); WaitingForStart with auto on (that flag, no auto print: the same Start button as auto off); Empty (remove the three jobs with the modal's trash icons); stats Activity (default mock history).
- [ ] **Step 4: Repeat at 480x272** (`-s 480x272`; half-cell sizes are declined at that tier, so start at `1 1`).
- [ ] **Step 5: Stop only your instance**: `for p in $(pgrep -x helix-screen); do grep -qz "$HELIX_SOCK" /proc/$p/cmdline && kill $p; done`.
- [ ] **Step 6: Show the renders** with `show_image` (one per state and size, grouped), and tell Preston exactly what to judge: hero weight, the accent on the button and the bars, the Cell button label, the bars' baseline.

---

### Task 9: Docs, plan clean-up, and the mutation gate

**Files:**
- Modify: `docs/user/guide/home-panel.md` (the Print Stats and Job Queue rows of "Print & Status": description, Min `1x0.5`, Hardware Required for Job Queue "Moonraker `[job_queue]`"; remove both names from the "two-cell minimum width" sentence; add a short "Job Queue widget" subsection before "Job Queue Manager": what each state shows; Bed clear · Start next starts the next job on the printer in one tap, as queued, without the pre-print options a job carries when started from the file view (say so plainly); "starts automatically" appears only while a print runs with Moonraker's `automatic_transition` on; tap elsewhere opens the manager; Print Stats tap opens Print History)
- Modify: `docs/devel/PANEL_WIDGET_GUIDE.md` § "The sized-widget family" (two rows: `job_queue` with `job_queue_tile_layout.h` and `include/job_queue_tile_state.h`, gate `printer_has_job_queue`; `print_stats` with `print_stats_tile_layout.h` and `include/print_activity.h`, no gate) and the count in "All seven offer half-cell spans" (now nine)
- Modify: `docs/devel/CHANGELOG_1_1_DRAFT.md` (one user-facing entry per tile, in `docs/devel/CHANGELOG_STYLE.md` voice)
- Delete: `docs/devel/plans/2026-10-14-queue-stats-tiles-design.md`, `docs/devel/plans/2026-10-14-queue-stats-tiles.md` (shipped scaffolding, `docs/CLAUDE.md` § "Plans and Specs")

- [ ] **Step 1: Write the docs.** Cite code as `path#symbol`, never line numbers. User docs carry no source paths. Run `python3 scripts/check_doc_refs.py` and `make check-doc-anchors`.
- [ ] **Step 2: Commit docs and the plan deletion**

```bash
git commit -m "docs(home): the job queue and print stats tiles" -- docs/user/guide/home-panel.md docs/devel/PANEL_WIDGET_GUIDE.md docs/devel/CHANGELOG_1_1_DRAFT.md docs/devel/plans/2026-10-14-queue-stats-tiles-design.md docs/devel/plans/2026-10-14-queue-stats-tiles.md
git show --stat HEAD
```

- [ ] **Step 3: Mutation gate, once, for the whole branch.** Push the feature branch (never main): `git push -u origin feature/queue-stats-tiles`. Then:

```bash
scripts/test-host-run.sh mutate --tests '[job_queue_tile],[next_media],[widget_size][job_queue],[print_activity],[print_stats_tile],[widget_size][print_stats],[print_stats],[queue_stats],[content_fits]'
```

Expected: `CLEAN`. A `SURVIVED` hunk gets a test in the task that owns it and a rerun of that tag; an `INCOMPLETE` names paths it could not mutate (XML under `ui_xml/` is mutated in place; `lib/` and `tests/` are not). Record one line per mutation in the last commit's body, e.g. `mutation: dropped the Ready term from AutoNext in job_queue_tile_state(); test_job_queue_tile_state went red`.

- [ ] **Step 4: Hand off.** The landing session runs `make full-test-run` once on the batch (CLAUDE.md § "Land in batches and gate once"). Release the `worktree:queue-stats-tiles` claim.
