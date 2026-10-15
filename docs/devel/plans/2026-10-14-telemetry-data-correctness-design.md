# Telemetry Data Correctness - Design

Spec 1 of 3. The telemetry dashboard (`analytics.helixscreen.org`) shows numbers that are
wrong at the data layer, not just badly drawn. This spec makes the numbers right. Spec 2
(dashboard information architecture, drill-down, model-name folding) and spec 3 (the
feedback loop) are out of scope here. Spec 3 covers emailed summaries, a summary page,
GitHub issues for actionable clusters, release-gate warnings and printer-DB PRs.

## Decisions already made

| Decision | Ruling |
|----------|--------|
| Sequencing | Data correctness first; dashboard and loop follow |
| Cumulative snapshots | Client sends deltas keyed by `(device_id, boot_id, snapshot_seq)`; old rows are excluded, not salvaged |
| Release line | Client changes land on `main`; the `release/1.0` port is a separate hand port (470 lines of drift in `telemetry_manager.cpp`) |
| Deploys | Worker deploys are manual `wrangler deploy`; each one is confirmed with Preston first |
| Backfill | None. `writeDataPoint` takes no timestamp and AE has no row identity, so a replay lands on the wrong day and duplicates rows already stored |
| Time axis | AE write time. Client `timestamp` is never used for bucketing: upload lag has a median of about 10h, and devices without an RTC report years-old dates |

## What is wrong today

Grouped by cause, because each cause is one fix.

1. **Cumulative counters are summed.** `panel_usage` and `connection_stability` resend
   lifetime totals every 4h (`SNAPSHOT_INTERVAL_MS`). After an unclean exit the totals carry
   into the next boot (`load_snapshot_state`), and printers almost always exit uncleanly.
   The worker `sum()`s every snapshot.
   - Inflated by this: panel time and visits, overlay visits, widget interactions, klippy
     shutdowns (74,715 in 30d), disconnects/day.
   - "Avg session" (98h, climbing) is `avg()` over fan-out rows, so one long boot counts
     once per panel per snapshot.
2. **Events without an envelope.** `print_outcome`, `print_start_context`,
   `error_encountered`, `connection_stability` and `hardware_profile` carry no
   `app_version` / `app_platform`. Only `session` and `crash` carry a board. As a result:
   - Releases shows 0% print success for every version.
   - Display anomalies show no version.
   - Version and platform filters empty whole tabs.
3. **Filter defaults hit the wrong column.**
   - `buildFilterClause` (`server/telemetry-worker/src/queries.ts#buildFilterClause`)
     accepts a per-query override, but any field without one falls back to platform=blob3,
     model=blob4. Example: a platform filter on `print_outcome` reads `filament_type`.
   - Queries that span several point kinds (overview totals, crash rate over
     crash+session) apply one clause to all of them.
   - The sanitizer strips spaces, so `model=Voron 2.4` matches nothing.
4. **Client/worker field-name drift.** `total_connected_sec` vs `connected_sec`,
   `auto_update_channel` vs `update_channel`, `screen_blank_timeout_sec` vs
   `blank_timeout_sec`, `estimated_duration_bucket` vs `duration_bucket`. Nothing compares
   the two sides.
5. **Type coercion.** `executeQuery` turns blob `"1"` into `1`, and `toYesNo` compares
   against the string. Helix macros and Moonraker topology therefore always read 0.
6. **Unsupported SQL.** `uniqExact` fails in Analytics Engine; the Crashes endpoint returns 502.
7. **Feature adoption is wrong at the source.** `build_feature_adoption_event` checks
   `has_panel()` for things that are overlays, uses widget ids that do not exist, and fires
   once from a 5-minute one-shot timer.
8. **Silent drops and wrong denominators.**
   - The error allow-list in `record_error` admits five categories and drops `ui`,
     `updates`, `migration`, `gcode_viewer`, `post_init` and `main_loop` (main-loop hangs
     included).
   - The drop rate divides dropped frames from a 1024-sample ring by every frame in the hour.
   - The longest outage is split at every 4h snapshot.
   - `task_handler_max_ms` is always 0.
   - Settings changes are tracked for about 10 motion settings only.
   - `async_lifetime_skips` has no mapping and falls into the mapper's default branch.
9. **Overlay names are display strings.** `NavigationManager` reports `lc->get_name()`, or
   "anon"/"unreg" when there is no lifecycle. Renames split one overlay into several rows,
   and lifecycle-less overlays are indistinguishable.
10. **Analytics Engine reports about half of what R2 implies.**

### The AE undercount (probed 2026-10-14)

One day (2026-10-12) of R2 batches (657 batches, 8,498 events) was replayed through
`mapEventToDataPoints`:

| | Expected from R2 | AE reports | Kept |
|---|---|---|---|
| All points | 15,137 | 7,937 | 52% |
| session | 256 | 243 | 95% |
| print_outcome | 187 | 158 | 84% |
| memory_warning | 98 | 52 | 53% |

**Ruled out:**
- A per-invocation write cap. No single cap fits all rows, and batches peak at 76 points
  against a limit of 250.
- Point shape. Every point has at most 12 blobs, 8 doubles and 497 B of blob data, the
  longest index is 20 B, and no double is non-finite.
- Batch position. memory_warning and print_outcome sit in identical batch contexts yet keep
  53% vs 84%.

**Remaining suspects:**
- Read-side sampling: queries use `count()` instead of `sum(_sample_interval)`.
- Measurement mismatch: the "AE reports" column came from dashboard queries with their own
  `WHERE` clauses.
- Skew between upload day and write day.

## Design

### Phase 0 - Measure the undercount

- **`GET /v1/admin/ae-counts?date=YYYY-MM-DD`** (admin key) returns per `index1`:
  `count()`, `sum(_sample_interval)` and `count(DISTINCT blob1)` for that UTC write day.
- **`server/telemetry-worker/scripts/reconcile.ts`** (run locally with `vite-node`):
  1. Pulls the same day's R2 batches through `/v1/events/list` and `/v1/events/get`.
  2. Maps them with `mapEventToDataPoints`.
  3. Prints expected vs AE per point kind.

  The R2 side stays offline because a day is 657+ objects, which is near the per-invocation
  subrequest cap and grows with the fleet.
- **The rule it decides:**
  - If `sum(_sample_interval)` matches R2, every count and sum in the queries becomes
    `_sample_interval`-weighted.
  - If AE rows are missing even when weighted, the write path is investigated before
    Phase 2b, and that investigation reports back before any query rule is chosen.

### Phase 1 - Client (C++)

**One envelope.**
- `stamp_envelope(json&)` in `src/system/telemetry_manager.cpp` sets `schema_version`,
  `app_version`, `app_platform`, `app_board` and `boot_id` on every event.
- Builders call it and never set those fields by hand.
- Update events keep their own target and from-version fields (`version`,
  `from_version`). The worker maps those explicitly, so the stamped `app_version` never
  overwrites the update's target.

**`boot_id`.** 16 random hex characters, generated once per process start and never
persisted. `snapshot_seq` restarts at 0 for each boot.

**`schema_version` 3.** It marks delta-era events. The worker stores it as a column (see
Phase 2b) and gates delta queries on it.

**Deltas.**
- `panel_usage` and `connection_stability` report only what accumulated since the previous
  snapshot of the same boot. Each event carries `boot_id` and `snapshot_seq`.
- `session_duration_sec` on a delta event is the length of the window it covers. A boot's
  length is the sum over its rows.
- An outage in progress at snapshot time keeps its start time across snapshots, so
  `longest_disconnect_sec` measures the real outage.

**Delivery guarantees for delta events.** A lost delta is gone for good, and a resent one
double counts. So:
- **Idempotency key.** `(device_id, boot_id, snapshot_seq)` makes every delta row unique.
  A resend after a lost response writes a duplicate that the worker's dedup removes (Phase 2b).
- **Saves.**
  - `fire_periodic_snapshot` saves the queue (`save_queue()`) together with the snapshot
    state.
  - The unsent accumulator is persisted hourly from the existing auto-send timer.
  - After an unclean exit, the recovered accumulator is sent on the next boot under its
    original `boot_id` and the next `snapshot_seq`.
- **Queue drops.** Delta events are exempt from the drop-oldest rule at `MAX_QUEUE_SIZE`.
  When the queue is full, a new delta of the same type merges into the queued one: counts
  add and the window extends.

**Feature adoption moves to the worker.**
- The `feature_adoption` event is deleted.
- Adoption is derived from `overlay_visit` and `widget_interaction` deltas: a feature counts
  as adopted on a device when its count is above 0 in the window.
- Overlays are identified by a stable id: the overlay root's object name
  (`lv_obj_get_name`), falling back to the lifecycle name only when the root has none.
- Feature flags with no overlay or widget source (spoolman configured, theme changed,
  multi-printer) become fields on `settings_snapshot`.

**Smaller fixes:**
- `record_error` admits every category the app emits. Per-category rate-limiting stays.
- The drop-rate numerator and denominator use the same window.
- `task_handler_max_ms` is removed from the event; nothing measures it.
- Every user-facing persisted setting gets a `telemetry_key`. The settings managers in
  `src/system/*_settings_manager.cpp` declare them, and `persisted_setting.cpp` reports
  changes.

### Phase 2a - Worker quick fixes (safe now, no client release)

- Field names follow the client: `total_connected_sec`, `total_disconnected_sec`,
  `auto_update_channel`, `screen_blank_timeout_sec`, `estimated_duration_bucket`.
- `uniqExact` becomes `count(DISTINCT ...)`.
- `toYesNo` compares `String(value)`. `executeQuery`'s coercion stays, since other
  endpoints rely on it.
- The filter sanitizer keeps spaces and rejects only quote and backslash characters.
- Version sort handles prerelease suffixes. `1.1.0-beta.10` sorts after `1.1.0-beta.9`,
  which sorts before `1.1.0`. This applies in the worker and in the dashboard's
  `compareVersions`.
- `/health` reports the git SHA the worker was deployed from (`--var GIT_SHA:<sha>` in the
  documented deploy command).

### Phase 2b - Worker schema table and delta-era queries

**Schema table keyed by point kind.**
- One declaration per `index1` value (`session`, `panel_usage`, `overlay_visit`,
  `widget_interaction`, `widget_placement`, `settings_change`, ...) names its blob and
  double fields.
- Each event type declares which point kinds it fans out to.
- `mapEventToDataPoints` is driven by the table.
- `queries.ts` asks for columns through `col(kind, field)`.

**Positions are append-only.** Existing fields keep their current `blobN` / `doubleN`.
Moving one makes 90 days of stored rows unreadable. The existing positional assertions in
`__tests__/index.test.ts` guard this. New fields take free slots (13-20):
- `schema_version`, `boot_id` and `snapshot_seq` on every delta point kind
- `app_version`, `app_platform` and `app_board` on the five events that lacked them
- `async_lifetime_skips` gets a kind, or an explicit `R2_ONLY` entry

**Filter resolver.**
- `platform` / `version` / `model` / `board` resolve per point kind from the table.
- A query over several kinds gets `((index1='a' AND blobX=v) OR (index1='b' AND blobY=v))`.
- A kind that lacks the field is excluded under that filter rather than matched against
  the wrong column.

**Delta-era queries.**
- Queries over `panel_usage`, `overlay_visit`, `widget_interaction` and
  `connection_stability` read only `schema_version >= 3`.
- They dedup on the idempotency key first (inner `GROUP BY device, boot_id, snapshot_seq,
  <dims>` with `any()`), then aggregate.
- Each affected value is returned as `null` with a `delta_era_devices` count until enough
  devices report.

**Dashboard guards ship in the same change.** The Engagement, Reliability, Stability, UX
Insights and Features views render the `null` state ("waiting for devices on the new
client: N so far") instead of a number. `api.ts` types those fields as `number | null`.
The Features view switches to the worker-derived adoption.

**Counting** follows the Phase 0 rule.

### Phase 3 - Contract test

- A C++ test (`[telemetry][contract]`) runs every event builder against a fixed mock state.
  It compares the output with committed goldens in `tests/fixtures/telemetry/<event>.json`,
  with volatile fields (timestamp, device_id, boot_id) normalized.
  `HELIX_UPDATE_GOLDENS=1` rewrites them.
- A worker vitest loads the same files. It fails when any leaf field is neither mapped in
  the schema table nor listed in `R2_ONLY`, and it checks that update events map their
  target version, not `app_version`.
- Renaming a field on either side turns CI red. The worker suite already runs in CI
  (`build.yml`).

## Testing

- **C++:**
  - every builder carries the envelope
  - three delta snapshots plus a simulated unclean exit sum to the true totals, with no
    window counted twice
  - a full queue merges deltas instead of dropping them
  - an outage spanning two snapshots reports one longest outage
  - an overlay with a named root reports that name
  - `record_error` admits `ui` and `updates`
  - the update events keep their target version
- **Tests that change because they pin the old behavior:**
  - snapshot state persistence and recovery (`tests/unit/test_telemetry_manager.cpp`)
  - the feature-adoption tests
  - the error allow-list tests
- **Worker:**
  - `col()` and the multi-kind filter resolver
  - a filter on a kind lacking the field excludes it
  - a duplicate `(device, boot, seq)` row counts once
  - the schema-3 gate and the null shape
  - the prerelease version sort
  - `toYesNo` on numeric-coerced blobs
  - the `ae-counts` endpoint
- `make mutate-diff` once at the end of the branch.

## Rollout

1. Phase 2a and Phase 0's endpoint deploy first, with no client release. Phase 0's numbers
   set the counting rule.
2. Phase 2b deploys before any schema-3 client exists. Its gates return the `null` state
   until devices update.
3. Phase 1 and Phase 3 ride the next main release. The `release/1.0` port follows as a
   separate branch: `app_board` is `""` there, since 1.0 has no board resolver, and it gets
   its own goldens.

## Out of scope

- Dashboard layout, chart readability, drill-down and model-name folding (spec 2).
- Digest, emails, summary page, GitHub automation, release gate and printer-DB PRs (spec 3).
- Recovering correct values from pre-3 cumulative rows.
