# Telemetry Data Correctness Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make every number the telemetry pipeline stores and the dashboard shows correct.

**Architecture:**
- **Worker track:** a schema table keyed by point kind drives both the AE writer and the
  query column lookups; delta-era queries dedup on an idempotency key.
- **Client track:** one envelope helper, delta snapshots with delivery guarantees, stable
  overlay ids.
- **A contract test** ties the two tracks together.

**Tech Stack:** C++17 / Catch2 (client), TypeScript / Cloudflare Workers / vitest (worker),
Vue 3 (dashboard).

**Spec:** `docs/devel/plans/2026-10-14-telemetry-data-correctness-design.md` (read it first).

## Global Constraints

- Existing AE column positions (`blobN` / `doubleN` per point kind) never move. New fields
  take free slots only (blobs 13-20, doubles 9-20).
- AE write time is the time axis. Client `timestamp` is never used for bucketing.
- No backfill path survives this work.
- Every count and sum in `queries.ts` is `_sample_interval`-weighted:
  - `count()` becomes `sum(_sample_interval)`
  - `sum(x)` becomes `sum(x * _sample_interval)`
  - `avg(x)` becomes `sum(x * _sample_interval) / sum(_sample_interval)`

  Weighting is exact when AE did not sample (the interval is 1). `count(DISTINCT ...)` stays.
- Delta point kinds: `panel_usage`, `overlay_visit`, `widget_interaction`,
  `connection_stability`. The idempotency key is `(blob1 device_id, boot_id, snapshot_seq)`.
- `schema_version` 3 marks delta-era client events.
- Client code follows CLAUDE.md:
  - spdlog only
  - `#include "hv/json.hpp"`
  - no exceptions in firmware-compiled code
  - comments state constraints, never history
- Worker deploys are NOT part of any task. The controller asks Preston before each
  `wrangler deploy`.
- **Commits:** subject plus about 4 lines of body, run from the task's worktree, using
  `git commit -- <paths>` (with `git add -N` for new files), never `git add -A`.

## Review Focus

1. **A resent batch** (worker stored it, the response was lost). The dashboard counts it once.
   Pinned in Task 5 (dedup test) and Task 7 (the client resends the same seq).
2. **Power cut mid-window.** Activity since the last snapshot is reported exactly once on
   the next boot. Pinned in Task 7.
3. **Mixed fleet:** old clients sending cumulative schema-2 rows next to schema-3 deltas.
   Old rows never enter a delta sum. Pinned in Task 5.
4. **A filter on an event kind that lacks the field.** That kind drops out; it never matches
   another column. Pinned in Task 4.
5. **Update events after the envelope lands.** `update_failed`'s target version still maps
   to its own column, not `HELIX_VERSION`. Pinned in Tasks 3 and 6.

## Branches and order

| Track | Worktree / branch | Tasks |
|-------|-------------------|-------|
| Worker | `.worktrees/telemetry-worker` / `feature/telemetry-worker` | 1, 2, 3, 4, 5, 11 |
| Client | `.worktrees/telemetry-data-correctness` / `feature/telemetry-data-correctness` | 6, 7, 8, 9, then merge worker branch, then 10 |
| 1.0 port | branch off `origin/release/1.0`, after main lands | 12 |

The two tracks run in parallel. Within a track, tasks run in order.

**Worker tests:** `(cd server/telemetry-worker && npx vitest run)`.

**Client tests:** `make t F='[telemetry]'` from the client worktree root. Build first per
CLAUDE.md: check `pgrep -x -d' ' 'make|clang++|cc1plus'`.

---

### Task 1: Worker quick fixes (Phase 2a)

**Files:**
- Modify: `server/telemetry-worker/src/analytics.ts`, `src/queries.ts`, `src/index.ts`
- Modify: `server/analytics-dashboard/src/utils/chart.ts`
- Test: `server/telemetry-worker/src/__tests__/index.test.ts`

**Interfaces:**
- Produces: `export function compareVersions(a: string, b: string): number` in the
  dashboard's `chart.ts`.
- Produces: the same rule as `compareVersions` in `server/telemetry-worker/src/versions.ts`
  (new file).
- `/health` returns `{ status: "healthy", git_sha: env.GIT_SHA ?? "unknown" }`. `Env` gains
  `GIT_SHA?: string`.

**Changes:**
- Mapper reads `total_connected_sec`, `total_disconnected_sec`, `auto_update_channel`,
  `screen_blank_timeout_sec` and `estimated_duration_bucket`.
- `uniqExact(` becomes `count(DISTINCT `.
- `toYesNo` compares `String(r.name) === "1"`.
- `sanitizeFilterValue` keeps spaces, letters, digits, `.`, `-`, `_`, `(`, `)`, `/` and `+`,
  and removes `'`, `"`, `\` and `;`.
- **Prerelease ordering:**
  1. Numeric parts compare numerically.
  2. A release sorts after any prerelease of the same version.
  3. Prerelease identifiers compare numerically when both are numeric.

**Tests (write first, see them fail):**
- The mapper puts a `total_connected_sec: 3600` value into the connection point's
  connected-seconds double.
- The same holds for each of the other three renamed fields.
- No SQL string from any `*Queries()` builder contains `uniqExact`. Iterate every exported
  builder with `range=30`.
- `toYesNo([{name: 1, count: 3}, {name: 0, count: 1}])` yields yes=3, no=1. Use whatever
  shape `toYesNo` takes; export it if it is file-local.
- `buildFilterClause({model: "Voron 2.4"})` contains `'Voron 2.4'`.
- `buildFilterClause({model: "x'; DROP"})` contains no `'` inside the value.
- `compareVersions` sorts `["1.1.0","1.1.0-beta.10","1.0.5","1.1.0-beta.9","0.99.118"]` to
  `["0.99.118","1.0.5","1.1.0-beta.9","1.1.0-beta.10","1.1.0"]` (both copies).
- `GET /health` includes `git_sha`.

**Also:**
- In `docs/devel/TELEMETRY_ADMIN.md` § Deployment, the deploy command becomes
  `wrangler deploy --var GIT_SHA:$(git rev-parse --short HEAD)`.
- Commit: `fix(telemetry-worker): field names, uniqExact, filter sanitizer, prerelease sort, deployed SHA`.

### Task 2: Undercount measurement (Phase 0)

**Files:**
- Modify: `server/telemetry-worker/src/index.ts`, `src/queries.ts`
- Create: `server/telemetry-worker/scripts/reconcile.ts`
- Test: `src/__tests__/index.test.ts`

**Interfaces:**
- `export function aeCountsQuery(date: string): string`. The `date` is validated as
  `YYYY-MM-DD`; anything else throws before any SQL is built. The query returns
  `index1 AS kind, count() AS rows, sum(_sample_interval) AS weighted,
  count(DISTINCT blob1) AS devices` for `toDate(timestamp) = '<date>'`, grouped by `index1`.
- `GET /v1/admin/ae-counts?date=YYYY-MM-DD` requires the admin key and returns
  `{ date, kinds: [{kind, rows, weighted, devices}] }`. A bad date gets a 400.
- `scripts/reconcile.ts <date>`:
  - reads `HELIX_TELEMETRY_ADMIN_KEY` from env
  - lists and gets R2 batches for that day
  - maps them with `mapEventToDataPoints`
  - fetches `ae-counts`
  - prints a per-kind table: `kind, r2_points, ae_rows, ae_weighted, kept%`
  - runs as `(cd server/telemetry-worker && npx vite-node scripts/reconcile.ts 2026-10-12)`

**Tests:**
- `aeCountsQuery("2026-10-12")` groups by `index1` and filters that date.
- `aeCountsQuery("2026-10-12'; --")` throws.
- The endpoint without the admin key returns 401.
- With a bad date it returns 400.

**Also:**
- Remove the backfill path: the `/v1/admin/backfill` route, `scripts/backfill-analytics.ts`,
  `scripts/telemetry-backfill.sh` and their docs.
- `TELEMETRY_ADMIN.md` gains a "Reconciling AE against R2" section and loses backfill.
- Commit: `feat(telemetry-worker): per-kind AE counts endpoint and an R2 reconcile script; drop backfill`.

### Task 3: Schema table drives the writer (Phase 2b, part 1)

**Files:**
- Create: `server/telemetry-worker/src/schema.ts`
- Modify: `src/analytics.ts`
- Test: `src/__tests__/schema.test.ts`, existing `index.test.ts` (positional assertions stay green)

**Interfaces (produced):**
```ts
export type FieldType = "blob" | "double";
export interface ColumnDef { field: string; type: FieldType; slot: number } // slot 1-based
export interface PointKind { kind: string; columns: ColumnDef[] }           // kind == index1
export const POINT_KINDS: Record<string, PointKind>;
export const EVENT_FANOUT: Record<string, string[]>;  // event type -> point kinds it writes
export const R2_ONLY: Record<string, string[]>;       // event type -> dotted field paths deliberately not mapped
export function col(kind: string, field: string): string; // "blob7" / "double3"; throws on unknown
export function hasCol(kind: string, field: string): boolean;
```

**Rules:**
- Every existing mapping is transcribed with its current slot. Nothing is renumbered.
- **New columns, in free slots, appended:**
  - `schema_version`, `boot_id` and `snapshot_seq` on the four delta kinds
  - `app_version`, `app_platform` and `app_board` on `print_outcome`,
    `print_start_context`, `error_encountered`, `connection_stability` and
    `hardware_profile` (and on their fan-out kinds)
  - `async_lifetime_skips` gets a point kind or an `R2_ONLY` entry
- **Update events:**
  - `update_failed` maps target version from `event.version` and from-version from
    `event.from_version`.
  - `update_success` maps its version from `event.version`.
  - Neither reads `app_version` for those columns.
- `mapEventToDataPointInternal` builds every point through the table: blob and double
  arrays are filled by slot, and missing values are `""` / `0`.

**Tests:**
- A table self-check: no two columns in a kind share a slot, blob slots are 1-20, double
  slots are 1-20, and every `EVENT_FANOUT` target exists in `POINT_KINDS`.
- A golden-slot snapshot of `POINT_KINDS`, stored as a vitest inline snapshot of
  `kind -> field -> slot`. Any move shows up as a diff a reviewer must accept.
- An `update_failed` event with `version: "1.0.5"` and `app_version: "1.0.4"` writes
  `1.0.5` to the target column.
- A `print_outcome` with `app_version` writes it to its new slot.
- All existing `index.test.ts` positional assertions still pass unchanged.

**Commit:** `refactor(telemetry-worker): one schema table per point kind drives the AE writer`.

### Task 4: Queries resolve columns and filters from the table (Phase 2b, part 2)

**Files:**
- Modify: `server/telemetry-worker/src/queries.ts`, `src/index.ts`
- Test: `src/__tests__/queries.test.ts` (new)

**Interfaces:**
- Consumes: `col`, `hasCol`, `POINT_KINDS` from Task 3.
- Produces:
```ts
export interface FilterParams { platform?: string; version?: string; model?: string; board?: string }
// One kind: " AND blobX IN (...)" for each filter; returns " AND 0" when the kind lacks a filtered field.
export function filterFor(kind: string, filters?: FilterParams): string;
// Several kinds: " AND ((index1='a' AND ...) OR (index1='b' AND ...))", dropping kinds that lack a field.
export function filterForKinds(kinds: string[], filters?: FilterParams): string;
export function wCount(): string;            // "sum(_sample_interval)"
export function wSum(expr: string): string;  // "sum((expr) * _sample_interval)"
export function wAvg(expr: string): string;  // "sum((expr) * _sample_interval) / sum(_sample_interval)"
```
- `buildFilterClause` is deleted. Every query names its columns through `col(kind, field)`;
  no literal `blobN` / `doubleN` remains in `queries.ts` outside `schema.ts`.
- The `model` filter maps to `detected_model` on `session` and `hardware_profile`. On kinds
  without a model column it excludes them.
- Queries that span kinds without `index1` (overview total events, events over time)
  enumerate their kinds explicitly and use `filterForKinds`.

**Tests:**
- `filterFor("print_outcome", {platform: "pi"})` uses print_outcome's `app_platform`
  column, not `filament_type`'s.
- `filterFor("memory_warning", {model: "Voron 2.4"})` returns `" AND 0"`.
- `filterForKinds(["crash","session"], {platform: "pi"})` produces an OR of two branches
  with each kind's platform column.
- `grep -E "blob[0-9]|double[0-9]" src/queries.ts` finds nothing. Write it as a vitest that
  reads the file.
- Every exported `*Queries()` builder, called with every single filter, produces SQL
  containing no `count()` and no bare `sum(` / `avg(` outside the w-helpers. This checks
  the SQL text.

**Commit:** `refactor(telemetry-worker): queries ask the schema table for columns and filters`.

### Task 5: Delta-era queries, derived feature adoption, dashboard null state (Phase 2b, part 3)

**Files:**
- Modify: `server/telemetry-worker/src/queries.ts`, `src/index.ts`
- Modify: `server/analytics-dashboard/src/services/api.ts` and `src/views/{EngagementView,ReliabilityView,StabilityView,UxInsightsView,FeaturesView}.vue`
- Test: `src/__tests__/queries.test.ts`, `index.test.ts`

**Interfaces:**
- `export function deltaSource(kind: string, dims: string[], days: number, filters?: FilterParams): string`
  returns a subquery that:
  1. selects `schema_version >= 3` rows of that kind
  2. dedups on `(blob1, boot_id, snapshot_seq, ...dims)` with `any()` for every other column
     used downstream
  3. exposes `_sample_interval`

  Every query over a delta kind selects `FROM (deltaSource(...))`.
- Response fields from delta kinds are `number | null`, plus `delta_era_devices: number`
  (distinct devices with schema >= 3 rows in range). A value is `null` when
  `delta_era_devices === 0`.
- **Feature adoption** is computed from deltas: `features: [{id, devices, adoption_rate}]`.
  - `devices` counts distinct devices with a count above 0 for that overlay or widget id.
  - The denominator is `delta_era_devices`.
  - Ids come from `overlay_visit` names and `widget_interaction` widget ids.
  - The `feature_adoption` point kind stays in the schema table for old rows but is no
    longer queried.
- **Dashboard:** `api.ts` types those fields as `number | null`. Each view shows "Waiting
  for devices on the new client ({n} so far)" where a value is null. The Features view lists
  derived adoption.

**Tests:**
- `deltaSource("panel_usage", ["panel"], 30)` contains `schema_version` / `>= 3` on the
  resolved column, and a GROUP BY over device, boot_id, snapshot_seq and panel.
- **Duplicate key counts once.** Unit-test the TS post-processing that turns rows into the
  response (feed it two identical deduped rows).
  - If dedup is pure SQL, assert the SQL shape instead and add a fixture-driven test of the
    endpoint with a mocked `executeQuery` returning the deduped result.
- The engagement endpoint with a mocked AE returning zero delta-era devices responds with
  `avg_session_sec: null`, `delta_era_devices: 0`.
- `vue-tsc --noEmit` passes for the dashboard: `(cd server/analytics-dashboard && npx vue-tsc --noEmit)`.

**Commit:** `feat(telemetry): delta-era queries dedup on boot and sequence; adoption derived from visits`.

### Task 6: Client envelope, boot_id, schema 3

**Files:**
- Modify: `include/system/telemetry_manager.h`, `src/system/telemetry_manager.cpp`
- Test: `tests/unit/test_telemetry_manager.cpp`

**Interfaces:**
- `void stamp_envelope(nlohmann::json& event) const;` (private) sets:
  - `schema_version` = `SCHEMA_VERSION` (now `3`)
  - `app_version` = `HELIX_VERSION`
  - `app_platform` = `helix::platform::current_key()`
  - `app_board` = `helix::board::current_info().key`
  - `boot_id` = `boot_id_`
- `std::string boot_id_;` is set once in `init()`: 16 lowercase hex characters from the
  same RNG the device id uses (find it in the file). It is never persisted.
- Every `build_*_event` calls `stamp_envelope`. No builder writes those five keys itself.
- Update events keep `version` / `from_version` as today.

**Tests (first):**
- For every builder reachable from the test fixture (there are existing tests per builder;
  extend them, don't fork new fixtures):
  - all five envelope keys are present and non-empty
  - `schema_version == 3`
- `boot_id` is identical across two events of one `init()` and differs after
  `shutdown()` + `init()`.
- `build_update_failed_event("x","1.0.5", ...)` has `version == "1.0.5"` and
  `app_version == HELIX_VERSION`.

**Commit:** `feat(telemetry): every event carries one envelope with version, platform, board and boot id`.

### Task 7: Delta snapshots with delivery guarantees

**Files:**
- Modify: `include/system/telemetry_manager.h`, `src/system/telemetry_manager.cpp`
- Test: `tests/unit/test_telemetry_manager.cpp`

**Behavior:**
- **What deltas contain:**
  - `panel_usage` and `connection_stability` carry only counts and durations accumulated
    since the previous snapshot of this boot.
  - `snapshot_seq` increments per snapshot and starts at 0 per boot.
  - `session_duration_sec` is the window length.
  - After building the event, the accumulators reset to zero.
- **Outages:** an outage in progress keeps its start across snapshots.
  `longest_disconnect_sec` in a window reports the longest outage ending in it, or the one
  still open measured to now. An open outage is never split.
- **Persistence:**
  - `fire_periodic_snapshot` calls `save_queue()` after enqueueing, next to the state save.
  - The hourly auto-send timer also persists the unsent accumulator, with its `boot_id` and
    next `snapshot_seq`.
- **Recovery:** on `init()`, a recovered accumulator from a previous boot is emitted as one
  delta per kind under that previous `boot_id` and its next seq, then cleared. Its window is
  `[last snapshot, last persist]`.
- **Queue limit:** at `MAX_QUEUE_SIZE`, an incoming delta event whose type is already queued
  merges into the queued one (counts add, durations add, longest takes the max, seq takes
  the newer). Other events keep drop-oldest.
- **Removed:** the cumulative-carryover state (`save_snapshot_state` / `load_snapshot_state`
  carrying lifetime totals). The persisted file now holds only the unsent accumulator.

**Tests (first; rewrite the existing "Snapshot: state persisted to disk and recovered" test
and any test pinning cumulative totals):**
- Three snapshots with known activity between them sum, across the three events, to the
  true totals. No window is counted twice.
- Activity, then hourly persist, then simulated unclean exit (no shutdown), then `init()`:
  - one recovery delta under the old `boot_id` carries exactly the post-snapshot activity
  - the new boot's first delta excludes it
- A disconnect at t=3h50m that reconnects at t=4h20m, with a snapshot at 4h:
  - the first window reports `longest_disconnect_sec` >= 600 as open
  - the second window reports 1800 for the closed outage, counted once as one outage
- A full queue (100 events) plus a new `panel_usage` merges: the queue length stays 100,
  and the queued panel_usage counts equal the sum.
- `fire_periodic_snapshot` leaves the queue file on disk containing the new events.

**Commit:** `feat(telemetry): snapshots send deltas that survive a power cut and a full queue`.

### Task 8: Stable overlay ids; feature_adoption event removed

**Files:**
- Modify: `src/ui/ui_nav_manager.cpp` (the `notify_overlay_opened` call site),
  `src/system/telemetry_manager.cpp`, `include/system/telemetry_manager.h`
- Test: `tests/unit/test_telemetry_manager.cpp`; an LVGL-fixture test for the nav id rule
  where nav tests live (grep `push_overlay` in `tests/unit`)

**Interfaces:**
- Pure decision function, in a header next to `NavigationManager` or in `telemetry_manager.h`:
  ```cpp
  // Stable telemetry id for an overlay: the root's object name, else the lifecycle name, else "unnamed".
  std::string overlay_telemetry_id(const char* root_obj_name, const char* lifecycle_name);
  ```
  The nav call site passes `lv_obj_get_name(overlay_panel)` and `lc ? lc->get_name() : nullptr`.
- `build_feature_adoption_event`, its timer and its tests are deleted.
- `build_settings_snapshot_event` gains `spoolman_configured`, `theme_changed` and
  `multi_printer` booleans, read from the same sources the deleted builder used.

**Tests:**
- `overlay_telemetry_id("fan_control_overlay", "Fan Control") == "fan_control_overlay"`
- `overlay_telemetry_id(nullptr, "Fan Control") == "Fan Control"`
- `overlay_telemetry_id("", nullptr) == "unnamed"`
- The settings snapshot carries the three booleans.

**Commit:** `feat(telemetry): overlays report a stable id; adoption flags move to the settings snapshot`.

### Task 9: Smaller client fixes

**Files:**
- Modify: `src/system/telemetry_manager.cpp`, `include/system/telemetry_manager.h`,
  `src/system/*_settings_manager.cpp`
- Test: `tests/unit/test_telemetry_manager.cpp`, the settings tests that cover
  `persisted_setting`

**Changes:**
- `record_error` accepts any non-empty category matching `[a-z0-9_]{1,32}`. Per-category
  rate-limiting stays. Rewrite the allow-list tests.
- Drop rate: `total_frame_count` in `performance_snapshot` counts the same window as
  `dropped_frame_count`. Use the ring's own sample count, or count drops over all frames;
  pick whichever the existing frame-timing code supports without a new hot-path cost.
- `task_handler_max_ms` is removed from the event and from the worker schema (Task 3's
  table entry is kept for old rows; the client stops sending it).
- **`telemetry_key`:**
  - Every `PersistedSetting` in the display, audio, safety, input and system settings
    managers gets a stable snake_case `telemetry_key`.
  - Keys are unique across managers. Add a test that walks every manager's setting table
    and asserts uniqueness and non-null.
  - Free-text settings (names, hosts, paths) stay `nullptr`, and the test lists them
    explicitly as exempt.

**Tests:**
- `record_error("ui","main_loop_hang",...)` enqueues an event.
- `record_error("Bad Category",...)` does not.
- Drop rate over a window with N frames and D drops reports exactly D / N.
- The key-uniqueness and coverage test above.

**Commit:** `fix(telemetry): every error category reaches the worker, drop rate shares one window, settings changes cover every user setting`.

### Task 10: Contract test (Phase 3)

Runs on the client branch after `feature/telemetry-worker` is merged into it.

**Files:**
- Create: `tests/fixtures/telemetry/<event>.json`, one per event type
- Test: `tests/unit/test_telemetry_contract.cpp` (`[telemetry][contract]`),
  `server/telemetry-worker/src/__tests__/contract.test.ts`

**Interfaces:**
- **C++ side:**
  - Builds each event from the existing telemetry test fixture state.
  - Normalizes `timestamp`, `device_id` and `boot_id` to fixed strings.
  - Compares to the golden. With `HELIX_UPDATE_GOLDENS=1` it rewrites the golden instead.
- **TS side**, for each fixture:
  - Every leaf path is either mapped by some `POINT_KINDS` column of the event's
    `EVENT_FANOUT` kinds or listed in `R2_ONLY[event]`.
  - `mapEventToDataPoints` returns at least one point.
  - For update events, the target-version column equals the fixture's `version`.
- Path conventions for fan-out maps (panel names, overlay ids): a map-valued field is
  matched as `field.*`.

**Tests:**
- Prove each side can go red:
  - rename one client field and see the C++ golden test fail
  - remove one mapping and see the vitest fail
- Revert both.

**Commit:** `test(telemetry): client goldens and the worker mapping are checked against each other`.

### Task 11: Docs

**Files:** `docs/devel/TELEMETRY_ADMIN.md`

**Changes:**
- Event table: envelope fields, delta semantics, removed `feature_adoption`, new settings
  flags.
- New sections:
  - "Delta snapshots and the idempotency key"
  - "Weighted counts"
  - "Reconciling AE against R2"
- Delete the stale note that newer events have no dashboard views.
- Deploy command with `GIT_SHA`.

**Commit:** `docs(telemetry): delta snapshots, weighted counts, reconcile, deploy provenance`.

### Task 12: release/1.0 port (separate branch, after main lands)

- Branch `fix/telemetry-envelope-1.0` off `origin/release/1.0`.
- Hand-port Tasks 6, 7, 8 and 9:
  - `app_board` is `""`
  - the platform comes from `UpdateChecker::get_platform_key()`
- Port Task 10's C++ goldens, regenerated for 1.0.
- The worker needs no port.
- Gate: `make t F='[telemetry]'` and `make full-test-run` on that branch.

**Commit:** `fix(telemetry): 1.0 sends the delta snapshots and envelope main does`.

---

## Finish

1. Each track gets an independent Opus review of its branch.
2. `make mutate-diff` once on the client branch.
3. `make full-test-run` once on the combined result.
4. Land on main with the claim protocol.
5. Delete both plan files in the landing change; durable knowledge is in
   `TELEMETRY_ADMIN.md` by then.
6. Ask Preston before the worker deploy.
7. Run `reconcile.ts` after the deploy and report the per-kind numbers.
