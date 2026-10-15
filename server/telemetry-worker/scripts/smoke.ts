// SPDX-License-Identifier: GPL-3.0-or-later
// Hit every dashboard endpoint across the filter matrix and report PASS/FAIL.
//
//   (cd server/telemetry-worker && npx vite-node scripts/smoke.ts [base-url])
//
// The base URL defaults to the production worker. The admin key comes from
// HELIX_TELEMETRY_ADMIN_KEY or the repo root's .env.telemetry. Exits 1 when any
// request returns an unexpected status.

import { adminKeyFromEnvOrFile } from "./adminKey";
import { compareVersions } from "../src/versions";

const DEFAULT_BASE_URL = "https://telemetry.helixscreen.org";
const RANGES = ["7d", "30d", "90d"];
const ZERO_DEVICE_ID = "0".repeat(64);

/** Endpoints that take the range and filter parameters, in /v1/dashboard/. */
export const FILTERED_ENDPOINTS = [
  "overview",
  "adoption",
  "prints",
  "crashes",
  "crash-list",
  "memory",
  "memory-warnings",
  "hardware",
  "engagement",
  "reliability",
  "stability",
  "releases",
  "performance",
  "features",
  "ux",
  "health",
  "devices",
  "fleet/retention",
];

export interface SmokeResult {
  label: string;
  path: string;
  expected: number;
  status: number | string;
}

export function passed(r: SmokeResult): boolean {
  return r.status === r.expected;
}

/** Fixed-width PASS/FAIL table ending in a count of passes and failures. */
export function formatSmokeTable(results: SmokeResult[]): string {
  const labelW = Math.max(5, ...results.map((r) => r.label.length));
  const lines = results.map(
    (r) =>
      `${passed(r) ? "PASS" : "FAIL"}  ${String(r.status).padEnd(5)} ${r.label.padEnd(labelW)}  ${r.path}`,
  );
  const failed = results.filter((r) => !passed(r)).length;
  lines.push("", `${results.length - failed}/${results.length} passed${failed ? `, ${failed} FAILED` : ""}`);
  return lines.join("\n");
}

export interface Matrix {
  newestVersion?: string;
  /** The two newest versions, newest first, for the explicit-versions releases request. */
  newestTwo?: string[];
  topModel?: string;
}

/** Filter sets applied to every endpoint: none, then one per filter key. */
export function filterSets(m: Matrix): Array<{ label: string; qs: string }> {
  const sets = [{ label: "no filter", qs: "" }, { label: "platform=pi", qs: "platform=pi" }];
  if (m.newestVersion) sets.push({ label: "version", qs: `version=${encodeURIComponent(m.newestVersion)}` });
  if (m.topModel) sets.push({ label: "model", qs: `model=${encodeURIComponent(m.topModel)}` });
  sets.push({ label: "board=rpi4", qs: "board=rpi4" });
  return sets;
}

export function pickMatrix(adoption: {
  versions?: Array<{ name: string }>;
  printer_models?: Array<{ name: string; count: number }>;
}): Matrix {
  const versions = (adoption.versions ?? []).map((v) => v.name).sort(compareVersions);
  const models = [...(adoption.printer_models ?? [])].sort((a, b) => b.count - a.count);
  return {
    newestVersion: versions[versions.length - 1],
    newestTwo: versions.slice(-2).reverse(),
    topModel: models[0]?.name,
  };
}

/** The releases request that names its versions, or null when adoption listed none. */
export function releasesVersionsPath(m: Matrix): string | null {
  if (!m.newestTwo?.length) return null;
  return `/v1/dashboard/releases?range=30d&versions=${m.newestTwo.map(encodeURIComponent).join(",")}`;
}

async function get(base: string, path: string, key: string): Promise<{ status: number; body: unknown }> {
  const res = await fetch(`${base}${path}`, { headers: { "X-API-Key": key } });
  let body: unknown = null;
  try {
    body = await res.json();
  } catch {
    // A non-JSON body only matters for the status.
  }
  return { status: res.status, body };
}

async function main(): Promise<void> {
  const base = (process.argv[2] ?? DEFAULT_BASE_URL).replace(/\/$/, "");
  const key = adminKeyFromEnvOrFile();
  if (!key) {
    console.error("HELIX_TELEMETRY_ADMIN_KEY is not set in the environment or .env.telemetry");
    process.exit(2);
  }
  const results: SmokeResult[] = [];
  const check = async (label: string, path: string, expected = 200) => {
    try {
      const { status, body } = await get(base, path, key);
      results.push({ label, path, expected, status });
      return body;
    } catch (err) {
      results.push({ label, path, expected, status: err instanceof Error ? err.message : "error" });
      return null;
    }
  };

  const adoption = (await check("matrix source", "/v1/dashboard/adoption?range=30d")) as Parameters<
    typeof pickMatrix
  >[0] | null;
  const matrix = pickMatrix(adoption ?? {});
  for (const endpoint of FILTERED_ENDPOINTS) {
    for (const range of RANGES) {
      for (const set of filterSets(matrix)) {
        const qs = [`range=${range}`, set.qs].filter(Boolean).join("&");
        await check(`${endpoint} ${range} ${set.label}`, `/v1/dashboard/${endpoint}?${qs}`);
      }
    }
  }

  const releasesPath = releasesVersionsPath(matrix);
  if (releasesPath) await check("releases two newest versions", releasesPath);

  const devices = (await check("devices (for id)", "/v1/dashboard/devices?range=90d")) as {
    devices?: Array<{ device_id: string }>;
  } | null;
  const firstId = devices?.devices?.[0]?.device_id;
  if (firstId) await check("device/<first id>", `/v1/dashboard/device/${firstId}`);
  else results.push({ label: "device/<first id>", path: "(devices list is empty)", expected: 200, status: "none" });
  await check("device/<64 zeros> is 404", `/v1/dashboard/device/${ZERO_DEVICE_ID}`, 404);
  await check("device/xyz is 400", "/v1/dashboard/device/xyz", 400);

  console.log(formatSmokeTable(results));
  if (results.some((r) => !passed(r))) process.exit(1);
}

// vitest imports this file for the pure functions above; it must not run main().
if (!process.env.VITEST) {
  main().catch((err) => {
    console.error(err instanceof Error ? err.message : err);
    process.exit(1);
  });
}
