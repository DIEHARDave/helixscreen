# Telemetry Dashboard IA Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rebuild the telemetry dashboard as seven decision tabs with click-through drill-down to a device page, backed by four new worker endpoints and three extended ones.

**Architecture:** The worker (`server/telemetry-worker`, a Cloudflare Worker over Analytics Engine SQL) gains query builders in `queries.ts` and handlers in `index.ts`; every column comes from the schema table via `col()`/`colsOf()`, every filter from `filterFor`/`filterForKinds`, every count from the weighted aggregates. The dashboard (`server/analytics-dashboard`, Vue 3 + Pinia + vue-router + chart.js) moves each old view's body into a section component, stacks sections into seven tab views, and routes all readability rules through pure functions in `src/utils/` that the worker's vitest suite tests.

**Tech Stack:** TypeScript, Cloudflare Workers, Analytics Engine SQL, vitest, Vue 3, Pinia, vue-router 4, chart.js 4 via vue-chartjs.

**Spec:** `docs/devel/plans/2026-10-15-telemetry-dashboard-ia-design.md`

## Global Constraints

- Work in `.worktrees/telemetry-dashboard-ia` on `feature/telemetry-dashboard-ia`. `node_modules` in both server packages are symlinks to the main tree; never run `npm ci` or `npm install` (the lockfile is out of sync and it would rewrite the shared copy).
- Worker gate: `cd server/telemetry-worker && npx vitest run` (all green) and `npx tsc --noEmit -p .`.
- Dashboard gate: `cd server/analytics-dashboard && npx vue-tsc --noEmit -p tsconfig.app.json && npx vite build`.
- Dashboard pure utils are tested from `server/telemetry-worker/src/__tests__/dashboard_*.test.ts`, importing `../../../analytics-dashboard/src/utils/<file>`. Those util files import nothing from `@/` except types (`import type`), or vitest cannot resolve them.
- Every SQL column name comes from `col(kind, field)` / `colsOf(kind)`; never write `blobN`/`doubleN` by hand except in the device-timeline query that selects every slot (Task 3).
- Every count, sum and average over raw rows uses `wCount`/`wSum`/`wAvg`/`wCountIf` (sample-weighted). `count(DISTINCT device)` stays unweighted.
- Analytics Engine SQL rules learned live: a constant predicate is `SQL_FALSE`/`SQL_TRUE`, never a bare `0`/`1`; `GROUP BY` names the SELECT alias, never the underlying column when the alias differs; an output alias never shadows a column name (`rows` is reserved: use `row_count`). Use only functions already used in `queries.ts` (`toDate`, `argMax`, `quantileExactWeighted`, `count(DISTINCT ...)`, `bitAnd`, `toUInt32`, `NOW() - INTERVAL 'N' DAY`).
- AE keeps 90 days. No range or window may exceed `parseRange`'s 90-day cap.
- Device ids are 64 lowercase hex characters; anything else is a 400 before any query runs.
- `health.ts`, `devices.ts` and `retention.ts` import only from `./versions` and `./schema`, so the dashboard can `import type` their payload types without pulling in Worker runtime types.
- No top-N cut and no "Other" bucket in any distribution the dashboard renders. Existing `LIMIT 20` clauses on model and MCU distributions are removed.
- Every chart that renders a daily series passes it through `trimPartialDays`; every ratio renders through `formatRate`; every horizontal bar chart sets its height with `barChartHeight`.
- Comments describe the code as it is; no history, no em-dashes. SPDX header `// SPDX-License-Identifier: GPL-3.0-or-later` on new `.ts` files; `.vue` files follow their neighbors (none carry one).
- Commit with explicit pathspecs (`git add -N` for new files, then `git commit -- <paths>`); never `git add -A`, never `--no-verify`.

## Review Focus

1. **A grouped value as a filter.** Clicking "0.99.x (34 versions)" or a canonical model must filter on every raw member; a reviewer should see a test where a group's `members` reach the URL and `FilterParams` unchanged. (Task 1 `groupVersions`/`canonicalModels` members tests; Task 2 `applyDrill` test.)
2. **A raw model the sanitizer alters** (`CdO - Cubo dell'Odio`): it must render as not filterable, not as a click that returns an empty page. (Task 1 `isFilterable` test.)
3. **An empty window.** A new filter combination with no rows in `cur` or `prev` must give zeros and `null` rates, never NaN or a thrown handler (Task 4 empty-rows test; Task 5 zero-session release test).
4. **A device id from user input** reaching SQL: only `^[0-9a-f]{64}$` passes (Task 3 validation tests, including uppercase, 63 chars and a quote).
5. **Old bookmarks.** `/adoption?platform=pi` must land on `/fleet?platform=pi` with the filter intact (Task 7 redirect test via `legacyRedirect`).

---

## File map

Worker (`server/telemetry-worker/src/`):

| File | Change |
|------|--------|
| `schema.ts` | add `decodeRow(kind, row)` |
| `queries.ts` | add `devicesQueries`, `deviceQueries`, `healthQueries`, `retentionQuery`, `releaseScorecardQueries`; extend `performanceQueries`, `printsQueries`; drop the `LIMIT 20`s |
| `health.ts` (new) | pure `buildHealth(rows)` assembling the health payload |
| `retention.ts` (new) | pure `buildRetention(rows, today)` |
| `devices.ts` (new) | pure `isDeviceId`, `buildDeviceList`, `buildDevicePage` |
| `index.ts` | routes for `health`, `devices`, `device/:id`, `fleet/retention`; releases, performance, prints payload changes |
| `__tests__/health.test.ts`, `retention.test.ts`, `devices.test.ts`, `dashboard_utils.test.ts`, `dashboard_url.test.ts` (new) | tests |
| `scripts/smoke.ts` (new) | live smoke over every endpoint × filter |

Dashboard (`server/analytics-dashboard/src/`):

| File | Change |
|------|--------|
| `utils/versions.ts` (new) | `groupVersions` |
| `utils/models.ts` (new) | `canonicalModels`, `isFilterable` |
| `utils/materials.ts` (new) | `materialSet`, `groupMaterialSets` |
| `utils/series.ts` (new) | `trimPartialDays`, `seriesColor`, `barChartHeight` |
| `utils/urlState.ts` (new) | `DashQuery`, `parseDashQuery`, `buildDashQuery`, `applyDrill`, `legacyRedirect` |
| `utils/format.ts` | `formatRate` stays the one ratio formatter |
| `stores/filters.ts` | board filter, expanded groups, built on `urlState.ts` |
| `services/api.ts` | types + calls for new and changed endpoints |
| `components/Breadcrumb.vue`, `ChartCaption.vue`, `DistributionBar.vue` (new) | shared UI |
| `components/BarChart.vue`, `PieChart.vue`, `LineChart.vue` | emit `select` with the clicked label index |
| `components/sections/*.vue` (new) | old view bodies, one per section |
| `views/HealthView.vue`, `FleetView.vue`, `UsageView.vue`, `DevicesView.vue`, `DeviceView.vue` (new) | new tabs and pages |
| `views/ReleasesView.vue`, `StabilityView.vue`, `PerformanceView.vue`, `PrintsView.vue` | rebuilt as section stacks |
| `views/OverviewView.vue`, `AdoptionView.vue`, `HardwareView.vue`, `EngagementView.vue`, `FeaturesView.vue`, `UxInsightsView.vue`, `MemoryView.vue`, `CrashesView.vue`, `ReliabilityView.vue` | deleted once their content lives in sections |
| `router/index.ts`, `components/AppLayout.vue` | seven tabs, device routes, legacy redirects |

Docs: `docs/devel/TELEMETRY_ADMIN.md` dashboard section rewritten for the seven tabs, the device page and `scripts/smoke.ts`.

---

### Task 1: Readability utils

**Files:**
- Create: `server/analytics-dashboard/src/utils/versions.ts`, `models.ts`, `materials.ts`, `series.ts`
- Test: `server/telemetry-worker/src/__tests__/dashboard_utils.test.ts`

**Interfaces (produces):**

```ts
// Shared row shape for every grouped distribution.
export interface Grouped {
  key: string            // stable id: the raw value, or 'line:0.99', 'betas:1.1.0', 'model:<canonical key>', 'set:PETG + PLA'
  label: string          // what the chart shows
  count: number          // summed over members
  members: string[]      // raw values to send as a filter, in input order
  expandable: boolean    // a collapsed version line or beta group
}

// versions.ts
export function groupVersions(rows: { name: string; count: number }[], expanded: ReadonlySet<string>, keepLines = 2): Grouped[]
// models.ts
export function canonicalModelKey(raw: string): string
export function canonicalModels(rows: { name: string; count: number }[]): Grouped[]
export function isFilterable(raw: string): boolean
// materials.ts
export function materialSet(filamentType: string): { set: string; materials: string[]; multi: boolean }
export function groupMaterialSets(rows: { type: string; count: number; success_rate: number }[]): (Grouped & { success_rate: number; multi: boolean })[]
// series.ts
export function trimPartialDays<T extends { date: string }>(series: T[], today: string): T[]
export function seriesColor(index: number): string
export function barChartHeight(rows: number): number
```

Rules (verbatim from the spec):
- `groupVersions`: lines are `major.minor`, ordered by `compareVersions` (import from `./chart`). The `keepLines` newest lines that contain a release show each release individually. Prereleases whose release is absent from `rows` collapse to one row `"<x.y.z> betas (<n>)"`, key `betas:<x.y.z>`; once the release is present they are members of the release's line like any version. Older lines collapse to `"<x.y>.x (<n> versions)"`, key `line:<x.y>`. A key in `expanded` renders its members individually (each `expandable: false`). Output is ordered newest first. `n` counts members, not devices.
- `canonicalModelKey`: lowercase, strip one trailing ` (…)`, fold runs of `[\s_-]+` to one space, trim, then apply `MODEL_ALIASES: Record<string, string>` = `{ 'k1': 'creality k1', 'zerog mercury hydra 255': 'zero g mercury hydra 255', 'creality ender 3': 'creality ender 3' }`. `canonicalModels` label = the member with the largest count (ties: first seen); ordered by count descending. `Unknown`, `Custom/Other` and `''` keep their own rows untouched.
- `isFilterable(raw)`: `raw === raw.replace(/[^a-zA-Z0-9 .\-_()/+]/g, '')` (the worker's `sanitizeFilterValue` character set; keep a comment saying the two change together) and `!raw.includes(',')`.
- `materialSet`: split on `;`, trim, uppercase, drop empty, distinct, sort, join `' + '`; `multi = materials.length >= 2`. `''` gives `{ set: '', materials: [], multi: false }`.
- `groupMaterialSets`: sums counts per set; `success_rate` is the count-weighted mean of members; ordered by count descending.
- `trimPartialDays`: drops entries whose `date` equals `today` and the earliest date in the series. A series of one or two days returns `[]`.
- `seriesColor`: a 12-entry palette constant `SERIES_PALETTE` (`#3b82f6 #10b981 #f59e0b #ef4444 #8b5cf6 #ec4899 #06b6d4 #84cc16 #f97316 #14b8a6 #a855f7 #eab308`), index modulo 12.
- `barChartHeight(rows) = Math.max(300, rows * 22 + 60)`.

- [ ] **Step 1: Write the failing tests** in `dashboard_utils.test.ts`, one `describe` per function:
  - versions from live data `['1.1.0-beta.9','1.1.0-beta.8','1.0.5','1.0.4','1.0.0','0.99.118','0.99.30']` with no `1.1.0`: rows are `1.1.0 betas (2)`, `1.0.5`, `1.0.4`, `1.0.0`, `0.99.x (2 versions)`; the betas row's `members` are both beta strings; the `0.99` row `expandable: true` and count = sum.
  - same input plus `1.1.0`: no betas row; `1.1.0`, the two betas, `1.0.5`, `1.0.4`, `1.0.0` are individual (the 1.1 and 1.0 lines are the two kept), `0.99.x` collapsed.
  - `expanded = new Set(['line:0.99'])`: `0.99.118` and `0.99.30` appear individually, newest first.
  - empty input gives `[]`.
  - models: `'creality_ender_3_v3_ke'`, `'Creality Ender 3 V3 KE'`, `'Creality Ender-3 V3 KE'` share one key; `'FlashForge Adventurer 5M Pro (ForgeX)'` groups with `'FlashForge Adventurer 5M Pro'`; `'K1'` groups with `'Creality K1'`; label is the highest-count spelling; `members` has all raw spellings.
  - `isFilterable("CdO - Cubo dell'Odio") === false`, `isFilterable('Voron 2.4') === true`, `isFilterable('a,b') === false`.
  - `materialSet('PLA;PLA;PETG;PLA')` is `{ set: 'PETG + PLA', materials: ['PETG','PLA'], multi: true }`; `'pla '` gives `PLA`, `multi: false`; `''` as above; `'PLA;;PLA'` gives `PLA`.
  - `groupMaterialSets` merges `'PLA;PLA'` (count 72, rate 0.9) and `'PLA'` (count 28, rate 0.5) into one `PLA` row with count 100 and rate 0.788.
  - `trimPartialDays` drops first and today; leaves a series not containing today minus its first day; 2-day series gives `[]`.
  - `seriesColor(0) !== seriesColor(1)`, `seriesColor(12) === seriesColor(0)`; `barChartHeight(5) === 300`, `barChartHeight(56) === 1292`.
- [ ] **Step 2: Run to see them fail:** `cd server/telemetry-worker && npx vitest run src/__tests__/dashboard_utils.test.ts` (FAIL: modules not found).
- [ ] **Step 3: Implement the four util files** to the interfaces above.
- [ ] **Step 4: Run the test file, then the worker gate and dashboard gate.** All PASS.
- [ ] **Step 5: Commit** `feat(telemetry-dashboard): grouping, normalization and series utils`.

---

### Task 2: URL state, filter store, breadcrumb

**Files:**
- Create: `server/analytics-dashboard/src/utils/urlState.ts`, `components/Breadcrumb.vue`
- Modify: `stores/filters.ts`, `components/FilterBar.vue`
- Test: `server/telemetry-worker/src/__tests__/dashboard_url.test.ts`

**Interfaces:**

```ts
export type DashFilterKey = 'platform' | 'version' | 'model' | 'board'
export interface DashQuery {
  range: string                                  // '7d' | '30d' | '90d', default '30d'
  filters: Record<DashFilterKey, string[]>       // raw values
  expand: string[]                               // Grouped.key values expanded in place
}
export function parseDashQuery(q: Record<string, string | string[] | null | undefined>): DashQuery
export function buildDashQuery(s: DashQuery): Record<string, string>   // omits empty keys; range always present
export function applyDrill(s: DashQuery, key: DashFilterKey, members: string[]): DashQuery  // replaces that key's values with members, deduped
export function legacyRedirect(path: string): string | null           // '/adoption' -> '/fleet', etc; null when not legacy
```

- `parseDashQuery`: comma-split, trim, drop empty; unknown keys ignored; a `range` not matching `^\d+d$` falls back to `'30d'`. `expand` comes from `expand=` (comma list).
- `legacyRedirect` map (spec): `/adoption`,`/hardware` to `/fleet`; `/memory`,`/crashes`,`/reliability` to `/stability`; `/engagement`,`/features`,`/ux` to `/usage`.
- The store keeps `range`, the four filter arrays and `expand` as refs, and `syncFromRoute`/`syncToRoute` go through `parseDashQuery`/`buildDashQuery`. It exposes `drill(key, members)` (calls `applyDrill`, then `syncToRoute` with `router.push`, not `replace`, so back walks the drill), `toggleExpand(key)`, `remove(key, value)`. `queryString` includes `board`.
- `FilterBar.vue` gains a Board dropdown fed from a new `boards` list on the `adoption` response. Worker side of this task: `adoptionQueries` appends `distribution("app_board")` as query [4], the handler returns it as `boards`, and `AdoptionData` gains `boards: { name: string; count: number }[]`. Add a worker test that the adoption handler returns `boards`.
- `Breadcrumb.vue`: props none; reads the store and current route; renders the tab name, then one chip per active filter value (`model = Voron 2.4 ×`), then for `/devices` "Devices", for `/device/:id` "Devices › <first 8 chars>…". Chip × calls `remove`.

- [ ] **Step 1: Failing tests** in `dashboard_url.test.ts`: round trip `buildDashQuery(parseDashQuery(x))` for a query with every key; empty arrays omitted; bad range falls back; `applyDrill` replaces existing values and dedupes; `applyDrill` with group members keeps every member (Review Focus 1); `legacyRedirect('/adoption') === '/fleet'`, `legacyRedirect('/fleet') === null`, every legacy path in the map covered.
- [ ] **Step 2: Run, see FAIL.**
- [ ] **Step 3: Implement** `urlState.ts`, rewrite the store on it, add board to `FilterBar.vue`, add `Breadcrumb.vue` (not yet mounted).
- [ ] **Step 4: Worker gate + dashboard gate.**
- [ ] **Step 5: Commit** `feat(telemetry-dashboard): filters, expansion and drill path live in the URL`.

---

### Task 3: Device list and device page endpoints

**Files:**
- Modify: `server/telemetry-worker/src/schema.ts`, `queries.ts`, `index.ts`
- Create: `server/telemetry-worker/src/devices.ts`, `__tests__/devices.test.ts`

**Interfaces:**

```ts
// schema.ts
export type RawRow = Record<string, unknown>   // AE row with blob1..blob20, double1..double20, timestamp, index1
export function decodeRow(kind: string, row: RawRow): Record<string, string | number>
//   every ColumnDef of POINT_KINDS[kind] read from its slot, empty strings and zero doubles kept;
//   unknown kind throws like col() does.

// devices.ts
export function isDeviceId(id: string): boolean          // /^[0-9a-f]{64}$/
export interface DeviceListRow { device_id: string; last_seen: string; version: string; platform: string; board: string; model: string; sessions: number; crashes: number; prints: number }
export function buildDeviceList(sessions: unknown, crashes: unknown, prints: unknown): { total: number; devices: DeviceListRow[] }
export interface DevicePage {
  device_id: string
  model: string; platform: string; board: string; version: string
  first_seen: string; last_seen: string
  hardware: Record<string, string | number> | null       // latest hardware_profile, decoded
  settings: Record<string, string | number> | null       // latest settings_snapshot, decoded
  versions: { version: string; first_seen: string; last_seen: string }[]   // newest first by compareVersions
  timeline: { timestamp: string; kind: string; fields: Record<string, string | number> }[]  // newest first, fields with '' and 0 dropped, device_id dropped
}
export function buildDevicePage(id: string, rows: unknown, versionRows: unknown): DevicePage | null   // null when no rows

// queries.ts
export function devicesQueries(days: number, filters?: FilterParams): string[]
//   [0] per device from session rows: device_id, max(timestamp) last_seen, argMax version/platform/board/model, weighted session count; filters via filterFor('session'); ORDER BY last_seen DESC LIMIT 200
//   [1] count(DISTINCT device) over the same filter as total
//   [2] weighted crash count per device, filterFor('crash')
//   [3] weighted print count per device, filterFor('print_outcome')
export function deviceQueries(id: string): string[]
//   [0] SELECT timestamp, index1, blob1..blob20, double1..double20 FROM dataset WHERE timestamp >= NOW() - INTERVAL '90' DAY AND index1 IN (<every event kind>) AND blob1 = '<id>' ORDER BY timestamp DESC LIMIT 500
//   [1] per running version (the session app_version column): min(timestamp), max(timestamp) for this device over 90 days
//   throws when !isDeviceId(id)
```

Handlers: `GET /v1/dashboard/devices` returns `buildDeviceList(...)`; `GET /v1/dashboard/device/<id>` returns 400 `{error:'invalid device id'}` before any query when `!isDeviceId`, 404 when `buildDevicePage` is null, else the page. Crash and print counts join to the 200 listed devices in TypeScript; a device with none shows 0. `blob1` in `deviceQueries` is the one hand-written slot: add an assertion test that `col(k, 'device_id') === 'blob1'` for every event kind so the shortcut cannot drift.

- [ ] **Step 1: Failing tests:** `isDeviceId` accepts a real 64-hex id; rejects uppercase, 63 chars, 65 chars, `'' `, `"abc' OR 1=1"`; `deviceQueries` throws on a bad id; `decodeRow('crash', row)` maps `blob3` to `signal_name` and `double1` to `uptime_sec`; `col(k,'device_id')==='blob1'` for every kind in `EVENT_FANOUT`; `buildDeviceList` joins crashes/prints by id and zero-fills; `buildDevicePage` drops empty fields, sorts versions with `compareVersions`, picks the latest hardware and settings rows, returns null on no rows; handler tests via the mocked `executeQuery`: 400 on bad id with `executeQuery` never called, 404 on empty, 200 shape on canned rows; devices handler with `platform=pi` puts `filterFor('session', ...)` text into query 0.
- [ ] **Step 2: Run, see FAIL.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Worker gate.**
- [ ] **Step 5: Commit** `feat(telemetry-worker): device list and per-device timeline endpoints`.

---

### Task 4: Health endpoint

**Files:**
- Create: `server/telemetry-worker/src/health.ts`, `__tests__/health.test.ts`
- Modify: `queries.ts`, `index.ts`

**Interfaces:**

```ts
// queries.ts
export const HEALTH_WINDOWS = { cur: [0, 7], prev: [7, 14], baseline: [7, 35] } as const   // [newerDaysAgo, olderDaysAgo]
export function windowClause(w: readonly [number, number]): string
//   newer 0: "timestamp >= NOW() - INTERVAL '<older>' DAY"
//   else:    "timestamp >= NOW() - INTERVAL '<older>' DAY AND timestamp < NOW() - INTERVAL '<newer>' DAY"
export function healthQueries(filters?: FilterParams): Record<HealthQueryKey, string>
export type HealthQueryKey =
  | 'actives_cur' | 'actives_prev'            // count(DISTINCT device) from session rows
  | 'top_versions'                            // 8 versions by distinct session devices in cur
  | 'release_cur' | 'release_prev'            // per running version: sessions, crashes, update_failed, update_success (weighted), as releasesQueries does
  | 'release_devices_cur' | 'release_devices_prev'   // per version: distinct session devices, distinct crashing devices
  | 'prints_cur' | 'prints_prev'              // per version: weighted successes, total
  | 'errors_cur' | 'errors_prev' | 'errors_baseline' // per (category, code): weighted count, distinct devices
  | 'crashes_cur' | 'crashes_prev'            // per (signal_name, fault_code_name, app_version, app_board): weighted count, distinct devices
  | 'cdn_latest'                              // newest cdn_fleet_daily row
  | 'kind_days'                               // per (toDate(timestamp), index1): weighted count, last 9 days

// health.ts
export interface HealthPayload {
  fleet: { actives_cur: number; actives_prev: number; cdn_sources: number | null; cdn_date: string | null }
  releases: { version: string; cur: ReleaseWindow; prev: ReleaseWindow }[]   // the 8 top versions, newest first
  errors: { category: string; code: string; cur: number; prev: number; devices: number; status: 'new' | 'rising' | 'steady' }[]
  crash_clusters: { signal: string; fault_code: string; version: string; board: string; cur: number; prev: number; devices: number }[]
  data_health: { kind: string; latest: number; median: number; low: boolean }[]
}
export interface ReleaseWindow { devices: number; crash_free: number | null; crash_rate: number | null; update_failure_rate: number | null; print_success: number | null }
export function buildHealth(rows: Record<HealthQueryKey, unknown[]>, today: string): HealthPayload
```

Rules:
- `release_*` are restricted to the `top_versions` result; the handler runs `top_versions` first, then the rest in parallel with those versions (sanitized as `releasesQueries` does).
- `crash_free = 1 - crashing_devices / devices`, `null` when devices is 0. `crash_rate = crashes / sessions`, `null` when sessions is 0. `update_failure_rate = failures / (failures + successes)`, `null` when both 0. `print_success = successes / total`, `null` when total 0.
- Error status: `new` when `cur > 0` and `errors_baseline` (the 28 days before `cur`, which contain `prev`) has no row for that (category, code); `rising` when `cur >= 2 * prev && cur >= 5`; else `steady`. Ordered new, rising, steady, then by `cur` descending. Rows with `cur === 0` are dropped.
- Crash clusters: rows with `cur > 0`, ordered by `cur` descending.
- Data health: from `kind_days`, drop `today` (the `today` argument, `YYYY-MM-DD` UTC); `latest` = newest remaining day; `median` = median of the up to 7 days before it; `low = median > 0 && latest < median / 2`. `cdn_fleet_daily` is excluded (written by cron).
- Filters apply per kind through `filterFor`; `cdn_latest` and `kind_days` ignore them.

- [ ] **Step 1: Failing tests:** `windowClause` for cur and prev text; every `healthQueries` value contains its window and, with `version=1.0.4`, the session query contains `filterFor('session', ...)` text; `buildHealth` with canned rows: crash_free/rates computed; all-zero rows give `null` rates not NaN (Review Focus 3); error status `new`/`rising`/`steady` cases including `cur 4, prev 1` (steady: under 5) and `cur 10, prev 5` (rising); `cur 0` dropped; data health flags a kind at 40 against a median of 100 and does not flag one at 60; today's partial row excluded; empty `rows` for every key gives empty arrays and zero actives. Handler test: `/v1/dashboard/health` returns 200 with all five blocks using the mocked `executeQuery` keyed on SQL substrings.
- [ ] **Step 2: Run, see FAIL.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Worker gate.**
- [ ] **Step 5: Commit** `feat(telemetry-worker): health endpoint compares this week with last`.

---

### Task 5: Releases scorecard, retention, performance and prints additions

**Files:**
- Create: `server/telemetry-worker/src/retention.ts`, `__tests__/retention.test.ts`
- Modify: `queries.ts`, `index.ts`, `__tests__/queries.test.ts`, `__tests__/index.test.ts`

**Interfaces:**

```ts
// queries.ts
export function topVersionsQuery(days: number, filters?: FilterParams, limit = 8): string   // shared with healthQueries' top_versions (refactor that to call this)
export function releaseScorecardQueries(versions: string[], days: number, filters?: FilterParams): string[]
//   [0..2] what releasesQueries returns today, now windowed by days and filtered
//   [3] per version: distinct crashing devices
//   [4] per version: wAvg frame_time_p95_ms (performance_snapshot)
//   [5] per version: quantileExactWeighted(0.95) rss_kb (memory_snapshot)
//   [6] per (toDate(timestamp), version): distinct session devices (adoption curve)
export function retentionQuery(filters?: FilterParams): string
//   per session device over 90 days: min(toDate(timestamp)) as first_day, max(toDate(timestamp)) as last_day
// performanceQueries gains, appended after the existing six:
//   [6] per (platform, version): wAvg frame_time_p95_ms, distinct devices
//   [7] per (platform, worst_panel): count of snapshots naming it worst, wAvg worst_panel_p95_ms
//   [8] per (platform, version): wAvg rss_kb from memory_snapshot
// printsQueries gains, appended after the existing three:
//   print_outcome has no printer_model column, so per-model success joins in TypeScript:
//   [3] per session device: argMax(printer_model, timestamp) as model (filterFor('session'))
//   [4] per device: weighted successes, weighted total (filterFor('print_outcome'))
//   [5] per version: weighted successes, weighted total
//   The handler sums [4] into models via [3]; a device missing from [3] counts under 'Unknown'.

// retention.ts
export interface RetentionCohort { start: string; label: string; size: number; survival: number[] }  // survival[k] = share still seen on/after week k, k = 0..weeks-1
export function buildRetention(rows: { first_day: string; last_day: string }[], today: string, weeks = 13): RetentionCohort[]
```

Payload changes:
- `releases`: `versions` optional; when absent, `topVersionsQuery(days, filters)` picks them. Each version adds `crash_free_devices` (`1 - crashing/active`, null at 0 active), `p95_frame_ms`, `p95_rss_kb` (null when no rows), and the top-level `adoption: { date, version, devices }[]`. `crash_rate` is `null` when `total_sessions` is 0 (the dashboard renders `n/a`).
- `performance`: adds `by_platform_version: { platform, version, p95_frame_ms, avg_rss_kb, devices }[]` (rss joined from [8]) and `jankiest_by_platform: { platform, panel, times_worst, avg_p95_ms }[]`.
- `prints`: adds `by_model: { model, successes, total, rate }[]` and `by_version: { version, successes, total, rate }[]`, rate null at total 0.
- `fleet/retention`: `{ cohorts: RetentionCohort[] }`. Cohort `k` covers `[today - 7(k+1), today - 7k)` counted back from `today`; the oldest cohort's label is `"before window"` because devices already running when the window opened land there. Cohorts are oldest first. `survival[0]` is 1 for a non-empty cohort.
- Remove ` LIMIT 20` from `hardwareQueries`' model and MCU distributions.

- [ ] **Step 1: Failing tests:** releases handler with no `versions` param issues `topVersionsQuery` and returns 200; zero sessions gives `crash_rate: null` (Review Focus 3); `crash_free_devices` math; `buildRetention`: a device seen days 0 to 30 counts in weeks 0..4 of its cohort and not week 5; cohort boundaries at exactly 7 days; empty input gives cohorts of size 0 with `survival` all 0; oldest label `before window`; prints `by_model` sums two devices of one model; performance new keys present; `hardwareQueries` contains no `LIMIT 20`.
- [ ] **Step 2: Run, see FAIL.**
- [ ] **Step 3: Implement.** Keep `releasesQueries(versions)` as a thin call to `releaseScorecardQueries(versions, 90)` only if something else still imports it; otherwise delete it and update its tests.
- [ ] **Step 4: Worker gate.**
- [ ] **Step 5: Commit** `feat(telemetry-worker): release scorecard, retention cohorts, per-platform performance and per-model prints`.

---

### Task 6: Dashboard API client

**Files:**
- Modify: `server/analytics-dashboard/src/services/api.ts`

**Interfaces:** add types matching Tasks 3 to 5 exactly (`HealthData` = `HealthPayload`, `DeviceListData`, `DevicePageData` = `DevicePage`, `RetentionData`, the new fields on `ReleasesData`, `PerformanceData`, `PrintsData`), and calls `api.getHealth()`, `api.getDevices()`, `api.getDevice(id)`, `api.getRetention()`, `api.getReleases(versions?: string[])`. All but `getDevice` send the store's `queryString`; `getDevice` sends none (one device, fixed 90 days). The new payload types are imported, not copied: `import type { HealthPayload } from '../../../telemetry-worker/src/health'`, likewise `DevicePage`/`DeviceListRow` from `devices` and `RetentionCohort` from `retention`. If vue-tsc rejects a file outside `src/`, add `"../telemetry-worker/src/{health,devices,retention,versions,schema}.ts"` to `tsconfig.app.json` `include`. This works only because those modules import nothing but `./versions` and `./schema` (Global Constraints).

- [ ] **Step 1:** Write the types and calls.
- [ ] **Step 2:** Dashboard gate (vue-tsc proves the shapes line up with existing views that use `ReleasesData`; fix every view the `crash_rate: number | null` change breaks by rendering through `formatRate`).
- [ ] **Step 3: Commit** `feat(telemetry-dashboard): API client for health, devices, retention and the scorecard`.

---

### Task 7: Sections, seven tabs, routes

**Files:**
- Create: `components/sections/` one file per section below, `components/ChartCaption.vue`, `components/DistributionBar.vue`, `views/FleetView.vue`, `views/UsageView.vue`
- Modify: `components/BarChart.vue`, `PieChart.vue`, `LineChart.vue`, `components/AppLayout.vue`, `router/index.ts`, `views/ReleasesView.vue`, `StabilityView.vue`, `PerformanceView.vue`, `PrintsView.vue`
- Delete: `views/AdoptionView.vue`, `HardwareView.vue`, `EngagementView.vue`, `FeaturesView.vue`, `UxInsightsView.vue`, `MemoryView.vue`, `CrashesView.vue`, `ReliabilityView.vue` (after their content is in sections; `OverviewView.vue` is deleted in Task 8)

**Interfaces:**
- Chart components emit `select(index: number, label: string)` on a click that hits an element (chart.js `onClick` with `getElementsAtEventForMode(evt, 'nearest', { intersect: true }, true)`), and accept an optional `height?: number` prop that overrides the 300px container.
- `ChartCaption.vue`: prop `text: string`, renders one muted line under a chart heading.
- `DistributionBar.vue`: props `rows: Grouped[]`, `filterKey: DashFilterKey`, `caption: string`, `title: string`. Renders a horizontal `BarChart` with `barChartHeight(rows.length)`, colors via `seriesColor`, an expandable row's label suffixed ` ▸`. Click: expandable row calls `store.toggleExpand(row.key)`; otherwise, when every member `isFilterable`, calls `store.drill(filterKey, row.members)`; a non-filterable row shows a tooltip line "not filterable" and does nothing.
- Each section is a self-contained component that fetches its own endpoint on store change, the way the old views do. Section list and the tab that stacks them:

| Tab | Sections (in order) | From |
|-----|---------------------|------|
| Releases | `ReleaseScorecardSection` (table: version, active, crash-free, crash rate, update failure, print success, p95 frame, p95 RSS; version picker kept), `AdoptionCurveSection` (line per version, `seriesColor` by version key), `StuckVersionsSection` (`groupVersions` over adoption versions, filtered to lines older than the newest two) | ReleasesView, AdoptionView |
| Stability | `CrashClustersSection`, `CrashRateSection` (by version, by platform, trailing 14d), `ErrorCodesSection`, `MemoryWarningsSection`, `ConnectionSection` | StabilityView, CrashesView, MemoryView (warnings), ReliabilityView |
| Performance | `FrameTimeSection`, `DropRateSection`, `PlatformVersionPerfSection` (new data), `JankiestPanelsSection` (per platform), `RssTrendSection` | PerformanceView, MemoryView (rss) |
| Fleet | `FleetSizeSection` (daily actives, cumulative, CDN), `PlatformsSection`, `BoardsSection`, `ModelsSection` (`canonicalModels`), `HardwareSection` (kinematics, MCU, AMS, capabilities, RAM, build volume), `RetentionSection` (cohort table, survival as `formatRate`) | OverviewView, AdoptionView, HardwareView |
| Usage | `PanelsSection`, `FeaturesSection`, `SettingsSection`, `ThemesLocalesSection` | EngagementView, FeaturesView, UxInsightsView |
| Prints | `PrintSuccessSection` (trend), `PrintsByModelSection`, `PrintsByVersionSection`, `MaterialSetsSection` (`groupMaterialSets`, multi-material share card), `PrintStartSection` (slicer, thumbnails, file size, source, AMS) | PrintsView |

- Every distribution of platform, version, model or board renders through `DistributionBar` (pie charts of those four become bars). Every chart has a `ChartCaption` naming unit and window (for example "Distinct devices with a session, last 30 days" or "Sample-weighted print outcomes per day; first and current day omitted").
- Router: the tab routes that exist after this task (Health and the device routes are added by Tasks 8 and 9), and one route per legacy path: `{ path: '/adoption', redirect: (to) => ({ path: legacyRedirect(to.path)!, query: to.query }) }` for each legacy path. The current `/crashes` and `/reliability` redirects lose their query today; the new form keeps it.
- `AppLayout.vue` nav: Health, Releases, Stability, Performance, Fleet, Usage, Prints, in that order, with `Breadcrumb` mounted above the page slot.
- Every daily series goes through `trimPartialDays(series, todayUtc())` where `todayUtc()` is `new Date().toISOString().slice(0, 10)` in `utils/series.ts` (add it there with a test in Task 1's file).

- [ ] **Step 1:** Add `select` and `height` to the chart components; add `ChartCaption`, `DistributionBar`.
- [ ] **Step 2:** Move each old view body into its section(s): template, script and scoped style move verbatim, then apply the readability rules. One commit per tab: `refactor(telemetry-dashboard): <tab> tab from sections`.
- [ ] **Step 3:** Router and nav.
- [ ] **Step 4:** Dashboard gate after each tab; `grep -rn "COLORS = \[" src/` returns nothing (palette is `seriesColor` only); `grep -rn "LIMIT\|slice(0, *[0-9]" src/components/sections` shows no top-N cut.
- [ ] **Step 5:** Final commit for router/nav `feat(telemetry-dashboard): seven decision tabs with legacy redirects`.

---

### Task 8: Health tab

**Files:**
- Create: `views/HealthView.vue`
- Delete: `views/OverviewView.vue`
- Modify: `router/index.ts` (`/` is Health)

Layout, top to bottom:
1. Fleet row: four `MetricCard`s: opt-in actives this week (delta vs last week as `+12 (+4.1%)`), CDN sources (with date), new error codes count, low-data kinds count.
2. Release table: one row per version from `releases`, each metric `cur` with an arrow and the signed change against `prev` (`crash_free` and `print_success` up is good; `crash_rate` and `update_failure_rate` up is bad; colors from those senses). `null` renders `n/a` via `formatRate`. A version row links to `/releases?version=<v>`.
3. Errors table: category, code, cur, prev, devices, status badge (`new`, `rising`). Row links to `/stability?version=` unchanged filters plus a hash `#error-<code>` that `ErrorCodesSection` scrolls to.
4. Crash clusters table: signal, fault code, version, board, cur, prev, devices. Row click drills `version` and `board` and opens `/stability`.
5. Data-health line: "All kinds within normal volume" or the low kinds listed with latest vs median.

- [ ] **Step 1:** Build the view on `api.getHealth()`; `ChartCaption` on each block says "Last 7 days vs the 7 before".
- [ ] **Step 2:** Dashboard gate.
- [ ] **Step 3: Commit** `feat(telemetry-dashboard): Health tab is the home page`.

---

### Task 9: Devices list and device page

**Files:**
- Create: `views/DevicesView.vue`, `views/DeviceView.vue`
- Modify: `router/index.ts`, `components/AppLayout.vue` (a "Devices" link in each tab header, carrying the current query)

- `DevicesView`: table from `api.getDevices()`; header "Showing 200 of <total> devices" when total exceeds 200; row click goes to `/device/<id>` keeping the query string so back returns to the same list. Model column shows the raw model (variant visible, per spec).
- `DeviceView`: header block, hardware and settings as two key/value tables (field names through `titleCase`), version history table, timeline table (timestamp via `formatTimestamp`, kind, fields as `key=value` pairs). 400 and 404 responses render "No such device in the last 90 days" rather than an error banner.

- [ ] **Step 1:** Build both views and routes.
- [ ] **Step 2:** Dashboard gate.
- [ ] **Step 3: Commit** `feat(telemetry-dashboard): drill down to a device list and one device's timeline`.

---

### Task 10: Smoke script, docs, plan removal

**Files:**
- Create: `server/telemetry-worker/scripts/smoke.ts`
- Modify: `docs/devel/TELEMETRY_ADMIN.md`
- Delete: `docs/devel/plans/2026-10-15-telemetry-dashboard-ia-design.md`, `docs/devel/plans/2026-10-15-telemetry-dashboard-ia.md` (in the landing commit, after the final review, not before)

`smoke.ts` (run with `npx vite-node scripts/smoke.ts [base-url]`, default `https://telemetry.helixscreen.org`): reads the admin key the way `scripts/reconcile.ts` does (reuse its `parseEnvValue` and env lookup, do not copy them); hits every `/v1/dashboard/*` endpoint across ranges `7d,30d,90d` and filters none, `platform=pi`, `version=<newest from adoption>`, `model=<top from adoption>`, `board=rpi4`; then `devices`, `device/<first id from devices>`, `health`, `fleet/retention`; prints a PASS/FAIL table and exits non-zero on any non-200. It also asserts `device/<64 zeros>` is 404 and `device/xyz` is 400.

- [ ] **Step 1:** Write `smoke.ts`; test its pure table formatter in `__tests__/smoke.test.ts` (the network part is exercised live after deploy).
- [ ] **Step 2:** Rewrite the dashboard section of `TELEMETRY_ADMIN.md`: the seven tabs and the question each answers, drill-down, readability rules, the smoke script.
- [ ] **Step 3:** Worker gate + dashboard gate.
- [ ] **Step 4: Commit** `docs(telemetry): dashboard tabs, drill-down and the smoke script`.

---

## After the tasks

1. Final whole-branch review on the most capable model (spec compliance, DRY against existing helpers, SQL rules above).
2. `make mutate-diff` does not cover TypeScript; instead, for each new pure module, delete one branch of logic by hand and confirm a test fails (record which in the ledger).
3. Ask Preston before: deploying the worker (`npx wrangler deploy --var GIT_SHA:$(git rev-parse --short HEAD)`, rollback target = current version id from `npx wrangler deployments list`), deploying Pages, and landing on main.
4. After deploy: `npx vite-node scripts/smoke.ts`, then open the Health tab and one device page and show screenshots.
