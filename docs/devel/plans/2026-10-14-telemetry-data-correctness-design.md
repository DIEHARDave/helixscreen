# Telemetry Data Correctness - Design

Spec 1 of 3. The telemetry dashboard (`analytics.helixscreen.org`) shows numbers that are
wrong at the data layer, not just badly drawn. This spec makes the numbers right. Spec 2
(dashboard information architecture and drill-down) and spec 3 (the feedback loop: emailed
summaries, a summary page, GitHub issues for actionable clusters, release-gate warnings,
printer-DB PRs) are built on top of it and are out of scope here.

## Decisions already made

| Decision | Ruling |
|----------|--------|
| Sequencing | Data correctness first; dashboard and loop follow |
| Cumulative snapshots | Client sends deltas plus a per-boot `boot_id`; old rows are excluded, not salvaged |
| Release line | Client changes land on `main` and port to `release/1.0` (most of the fleet runs 1.0.x) |
| Deploys | Worker deploys are manual `wrangler deploy`; each one is confirmed with Preston first |

## What is wrong today

Grouped by cause, because each cause is one fix.

1. **Cumulative counters are summed.** `panel_usage` and `connection_stability` resend
   lifetime totals every 4h (`SNAPSHOT_INTERVAL_MS`), and the totals survive reboots
   (`save_snapshot_state` / `load_snapshot_state`). The worker `sum()`s every snapshot.
   Inflated: panel time and visits, overlay visits, widget interactions, "avg session"
   (98h and climbing), klippy shutdowns (74,715 in 30d), disconnects/day.
2. **Events without an envelope.** `print_outcome`, `print_start_context`,
   `error_encountered`, `connection_stability` and `hardware_profile` carry no
   `app_version` / `app_platform` / `app_board`. Releases shows 0% print success for every
   version; display anomalies show no version; version and platform filters empty whole tabs.
3. **Raw `blobN` indexes in queries.** `buildFilterClause` (`server/telemetry-worker/src/queries.ts#buildFilterClause`)
   assumes platform=blob3, model=blob4 for every event; most events store something else
   there. Each query also hand-picks blob numbers, so a mapping change silently breaks
   queries far away. The sanitizer strips spaces, so `model=Voron 2.4` matches nothing.
4. **Client/worker field-name drift.** `total_connected_sec` vs `connected_sec`,
   `auto_update_channel` vs `update_channel`, `screen_blank_timeout_sec` vs
   `blank_timeout_sec`, `estimated_duration_bucket` vs `duration_bucket`. Nothing compares
   the two sides.
5. **Type coercion.** `executeQuery` turns blob strings `"1"`/`"0"` into numbers and
   `toYesNo` compares against the string, so Helix macros and Moonraker topology always
   read 0.
6. **Unsupported SQL.** `uniqExact` fails in Analytics Engine; the Crashes endpoint returns 502.
7. **Feature adoption is wrong at the source.** `build_feature_adoption_event` checks
   `has_panel()` for things that are overlays (macros, console, bed mesh, input shaper,
   camera), uses widget ids that do not exist, and samples once, 5 minutes after boot.
8. **Silent drops and wrong denominators.**
   - The error allow-list drops `ui`, `updates`, `migration`, `gcode_viewer`, `post_init`
     and `main_loop`, including main-loop hangs.
   - The drop rate divides dropped frames from a 1024-sample ring by every frame in the hour.
   - The longest outage is split at every 4h snapshot.
   - `task_handler_max_ms` is hard-coded 0.
   - Settings changes are tracked for about 10 motion settings only.
9. **Analytics Engine holds about half of what R2 holds.**

### The AE undercount (probed 2026-10-14)

One day (2026-10-12) of R2 batches (657 batches, 8,498 events) was replayed through
`mapEventToDataPoints`:

| | Expected from R2 | AE reports | Kept |
|---|---|---|---|
| All points | 15,137 | 7,937 | 52% |
| session | 256 | 243 | 95% |
| print_outcome | 187 | 158 | 84% |
| memory_warning | 98 | 52 | 53% |

A fixed per-invocation write ceiling does not fit. The total needs a cap of about 15 points
per batch, but sessions need a cap of at least 30. The loss is concentrated by event type.
The total is close to "everything except the `panel_usage` and `settings_snapshot` fan-out
points" (15,137 - 5,356 - 2,056 = 7,725). The remaining suspects:

- point shape on the fan-out events (blob size, NaN doubles, index length)
- `_sample_interval` sampling
- day-boundary skew between R2 upload time and AE timestamp

Settling it needs `count()` grouped by `index1` from AE, which only the worker's token can
run. Hence the reconciliation endpoint below, built first.

## Design

### Phase 0 - Reconciliation endpoint (worker; first, decides the query rules)

`GET /v1/admin/reconcile?date=YYYY-MM-DD` returns, per event type:

- events in R2
- points `mapEventToDataPoints` would write
- AE `count()`
- AE `sum(_sample_interval)`
- distinct devices on both sides

It reads the day's R2 batches itself, so the comparison needs nothing local.

**Rule it decides:**
- If AE rows are missing outright, fix the write path (point shape, or fewer points per
  batch) and backfill from R2.
- If the rows exist but are sampled, every count becomes `sum(_sample_interval)`.

Either way, the endpoint stays. Spec 3's digest reports its numbers as a data-health line,
so a future silent loss shows up within a week instead of never.

### Phase 1 - Client (C++)

**One envelope.** A single `stamp_envelope(json&)` in `src/system/telemetry_manager.cpp`
sets `schema_version`, `app_version`, `app_platform`, `app_board` and `boot_id` on every
event. Every builder calls it and none sets those fields by hand. Today 8 builders copy the
pair and 5 forgot.

**`boot_id`.** It is 16 random hex characters generated once per process start and never
persisted. `snapshot_seq` restarts at 0 for each boot.

**Deltas.** `panel_usage` and `connection_stability` report only what accumulated since the
previous snapshot of the same boot.
- Persisted state holds only the unsent delta. After a crash, that delta is sent on the next
  boot under its original `boot_id`, so it is neither lost nor double counted.
- An outage in progress at snapshot time keeps its start time across snapshots, so
  `longest_disconnect_sec` measures the real outage.

**`schema_version` 3.** It marks delta-era events. The worker uses it, not a version-string
comparison, to decide which rows a cumulative-sensitive query may read.

**Feature adoption, rebuilt.**
- "Used" means the user opened the overlay or panel, or interacted with the widget, at any
  time in the snapshot window.
- It is sent with the periodic and shutdown snapshot, as a delta of features first used in
  this window, instead of once at 5 minutes.
- Feature ids come from the overlay and widget registries, not a hand-written list, so a
  renamed overlay cannot leave a dead id behind.

**Smaller fixes:**
- The error allow-list admits every category the app emits. Rate-limiting stays.
- The drop-rate numerator and denominator use the same window.
- `task_handler_max_ms` is removed from the event; nothing measures it.
- Every user-facing setting that `settings_changes` should see gets a `telemetry_key`.

### Phase 2 - Worker

**Schema table instead of raw indexes.**
- One declaration per event type in `server/telemetry-worker/src/analytics.ts` lists its
  blob fields and double fields by name.
- `mapEventToDataPoints` is driven by that table.
- `queries.ts` asks for columns by name, through a `col(event, field)` helper that resolves
  to `blobN` / `doubleN`.
- `buildFilterClause` resolves `platform` / `version` / `model` / `board` per event type from
  the same table. An event that lacks a field is excluded from that filter instead of
  matching the wrong column.
- This is the structural fix for causes 2-4: index numbers live in one place.

**Specific fixes:**
- Field names follow the client.
- Remove `uniqExact`.
- Blob values stay strings; `toYesNo` compares on that basis.
- The filter sanitizer keeps spaces and rejects only quote and backslash characters.
- Delta-sensitive queries filter `schema_version >= 3`. Panels backed only by pre-3 data
  return an explicit "no delta-era data yet" state, not a number.
- Version sort handles prerelease suffixes. `1.1.0-beta.10` sorts after `1.1.0-beta.9`.
- `canonicalModel()` folds model-name variants such as "Creality Ender-3 V3 KE" and
  `creality_ender_3_v3_ke` before display. It is applied after the query, since AE has no
  lookup tables.
- Counting follows the Phase 0 rule.

**Deploy provenance.** `/health` reports the git SHA the worker was built from, so "is the
deployed worker this code" stops being a guess.

### Phase 3 - Contract test

- A C++ test (`[telemetry][contract]`) runs every event builder against a fixed mock state.
  It compares the output to committed goldens in `tests/fixtures/telemetry/<event>.json`,
  with volatile fields (timestamp, device_id, boot_id) normalized.
  `HELIX_UPDATE_GOLDENS=1` rewrites them.
- A worker vitest loads the same files and fails when any leaf field is neither mapped in
  the schema table nor listed in an explicit `R2_ONLY` set.
- Renaming a field on either side turns CI red. The worker suite already runs in CI
  (`build.yml`).

## Testing

- **C++:**
  - every builder carries the envelope
  - delta snapshots sum to the true totals across three snapshots and a simulated crash/reboot
  - an outage spanning two snapshots reports one longest outage
  - feature adoption fires for an overlay opened after minute 5
  - the allow-list admits `ui` and `updates`
- **Worker:**
  - `col()` and the filter resolver per event type
  - a filter on an event lacking the field excludes it
  - the prerelease version sort
  - `toYesNo` on string blobs
  - `canonicalModel` folds known variants
  - a schema-3 gate on delta queries
  - the reconcile endpoint against a recorded R2 day
- `make mutate-diff` once at the end of the branch.

## Rollout

1. Phase 0 ships alone; its numbers decide the Phase 2 counting rule.
2. Phase 2 ships to the worker without a client release, fixing every panel whose data is
   already correct.
3. Phase 1 rides the next main release and the next 1.0.x. Delta-era panels fill in as
   devices update.
4. Phase 3 lands with Phase 1, since the goldens need the new envelope.

Backfill from R2 is run only if Phase 0 shows rows missing from AE.

## Out of scope

- Dashboard layout, chart readability, drill-down, the Features and Reliability tabs (spec 2).
- Digest, emails, summary page, GitHub automation, release gate, printer-DB PRs (spec 3).
- Recovering correct values from pre-3 cumulative rows.
