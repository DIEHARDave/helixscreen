// SPDX-License-Identifier: GPL-3.0-or-later
// Health tab payload types and the row folding that builds them. SQL lives in
// queries.ts. Imports only versions and schema so the dashboard can import
// these types without Worker runtime types.

import type { RawRow } from "./schema";
import { compareVersions } from "./versions";

export interface ReleaseWindow {
  devices: number;
  crash_free: number | null;
  crash_rate: number | null;
  update_failure_rate: number | null;
  print_success: number | null;
}

export interface HealthPayload {
  fleet: { actives_cur: number; actives_prev: number; cdn_sources: number | null; cdn_date: string | null };
  releases: { version: string; cur: ReleaseWindow; prev: ReleaseWindow }[];
  errors: { category: string; code: string; cur: number; prev: number; devices: number; status: "new" | "rising" | "steady" }[];
  crash_clusters: { signal: string; fault_code: string; version: string; board: string; cur: number; prev: number; devices: number }[];
  data_health: { kind: string; latest: number; median: number; low: boolean }[];
}

/** The query keys buildHealth reads; the Worker's healthQueries returns one SQL string per key. */
export type HealthRowKey =
  | "actives_cur" | "actives_prev" | "top_versions"
  | "release_cur" | "release_prev"
  | "release_devices_cur" | "release_devices_prev"
  | "prints_cur" | "prints_prev"
  | "errors_cur" | "errors_prev" | "errors_baseline"
  | "crashes_cur" | "crashes_prev"
  | "cdn_latest" | "kind_days";

/** An error rises when this week is at least this multiple of last week and at least RISING_MIN_COUNT. */
const RISING_FACTOR = 2;
const RISING_MIN_COUNT = 5;

/** A kind is low when its latest complete day is under this share of its median. */
const LOW_SHARE = 0.5;

/** Complete days compared against: the latest plus up to this many before it. */
const MEDIAN_DAYS = 7;

/** Kinds written by cron rather than by devices; their daily count says nothing about ingest. */
const CRON_KINDS = new Set(["cdn_fleet_daily"]);

const num = (v: unknown): number => Number(v ?? 0) || 0;
const str = (v: unknown): string => String(v ?? "");
const ratio = (n: number, d: number): number | null => (d > 0 ? n / d : null);

function median(values: number[]): number {
  const s = [...values].sort((a, b) => a - b);
  const mid = Math.floor(s.length / 2);
  return s.length === 0 ? 0 : s.length % 2 ? s[mid] : (s[mid - 1] + s[mid]) / 2;
}

function releaseWindow(
  ver: string,
  stats: RawRow[],
  devices: RawRow[],
  prints: RawRow[],
): ReleaseWindow {
  const st = stats.find((r) => str(r.ver) === ver);
  const pr = prints.find((r) => str(r.ver) === ver);
  const sessionDevices = num(devices.find((r) => str(r.ver) === ver && str(r.event) === "session")?.devices);
  const crashDevices = num(devices.find((r) => str(r.ver) === ver && str(r.event) === "crash")?.devices);
  const failures = num(st?.update_failures);
  const successes = num(st?.update_successes);
  return {
    devices: sessionDevices,
    crash_free: sessionDevices > 0 ? Math.max(0, 1 - crashDevices / sessionDevices) : null,
    crash_rate: ratio(num(st?.crashes), num(st?.sessions)),
    update_failure_rate: ratio(failures, failures + successes),
    print_success: ratio(num(pr?.successes), num(pr?.total)),
  };
}

const STATUS_ORDER = { new: 0, rising: 1, steady: 2 } as const;

/**
 * Folds the health query rows into the payload. `today` is the UTC day
 * (YYYY-MM-DD) whose partial row the data-health block ignores.
 */
export function buildHealth(rows: Record<HealthRowKey, unknown[]>, today: string): HealthPayload {
  const r = (k: HealthRowKey): RawRow[] => (rows[k] ?? []) as RawRow[];

  const releases = r("top_versions")
    .map((t) => str(t.ver))
    .filter(Boolean)
    .sort((a, b) => compareVersions(b, a))
    .map((version) => ({
      version,
      cur: releaseWindow(version, r("release_cur"), r("release_devices_cur"), r("prints_cur")),
      prev: releaseWindow(version, r("release_prev"), r("release_devices_prev"), r("prints_prev")),
    }));

  const errKey = (x: RawRow) => `${str(x.category)}\u0000${str(x.code)}`;
  const errPrev = new Map(r("errors_prev").map((x) => [errKey(x), num(x.count)]));
  const errBaseline = new Set(r("errors_baseline").map(errKey));
  const errors = r("errors_cur")
    .filter((x) => num(x.count) > 0)
    .map((x) => {
      const cur = num(x.count);
      const prev = errPrev.get(errKey(x)) ?? 0;
      const status: HealthPayload["errors"][number]["status"] = !errBaseline.has(errKey(x))
        ? "new"
        : cur >= RISING_FACTOR * prev && cur >= RISING_MIN_COUNT
          ? "rising"
          : "steady";
      return { category: str(x.category), code: str(x.code), cur, prev, devices: num(x.devices), status };
    })
    .sort((a, b) => STATUS_ORDER[a.status] - STATUS_ORDER[b.status] || b.cur - a.cur);

  const clusterKey = (x: RawRow) => [x.sig, x.fault, x.ver, x.board].map(str).join("\u0000");
  const crashPrev = new Map(r("crashes_prev").map((x) => [clusterKey(x), num(x.count)]));
  const crash_clusters = r("crashes_cur")
    .filter((x) => num(x.count) > 0)
    .map((x) => ({
      signal: str(x.sig),
      fault_code: str(x.fault),
      version: str(x.ver),
      board: str(x.board),
      cur: num(x.count),
      prev: crashPrev.get(clusterKey(x)) ?? 0,
      devices: num(x.devices),
    }))
    .sort((a, b) => b.cur - a.cur);

  const daysByKind = new Map<string, { day: string; count: number }[]>();
  for (const x of r("kind_days")) {
    const kind = str(x.event);
    const day = str(x.day).slice(0, 10);
    if (CRON_KINDS.has(kind) || day === today) continue;
    const list = daysByKind.get(kind) ?? [];
    list.push({ day, count: num(x.count) });
    daysByKind.set(kind, list);
  }
  const data_health = [...daysByKind]
    .map(([kind, days]) => {
      days.sort((a, b) => (a.day < b.day ? 1 : a.day > b.day ? -1 : 0));
      const latest = days[0].count;
      const med = median(days.slice(1, 1 + MEDIAN_DAYS).map((d) => d.count));
      return { kind, latest, median: med, low: med > 0 && latest < med * LOW_SHARE };
    })
    .sort((a, b) => a.kind.localeCompare(b.kind));

  const cdn = r("cdn_latest")[0];
  return {
    fleet: {
      actives_cur: num(r("actives_cur")[0]?.actives),
      actives_prev: num(r("actives_prev")[0]?.actives),
      cdn_sources: cdn ? num(cdn.sources) : null,
      cdn_date: cdn ? str(cdn.date).slice(0, 10) : null,
    },
    releases,
    errors,
    crash_clusters,
    data_health,
  };
}
