# Telemetry Feedback Loop Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the loop from telemetry to action: a weekly report (snapshot, dashboard page, email), daily alerts, GitHub issues for actionable patterns, printer-database candidate issues from a weekly Action, and a release check before tagging.

**Architecture:** Pure "findings" functions turn the Health payload (plus two new queries) into fingerprinted findings. A loop runner decides what to alert, what to file and what to store. It talks to GitHub, Resend and R2 through three small interfaces so tests use fakes. The worker's existing daily cron calls the runner. The GitHub App token code and the Resend call move from the crash worker to `server/shared/` so both workers share one copy. The printer-database half runs in a GitHub Action on the existing Python pipeline. The release check is a TypeScript script behind a shell wrapper wired into `release-gate.sh`.

**Tech Stack:** TypeScript, Cloudflare Workers (cron, R2), Analytics Engine SQL, Resend HTTP API, GitHub REST API via a GitHub App, vitest, Vue 3, Python 3 with pytest, bats, GitHub Actions.

**Spec:** `docs/devel/plans/2026-10-15-telemetry-feedback-loop-design.md`

## Global Constraints

- Work in `.worktrees/telemetry-loop` on `feature/telemetry-loop`. `node_modules` in `server/telemetry-worker`, `server/analytics-dashboard` and `server/crash-worker` are symlinks to the main tree; never `npm install` or `npm ci`.
- Gates:
  - Telemetry worker: `cd server/telemetry-worker && npx vitest run`, all green, and `npx tsc --noEmit -p .` with 0 errors outside `src/__tests__/` and no more than 161 inside.
  - Crash worker: `cd server/crash-worker && npx vitest run` (it gains no new type errors: compare `npx tsc --noEmit -p .` counts against main).
  - Dashboard: `cd server/analytics-dashboard && npx vue-tsc --noEmit -p tsconfig.app.json && npx vite build`.
  - Python: `python3 -m pytest tests/python/<file>`.
  - bats: `bats tests/shell/<file>` for touched suites, and `make test-shell` once at the end (the whole shell suite when `tests/shell/` changes).
- Analytics Engine SQL rules (from live 502s): GROUP BY names the SELECT alias; no alias equals a column name; constant predicates are `SQL_FALSE`/`SQL_TRUE`; counts go through `wCount`/`wSum`/`wCountIf` (only `count(DISTINCT ...)` unweighted); columns come from `col()`/`colsOf()`; use only functions already used in `queries.ts`.
- Thresholds are named constants in one module (`server/telemetry-worker/src/loop/thresholds.ts`), with the values from the spec. Tests import them; no number is repeated.
- No device ids, IPs or other identifying data in any email or issue. A device count is fine; an id prefix is not.
- Nothing sends, files or writes R2 under `dry=1`. Every test runs against fakes; no test touches the network.
- Comments describe the code as it is (no history), no em-dashes in anything written, SPDX header `// SPDX-License-Identifier: GPL-3.0-or-later` (`#` for Python and shell) on new source files.
- Commits: `git add -N <new files>` then `git commit -m "..." -- <paths>`. Never `git add -A`, `--no-verify`, stash, restore, reset or file checkout. Run `git show --stat HEAD` after each commit.

## Review Focus

1. **The same finding two weeks running** must update one issue, not open a second. Tested in Task 5 with the fake GitHub.
2. **A closed issue whose finding returns** is reopened with a comment and never duplicated (Task 5).
3. **The issue cap**: the 6th new finding in a run is held back, listed in the email and filed next week (Task 5).
4. **A missing secret or a GitHub or Resend failure** must not break the CDN snapshot that shares the cron, and must not mark an alert as sent when it was not (Task 7).
5. **Report immutability**: re-running the weekly mode for a stored week returns the stored report and files nothing new (Task 6).

---

### Task 1: Shared GitHub App and email helpers

**Files:**
- Create: `server/shared/github.ts`, `server/shared/email.ts`, `server/shared/README.md` (one paragraph: what lives here and that both workers import it)
- Modify: `server/crash-worker/src/github-app.ts` (keeps `crashFingerprint`, `normalizeSymbol`, `isKnownRelease`, `findExistingIssue`, `addDuplicateComment`; imports token and fetch helpers from `../../shared/github`), `server/crash-worker/src/index.ts` (`sendBundleNotification` sends through `sendEmail`)
- Test: `server/telemetry-worker/src/__tests__/shared_email.test.ts` (the telemetry worker's vitest `include` gains `../shared/**/*.test.ts` only if the test lives there; prefer testing from `src/__tests__/` with a relative import)

**Interfaces:**

```ts
// server/shared/github.ts
export async function importPrivateKey(pem: string): Promise<CryptoKey>
export async function githubFetch(path: string, token: string, options?: RequestInit, userAgent?: string): Promise<Response>
export async function getInstallationToken(appId: string, pemKey: string, owner: string, repo: string): Promise<string>

// server/shared/email.ts
export interface EmailConfig { apiKey: string; from: string; to: string }
export interface EmailMessage { subject: string; html: string; text: string }
export async function sendEmail(config: EmailConfig, message: EmailMessage, fetchImpl?: typeof fetch): Promise<void>  // throws with the Resend status and body on a non-2xx
```

- Move the existing bodies, do not rewrite them. `githubFetch`'s User-Agent defaults to the crash worker's current string, and the telemetry worker passes `HelixScreen-Telemetry`.
- Both workers bundle the shared file through wrangler's esbuild; confirm with `npx wrangler deploy --dry-run --outdir /tmp/<scratch>` in each worker (no network).

- [ ] Step 1: Failing tests: `sendEmail` posts to `https://api.resend.com/emails` with `Authorization: Bearer <key>` and the `from/to/subject/html/text` body, using an injected fake fetch; a 422 throws an error containing `422`.
- [ ] Step 2: Move the code; crash-worker suite stays green (`crash-fingerprint.test.ts`, `version-gate.test.ts` still import from `../github-app`).
- [ ] Step 3: Both `wrangler deploy --dry-run` bundles build.
- [ ] Step 4: Commit `refactor(server): GitHub App token and Resend email helpers shared by both workers`.

---

### Task 2: Loop thresholds, fingerprints and ISO weeks

**Files:**
- Create: `server/telemetry-worker/src/loop/thresholds.ts`, `server/telemetry-worker/src/loop/fingerprint.ts`, `server/telemetry-worker/src/loop/week.ts`
- Test: `server/telemetry-worker/src/__tests__/loop_basics.test.ts`

**Interfaces:**

```ts
// thresholds.ts (values verbatim from the spec)
export const CLUSTER_ALERT_DEVICES = 3
export const CLUSTER_ISSUE_DEVICES = 3
export const ERROR_ISSUE_DEVICES = 5
export const UPDATE_FAILURE_ISSUE_DEVICES = 3
export const RELEASE_REGRESSION_MIN_DEVICES = 20
export const RELEASE_REGRESSION_CRASH_FREE_DROP = 0.02
export const PRINTER_ISSUE_DEVICES = 5
export const NEW_ISSUES_PER_RUN = 5
export const ALERT_REPEAT_DAYS = 30

// fingerprint.ts
export type FindingKind = 'crash_cluster' | 'error_code' | 'update_failure' | 'release_regression' | 'data_health' | 'printer'
export function fingerprint(kind: FindingKind, parts: string[]): string   // "<kind>:" + parts lowercased, trimmed, joined with "|"; stable across runs
export function markerFor(fp: string): string                             // "<!-- telemetry-fp: <fp> -->"
export function fingerprintInBody(body: string): string | null            // parses the marker back out

// week.ts
export function isoWeek(date: Date): string          // "2026-W42", ISO 8601 (Monday-start, week 1 holds Jan 4), UTC
export function previousIsoWeek(date: Date): string  // the week before the one `date` falls in
export function isMonday(date: Date): boolean        // UTC
```

- [ ] Step 1: Failing tests: `isoWeek` for 2026-01-01 (2026-W01), 2027-01-01 (2026-W53), 2026-12-28 (2026-W53), 2026-10-19 (Monday, 2026-W43) and `previousIsoWeek(2026-10-19) === "2026-W42"`. Fingerprints: case and whitespace insensitive, order-preserving, distinct across kinds. Marker round-trip, plus a body with no marker giving null.
- [ ] Step 2: Implement.
- [ ] Step 3: Commit `feat(telemetry-worker): loop thresholds, finding fingerprints and ISO week keys`.

---

### Task 3: Findings

**Files:**
- Create: `server/telemetry-worker/src/loop/findings.ts`
- Modify: `server/telemetry-worker/src/queries.ts` (one new query builder), `server/telemetry-worker/src/health.ts` only if a field the findings need is missing
- Test: `server/telemetry-worker/src/__tests__/loop_findings.test.ts`

**Interfaces:**

```ts
// queries.ts
export function updateFailureGroupsQuery(window: readonly [number, number]): string
//   per (app_platform as platform, reason): weighted count as failures, count(DISTINCT device) as devices,
//   from update_failed rows in the window, reason != UPDATE_DEFERRED_REASON (reuse the existing constant)

// findings.ts
export type Action = 'alert' | 'issue' | 'report'
export interface Finding {
  kind: FindingKind
  fingerprint: string
  actions: Action[]            // every finding carries 'report'
  title: string                // issue title / alert line, no ids
  summary: Record<string, string | number>   // the counts the email and issue body show
  dashboardPath: string        // e.g. "/stability?version=1.0.4&board=rpi4"
}
export function findingsFromHealth(health: HealthPayload, updateGroups: UpdateFailureGroup[]): Finding[]
export interface UpdateFailureGroup { platform: string; reason: string; failures: number; devices: number }
```

Rules (thresholds from Task 2):
- Crash cluster: every `health.crash_clusters` row. `alert` when devices >= `CLUSTER_ALERT_DEVICES` and `prev === 0`. `issue` when devices >= `CLUSTER_ISSUE_DEVICES`. Fingerprint parts: signal, fault code, version.
- Error code: `status` new or rising and devices >= `ERROR_ISSUE_DEVICES` gives `issue`. Fingerprint: category, code.
- Update failure: devices >= `UPDATE_FAILURE_ISSUE_DEVICES` gives `issue`. Fingerprint: platform, reason.
- Release regression: cur.devices >= `RELEASE_REGRESSION_MIN_DEVICES`, both crash_free values non-null, and the drop > `RELEASE_REGRESSION_CRASH_FREE_DROP` gives `alert`. Fingerprint: version plus the ISO week.
- Data health: every `low` kind gives `alert`. Fingerprint: kind plus the date of `latest`.
- Ordering: alerts first, then issues, then by device count descending.

- [ ] Step 1: Failing tests, one per rule, with the boundary on each side (2 vs 3 devices, a 2.0pp vs 2.1pp drop, 19 vs 20 devices, a null crash_free), plus: the error-code status `steady` never files; `print_in_progress` is excluded in the query SQL text; a finding never contains a 64-hex string.
- [ ] Step 2: Implement.
- [ ] Step 3: Commit `feat(telemetry-worker): findings from the health payload with fingerprints and actions`.

---

### Task 4: Ports, the R2 state store and fakes

**Files:**
- Create: `server/telemetry-worker/src/loop/ports.ts`, `server/telemetry-worker/src/loop/state.ts`, `server/telemetry-worker/src/__tests__/loop_fakes.ts` (fakes shared by the loop tests)
- Test: `server/telemetry-worker/src/__tests__/loop_state.test.ts`

**Interfaces:**

```ts
// ports.ts
export interface IssueRef { number: number; state: 'open' | 'closed'; url: string; body: string; lastTelemetryCommentWeek: string | null }
export interface IssueTracker {
  findByFingerprint(fp: string): Promise<IssueRef | null>      // searches open and closed issues with label telemetry for the marker
  create(title: string, body: string, labels: string[]): Promise<IssueRef>
  comment(issue: number, body: string): Promise<void>
  reopen(issue: number): Promise<void>
}
export interface Mailer { send(message: EmailMessage): Promise<void> }
export interface ObjectStore { get(key: string): Promise<string | null>; put(key: string, value: string): Promise<void>; list(prefix: string): Promise<string[]> }
export function githubIssueTracker(env: LoopEnv): IssueTracker   // built on server/shared/github.ts
export function resendMailer(env: LoopEnv): Mailer               // built on server/shared/email.ts
export function r2Store(bucket: R2Bucket): ObjectStore
export interface LoopEnv { GITHUB_REPO?: string; GITHUB_APP_ID?: string; GITHUB_APP_PRIVATE_KEY?: string; RESEND_API_KEY?: string; NOTIFICATION_EMAIL?: string; EMAIL_FROM?: string; DASHBOARD_URL?: string }

// state.ts
export interface LoopState { alerts: Record<string, string>; issueComments: Record<string, string> }  // fingerprint -> ISO date sent; fingerprint -> ISO week commented
export async function loadState(store: ObjectStore): Promise<LoopState>      // "loop/state.json"; missing or unparsable gives empty state, logged
export async function saveState(store: ObjectStore, state: LoopState): Promise<void>
export function alertDue(state: LoopState, fp: string, today: string): boolean   // true when never sent or sent more than ALERT_REPEAT_DAYS ago
```

- `lastTelemetryCommentWeek` comes from state, not from scraping comments; `findByFingerprint` fills it from `state.issueComments` at the call site (the tracker returns null for it).
- The GitHub tracker searches `repo:<repo> is:issue label:telemetry "telemetry-fp: <fp>" in:body` (all states). It is the only GitHub search; issue creation uses `/repos/<repo>/issues`.
- The fakes record every call and are the only implementations the loop tests use.

- [ ] Step 1: Failing tests: state round-trip; corrupt JSON gives empty state; `alertDue` at 29, 30 and 31 days; the fake tracker finds by fingerprint across open and closed issues.
- [ ] Step 2: Implement (the GitHub and Resend adapters are thin and covered by Task 7's dry-run check plus a request-shape test with a fake fetch).
- [ ] Step 3: Commit `feat(telemetry-worker): loop ports for GitHub, email and R2 with state persistence`.

---

### Task 5: Issue sync

**Files:**
- Create: `server/telemetry-worker/src/loop/issues.ts`
- Test: `server/telemetry-worker/src/__tests__/loop_issues.test.ts`

**Interfaces:**

```ts
export interface IssueOutcome { fingerprint: string; action: 'created' | 'commented' | 'reopened' | 'unchanged' | 'held'; issue?: { number: number; url: string } }
export function issueBody(f: Finding, dashboardUrl: string, week: string): string    // marker first line, then counts table, dashboard link, the window
export function issueLabels(kind: FindingKind): string[]                             // ['telemetry', 'crash' | 'error-code' | 'update-failure' | 'printer-db']
export async function syncIssues(findings: Finding[], tracker: IssueTracker, state: LoopState, week: string, dashboardUrl: string, dryRun: boolean): Promise<IssueOutcome[]>
```

Rules:
- Only findings carrying `issue`.
- Match open and not yet commented this week: comment with this week's counts and record the week in state. Open and already commented this week: `unchanged`.
- Match closed: reopen and comment (says it returned this week).
- No match: create, up to `NEW_ISSUES_PER_RUN`; the rest are `held`.
- `dryRun` computes the same outcomes and calls nothing that writes.

- [ ] Step 1: Failing tests (fake tracker): create, comment, a second run in the same week gives `unchanged` with no second comment, reopen, the cap (6 new findings give 5 created and 1 held), dry run makes zero write calls but returns the same actions, `issueBody` has the marker on its first line and no 64-hex string.
- [ ] Step 2: Implement.
- [ ] Step 3: Commit `feat(telemetry-worker): file, update and reopen telemetry issues by fingerprint`.

---

### Task 6: Report, email and the loop runner

**Files:**
- Create: `server/telemetry-worker/src/loop/report.ts`, `server/telemetry-worker/src/loop/email.ts`, `server/telemetry-worker/src/loop/run.ts`
- Test: `server/telemetry-worker/src/__tests__/loop_run.test.ts`, `server/telemetry-worker/src/__tests__/loop_email.test.ts`

**Interfaces:**

```ts
// report.ts
export interface WeeklyReport { week: string; generated_at: string; health: HealthPayload; findings: Finding[]; issues: IssueOutcome[] }
export const reportKey = (week: string) => `reports/${week}.json`

// email.ts (pure)
export function weeklyEmail(report: WeeklyReport, dashboardUrl: string): EmailMessage   // subject "HelixScreen telemetry, week <week>"
export function alertEmail(findings: Finding[], dashboardUrl: string, today: string): EmailMessage   // subject "HelixScreen telemetry alert: <n> new"

// run.ts
export interface LoopDeps { health(): Promise<HealthPayload>; updateGroups(): Promise<UpdateFailureGroup[]>; tracker: IssueTracker; mailer: Mailer | null; store: ObjectStore; now: Date; dashboardUrl: string }
export interface LoopResult { mode: 'daily' | 'weekly'; week?: string; findings: Finding[]; alertsSent: string[]; issues: IssueOutcome[]; email?: EmailMessage; reused?: boolean; skipped: string[] }
export async function runLoop(mode: 'daily' | 'weekly', deps: LoopDeps, dryRun: boolean): Promise<LoopResult>
```

Rules:
- Daily: findings, then the alert findings with `alertDue` true. When there are any, send one `alertEmail` and record each fingerprint with today's date. When `mailer` is null, record nothing and add "email: not configured" to `skipped`.
- Weekly (the week is `previousIsoWeek(now)`):
  - When `reportKey(week)` exists, return it with `reused: true` and file nothing.
  - Otherwise compute findings, then `syncIssues`, store the report (immutable), send `weeklyEmail`, save state.
  - The alert step still runs on a Monday daily run; weekly does not send alerts.
- Order and failure handling: store the report before emailing. A GitHub error records that finding as `held` with the error in `skipped`; the run continues. A mail error leaves the alerts unrecorded so the next day retries.
- Escape every interpolated value in the HTML. The text body carries the same facts.

- [ ] Step 1: Failing tests: daily with two due alerts sends one email and records both; a second daily run the same day sends nothing; a mailer that throws leaves both unrecorded; weekly stores, files, emails; weekly re-run reuses and files nothing (Review Focus 5); GitHub throwing on one create holds that one and still emails; the email HTML escapes `<script>` in a title; neither email contains a 64-hex string; the dry run returns an email and writes nothing.
- [ ] Step 2: Implement.
- [ ] Step 3: Commit `feat(telemetry-worker): weekly report snapshots, alert and summary emails, and the loop runner`.

---

### Task 7: Cron, endpoints and configuration

**Files:**
- Modify: `server/telemetry-worker/src/index.ts`, `server/telemetry-worker/wrangler.toml`, `server/telemetry-worker/src/__tests__/index.test.ts`

**Changes:**
- `wrangler.toml`:
  - `crons = ["10 2 * * *", "0 13 * * *"]`.
  - `[vars]` gains `GITHUB_REPO = "prestonbrown/helixscreen"`, `GITHUB_APP_ID = "2892859"`, `NOTIFICATION_EMAIL = "pbrown+helixscreen@brown-house.net"`, `EMAIL_FROM = "HelixScreen <noreply@helixscreen.org>"` and `DASHBOARD_URL = "https://helixscreen-analytics.pages.dev"`.
  - A comment lists the two secrets (`RESEND_API_KEY`, `GITHUB_APP_PRIVATE_KEY`).
- `scheduled(event)` dispatches on `event.cron`:
  - `"10 2 * * *"`: the existing CDN snapshot, unchanged.
  - `"0 13 * * *"`: `runLoop('daily')`, and on Mondays also `runLoop('weekly')`.
  - The loop is wrapped so its rejection is logged with `console.error` and never affects the CDN path.
  - A missing `GITHUB_APP_PRIVATE_KEY` makes the tracker null-op (`skipped` says so). A missing `RESEND_API_KEY` makes `mailer` null.
- `POST /v1/admin/loop/run?mode=daily|weekly&dry=1` (admin key, 401 otherwise): runs the loop with real reads and returns `LoopResult` JSON. With `dry=1` (the default when `dry` is absent) nothing is written or sent; `dry=0` must be explicit.
- `GET /v1/dashboard/reports` returns `{ weeks: string[] }`, newest first, from the R2 listing. `GET /v1/dashboard/reports/<week>` returns the stored `WeeklyReport`; the week must match `^\d{4}-W\d{2}$` or it is a 400, and missing is a 404.

- [ ] Step 1: Failing handler tests: cron dispatch by expression; a loop failure still runs the CDN capture on the 02:10 cron and does not throw from `scheduled`; admin route 401 without the key; `dry` absent means dry; the reports list is sorted newest first; a bad week is a 400.
- [ ] Step 2: Implement. Add the endpoints to `scripts/smoke.ts`'s endpoint list (the reports list, plus one report when the list is non-empty) and keep the smoke test that parses `index.ts` routes green.
- [ ] Step 3: `npx wrangler deploy --dry-run --outdir <scratch>` builds.
- [ ] Step 4: Commit `feat(telemetry-worker): run the feedback loop from the daily cron, with a dry-run admin route and report endpoints`.

---

### Task 8: Weekly reports in the dashboard

**Files:**
- Create: `server/analytics-dashboard/src/views/ReportsView.vue`, `server/analytics-dashboard/src/views/ReportView.vue`
- Modify: `server/analytics-dashboard/src/views/HealthView.vue` (the payload becomes a prop on an extracted `HealthReportBody.vue` component used by both pages), `src/services/api.ts` (`getReports`, `getReport(week)`; types imported from the worker's `loop/report.ts`, which imports only leaf modules), `src/router/index.ts`, `src/components/AppLayout.vue` (a "Reports" nav entry after Prints)

- `/reports` lists weeks, newest first. Each week links to `/reports/<week>`.
- `/reports/:week` renders the stored health payload through `HealthReportBody`, then a "Findings" table (kind, title, the counts) and an "Issues" table (action, a link to the issue). A 404 renders "No report for <week>".
- The live Health page is unchanged on screen.

- [ ] Step 1: Implement; dashboard gate.
- [ ] Step 2: Render check against a local mock that serves one report (extend the scratchpad mock or serve a fixture JSON). Screenshot `/reports` and one `/reports/<week>`.
- [ ] Step 3: Commit `feat(telemetry-dashboard): weekly report pages built from stored snapshots`.

---

### Task 9: Unrecognized-printer issues from a weekly Action

**Files:**
- Create: `scripts/telemetry-printer-issues.py`, `.github/workflows/telemetry-printers.yml`, `tests/python/test_telemetry_printer_issues.py`, `tests/fixtures/telemetry/printer_analysis_sample.json`
- Modify: `scripts/telemetry-update-printer-db.py` only to expose its entry-building logic as importable functions with no prompts (its interactive behavior unchanged), `scripts/CLAUDE.md` (one row)

**Interfaces:**

```python
def candidates_to_issues(analysis: dict, db: dict, min_devices: int = 5) -> list[dict]   # [{fingerprint, title, body, labels}]
def main(argv) -> int   # --analysis PATH --db PATH [--min-devices N] [--dry-run]; files or comments via `gh`
```

- Which candidates: undetected clusters and new-model candidates with device count >= 5. Read the analysis JSON shape from `telemetry-printer-profiles.py`'s `--json --candidates` output, and build the fixture from a real run's shape with ids removed.
- Fingerprint: `printer:` plus the cluster's hardware signature fields, joined the same way as Task 2 (lowercased, trimmed, `|`).
- Body:
  - the marker (same form as Task 2)
  - the device count
  - the hardware signature (kinematics, MCU, build volume, sensors, notable macros)
  - the draft entry as a fenced JSON block, built by `telemetry-update-printer-db.py`'s functions
  - "review before adding" guidance
- Dedupe via `gh issue list --label telemetry --state all --search "telemetry-fp: <fp> in:body" --json number,state`:
  - Open: comment with the new count, at most once per ISO week (check existing comments' first line `Week <W>`).
  - Closed: leave it closed and print "closed, not reopened" (printer issues are closed when the entry lands).
  - None: create with labels `telemetry,printer-db`, up to 5 per run.
- `--dry-run` prints titles and bodies and runs no `gh` write.
- Workflow:
  - Triggers: `schedule: cron "0 14 * * 1"` and `workflow_dispatch` (input `dry_run`, default true).
  - Python setup, then `pip install -r scripts/telemetry-requirements.txt`.
  - `HELIX_TELEMETRY_ADMIN_KEY` from `secrets.HELIX_TELEMETRY_ADMIN_KEY`.
  - Runs `scripts/telemetry-pull.sh --since <28 days ago>`, `scripts/telemetry-printer-profiles.py --json --candidates > analysis.json`, then the issue script, with `GH_TOKEN: ${{ github.token }}` and `permissions: issues: write`.
  - Exits 0 with a notice when the secret is absent.

- [ ] Step 1: Failing pytest: fixture with two clusters (6 and 3 devices) gives one issue; the body has the marker, the draft JSON parses, no 64-hex string; the fingerprint is stable when cluster field order changes.
- [ ] Step 2: Implement; the workflow passes `actionlint` if installed (else note it).
- [ ] Step 3: Commit `feat(telemetry): weekly Action files printer-database candidate issues from telemetry`.

---

### Task 10: Release check

**Files:**
- Create: `server/telemetry-worker/scripts/releaseHealth.ts`, `scripts/release-health.sh`, `server/telemetry-worker/src/__tests__/release_health.test.ts`, `tests/shell/test_release_health.bats`
- Modify: `scripts/release-gate.sh` (fast tier step), `tests/shell/test_release_gate.bats` if it enumerates steps, `scripts/CLAUDE.md` (one row), `docs/devel/RELEASE_PROCESS.md` (one paragraph)

**Interfaces:**

```ts
export interface ReleaseRow { version: string; active_devices: number; crash_free_devices: number | null; crash_rate: number | null; update_failure_rate: number | null; update_failures: number; update_successes: number; print_success_rate: number | null; p95_frame_ms: number | null; p95_rss_kb: number | null }
export interface Verdict { metric: string; candidate: string; baseline: string; threshold: string; warn: boolean }
export const RELEASE_WARN = { crashFreeDrop: 0.01, crashRateRatio: 1.5, crashRateMinDelta: 0.001, updateFailureRise: 0.05, printSuccessDrop: 0.03, frameRise: 0.2, rssRise: 0.15 }
export function compareRelease(candidate: ReleaseRow, baseline: ReleaseRow): Verdict[]
export function pickCandidate(versionTxt: string, seenVersions: string[]): string | null   // newest prerelease of VERSION.txt's release in telemetry (compareVersions)
export function formatTable(c: ReleaseRow, b: ReleaseRow, verdicts: Verdict[]): string
```

- The script reads `VERSION.txt`, fetches the stable manifest's `version` and the releases endpoint (`range=14d`, both versions), and prints the table, the WARN lines and a small-sample note under 10 candidate devices. Every outcome exits 0, including no key, no network and no candidate, each with a one-line reason.
- `scripts/release-health.sh` runs it with `npx vite-node` from `server/telemetry-worker` and always exits 0.
- `release-gate.sh` fast tier adds a step `release-health` whose command is `scripts/release-health.sh` (which cannot fail). The step's row shows PASS, and the step log holds the table. `RELEASE_HEALTH=0` skips it.

- [ ] Step 1: Failing tests: `compareRelease` at each threshold boundary; null metrics never warn; `pickCandidate("1.1.0", [...])` picks `1.1.0-beta.10` over `1.1.0-beta.9` and ignores `1.0.x`; the bats test fakes the script's network with an env override (`RELEASE_HEALTH_FIXTURE=<json>`) and asserts exit 0 and a WARN line, and that `release-gate.sh fast` lists the `release-health` step.
- [ ] Step 2: Implement.
- [ ] Step 3: Commit `feat(release): release-health warns when the newest beta trails stable in telemetry`.

---

### Task 11: Docs

**Files:**
- Modify: `docs/devel/TELEMETRY_ADMIN.md`: a "Feedback loop" section covering:
  - what runs when (cron times)
  - thresholds, pointing at `thresholds.ts` rather than repeating the numbers
  - fingerprints and dedupe
  - the dry-run route and how to use it
  - reports storage and pages
  - the Action and its secret
  - the release check
  - the secrets list, plus operating notes: re-running a week, silencing a fingerprint by closing its issue (except printer issues), and where state lives
- Modify: `docs/devel/RELEASE_PROCESS.md` if Task 10 did not already.

- [ ] Step 1: Write. No em-dashes, no history.
- [ ] Step 2: Commit `docs(telemetry): the feedback loop, its schedule, thresholds and operations`.

## After the tasks

1. Final whole-branch review on the most capable model.
2. Ask Preston before landing, before `wrangler secret put` (RESEND_API_KEY, GITHUB_APP_PRIVATE_KEY), before deploying, and before adding the Action's repository secret.
3. After deploy: `POST /v1/admin/loop/run?mode=weekly` and `?mode=daily` dry runs, read the would-be issues and email with Preston, then decide on the first real run.
