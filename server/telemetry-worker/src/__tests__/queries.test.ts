// SPDX-License-Identifier: GPL-3.0-or-later
import { describe, it, expect } from "vitest";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import * as queries from "../queries";
import {
  DELTA_KINDS,
  deltaSource,
  engagementQueries,
  excludedBy,
  featuresQueries,
  filterNotApplicable,
  filterFor,
  filterForKinds,
  adoptionQueries,
  releasesQueries,
  overviewQueries,
  SQL_FALSE,
  wAvg,
  wCount,
  wCountIf,
  wSum,
  type FilterParams,
} from "../queries";
import { col } from "../schema";

describe("filterFor", () => {
  it("resolves platform to the kind's own app_platform column", () => {
    const clause = filterFor("print_outcome", { platform: "pi" });
    expect(clause).toBe(` AND ${col("print_outcome", "app_platform")} IN ('pi')`);
    expect(clause).not.toContain(`${col("print_outcome", "filament_type")} `);
  });

  it("excludes a kind that lacks the filtered field", () => {
    expect(filterFor("memory_warning", { model: "Voron 2.4" })).toBe(` AND ${SQL_FALSE}`);
    expect(filterFor("memory_snapshot", { board: "k1" })).toBe(` AND ${SQL_FALSE}`);
  });

  it("resolves model to the detected printer model on session and hardware_profile", () => {
    expect(filterFor("session", { model: "Voron 2.4" })).toBe(
      ` AND ${col("session", "printer_model")} IN ('Voron 2.4')`,
    );
    expect(filterFor("hardware_profile", { model: "Voron 2.4" })).toBe(
      ` AND ${col("hardware_profile", "printer_model")} IN ('Voron 2.4')`,
    );
  });

  it("resolves board to app_board", () => {
    expect(filterFor("crash", { board: "k1" })).toBe(` AND ${col("crash", "app_board")} IN ('k1')`);
  });

  it("filters update events on the version the device was running", () => {
    expect(filterFor("update_failed", { version: "1.0.0" })).toBe(
      ` AND ${col("update_failed", "from_version")} IN ('1.0.0')`,
    );
    expect(filterFor("update_success", { version: "1.0.0" })).toBe(
      ` AND ${col("update_success", "version")} IN ('1.0.0')`,
    );
    expect(filterFor("crash", { version: "1.0.0" })).toBe(` AND ${col("crash", "app_version")} IN ('1.0.0')`);
  });

  it("lists comma-separated values and joins several filters", () => {
    expect(filterFor("session", { platform: "pi, k1", version: "1.0.0" })).toBe(
      ` AND ${col("session", "app_platform")} IN ('pi', 'k1')` +
        ` AND ${col("session", "app_version")} IN ('1.0.0')`,
    );
  });

  it("is empty with no filters, or only empty values", () => {
    expect(filterFor("session")).toBe("");
    expect(filterFor("session", {})).toBe("");
    expect(filterFor("memory_warning", { model: " , '" })).toBe("");
  });

  it("keeps spaces and parentheses in a model name", () => {
    expect(filterFor("session", { model: "Creality K1 (Max)/A+" })).toContain("'Creality K1 (Max)/A+'");
  });

  it("strips quotes, backslashes and semicolons", () => {
    expect(filterFor("session", { model: "x'; DROP\\\"" })).toBe(
      ` AND ${col("session", "printer_model")} IN ('x DROP')`,
    );
  });

  it("throws on an unknown kind", () => {
    expect(() => filterFor("nope", { platform: "pi" })).toThrow(/unknown point kind/);
  });
});

describe("filterForKinds", () => {
  it("ORs one branch per kind, each with that kind's column", () => {
    expect(filterForKinds(["crash", "session"], { platform: "pi" })).toBe(
      ` AND ((index1 = 'crash' AND ${col("crash", "app_platform")} IN ('pi'))` +
        ` OR (index1 = 'session' AND ${col("session", "app_platform")} IN ('pi')))`,
    );
    expect(col("crash", "app_platform")).not.toBe(col("session", "app_platform"));
  });

  it("drops the branch of a kind that lacks the field", () => {
    expect(filterForKinds(["memory_warning", "session"], { model: "Voron 2.4" })).toBe(
      ` AND ((index1 = 'session' AND ${col("session", "printer_model")} IN ('Voron 2.4')))`,
    );
  });

  it("excludes everything when no kind has the field", () => {
    expect(filterForKinds(["memory_warning", "crash"], { model: "Voron 2.4" })).toBe(` AND ${SQL_FALSE}`);
  });

  it("restricts to the listed kinds with one IN list when no filter is active", () => {
    expect(filterForKinds(["crash", "session"])).toBe(" AND index1 IN ('crash', 'session')");
    expect(filterForKinds(["crash", "session"], { model: " , '" })).toBe(" AND index1 IN ('crash', 'session')");
  });
});

describe("weighted aggregates", () => {
  it("weight every row by its sample interval", () => {
    expect(wCount()).toBe("sum(_sample_interval)");
    expect(wSum("double2")).toBe("sum((double2) * _sample_interval)");
    expect(wAvg("double2")).toBe("sum((double2) * _sample_interval) / sum(_sample_interval)");
    expect(wCountIf("index1 = 'crash'")).toBe("sumIf(_sample_interval, index1 = 'crash')");
  });
});

describe("queries.ts column literals", () => {
  it("names no blob or double column itself", () => {
    const path = fileURLToPath(new URL("../queries.ts", import.meta.url).href);
    const literals = readFileSync(path, "utf8")
      .split("\n")
      .map((line, i) => [i + 1, line] as const)
      .filter(([, line]) => /blob[0-9]|double[0-9]/.test(line))
      .map(([n, line]) => `${n}: ${line.trim()}`);
    expect(literals).toEqual([]);
  });
});

/** The text between the parenthesis at `open` and its match. */
function parenBody(sql: string, open: number): string {
  let depth = 0;
  for (let i = open; i < sql.length; i++) {
    if (sql[i] === "(") depth++;
    else if (sql[i] === ")" && --depth === 0) return sql.slice(open + 1, i);
  }
  throw new Error(`unbalanced parenthesis at ${open}: ${sql.slice(open, open + 40)}`);
}

const WEIGHTS = ["_sample_interval", "device_weight", "delta_weight"];

/** Every count, sum and avg in `sql` that is not one of the weighted forms. */
function unweightedAggregates(sql: string): string[] {
  const bad: string[] = [];
  if (/\bcount\(\s*\)/.test(sql)) bad.push("count()");
  if (/\bavg\(/.test(sql)) bad.push("avg(");
  for (const m of sql.matchAll(/\b(sum|sumIf)\(/g)) {
    const open = m.index! + m[0].length - 1;
    const body = parenBody(sql, open);
    const ok =
      m[1] === "sumIf"
        ? WEIGHTS.some((w) => body.startsWith(`${w}, `))
        : WEIGHTS.some(
            (w) =>
              body === w ||
              (body.endsWith(` * ${w}`) &&
                body.startsWith("(") &&
                parenBody(body, 0).length === body.length - ` * ${w}`.length - 2),
          );
    if (!ok) bad.push(`${m[1]}(${body})`);
  }
  return bad;
}

describe("query builders", () => {
  const SINGLE_FILTERS: Array<FilterParams | undefined> = [
    undefined,
    { platform: "pi" },
    { version: "1.0.0" },
    { model: "Voron 2.4" },
    { board: "k1" },
  ];

  const builders = Object.entries(queries).filter(
    ([name]) => /Quer(y|ies)$/.test(name) && name !== "executeQuery" && name !== "aeCountsQuery",
  );

  function run(name: string, fn: unknown, filters?: FilterParams): string[] {
    if (name === "healthQueries") {
      return Object.values((fn as (f?: FilterParams, v?: string[]) => Record<string, string>)(filters, ["1.0.0", "v1.1.0"]));
    }
    const out =
      name === "crashListQuery"
        ? (fn as (d: number, l: number, f?: FilterParams) => string)(30, 10, filters)
        : name === "releasesQueries"
          ? (fn as (v: string[]) => string[])(["1.0.0", "v1.1.0"])
          : name === "deviceQueries"
            ? (fn as unknown as (id: string) => string[])("0".repeat(64))
            : (fn as (d: number, f?: FilterParams) => string | string[])(30, filters);
    return Array.isArray(out) ? out : [out];
  }

  it("cover every dashboard builder", () => {
    expect(builders.length).toBeGreaterThan(14);
  });

  it("weight every count, sum and avg by the sample interval under every filter", () => {
    for (const [name, fn] of builders) {
      for (const filters of SINGLE_FILTERS) {
        run(name, fn, filters).forEach((sql, i) => {
          expect(unweightedAggregates(sql), `${name}[${i}] ${JSON.stringify(filters)}`).toEqual([]);
        });
      }
    }
  });

  it("the checker catches each unweighted form", () => {
    expect(unweightedAggregates("SELECT count() FROM t")).toEqual(["count()"]);
    expect(unweightedAggregates("SELECT avg(x) FROM t")).toEqual(["avg("]);
    expect(unweightedAggregates("SELECT sum(x) FROM t")).toEqual(["sum(x)"]);
    expect(unweightedAggregates("SELECT sumIf(1, a = 1) FROM t")).toEqual(["sumIf(1, a = 1)"]);
    expect(unweightedAggregates("SELECT sum((a) * b) * _sample_interval FROM t")).toEqual([
      "sum((a) * b)",
    ]);
    expect(unweightedAggregates("SELECT sum((a) + (b) * _sample_interval) FROM t")).toEqual([
      "sum((a) + (b) * _sample_interval)",
    ]);
    expect(unweightedAggregates(`SELECT ${wAvg("x")}, ${wCountIf("y = 1")}, count(DISTINCT z) FROM t`)).toEqual([]);
  });
});

describe("releasesQueries", () => {
  it("sanitizes the version list", () => {
    const sql = releasesQueries(["v1.0.0'; DROP TABLE x --"]).join("\n");
    expect(sql).toContain("'1.0.0 DROP TABLE x --'");
    expect(sql).not.toContain("';");
  });

  it("matches nothing rather than emitting an empty IN list", () => {
    for (const sql of releasesQueries(["';"])) {
      expect(sql).not.toMatch(/IN \(\s*\)/);
    }
  });
});

describe("overviewQueries", () => {
  it("counts every event-written kind and leaves the CDN fleet rows out of total events", () => {
    const total = overviewQueries(30)[1];
    expect(total).toMatch(/index1 IN \(.*'session'.*\)/);
    expect(total).toMatch(/index1 IN \(.*'crash'.*\)/);
    expect(total).not.toContain("cdn_fleet_daily");
  });

  it("drops kinds without a model column from total events under a model filter", () => {
    const [, total, , , overTime] = overviewQueries(30, { model: "Voron 2.4" });
    for (const sql of [total, overTime]) {
      expect(sql).toContain(`index1 = 'session' AND ${col("session", "printer_model")} IN ('Voron 2.4')`);
      expect(sql).not.toContain("index1 = 'crash'");
    }
  });
});

describe("filterNotApplicable", () => {
  it("names the filter that excludes every one of the kinds", () => {
    expect(filterNotApplicable(["panel_usage", "overlay_visit"], { model: "Voron 2.4" })).toBe("model");
    expect(filterNotApplicable(["crash"], { model: "Voron 2.4", board: "k1" })).toBe("model");
    expect(filterNotApplicable(["connection_stability"], { model: "Voron 2.4", board: "k1" })).toBe("model");
  });

  it("is null when a kind still carries every filtered field", () => {
    expect(filterNotApplicable(["connection_stability"], { board: "k1" })).toBeNull();
    expect(filterNotApplicable(["panel_usage", "widget_interaction", "overlay_visit"], { board: "k1" })).toBeNull();
    expect(filterNotApplicable(["panel_usage"], { platform: "pi" })).toBeNull();
    expect(filterNotApplicable(["panel_usage"])).toBeNull();
    expect(filterNotApplicable(["panel_usage", "session"], { model: "Voron 2.4" })).toBeNull();
  });
});

describe("excludedBy", () => {
  it("is true only when a filter names a field the kind lacks", () => {
    expect(excludedBy("crash", { model: "Voron 2.4" })).toBe(true);
    expect(excludedBy("session", { model: "Voron 2.4" })).toBe(false);
    expect(excludedBy("crash", { platform: "pi" })).toBe(false);
    expect(excludedBy("crash")).toBe(false);
  });
});

describe("deltaSource", () => {
  const key = ["device_id", "boot_id", "snapshot_seq"].map((f) => col("panel_usage", f));

  it("names the four delta point kinds", () => {
    expect([...DELTA_KINDS].sort()).toEqual(
      ["connection_stability", "overlay_visit", "panel_usage", "widget_interaction"],
    );
  });

  it("reads schema-3 rows of its kind only", () => {
    const sql = deltaSource("panel_usage", ["panel_name"], 30);
    expect(sql).toContain(`${col("panel_usage", "schema_version")} >= 3`);
    expect(sql).toContain("index1 = 'panel_usage'");
    expect(sql).toContain("INTERVAL '30' DAY");
  });

  it("groups on device, boot, sequence and the dimensions", () => {
    expect(deltaSource("panel_usage", ["panel_name"], 30)).toContain(
      `GROUP BY ${[...key, col("panel_usage", "panel_name")].join(", ")}`,
    );
    expect(deltaSource("panel_usage", [], 30)).toMatch(new RegExp(`GROUP BY ${key.join(", ")}$`));
  });

  it("carries one copy of every metric and the sample interval through the dedup", () => {
    const sql = deltaSource("panel_usage", ["panel_name"], 30);
    expect(sql).toContain(`argMax(${col("panel_usage", "time_sec")}, timestamp) as time_sec`);
    expect(sql).toContain(`argMax(${col("panel_usage", "session_duration_sec")}, timestamp) as session_duration_sec`);
    expect(sql).toContain("argMax(_sample_interval, timestamp) as delta_weight");
    // An alias named after an aggregate function is avoided.
    expect(deltaSource("overlay_visit", ["overlay_name"], 30)).toContain(
      `argMax(${col("overlay_visit", "count")}, timestamp) as visit_count`,
    );
    expect(deltaSource("overlay_visit", ["overlay_name"], 30)).not.toMatch(/ as count\b/);
    expect(sql).toContain(`${col("panel_usage", "panel_name")} as panel_name`);
    expect(sql).not.toContain("schema_version,");
  });

  it("applies the kind's own filter columns", () => {
    expect(deltaSource("connection_stability", [], 7, { board: "k1" })).toContain(
      ` AND ${col("connection_stability", "app_board")} IN ('k1')`,
    );
  });

  it("refuses a kind with no idempotency key", () => {
    expect(() => deltaSource("session", [], 30)).toThrow(/not a delta kind/);
  });
});

describe("queries over delta kinds", () => {
  const SKIP = new Set(["overviewQueries", "releasesQueries", "aeCountsQuery", "executeQuery", "deviceQueries", "healthQueries"]);
  const builders = Object.entries(queries).filter(
    ([name]) => /Quer(y|ies)$/.test(name) && !SKIP.has(name),
  ) as Array<[string, (d: number, l: number | undefined) => string | string[]]>;

  it("read only deduplicated schema-3 rows", () => {
    let checked = 0;
    for (const [name, fn] of builders) {
      const out = name === "crashListQuery" ? fn(30, 10) : fn(30, undefined);
      for (const sql of Array.isArray(out) ? out : [out]) {
        for (const kind of DELTA_KINDS) {
          if (!sql.includes(`'${kind}'`)) continue;
          checked++;
          expect(sql, `${name}: ${sql}`).toContain(`${col(kind, "schema_version")} >= 3`);
          if (!sql.includes("as delta_era_devices")) {
            const key = ["device_id", "boot_id", "snapshot_seq"].map((f) => col(kind, f)).join(", ");
            expect(sql, `${name}: ${sql}`).toContain(`GROUP BY ${key}`);
            expect(sql, `${name}: ${sql}`).toMatch(/FROM \(\s*SELECT/);
          }
        }
      }
    }
    expect(checked).toBeGreaterThan(10);
  });

  it("features are derived from overlay and widget deltas, not the feature_adoption kind", () => {
    for (const sql of featuresQueries(30)) expect(sql).not.toContain("feature_adoption");
  });

  it("features read each configured flag from its settings_snapshot column, excluding unreported", () => {
    const sqls = featuresQueries(30);
    for (const flag of ["spoolman_configured", "theme_changed", "multi_printer"]) {
      const c = col("settings_snapshot", flag);
      const sql = sqls.find((q) => q.includes(`${c} as name`));
      expect(sql, flag).toBeDefined();
      expect(sql).toContain("index1 = 'settings_snapshot'");
      expect(sql).toContain(`${c} != ''`);
    }
  });

  it("average session length over boots, not over rows", () => {
    const sql = engagementQueries(30)[2];
    expect(sql).toContain("count(DISTINCT boot_id)");
    expect(sql).toContain("sum((session_duration_sec) * delta_weight)");
    // One row per snapshot: the per-panel rows of one snapshot carry the same duration.
    expect(sql).not.toContain(`${col("panel_usage", "panel_name")} as panel_name`);
  });
});

describe("distinct devices per field", () => {
  it("renders the shared template for each distribution", () => {
    expect(adoptionQueries(7)[0]).toBe(
      `SELECT ${col("session", "app_platform")} as name, count(DISTINCT ${col("session", "device_id")}) as count` +
        ` FROM helixscreen_telemetry WHERE timestamp >= NOW() - INTERVAL '7' DAY AND index1 = 'session'` +
        ` AND ${col("session", "app_platform")} != '' GROUP BY name ORDER BY count DESC`,
    );
  });
});

describe("constant predicates", () => {
  const filterSets: FilterParams[] = [
    { model: "Voron 2.4" },
    { board: "k1" },
    { platform: "pi" },
    { version: "1.0.0" },
  ];
  it("never emit a bare integer as a boolean", () => {
    const builders = Object.entries(queries).filter(
      ([name, fn]) => name.endsWith("Queries") && name !== "releasesQueries" && name !== "deviceQueries" && name !== "healthQueries" && typeof fn === "function",
    ) as [string, (days: number, f?: FilterParams) => string[]][];
    expect(builders.length).toBeGreaterThan(5);
    for (const [name, build] of builders) {
      for (const f of filterSets) {
        for (const sql of build(7, f)) {
          expect(sql, `${name} ${JSON.stringify(f)}`).not.toMatch(/AND 0\b|OR 0\b|AND 1\b(?! =)/);
        }
      }
    }
    for (const sql of releasesQueries([])) expect(sql).not.toMatch(/AND 0\b|OR 0\b|AND 1\b(?! =)|\bWHERE 0\b/);
    for (const f of filterSets) {
      for (const [k, sql] of Object.entries(queries.healthQueries(f, ["1.0.0"]))) {
        expect(sql, `healthQueries.${k} ${JSON.stringify(f)}`).not.toMatch(/AND 0\b|OR 0\b|AND 1\b(?! =)|\bWHERE 0\b/);
      }
    }
  });
});
