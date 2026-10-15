# Telemetry dashboard: decision tabs and drill-down

Spec 2 of the telemetry overhaul. Spec 1 made the numbers right (sample-weighted
counts, delta snapshots, one schema table). This spec makes the dashboard answer
questions: seven tabs, each built around a decision, every distribution
clickable down to the devices behind it and then to one device's timeline.
Spec 3 (emailed summary, GitHub issues for actionable clusters, release gate)
builds on the Health tab defined here and is out of scope.

## Decisions already made

- Seven decision tabs replace the thirteen views.
- Drill-down goes all the way to one device's page.
- Health is the home tab and is the summary page spec 3's email links to.
- Old versions are grouped, prereleases sit under their release, multi-material
  filament lists are normalized, and every filter and drill state lives in the
  URL.
- Distributions show every value. No top-N cut and no "Other" bucket; length is
  controlled by grouping, not by dropping rows.

## Tabs

Routes are flat: `/` (Health), `/releases`, `/stability`, `/performance`,
`/fleet`, `/usage`, `/prints`, plus `/devices` and `/device/:id`. Every old
route redirects to the tab that now holds its content, keeping its query
string: `/adoption` and `/hardware` to `/fleet`; `/memory`, `/crashes` and
`/reliability` to `/stability`; `/engagement`, `/features` and `/ux` to
`/usage`.

| Tab | Question it answers | Content |
|-----|---------------------|---------|
| Health | Is anything getting worse this week? | This week vs last week: fleet size (opt-in actives and CDN sources), per-release scorecard deltas, new and rising error codes, crash clusters, data-health line |
| Releases | Is the newest release better than the one before it? | Scorecard per version (active devices, crash-free devices, crash rate, update failure rate, print success, p95 frame time, p95 RSS), adoption curve, devices stuck on old versions |
| Stability | What crashes and errors, where? | Crash clusters (signal, fault code, version, board), crash rate by version and platform, error codes by version, memory warnings by platform, connection stability |
| Performance | Is the UI fast enough on each platform? | Frame time p50/p95 trend, drop rate by platform, p95 frame time and RSS per platform × version, jankiest panels per platform, RSS trend |
| Fleet | Who runs it, on what? | Platforms, boards, printer models (canonical), kinematics, MCUs, AMS, RAM, capabilities, CDN fleet, daily actives, 30/90-day retention |
| Usage | What do people use and change? | Panels, overlays and widgets per device, settings changed from default, configured features, themes and locales |
| Prints | Do prints succeed, and with what? | Success rate trend, success by model, by version and by material set, multi-material share, slicer, thumbnails, print source |

Content moves from the old views into section components; a tab is a stack of
sections. Nothing an old view showed is dropped unless it is listed under
"Removed" below.

### Removed

- Overview's `total_events` card: an event count measures client chattiness,
  not use.
- Releases' manual version picker as the only way in: the tab opens on the
  newest eight versions with traffic in the range, and the picker stays for
  adding others.

## Drill-down

1. **Click a bar, slice or row** in any distribution: that value becomes a
   filter (platform, version, model or board), written to the URL. A grouped
   bar (a version line, a canonical model) filters on every raw value in the
   group.
2. **"Devices" link** on every tab header: `/devices?<filters>` lists the
   devices matching the current filters. Columns: short id, last seen, version,
   platform, board, model, sessions, crashes, prints (all within the range).
   Sorted by last seen, 200 rows max, with the true total shown above the
   table.
3. **Device page** `/device/:id`: header (model, platform, board, current
   version, first and last seen in AE's 90 days), latest hardware profile,
   latest settings snapshot, version history (each version with first and last
   seen), and a timeline of the newest 500 events, newest first, each row
   showing its kind and its non-empty fields by schema name.

A breadcrumb above every tab shows the active filters, each removable, and the
drill path (`Fleet › model = Voron 2.4 › Devices › 3f2a…`). Browser back and
forward walk the same path because every step is a URL.

The device page is admin-key only like every dashboard route. It shows the
hashed device id, never anything the device did not send.

## Readability rules

These are pure functions in the dashboard's `utils/`, shared by every chart.

- **Version grouping.** The two newest `major.minor` lines show each version.
  Older lines collapse to one row per line, labelled `0.99.x (34 versions)`.
  Prereleases of a version collapse to one row, `1.1.0 betas (9)`, until that
  release exists, after which they fold into the release's line like any other
  version. A collapsed row expands in place on click (the expansion is URL
  state), and its filter value is every member.
- **Material sets.** `filament_type` is a `;`-separated slot list. A set is the
  distinct materials, uppercased and trimmed, sorted, joined with ` + `
  (`PLA;PLA;PETG;PLA` is `PETG + PLA`). A set with two or more materials is
  multi-material. Slot count is not part of the set.
- **Canonical models.** A model's key is its name lowercased, a trailing
  parenthesized variant removed, and runs of space, `-` and `_` folded to one
  space, then mapped through a small alias table (`k1` to `creality k1`,
  `zerog` to `zero g`). The label is the most common raw spelling in the group.
  The variant (`ForgeX`, `k2-improvements`) shows in the device list.
- **Series colors.** One fixed palette of 12 distinguishable colors, assigned
  by series index; a series keeps its color across charts on the same tab by
  key.
- **Partial days.** Daily series drop the first day of the window (it starts
  mid-day) and today (still filling).
- **Percentages.** Every ratio is rendered as a percentage through one
  formatter; `null` renders `n/a`.
- **Height follows rows.** A horizontal bar chart is as tall as its rows need
  (22px per row plus axes, 300px minimum), so showing every value never
  squeezes labels together.
- **Captions.** Every chart carries one line saying what it counts (devices,
  sample-weighted events, boots) and over what window.

## Worker

New endpoints, all under `/v1/dashboard/` with the existing admin-key check,
range parsing and filters:

- `GET health`: two windows, `cur` = last 7 days and `prev` = 7 to 14 days ago.
  Returns fleet actives per window and the latest CDN sources; for the eight
  versions with the most active devices in `cur`, per window: active devices,
  crash-free device share, crash rate, update failure rate, print success;
  error codes per window with a 35-day baseline (`new` = none in the 28 days
  before `cur`; `rising` = `cur` at least double `prev` and at least 5); crash
  clusters per window keyed on (signal, fault code, version, board); and a
  data-health block: weighted rows per kind for each of the last 8 closed days,
  flagging a kind whose newest day is under half its prior 7-day median.
- `GET devices`: the device list above, filters applied per kind.
- `GET device/:id`: the device page above. The id must be 64 lowercase hex
  characters or the response is 400.
- `GET fleet/retention`: weekly cohorts over 90 days. A device's cohort is the
  7-day bucket (counted back from today) of its first session in the window;
  for each cohort and each later week k, how many of its devices were still
  seen on or after the start of week k (survival, from each device's first
  and last session date). The oldest cohort also holds every device that was
  already running before the window opened, so it is labelled "before window".
  The query uses only `min`/`max` of `toDate(timestamp)` per device, so it
  depends on no Analytics Engine function the worker does not already use.

Changed endpoints:

- `releases`: `versions` becomes optional (default: the eight versions with the
  most active devices in the range); adds crash-free devices, p95 frame time
  and p95 RSS per version, and a daily adoption series per version. A version
  with zero sessions reports `crash_rate: null`, not `0`.
- `performance`: adds p95 frame time and average RSS per (platform, version)
  and jankiest panels per platform. `performance_snapshot` carries no board,
  so "per board" is per platform until a client sends one.
- `prints`: adds success by printer model and by version.

Rows decode through the schema table: one function turns a raw AE row of a
kind into `{field: value}` using `POINT_KINDS`, so the device timeline needs
no per-kind code.

Model and material normalization happens in the dashboard, not in SQL: the
worker returns raw values with counts, and the dashboard groups them. The one
exception is filtering, where the dashboard sends every raw member of a group.

Filter values pass through `sanitizeFilterValue`, which strips `'`. A model
whose raw name contains a character the sanitizer removes cannot be matched;
the dashboard marks such a value as not filterable instead of sending a filter
that silently matches nothing.

## Testing

- Worker: vitest, mocking `executeQuery` as today. Each new query builder gets
  SQL-shape tests (window bounds, filter clauses per kind, weighted
  aggregates) and each endpoint a response-shape test with canned AE rows,
  including empty results and an all-excluded filter.
- Dashboard: pure utils (version grouping, material sets, model
  canonicalization, partial-day trimming, URL state) are tested from the
  worker's vitest suite, the way `compareVersions` already is. Views are gated
  by `vue-tsc` and `vite build`.
- Live: after deploy, the smoke script hits every endpoint across the filter
  matrix and expects 200, plus `devices`, one `device/:id` from that list, and
  `health`.

## Out of scope

Email, GitHub issues, the release gate (spec 3); any client-side telemetry
change, including a board column on `performance_snapshot`.
