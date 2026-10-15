// SPDX-License-Identifier: GPL-3.0-or-later
// Compare the newest beta of VERSION.txt's release against the stable release in telemetry.
//
//   (cd server/telemetry-worker && npx vite-node scripts/releaseHealth.ts [base-url])
//
// Reads ../../VERSION.txt, the stable manifest's version and the releases endpoint
// (range=14d, both versions), then prints a table, a WARN line per metric that trails
// stable and a note when the candidate has few devices. Every outcome exits 0, with a
// one-line reason when there is nothing to compare. RELEASE_HEALTH_FIXTURE is a JSON
// file path or JSON text of {"stable": "1.0.3", "versions": [<release rows>]} that
// replaces the network.

import { existsSync, readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { adminKeyFromEnvOrFile } from "./adminKey";
import { compareVersions } from "../src/versions";

const DEFAULT_BASE_URL = "https://telemetry.helixscreen.org";
const STABLE_MANIFEST_URL = "https://releases.helixscreen.org/stable/manifest.json";
const RANGE = "14d";
const SMALL_SAMPLE_DEVICES = 10;

export interface ReleaseRow {
  version: string;
  active_devices: number;
  crash_free_devices: number | null;
  crash_rate: number | null;
  update_failure_rate: number | null;
  update_failures: number;
  update_successes: number;
  print_success_rate: number | null;
  p95_frame_ms: number | null;
  p95_rss_kb: number | null;
}

export interface Verdict {
  metric: string;
  candidate: string;
  baseline: string;
  threshold: string;
  warn: boolean;
}

/** Absolute for rates (fractions), relative for the frame and memory rises. */
export const RELEASE_WARN = {
  crashFreeDrop: 0.01,
  crashRateRatio: 1.5,
  crashRateMinDelta: 0.001,
  updateFailureRise: 0.05,
  printSuccessDrop: 0.03,
  frameRise: 0.2,
  rssRise: 0.15,
};

// Absorbs float error so a change of exactly the threshold counts as reaching it.
const EPS = 1e-9;
const reaches = (change: number, threshold: number) => change >= threshold - EPS;

const pct = (v: number | null) => (v === null ? "n/a" : `${(v * 100).toFixed(2)}%`);
const fixed = (v: number | null) => (v === null ? "n/a" : String(Math.round(v * 10) / 10));

/** One verdict per compared metric; a metric either side lacks never warns. */
export function compareRelease(c: ReleaseRow, b: ReleaseRow): Verdict[] {
  const both = (x: number | null, y: number | null): x is number => x !== null && y !== null;
  const out: Verdict[] = [];

  out.push({
    metric: "crash-free devices",
    candidate: pct(c.crash_free_devices),
    baseline: pct(b.crash_free_devices),
    threshold: `drop of ${RELEASE_WARN.crashFreeDrop * 100} points`,
    warn:
      both(c.crash_free_devices, b.crash_free_devices) &&
      reaches(b.crash_free_devices! - c.crash_free_devices, RELEASE_WARN.crashFreeDrop),
  });

  const crashWarn =
    both(c.crash_rate, b.crash_rate) &&
    reaches(c.crash_rate - b.crash_rate!, RELEASE_WARN.crashRateMinDelta) &&
    reaches(c.crash_rate, b.crash_rate! * RELEASE_WARN.crashRateRatio);
  out.push({
    metric: "crash rate",
    candidate: pct(c.crash_rate),
    baseline: pct(b.crash_rate),
    threshold: `${RELEASE_WARN.crashRateRatio}x and +${RELEASE_WARN.crashRateMinDelta * 100} points`,
    warn: crashWarn,
  });

  out.push({
    metric: "update failure rate",
    candidate: pct(c.update_failure_rate),
    baseline: pct(b.update_failure_rate),
    threshold: `rise of ${RELEASE_WARN.updateFailureRise * 100} points`,
    warn:
      both(c.update_failure_rate, b.update_failure_rate) &&
      reaches(c.update_failure_rate - b.update_failure_rate!, RELEASE_WARN.updateFailureRise),
  });

  out.push({
    metric: "print success rate",
    candidate: pct(c.print_success_rate),
    baseline: pct(b.print_success_rate),
    threshold: `drop of ${RELEASE_WARN.printSuccessDrop * 100} points`,
    warn:
      both(c.print_success_rate, b.print_success_rate) &&
      reaches(b.print_success_rate! - c.print_success_rate, RELEASE_WARN.printSuccessDrop),
  });

  const rise = (x: number | null, y: number | null, limit: number) =>
    both(x, y) && y! > 0 && reaches((x - y!) / y!, limit);
  out.push({
    metric: "p95 frame ms",
    candidate: fixed(c.p95_frame_ms),
    baseline: fixed(b.p95_frame_ms),
    threshold: `rise of ${RELEASE_WARN.frameRise * 100}%`,
    warn: rise(c.p95_frame_ms, b.p95_frame_ms, RELEASE_WARN.frameRise),
  });
  out.push({
    metric: "p95 rss kb",
    candidate: fixed(c.p95_rss_kb),
    baseline: fixed(b.p95_rss_kb),
    threshold: `rise of ${RELEASE_WARN.rssRise * 100}%`,
    warn: rise(c.p95_rss_kb, b.p95_rss_kb, RELEASE_WARN.rssRise),
  });

  return out;
}

const coreOf = (v: string) => v.replace(/^v/, "").split("+")[0].split("-")[0];

/** The newest prerelease of VERSION.txt's release among the versions telemetry has seen. */
export function pickCandidate(versionTxt: string, seenVersions: string[]): string | null {
  const release = coreOf(versionTxt.trim());
  const pre = seenVersions.filter((v) => v.replace(/^v/, "").includes("-") && coreOf(v) === release);
  if (pre.length === 0) return null;
  return pre.sort(compareVersions)[pre.length - 1];
}

/** Fixed-width table of both versions' metrics, a WARN line per verdict that tripped, and a small-sample note. */
export function formatTable(c: ReleaseRow, b: ReleaseRow, verdicts: Verdict[]): string {
  const w = Math.max(6, ...verdicts.map((v) => v.metric.length));
  const cw = Math.max(c.version.length, 9);
  const lines = [
    `${"metric".padEnd(w)}  ${c.version.padEnd(cw)}  ${b.version}`,
    ...verdicts.map((v) => `${v.metric.padEnd(w)}  ${v.candidate.padEnd(cw)}  ${v.baseline}`),
    `${"devices".padEnd(w)}  ${String(c.active_devices).padEnd(cw)}  ${b.active_devices}`,
  ];
  for (const v of verdicts.filter((x) => x.warn)) {
    lines.push(`WARN ${v.metric}: ${c.version} ${v.candidate} vs ${b.version} ${v.baseline} (threshold ${v.threshold})`);
  }
  if (c.active_devices < SMALL_SAMPLE_DEVICES) {
    lines.push(`Note: ${c.version} has only ${c.active_devices} devices in ${RANGE}; read the numbers as a hint.`);
  }
  return lines.join("\n");
}

interface Fixture {
  stable: string;
  versions: ReleaseRow[];
}

function loadFixture(spec: string): Fixture {
  return JSON.parse(existsSync(spec) ? readFileSync(spec, "utf8") : spec) as Fixture;
}

async function getJson(url: string, key?: string): Promise<unknown> {
  const res = await fetch(url, { headers: key ? { "X-API-Key": key } : {} });
  if (!res.ok) throw new Error(`${url} returned ${res.status}`);
  return res.json();
}

async function run(): Promise<string> {
  const root = resolve(dirname(fileURLToPath(import.meta.url)), "../../..");
  const versionTxt = readFileSync(resolve(root, "VERSION.txt"), "utf8").trim();

  let stable: string;
  let rowsFor: (versions: string[] | null) => Promise<ReleaseRow[]>;
  const fixtureSpec = process.env.RELEASE_HEALTH_FIXTURE;
  if (fixtureSpec) {
    const fx = loadFixture(fixtureSpec);
    stable = fx.stable;
    rowsFor = async (versions) => fx.versions.filter((r) => !versions || versions.includes(r.version));
  } else {
    const key = adminKeyFromEnvOrFile();
    if (!key) return "release-health: no admin key (HELIX_TELEMETRY_ADMIN_KEY or .env.telemetry); skipped";
    const base = (process.argv[2] ?? DEFAULT_BASE_URL).replace(/\/$/, "");
    stable = String(((await getJson(STABLE_MANIFEST_URL)) as { version?: string }).version ?? "");
    rowsFor = async (versions) => {
      const qs = `range=${RANGE}${versions ? `&versions=${versions.map(encodeURIComponent).join(",")}` : ""}`;
      return ((await getJson(`${base}/v1/dashboard/releases?${qs}`, key)) as { versions?: ReleaseRow[] }).versions ?? [];
    };
  }
  if (!stable) return "release-health: the stable manifest names no version; skipped";

  const seen = (await rowsFor(null)).map((r) => r.version);
  const candidate = pickCandidate(versionTxt, seen);
  if (!candidate) return `release-health: no ${versionTxt} prerelease in telemetry over ${RANGE}; nothing to compare`;

  const rows = await rowsFor([candidate, stable]);
  const c = rows.find((r) => r.version === candidate);
  const b = rows.find((r) => compareVersions(r.version, stable) === 0);
  if (!c || !b) return `release-health: no telemetry for ${!c ? candidate : stable} over ${RANGE}; nothing to compare`;
  return formatTable(c, b, compareRelease(c, b));
}

// vitest imports this file for the pure functions above; it must not run the check.
if (!process.env.VITEST) {
  run()
    .then((out) => console.log(out))
    .catch((err) => console.log(`release-health: ${err instanceof Error ? err.message : err}; skipped`))
    .finally(() => process.exit(0));
}
