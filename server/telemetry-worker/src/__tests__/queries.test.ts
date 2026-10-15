// SPDX-License-Identifier: GPL-3.0-or-later
import { describe, it, expect } from "vitest";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import * as queries from "../queries";
import {
  filterFor,
  filterForKinds,
  releasesQueries,
  overviewQueries,
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
    expect(filterFor("memory_warning", { model: "Voron 2.4" })).toBe(" AND 0");
    expect(filterFor("memory_snapshot", { board: "k1" })).toBe(" AND 0");
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
    expect(filterForKinds(["memory_warning", "crash"], { model: "Voron 2.4" })).toBe(" AND 0");
  });

  it("still restricts to the listed kinds with no filters", () => {
    expect(filterForKinds(["crash", "session"])).toBe(" AND ((index1 = 'crash') OR (index1 = 'session'))");
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

const WEIGHTS = ["_sample_interval", "device_weight"];

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
    const out =
      name === "crashListQuery"
        ? (fn as (d: number, l: number, f?: FilterParams) => string)(30, 10, filters)
        : name === "releasesQueries"
          ? (fn as (v: string[]) => string[])(["1.0.0", "v1.1.0"])
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
    expect(total).toContain("index1 = 'session'");
    expect(total).toContain("index1 = 'crash'");
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
