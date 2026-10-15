// SPDX-License-Identifier: GPL-3.0-or-later
// SQL query builders for Analytics Engine dashboard endpoints. Every column is
// named through the schema table (col), never by position.

import { EVENT_FANOUT, POINT_KINDS, col, hasCol } from "./schema";

export interface QueryConfig {
  accountId: string;
  apiToken: string;
}

export interface FilterParams {
  platform?: string;
  version?: string;
  model?: string;
  board?: string;
}

/**
 * Sanitize a filter value for interpolation into a single-quoted SQL literal.
 * Keeps spaces, letters, digits and . - _ ( ) / + (what printer model names
 * and versions contain); drops everything else, including quotes, backslashes
 * and semicolons.
 */
export function sanitizeFilterValue(val: string): string {
  return val.replace(/[^a-zA-Z0-9 .\-_()/+]/g, "");
}

// ---------- Weighted aggregates ----------

// Analytics Engine samples rows under load and records how many rows each
// stored row stands for in _sample_interval. Counts, sums and averages over
// raw rows weight by it, or they undercount whatever was sampled.
const SAMPLE_WEIGHT = "_sample_interval";

/**
 * The weight column a per-device subquery exposes: the sample interval of the
 * device's latest row. It takes its own name because aliasing an aggregate to
 * the column it aggregates is ambiguous in ClickHouse SQL.
 */
const DEVICE_WEIGHT = "device_weight";
const deviceWeight = `argMax(${SAMPLE_WEIGHT}, timestamp) as ${DEVICE_WEIGHT}`;

export function wCount(weight = SAMPLE_WEIGHT): string {
  return `sum(${weight})`;
}

export function wSum(expr: string, weight = SAMPLE_WEIGHT): string {
  return `sum((${expr}) * ${weight})`;
}

export function wAvg(expr: string, weight = SAMPLE_WEIGHT): string {
  return `${wSum(expr, weight)} / ${wCount(weight)}`;
}

export function wCountIf(cond: string, weight = SAMPLE_WEIGHT): string {
  return `sumIf(${weight}, ${cond})`;
}

// ---------- Filters ----------

const FILTER_KEYS = ["platform", "version", "model", "board"] as const;
type FilterKey = (typeof FILTER_KEYS)[number];

/**
 * Update events carry two versions. The one a version filter, or a per-version
 * table, means is the version the device was running when it sent the event.
 */
const RUNNING_VERSION_FIELD: Record<string, string> = {
  update_failed: "from_version",
  update_success: "version",
};

function runningVersionField(kind: string): string {
  return RUNNING_VERSION_FIELD[kind] ?? "app_version";
}

/** The schema field each dashboard filter reads on a given point kind. */
const FILTER_FIELD: Record<FilterKey, (kind: string) => string> = {
  platform: () => "app_platform",
  version: runningVersionField,
  // Only session and hardware_profile report the detected printer model.
  model: () => "printer_model",
  board: () => "app_board",
};

function filterValues(raw?: string): string[] {
  if (!raw) return [];
  return raw
    .split(",")
    .map((v) => sanitizeFilterValue(v.trim()))
    .filter(Boolean);
}

function requireKind(kind: string): void {
  if (!Object.prototype.hasOwnProperty.call(POINT_KINDS, kind)) {
    throw new Error(`unknown point kind: ${kind}`);
  }
}

const EXCLUDED = " AND 0";

/**
 * The filter clause for one point kind: " AND <column> IN (...)" per filter,
 * each on that kind's own column. A kind that lacks a filtered field gets
 * " AND 0": it cannot be matched against a column that means something else.
 */
export function filterFor(kind: string, filters?: FilterParams): string {
  requireKind(kind);
  let clause = "";
  for (const key of FILTER_KEYS) {
    const values = filterValues(filters?.[key]);
    if (values.length === 0) continue;
    const field = FILTER_FIELD[key](kind);
    if (!hasCol(kind, field)) return EXCLUDED;
    clause += ` AND ${col(kind, field)} IN (${values.map((v) => `'${v}'`).join(", ")})`;
  }
  return clause;
}

/**
 * The filter clause for a query over several point kinds, whose columns differ:
 * " AND ((index1 = 'a' AND ...) OR (index1 = 'b' AND ...))". A kind the
 * filters exclude drops out of the OR; when every kind does, " AND 0".
 */
export function filterForKinds(kinds: string[], filters?: FilterParams): string {
  const branches: string[] = [];
  for (const kind of kinds) {
    const f = filterFor(kind, filters);
    if (f !== EXCLUDED) branches.push(`(index1 = '${kind}'${f})`);
  }
  return branches.length === 0 ? EXCLUDED : ` AND (${branches.join(" OR ")})`;
}

/** Every point kind a device event writes, in table order. */
const EVENT_KINDS: string[] = Object.keys(POINT_KINDS).filter((kind) =>
  Object.values(EVENT_FANOUT).some((kinds) => kinds.includes(kind)),
);

/**
 * The one column a field occupies across several kinds, for a query that reads
 * it without knowing which kind a row is. Throws when the kinds disagree.
 */
function sharedCol(kinds: string[], fieldFor: (kind: string) => string): string {
  const cols = new Set(kinds.map((kind) => col(kind, fieldFor(kind))));
  if (cols.size !== 1) {
    throw new Error(`kinds ${kinds.join(", ")} store this field in different columns: ${[...cols].join(", ")}`);
  }
  return [...cols][0];
}

const DEVICE_ID = sharedCol(EVENT_KINDS, () => "device_id");

/**
 * Parse a range string like "7d", "30d", "90d" into a SQL timestamp filter.
 * Returns the number of days, clamped to [1, 90].
 */
export function parseRange(range: string | null): number {
  if (!range) return 30;
  const match = range.match(/^(\d+)d$/);
  if (!match) return 30;
  const days = parseInt(match[1], 10);
  return Math.max(1, Math.min(days, 90));
}

/**
 * Execute a SQL query against the Analytics Engine SQL API.
 */
export async function executeQuery(
  config: QueryConfig,
  sql: string,
): Promise<unknown> {
  const url = `https://api.cloudflare.com/client/v4/accounts/${config.accountId}/analytics_engine/sql`;
  const res = await fetch(url, {
    method: "POST",
    headers: {
      Authorization: `Bearer ${config.apiToken}`,
      "Content-Type": "text/plain",
    },
    body: sql,
  });

  if (!res.ok) {
    const text = await res.text();
    throw new Error(`Analytics Engine SQL API error (${res.status}): ${text}`);
  }

  const result = (await res.json()) as { data?: Array<Record<string, unknown>> };

  // Analytics Engine returns ALL values as strings. Coerce numeric-looking values
  // to numbers so downstream code can safely do arithmetic without concatenation.
  if (result.data) {
    for (const row of result.data) {
      for (const [key, val] of Object.entries(row)) {
        if (typeof val === "string" && val !== "") {
          // Skip date-like (2026-03-24) and version-like (v0.99.0) strings
          if (/^\d{4}-\d{2}-\d{2}/.test(val) || /^v?\d+\.\d+/.test(val)) continue;
          const num = Number(val);
          if (!isNaN(num)) row[key] = num;
        }
      }
    }
  }

  return result;
}

const AE_DATE_RE = /^\d{4}-\d{2}-\d{2}$/;

/** True for a real calendar date written as YYYY-MM-DD. */
export function isIsoDate(date: string): boolean {
  if (!AE_DATE_RE.test(date)) return false;
  const parsed = new Date(`${date}T00:00:00Z`);
  return !isNaN(parsed.getTime()) && parsed.toISOString().slice(0, 10) === date;
}

/**
 * Per-kind row counts for one UTC day of Analytics Engine, for reconciling
 * against R2. `rows` is the raw count() next to `weighted`, the
 * _sample_interval-weighted count: their gap is how much AE sampled away, and
 * that comparison is this query's whole purpose. The date is validated before
 * it reaches the SQL string.
 */
export function aeCountsQuery(date: string): string {
  if (!isIsoDate(date)) {
    throw new Error(`date must be YYYY-MM-DD, got ${JSON.stringify(date)}`);
  }
  return `SELECT
      index1 AS kind,
      count() AS rows,
      sum(_sample_interval) AS weighted,
      count(DISTINCT ${DEVICE_ID}) AS devices
    FROM helixscreen_telemetry
    WHERE toDate(timestamp) = '${date}'
    GROUP BY index1
    ORDER BY index1`;
}

// SQL query builders for each dashboard endpoint

/** Column lookup bound to one point kind. */
const colsOf =
  (kind: string) =>
  (field: string): string =>
    col(kind, field);

const session = colsOf("session");
const printOutcome = colsOf("print_outcome");
const crash = colsOf("crash");
const memSnap = colsOf("memory_snapshot");
const memWarn = colsOf("memory_warning");
const hardware = colsOf("hardware_profile");
const settings = colsOf("settings_snapshot");
const panelUsage = colsOf("panel_usage");
const connection = colsOf("connection_stability");
const errorEnc = colsOf("error_encountered");
const printStart = colsOf("print_start_context");
const perf = colsOf("performance_snapshot");
const features = colsOf("feature_adoption");
const settingsChange = colsOf("settings_change");
const cdnFleet = colsOf("cdn_fleet_daily");

export function overviewQueries(days: number, filters?: FilterParams): string[] {
  const dataset = "helixscreen_telemetry";
  const sessF = filterFor("session", filters);
  const eventsF = filterForKinds(EVENT_KINDS, filters);
  return [
    // Active devices (unique device_ids from sessions)
    `SELECT count(DISTINCT ${session("device_id")}) as active_devices FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'session'${sessF}`,
    // Total events
    `SELECT ${wCount()} as total_events FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY${eventsF}`,
    // Crash rate (crashes / sessions)
    `SELECT
      ${wCountIf("index1 = 'crash'")} as crash_count,
      ${wCountIf("index1 = 'session'")} as session_count
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY${filterForKinds(["crash", "session"], filters)}`,
    // Print success rate
    `SELECT
      ${wCountIf(`${printOutcome("outcome")} = 'success'`)} as successes,
      ${wCount()} as total
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'print_outcome'${filterFor("print_outcome", filters)}`,
    // Events over time
    `SELECT
      toDate(timestamp) as date,
      ${wCount()} as count
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY${eventsF}
    GROUP BY date
    ORDER BY date`,
    // Daily active devices (unique device_ids with sessions per day)
    `SELECT
      toDate(timestamp) as date,
      count(DISTINCT ${session("device_id")}) as devices
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'session'${sessF}
    GROUP BY date
    ORDER BY date`,
    // First-seen date per device (for cumulative growth curve)
    `SELECT
      toDate(min(timestamp)) as first_seen,
      ${wCount()} as new_devices
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'session'${sessF}
    GROUP BY ${session("device_id")}`,
    // CDN fleet: distinct sources polling the update manifest per day.
    //
    // Deliberately unfiltered by platform/version/model/board - this kind is
    // written by the daily cron from zone HTTP analytics and carries none of
    // those dimensions, so applying a filter would blank the metric out
    // whenever a dashboard filter is set.
    //
    // `date` is the UTC day described (the kind has no device_id). max()
    // rather than sum() so a re-run of the same day collapses instead of
    // double-counting. The window is widened by two days because the row's own
    // timestamp is the snapshot time, a day after the day it covers.
    `SELECT
      ${cdnFleet("date")} as date,
      max(${cdnFleet("sources")}) as sources,
      max(${cdnFleet("polls")}) as polls,
      max(${cdnFleet("errors")}) as errors
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days + 2}' DAY
      AND index1 = 'cdn_fleet_daily'
      AND ${cdnFleet("channel")} = 'stable'
    GROUP BY date
    ORDER BY date`,
  ];
}

export function adoptionQueries(days: number, filters?: FilterParams): string[] {
  const dataset = "helixscreen_telemetry";
  const f = filterFor("session", filters);
  const distribution = (field: string) =>
    `SELECT ${session(field)} as name, count(DISTINCT ${session("device_id")}) as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'session' AND ${session(field)} != ''${f} GROUP BY name ORDER BY count DESC`;
  return [
    // Platforms
    distribution("app_platform"),
    // Versions
    distribution("app_version"),
    // Printer models
    distribution("printer_model"),
    // Kinematics
    distribution("kinematics"),
  ];
}

export function printsQueries(days: number, filters?: FilterParams): string[] {
  const dataset = "helixscreen_telemetry";
  const f = filterFor("print_outcome", filters);
  const success = wCountIf(`${printOutcome("outcome")} = 'success'`);
  return [
    // Success rate over time
    `SELECT
      toDate(timestamp) as date,
      ${success} / ${wCount()} as rate,
      ${wCount()} as total
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'print_outcome'${f}
    GROUP BY date
    ORDER BY date`,
    // By filament type
    `SELECT
      ${printOutcome("filament_type")} as type,
      ${success} / ${wCount()} as success_rate,
      ${wCount()} as count
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'print_outcome' AND ${printOutcome("filament_type")} != ''${f}
    GROUP BY type
    ORDER BY count DESC`,
    // Average duration
    `SELECT ${wAvg(printOutcome("duration_sec"))} as avg_duration_sec FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'print_outcome'${f}`,
  ];
}

export function crashesQueries(days: number, filters?: FilterParams): string[] {
  const dataset = "helixscreen_telemetry";
  const f = filterFor("crash", filters);
  const sessF = filterFor("session", filters);
  return [
    // By version (crash count + session count for rate)
    `SELECT
      ${crash("app_version")} as ver,
      ${wCount()} as crash_count
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'crash' AND ${crash("app_version")} != ''${f}
    GROUP BY ver
    ORDER BY crash_count DESC`,
    // Session counts by version (for crash rate denominator)
    `SELECT
      ${session("app_version")} as ver,
      ${wCount()} as session_count
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'session' AND ${session("app_version")} != ''${sessF}
    GROUP BY ver`,
    // By signal
    `SELECT ${crash("signal_name")} as signal, ${wCount()} as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'crash' AND ${crash("signal_name")} != ''${f} GROUP BY signal ORDER BY count DESC`,
    // Average uptime
    `SELECT ${wAvg(crash("uptime_sec"))} as avg_uptime_sec FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'crash'${f}`,
    // Crashes by platform (count + unique crashing devices)
    `SELECT
      ${crash("app_platform")} as platform,
      ${wCount()} as crash_count,
      count(DISTINCT ${crash("device_id")}) as device_count
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'crash' AND ${crash("app_platform")} != ''${f}
    GROUP BY platform
    ORDER BY crash_count DESC`,
    // Sessions by platform (for per-platform rate denominator + device count)
    `SELECT
      ${session("app_platform")} as platform,
      ${wCount()} as session_count,
      count(DISTINCT ${session("device_id")}) as device_count
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'session' AND ${session("app_platform")} != ''${sessF}
    GROUP BY platform
    ORDER BY session_count DESC`,
    // Trailing 14-day rate: overall crashes (all platforms, including empty).
    // NOTE: the per-platform queries below skip an empty platform, so the sum
    // of per-platform counts may be slightly less than these overall totals if
    // any events arrive with unset platform. In practice our client always sets
    // app_platform, so the gap is zero, but the overall totals are deliberately
    // un-filtered for truthfulness on the gauge.
    `SELECT ${wCount()} as crash_count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '14' DAY AND index1 = 'crash'${f}`,
    // Trailing 14-day rate: overall sessions
    `SELECT ${wCount()} as session_count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '14' DAY AND index1 = 'session'${sessF}`,
    // Trailing 14-day rate: per-platform crashes
    `SELECT ${crash("app_platform")} as platform, ${wCount()} as crash_count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '14' DAY AND index1 = 'crash' AND ${crash("app_platform")} != ''${f} GROUP BY platform`,
    // Trailing 14-day rate: per-platform sessions
    `SELECT ${session("app_platform")} as platform, ${wCount()} as session_count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '14' DAY AND index1 = 'session' AND ${session("app_platform")} != ''${sessF} GROUP BY platform`,
  ];
}

export function crashListQuery(days: number, limit: number, filters?: FilterParams): string {
  const dataset = "helixscreen_telemetry";
  return `SELECT
    timestamp,
    ${crash("device_id")} as device_id,
    ${crash("app_version")} as ver,
    ${crash("signal_name")} as sig,
    ${crash("app_platform")} as platform,
    ${crash("uptime_sec")} as uptime_sec
  FROM ${dataset}
  WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'crash'${filterFor("crash", filters)}
  ORDER BY timestamp DESC
  LIMIT ${limit}`;
}

export function memoryQueries(days: number, filters?: FilterParams): string[] {
  const dataset = "helixscreen_telemetry";
  const f = filterFor("memory_snapshot", filters);
  const rss = memSnap("rss_kb");
  return [
    // RSS over time (avg/p95/max by date)
    `SELECT
      toDate(timestamp) as date,
      ${wAvg(rss)} as avg_rss_kb,
      quantileExactWeighted(0.95)(${rss}, _sample_interval) as p95_rss_kb,
      max(${rss}) as max_rss_kb
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'memory_snapshot'${f}
    GROUP BY date
    ORDER BY date`,
    // RSS by platform
    `SELECT
      ${memSnap("app_platform")} as platform,
      ${wAvg(rss)} as avg_rss_kb
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'memory_snapshot' AND ${memSnap("app_platform")} != ''${f}
    GROUP BY platform
    ORDER BY avg_rss_kb DESC`,
    // VM peak trend (avg by date)
    `SELECT
      toDate(timestamp) as date,
      ${wAvg(memSnap("vm_peak_kb"))} as avg_vm_peak_kb
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'memory_snapshot'${f}
    GROUP BY date
    ORDER BY date`,
  ];
}

export function memoryWarningQueries(days: number, filters?: FilterParams): string[] {
  const dataset = "helixscreen_telemetry";
  const f = filterFor("memory_warning", filters);
  const level = memWarn("level");
  const rss = memWarn("rss_kb");
  return [
    // Warning count by level
    `SELECT
      ${level} as level,
      ${wCount()} as count
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'memory_warning' AND ${level} != ''${f}
    GROUP BY level
    ORDER BY count DESC`,
    // Warnings over time (count by date, split by level)
    `SELECT
      toDate(timestamp) as date,
      ${level} as level,
      ${wCount()} as count
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'memory_warning'${f}
    GROUP BY date, level
    ORDER BY date`,
    // RSS at warning time (avg/max by date)
    `SELECT
      toDate(timestamp) as date,
      ${wAvg(rss)} as avg_rss_kb,
      max(${rss}) as max_rss_kb
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'memory_warning'${f}
    GROUP BY date
    ORDER BY date`,
    // Warnings by platform
    `SELECT
      ${memWarn("app_platform")} as platform,
      ${wCount()} as count,
      ${wAvg(rss)} as avg_rss_kb
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'memory_warning' AND ${memWarn("app_platform")} != ''${f}
    GROUP BY platform
    ORDER BY count DESC`,
    // Affected devices count
    `SELECT count(DISTINCT ${memWarn("device_id")}) as affected_devices FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'memory_warning'${f}`,
    // Recent warnings list (newest first)
    `SELECT
      timestamp,
      ${memWarn("device_id")} as device_id,
      ${memWarn("app_version")} as version,
      ${memWarn("app_platform")} as platform,
      ${level} as level,
      ${memWarn("reason")} as reason,
      ${memWarn("uptime_sec")} as uptime_sec,
      ${rss} as rss_kb,
      ${memWarn("system_available_mb")} as system_available_mb,
      ${memWarn("growth_5min_kb")} as growth_5min_kb,
      ${memWarn("private_dirty_kb")} as private_dirty_kb,
      ${memWarn("pss_kb")} as pss_kb
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'memory_warning'${f}
    ORDER BY timestamp DESC
    LIMIT 100`,
  ];
}

export function hardwareQueries(days: number, filters?: FilterParams): string[] {
  const dataset = "helixscreen_telemetry";
  const f = filterFor("hardware_profile", filters);
  const dev = hardware("device_id");
  // Distinct devices per value of one hardware_profile field, empty excluded.
  const distribution = (field: string, limit = "") =>
    `SELECT ${hardware(field)} as name, count(DISTINCT ${dev}) as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'hardware_profile' AND ${hardware(field)} != ''${f} GROUP BY name ORDER BY count DESC${limit}`;
  const capBits = Array.from(
    { length: 13 },
    (_, i) => `      ${wSum(`toUInt32(bitAnd(toUInt32(caps), ${1 << i}) > 0)`, DEVICE_WEIGHT)} as cap_${i},`,
  ).join("\n");
  const fans = hardware("fan_count");
  const sensors = hardware("sensor_count");
  const macros = hardware("macro_count");
  return [
    // Printer models (top 20, deduplicated by device_id)
    distribution("printer_model", " LIMIT 20"),
    // Kinematics (deduplicated by device_id)
    distribution("kinematics"),
    // MCU chips (deduplicated by device_id)
    distribution("mcu_chip", " LIMIT 20"),
    // Capabilities bitmask — deduplicate: subquery gets latest row per device, outer query aggregates
    `SELECT
${capBits}
      ${wCount(DEVICE_WEIGHT)} as total
    FROM (
      SELECT ${dev}, argMax(${hardware("capabilities_bitmask")}, timestamp) as caps, ${deviceWeight}
      FROM ${dataset}
      WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'hardware_profile'${f}
      GROUP BY ${dev}
    )`,
    // Build volume averages (deduplicated: latest per device)
    `SELECT
      ${wAvg("vol_x", DEVICE_WEIGHT)} as avg_vol_x,
      ${wAvg("vol_y", DEVICE_WEIGHT)} as avg_vol_y,
      ${wAvg("vol_z", DEVICE_WEIGHT)} as avg_vol_z
    FROM (
      SELECT ${dev}, argMax(${hardware("build_vol_x")}, timestamp) as vol_x, argMax(${hardware("build_vol_y")}, timestamp) as vol_y, argMax(${hardware("build_vol_z")}, timestamp) as vol_z, ${deviceWeight}
      FROM ${dataset}
      WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'hardware_profile' AND ${hardware("build_vol_x")} > 0${f}
      GROUP BY ${dev}
    )`,
    // Fan/sensor/macro count averages (deduplicated: latest per device, filter out rows where all three are zero — old mis-mapped data)
    `SELECT
      ${wAvg("fans", DEVICE_WEIGHT)} as avg_fan_count,
      ${wAvg("sensors", DEVICE_WEIGHT)} as avg_sensor_count,
      ${wAvg("macros", DEVICE_WEIGHT)} as avg_macro_count
    FROM (
      SELECT ${dev}, argMax(${fans}, timestamp) as fans, argMax(${sensors}, timestamp) as sensors, argMax(${macros}, timestamp) as macros, ${deviceWeight}
      FROM ${dataset}
      WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'hardware_profile' AND (${fans} > 0 OR ${sensors} > 0 OR ${macros} > 0)${f}
      GROUP BY ${dev}
    )`,
    // Host RAM distribution (from session events, deduplicated by device_id)
    `SELECT
      ram_mb,
      ${wCount(DEVICE_WEIGHT)} as count
    FROM (
      SELECT ${session("device_id")}, argMax(${session("ram_total_mb")}, timestamp) as ram_mb, ${deviceWeight}
      FROM ${dataset}
      WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'session' AND ${session("ram_total_mb")} > 0${filterFor("session", filters)}
      GROUP BY ${session("device_id")}
    )
    GROUP BY ram_mb
    ORDER BY ram_mb`,
    // AMS backend distribution (deduplicated by device_id)
    distribution("ams_type"),
    // helix_macros.cfg adoption: "1" installed, "0" not. Empty is excluded, so
    // the denominator is devices that actually reported, not all devices.
    distribution("has_helix_macros"),
    // Moonraker topology: "1" on this machine, "0" over the network. Only
    // clients new enough to report it are counted (empty is excluded).
    distribution("moonraker_is_local"),
  ];
}

export function engagementQueries(days: number, filters?: FilterParams): string[] {
  const dataset = "helixscreen_telemetry";
  const panel = panelUsage("panel_name");
  const widgetInteraction = colsOf("widget_interaction");
  const overlay = colsOf("overlay_visit");
  const widgetPlacement = colsOf("widget_placement");
  const panelF = filterFor("panel_usage", filters);
  const settingsF = filterFor("settings_snapshot", filters);
  // Distinct devices per value of one settings_snapshot field, empty excluded.
  const settingsDistribution = (field: string) =>
    `SELECT ${settings(field)} as name, count(DISTINCT ${settings("device_id")}) as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'settings_snapshot' AND ${settings(field)} != ''${settingsF} GROUP BY name ORDER BY count DESC`;
  const brightness = settings("brightness_pct");
  return [
    // Panel time (exclude "home")
    `SELECT ${panel} as panel, ${wSum(panelUsage("time_sec"))} as total_time_sec FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'panel_usage' AND ${panel} != '' AND ${panel} != 'home'${panelF} GROUP BY panel ORDER BY total_time_sec DESC`,
    // Panel visits (exclude "home")
    `SELECT ${panel} as panel, ${wSum(panelUsage("visits"))} as total_visits FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'panel_usage' AND ${panel} != '' AND ${panel} != 'home'${panelF} GROUP BY panel ORDER BY total_visits DESC`,
    // Session duration trend (avg by date)
    `SELECT
      toDate(timestamp) as date,
      ${wAvg(panelUsage("session_duration_sec"))} as avg_session_sec
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'panel_usage'${panelF}
    GROUP BY date
    ORDER BY date`,
    // Theme distribution (deduplicated by device_id)
    settingsDistribution("theme_name"),
    // Locale distribution (deduplicated by device_id)
    settingsDistribution("locale"),
    // Brightness quantiles
    `SELECT
      quantileExactWeighted(0.25)(${brightness}, _sample_interval) as p25,
      quantileExactWeighted(0.5)(${brightness}, _sample_interval) as p50,
      quantileExactWeighted(0.75)(${brightness}, _sample_interval) as p75
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'settings_snapshot'${settingsF}`,
    // Dark vs Light mode distribution (deduplicated by device_id)
    settingsDistribution("theme"),
    // Widget placement (unique per device)
    `SELECT ${widgetPlacement("widget_id")} as widget, count(DISTINCT ${widgetPlacement("device_id")}) as devices FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'widget_placement' AND ${widgetPlacement("widget_id")} != ''${filterFor("widget_placement", filters)} GROUP BY widget ORDER BY devices DESC`,
    // Widget interactions
    `SELECT ${widgetInteraction("widget_id")} as widget, ${wSum(widgetInteraction("count"))} as interactions FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'widget_interaction' AND ${widgetInteraction("widget_id")} != ''${filterFor("widget_interaction", filters)} GROUP BY widget ORDER BY interactions DESC`,
    // Overlay visits
    `SELECT ${overlay("overlay_name")} as overlay, ${wSum(overlay("count"))} as total_visits FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'overlay_visit' AND ${overlay("overlay_name")} != ''${filterFor("overlay_visit", filters)} GROUP BY overlay ORDER BY total_visits DESC`,
  ];
}

export function reliabilityQueries(days: number, filters?: FilterParams): string[] {
  const dataset = "helixscreen_telemetry";
  const connF = filterFor("connection_stability", filters);
  const errF = filterFor("error_encountered", filters);
  const connected = connection("total_connected_sec");
  const disconnected = connection("total_disconnected_sec");
  const category = errorEnc("category");
  return [
    // Uptime % (avg connected/(connected+disconnected) by date)
    `SELECT
      toDate(timestamp) as date,
      ${wAvg(`${connected} / (${connected} + ${disconnected} + 0.001)`)} as avg_uptime_pct
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'connection_stability'${connF}
    GROUP BY date
    ORDER BY date`,
    // Disconnect frequency (avg by date)
    `SELECT
      toDate(timestamp) as date,
      ${wAvg(connection("disconnect_count"))} as avg_disconnects
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'connection_stability'${connF}
    GROUP BY date
    ORDER BY date`,
    // Longest outage
    `SELECT max(${connection("longest_disconnect_sec")}) as max_disconnect_sec FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'connection_stability'${connF}`,
    // Top error categories
    `SELECT ${category} as category, ${wCount()} as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'error_encountered' AND ${category} != ''${errF} GROUP BY category ORDER BY count DESC LIMIT 20`,
    // Error codes
    `SELECT ${category} as category, ${errorEnc("code")} as code, ${wCount()} as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'error_encountered' AND ${category} != ''${errF} GROUP BY category, code ORDER BY count DESC LIMIT 50`,
  ];
}

export function stabilityQueries(days: number, filters?: FilterParams): string[] {
  const dataset = "helixscreen_telemetry";
  const crashF = filterFor("crash", filters);
  const sessF = filterFor("session", filters);
  const connF = filterFor("connection_stability", filters);
  const errF = filterFor("error_encountered", filters);
  const category = errorEnc("category");
  const code = errorEnc("code");
  const display = `index1 = 'error_encountered' AND ${category} = 'display'`;
  return [
    // 0: Crash count over time
    `SELECT toDate(timestamp) as date, ${wCount()} as crash_count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'crash'${crashF} GROUP BY date ORDER BY date`,
    // 1: Session count over time (for rate denominator)
    `SELECT toDate(timestamp) as date, ${wCount()} as session_count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'session'${sessF} GROUP BY date ORDER BY date`,
    // 2: Crash count by version
    `SELECT ${crash("app_version")} as ver, ${wCount()} as crash_count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'crash' AND ${crash("app_version")} != ''${crashF} GROUP BY ver ORDER BY crash_count DESC`,
    // 3: Session count by version
    `SELECT ${session("app_version")} as ver, ${wCount()} as session_count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'session' AND ${session("app_version")} != ''${sessF} GROUP BY ver`,
    // 4: Crashes by signal
    `SELECT ${crash("signal_name")} as signal, ${wCount()} as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'crash' AND ${crash("signal_name")} != ''${crashF} GROUP BY signal ORDER BY count DESC`,
    // 5: Average uptime before crash
    `SELECT ${wAvg(crash("uptime_sec"))} as avg_uptime_sec FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'crash'${crashF}`,
    // 6: Klippy errors and shutdowns over time
    `SELECT toDate(timestamp) as date, ${wSum(connection("klippy_error_count"))} as errors, ${wSum(connection("klippy_shutdown_count"))} as shutdowns FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'connection_stability'${connF} GROUP BY date ORDER BY date`,
    // 7: Memory warning count over time
    `SELECT toDate(timestamp) as date, ${wCount()} as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'memory_warning'${filterFor("memory_warning", filters)} GROUP BY date ORDER BY date`,
    // 8: Error hotspots (categories)
    `SELECT ${category} as category, ${wCount()} as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'error_encountered' AND ${category} != ''${errF} GROUP BY category ORDER BY count DESC LIMIT 20`,
    // 9: Error codes detail
    `SELECT ${category} as category, ${code} as code, ${wCount()} as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'error_encountered' AND ${category} != ''${errF} GROUP BY category, code ORDER BY count DESC LIMIT 50`,
    // 10: Display anomaly count over time (category='display')
    `SELECT toDate(timestamp) as date, ${wCount()} as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND ${display}${errF} GROUP BY date ORDER BY date`,
    // 11: Display anomaly codes breakdown
    `SELECT ${code} as code, ${wCount()} as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND ${display}${errF} GROUP BY code ORDER BY count DESC LIMIT 20`,
    // 12: Display anomaly by version
    `SELECT ${errorEnc("app_version")} as version, ${wCount()} as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND ${display} AND ${errorEnc("app_version")} != ''${errF} GROUP BY version ORDER BY count DESC LIMIT 20`,
    // 13: Recent display anomalies with context (for backtrace inspection)
    `SELECT timestamp, ${errorEnc("device_id")} as device_id, ${errorEnc("app_version")} as version, ${errorEnc("app_platform")} as platform, ${code} as code, ${errorEnc("context")} as context, ${errorEnc("uptime_sec")} as uptime_sec FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND ${display}${errF} ORDER BY timestamp DESC LIMIT 50`,
    // 14: Display anomaly affected devices count
    `SELECT count(DISTINCT ${errorEnc("device_id")}) as affected_devices FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND ${display}${errF}`,
  ];
}

export function printStartQueries(days: number, filters?: FilterParams): string[] {
  const dataset = "helixscreen_telemetry";
  const f = filterFor("print_start_context", filters);
  // Weighted row count per value of one print_start_context field, empty excluded.
  const distribution = (field: string) =>
    `SELECT ${printStart(field)} as name, ${wCount()} as count FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'print_start_context' AND ${printStart(field)} != ''${f} GROUP BY name ORDER BY count DESC`;
  return [
    // Slicer distribution
    distribution("slicer"),
    // File size buckets
    distribution("file_size_bucket"),
    // Thumbnail adoption (0-1 flag as a percentage)
    `SELECT ${wAvg(printStart("has_thumbnail"))} * 100 as thumbnail_rate FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'print_start_context'${f}`,
    // AMS usage (0-1 flag as a percentage)
    `SELECT ${wAvg(printStart("ams_active"))} * 100 as ams_rate FROM ${dataset} WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'print_start_context'${f}`,
    // Source distribution
    distribution("source"),
  ];
}

export function performanceQueries(days: number, filters?: FilterParams): string[] {
  const dataset = "helixscreen_telemetry";
  const f = filterFor("performance_snapshot", filters);
  const dropped = perf("dropped_frame_count");
  const total = perf("total_frame_count");
  const dev = perf("device_id");
  return [
    // 0: Frame time percentiles over time
    `SELECT
      toDate(timestamp) as date,
      ${wAvg(perf("frame_time_p50_ms"))} as avg_p50,
      ${wAvg(perf("frame_time_p95_ms"))} as avg_p95,
      ${wAvg(perf("frame_time_p99_ms"))} as avg_p99
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'performance_snapshot'${f}
    GROUP BY date
    ORDER BY date`,
    // 1: Drop rate by platform
    `SELECT
      ${perf("app_platform")} as platform,
      ${wSum(dropped)} as dropped,
      ${wSum(total)} as total
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'performance_snapshot' AND ${perf("app_platform")} != ''${f}
    GROUP BY platform
    ORDER BY dropped DESC`,
    // 2: Drop rate by version over time
    `SELECT
      toDate(timestamp) as date,
      ${perf("app_version")} as version,
      ${wSum(dropped)} as dropped,
      ${wSum(total)} as total
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'performance_snapshot' AND ${perf("app_version")} != ''${f}
    GROUP BY date, version
    ORDER BY date`,
    // 3: Worst panels
    `SELECT
      ${perf("worst_panel")} as panel,
      ${wCount()} as times_worst,
      ${wAvg(perf("worst_panel_p95_ms"))} as avg_p95_ms
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'performance_snapshot' AND ${perf("worst_panel")} != ''${f}
    GROUP BY panel
    ORDER BY times_worst DESC
    LIMIT 20`,
    // 4: Fleet-wide metrics
    `SELECT
      ${wAvg(perf("frame_time_p50_ms"))} as fleet_p50,
      ${wSum(dropped)} as total_dropped,
      ${wSum(total)} as total_frames,
      count(DISTINCT ${dev}) as total_devices
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'performance_snapshot'${f}`,
    // 5: Devices with high drop rate (>5%)
    `SELECT ${wCount(DEVICE_WEIGHT)} as high_drop_devices
    FROM (
      SELECT ${dev}, ${wSum(dropped)} as dropped, ${wSum(total)} as total, ${deviceWeight}
      FROM ${dataset}
      WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'performance_snapshot'${f}
      GROUP BY ${dev}
      HAVING total > 0 AND dropped / total > 0.05
    )`,
  ];
}

export function featuresQueries(days: number, filters?: FilterParams): string[] {
  const dataset = "helixscreen_telemetry";
  const f = filterFor("feature_adoption", filters);
  const dev = features("device_id");
  // [response alias, feature_adoption field]
  const FEATURES: Array<[string, string]> = [
    ["macros", "macros"],
    ["camera", "camera"],
    ["bed_mesh", "bed_mesh"],
    ["console_gcode", "console_gcode"],
    ["input_shaper", "input_shaper"],
    ["filament_management", "filament_management"],
    ["manual_probe", "manual_probe"],
    ["extra_bitmask_avg", "feature_bitmask"],
  ];
  const latest = FEATURES.map(([alias, field]) => `argMax(${features(field)}, timestamp) as latest_${alias}`).join(", ");
  const latestAvgs = FEATURES.map(([alias]) => `${wAvg(`latest_${alias}`, DEVICE_WEIGHT)} as ${alias}`).join(", ");
  const rowAvgs = FEATURES.map(([alias, field]) => `${wAvg(features(field))} as ${alias}`).join(", ");
  return [
    // 0: Per-feature adoption rate (deduplicated by device)
    `SELECT
      ${latestAvgs},
      ${wCount(DEVICE_WEIGHT)} as total_devices
    FROM (
      SELECT ${dev}, ${latest}, ${deviceWeight}
      FROM ${dataset}
      WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'feature_adoption'${f}
      GROUP BY ${dev}
    )`,
    // 1: Feature adoption by version
    `SELECT
      ${features("app_version")} as version,
      ${rowAvgs},
      count(DISTINCT ${dev}) as devices
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'feature_adoption' AND ${features("app_version")} != ''${f}
    GROUP BY version
    ORDER BY version`,
    // 2: Total tracked devices
    `SELECT count(DISTINCT ${dev}) as total_devices
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'feature_adoption'${f}`,
  ];
}

export function uxInsightsQueries(days: number, filters?: FilterParams): string[] {
  const dataset = "helixscreen_telemetry";
  const panelF = filterFor("panel_usage", filters);
  const changeF = filterFor("settings_change", filters);
  const panel = panelUsage("panel_name");
  const setting = settingsChange("setting");
  return [
    // 0: Panel time distribution
    `SELECT ${panel} as panel, ${wSum(panelUsage("time_sec"))} as total_time_sec
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'panel_usage' AND ${panel} != '' AND ${panel} != 'home'${panelF}
    GROUP BY panel
    ORDER BY total_time_sec DESC`,
    // 1: Panel visit frequency
    `SELECT ${panel} as panel, ${wSum(panelUsage("visits"))} as total_visits, count(DISTINCT ${panelUsage("device_id")}) as devices
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'panel_usage' AND ${panel} != '' AND ${panel} != 'home'${panelF}
    GROUP BY panel
    ORDER BY total_visits DESC`,
    // 2: Settings change frequency
    `SELECT ${setting} as setting, ${wCount()} as change_count
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'settings_change' AND ${setting} != ''${changeF}
    GROUP BY setting
    ORDER BY change_count DESC`,
    // 3: Settings defaults (devices that changed each setting)
    `SELECT ${setting} as setting, count(DISTINCT ${settingsChange("device_id")}) as devices_changed
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'settings_change' AND ${setting} != ''${changeF}
    GROUP BY setting
    ORDER BY devices_changed DESC`,
    // 4: Total devices for normalization
    `SELECT count(DISTINCT ${session("device_id")}) as total_devices
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'session'${filterFor("session", filters)}`,
    // 5: Avg session duration
    `SELECT ${wAvg(panelUsage("session_duration_sec"))} as avg_session_sec
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'panel_usage'${panelF}`,
    // 6: Settings change rate
    `SELECT ${wCount()} as total_changes, count(DISTINCT ${settingsChange("device_id")}) as devices
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'settings_change'${changeF}`,
  ];
}

const RELEASE_KINDS = ["session", "crash", "update_failed", "update_success"];

export function releasesQueries(versions: string[]): string[] {
  const dataset = "helixscreen_telemetry";
  // Strip a 'v' prefix for matching; a version that sanitizes to nothing is dropped.
  const clean = versions.map((v) => sanitizeFilterValue(v.trim().replace(/^v/, ""))).filter(Boolean);
  const inVersions = (column: string) =>
    clean.length > 0 ? `${column} IN (${clean.map((v) => `'${v}'`).join(", ")})` : "0";
  // Per-version stats group every kind by the version the device was RUNNING:
  // for update_failed that is from_version, because keying on the target
  // would bucket every failure under the newest release and hide which
  // deployed versions are broken (prestonbrown/helixscreen#993).
  const running = sharedCol(RELEASE_KINDS, runningVersionField);
  const kindList = RELEASE_KINDS.map((k) => `'${k}'`).join(", ");
  return [
    // Per-version stats: sessions, crashes, self-update outcomes.
    `SELECT
      ${running} as ver,
      ${wCountIf("index1 = 'session'")} as total_sessions,
      ${wCountIf("index1 = 'crash'")} as total_crashes,
      ${wCountIf("index1 = 'update_failed'")} as update_failures,
      ${wCountIf("index1 = 'update_success'")} as update_successes
    FROM ${dataset}
    WHERE index1 IN (${kindList}) AND ${inVersions(running)}
    GROUP BY ver`,
    // Per-version active devices (count distinct requires separate query)
    `SELECT
      ${session("app_version")} as ver,
      count(DISTINCT ${session("device_id")}) as active_devices
    FROM ${dataset}
    WHERE index1 = 'session' AND ${inVersions(session("app_version"))}
    GROUP BY ver`,
    // Per-version print stats
    `SELECT
      ${printOutcome("app_version")} as ver,
      ${wCountIf(`${printOutcome("outcome")} = 'success'`)} as print_successes,
      ${wCount()} as print_total
    FROM ${dataset}
    WHERE index1 = 'print_outcome' AND ${inVersions(printOutcome("app_version"))}
    GROUP BY ver`,
  ];
}
