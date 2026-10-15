# Telemetry feedback loop

Spec 3 of the telemetry overhaul. Specs 1 and 2 made the data right and built a
dashboard around decisions, with Health as the summary page. This spec turns
that into a loop that runs without anyone opening the dashboard: a weekly
report, same-day alerts, GitHub issues for things someone can act on, and a
release check before tagging.

## Decisions already made

- Weekly summary email every Monday, plus a same-day alert when something
  crosses a threshold.
- GitHub issues for four kinds of finding: crash clusters, new or rising error
  codes, unrecognized printers, and self-update failures. Summaries never become
  issues.
- Unrecognized printers get an issue carrying a draft printer-database entry,
  never a pull request.
- The release check compares the release's newest beta with the current stable,
  warns with numbers, and never blocks.

## 1. Weekly report

The telemetry worker's daily cron at 13:00 UTC builds the report on Mondays.

- **Snapshot.** The Health payload for the 7 days before the run (the existing
  `buildHealth` over `healthQueries`) plus the run's findings and the issues it
  opened or updated, stored in R2 as `reports/<ISO week>.json` (for example
  `reports/2026-W42.json`, the week that just closed). A report is never
  rewritten once stored; a manual re-run for a week that has one returns the
  stored copy.
- **Dashboard.** `/reports` lists stored weeks, newest first. `/reports/:week`
  renders a stored report with the Health tab's components, and lists the
  findings and the issue links. The Health view takes its payload as a prop so
  both pages share one rendering.
- **Email.** To `NOTIFICATION_EMAIL` through Resend, subject
  `HelixScreen telemetry, week 2026-W42`. The body has the fleet line (opt-in
  actives and CDN sources with week-over-week change), the release table
  (crash-free, crash rate, update failure with counts, print success, each with
  its change), new and rising error codes, crash clusters, data health, the
  issues opened or commented on, any findings held back by the issue cap, and a
  link to `/reports/<week>`. HTML and plain text are built by one pure function
  from the stored report.

## 2. Alerts

The same 13:00 UTC cron runs every day and sends one alert email listing every
alert finding that has not been sent before. An alert fires on:

| Finding | Threshold |
|---|---|
| New crash cluster | A (signal, fault code, version, board) cluster on 3 or more devices in the last 7 days |
| Release regression | A release with 20 or more active devices whose crash-free share fell by more than 2 percentage points against the previous 7 days |
| Data health | A kind flagged low by Health's data-health rule |

Each finding has a fingerprint. Sent fingerprints are kept in R2 at
`loop/state.json` with the date sent, and a fingerprint is not alerted again for
30 days. Nothing is sent when there is nothing new.

## 3. GitHub issues

The Monday run files issues through the same GitHub App the crash worker uses
(`GITHUB_APP_ID`, `GITHUB_APP_PRIVATE_KEY`, repository `prestonbrown/helixscreen`).

| Kind | Threshold | Fingerprint |
|---|---|---|
| Crash cluster | 3 or more devices in the week | signal, fault code, version |
| Error code | Health status `new` or `rising`, and 5 or more devices | category, code |
| Update failure | One platform and reason, 3 or more devices in the week, `print_in_progress` excluded | platform, reason |
| Unrecognized printer | Section 4 | the candidate's hardware fingerprint |

- **Labels:** `telemetry` plus `crash`, `error-code`, `update-failure` or
  `printer-db`.
- **Marker:** every issue body carries `<!-- telemetry-fp: <fingerprint> -->`,
  and lookups search for it.
- **Open match:** add a comment with this week's counts, at most one comment per
  issue per week.
- **Closed match:** reopen it with a comment saying it came back, with the
  counts.
- **No match:** open a new issue, at most 5 per run; the rest are listed in the
  email as held back and wait for the next week.
- **Body:** what was seen (counts, devices, versions, platforms, window), how to
  look at it (a dashboard link with the right filters), and for crash clusters
  the crash worker's fingerprint form so a matching crash-report issue can be
  linked by hand. No device ids, IPs or other identifying data.
- Issues are never closed automatically.

## 4. Unrecognized printers

The worker cannot see what identifies a printer: sensors and macros live only in
the raw R2 events. A weekly GitHub Actions workflow runs the existing pipeline:

1. `scripts/telemetry-pull.sh --since <28 days ago>` with the admin key from a
   repository secret.
2. `scripts/telemetry-printer-profiles.py --json --candidates`.
3. A new `scripts/telemetry-printer-issues.py` reads that JSON. It takes every
   undetected cluster or new-model candidate with 5 or more devices, builds the
   draft database entry with `telemetry-update-printer-db.py`'s own
   entry-building functions (non-interactive, no files written), and files or
   updates issues with `gh`. It uses the same marker, label and dedupe rules as
   section 3.

Runs Mondays at 14:00 UTC and on manual dispatch, with a `--dry-run` mode that
prints the issues without filing.

## 5. Release check

`scripts/release-health.sh` (a wrapper around a TypeScript script in
`server/telemetry-worker/scripts/`, reusing `adminKey.ts`):

- **Candidate:** the newest prerelease of the version in `VERSION.txt` seen in
  telemetry. If `VERSION.txt` is itself a prerelease, its own release line is
  used.
- **Baseline:** the version in `https://releases.helixscreen.org/stable/manifest.json`.
- **Window:** 14 days. One `/v1/dashboard/releases?range=14d&versions=<candidate>,<baseline>`
  call.
- **Output:** a table of active devices, crash-free share, crash rate, update
  failure with counts, print success, and p95 frame time and RSS, with a WARN
  line for each metric past its threshold:

  | Metric | Warn when the candidate is |
  |---|---|
  | Crash-free share | more than 1 point lower |
  | Crash rate | more than 1.5 times the baseline and more than 0.1 points higher |
  | Update failure | more than 5 points higher |
  | Print success | more than 3 points lower |
  | p95 frame time | more than 20% higher |
  | p95 RSS | more than 15% higher |

  Each threshold is printed in its row. Under 10 active candidate devices, the
  table adds a "small sample" note.
- Always exits 0. No key, no network, or no candidate in telemetry prints why
  and skips.
- `release-gate.sh` runs it in the fast tier as a step whose failure is
  impossible by construction; its output goes to the step log, and the gate's
  table shows the WARN count.

## Shared code

The crash worker's GitHub App token code (`importPrivateKey`, the JWT, the
authenticated fetch, `getInstallationToken`) and its Resend call move to
`server/shared/`. Both workers import them; the crash-specific pieces
(`crashFingerprint`, `isKnownRelease`) stay in the crash worker.

## Operations

- New worker secrets: `RESEND_API_KEY` and `GITHUB_APP_PRIVATE_KEY` (the crash
  worker's values). New vars: `GITHUB_REPO`, `GITHUB_APP_ID`,
  `NOTIFICATION_EMAIL`, `EMAIL_FROM`, `DASHBOARD_URL`.
- A missing secret skips that output with a logged warning. It never fails the
  CDN snapshot that shares the cron.
- `POST /v1/admin/loop/run?mode=daily|weekly&dry=1` (admin key) runs the loop
  by hand. A dry run returns the findings, the would-be issue actions and the
  rendered email, and sends and stores nothing.
- New repository secret for the Action: `HELIX_TELEMETRY_ADMIN_KEY`.

## Testing

- Pure functions (fingerprints, thresholds, dedupe decisions, ISO week keys,
  email rendering, issue body rendering, release-check comparison) carry unit
  tests.
- GitHub, Resend and R2 are behind small interfaces with fakes in the tests: one
  test per issue action (create, comment, reopen, skip, cap), alert dedupe
  across two runs, and report immutability.
- The printer-issue script gets Python tests against a fixture analysis JSON.
- `release-health.sh` gets a bats test for the `release-gate.sh` wiring and the
  always-zero exit.
- Live: a dry run of `/v1/admin/loop/run` for both modes before the first real
  send.

## Out of scope

Closing issues automatically, pull requests that edit `printer_database.json`,
chat notifications, and changing what the client sends.
