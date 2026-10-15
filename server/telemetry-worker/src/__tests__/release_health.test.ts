// SPDX-License-Identifier: GPL-3.0-or-later
import { describe, expect, it } from "vitest";
import { RELEASE_WARN, compareRelease, formatTable, pickCandidate, type ReleaseRow } from "../../scripts/releaseHealth";

const row = (over: Partial<ReleaseRow> = {}): ReleaseRow => ({
  version: "1.0.3",
  active_devices: 100,
  crash_free_devices: 0.98,
  crash_rate: 0.01,
  update_failure_rate: 0.02,
  update_failures: 2,
  update_successes: 98,
  print_success_rate: 0.9,
  p95_frame_ms: 20,
  p95_rss_kb: 100000,
  ...over,
});
const warned = (c: ReleaseRow, b: ReleaseRow) => compareRelease(c, b).filter((v) => v.warn).map((v) => v.metric);

describe("compareRelease", () => {
  it("is quiet when the candidate matches the baseline", () => {
    expect(warned(row(), row())).toEqual([]);
  });

  it("warns on a crash-free drop at the threshold, not below it", () => {
    const b = row({ crash_free_devices: 0.98 });
    expect(warned(row({ crash_free_devices: 0.98 - RELEASE_WARN.crashFreeDrop }), b)).toContain("crash-free devices");
    expect(warned(row({ crash_free_devices: 0.98 - RELEASE_WARN.crashFreeDrop + 0.001 }), b)).not.toContain("crash-free devices");
  });

  it("warns on a crash rate at the ratio only when the absolute rise also counts", () => {
    const b = row({ crash_rate: 0.02 });
    expect(warned(row({ crash_rate: 0.02 * RELEASE_WARN.crashRateRatio }), b)).toContain("crash rate");
    expect(warned(row({ crash_rate: 0.02 * RELEASE_WARN.crashRateRatio - 0.001 }), b)).not.toContain("crash rate");
    // A tiny base makes any rise a big ratio; the minimum delta keeps it quiet.
    const tiny = row({ crash_rate: 0.0001 });
    expect(warned(row({ crash_rate: 0.0001 * 3 }), tiny)).not.toContain("crash rate");
    expect(warned(row({ crash_rate: RELEASE_WARN.crashRateMinDelta }), row({ crash_rate: 0 }))).toContain("crash rate");
    expect(warned(row({ crash_rate: RELEASE_WARN.crashRateMinDelta / 2 }), row({ crash_rate: 0 }))).not.toContain("crash rate");
  });

  it("warns on an update failure rise at the threshold", () => {
    const b = row({ update_failure_rate: 0.02 });
    expect(warned(row({ update_failure_rate: 0.02 + RELEASE_WARN.updateFailureRise }), b)).toContain("update failure rate");
    expect(warned(row({ update_failure_rate: 0.02 + RELEASE_WARN.updateFailureRise - 0.01 }), b)).not.toContain("update failure rate");
  });

  it("warns on a print success drop at the threshold", () => {
    const b = row({ print_success_rate: 0.9 });
    expect(warned(row({ print_success_rate: 0.9 - RELEASE_WARN.printSuccessDrop }), b)).toContain("print success rate");
    expect(warned(row({ print_success_rate: 0.9 - RELEASE_WARN.printSuccessDrop + 0.01 }), b)).not.toContain("print success rate");
  });

  it("warns on a frame time rise at the threshold", () => {
    expect(warned(row({ p95_frame_ms: 20 * (1 + RELEASE_WARN.frameRise) }), row())).toContain("p95 frame ms");
    expect(warned(row({ p95_frame_ms: 20 * (1 + RELEASE_WARN.frameRise) - 0.5 }), row())).not.toContain("p95 frame ms");
  });

  it("warns on an RSS rise at the threshold", () => {
    expect(warned(row({ p95_rss_kb: 100000 * (1 + RELEASE_WARN.rssRise) }), row())).toContain("p95 rss kb");
    expect(warned(row({ p95_rss_kb: 100000 * (1 + RELEASE_WARN.rssRise) - 100 }), row())).not.toContain("p95 rss kb");
  });

  it("never warns on a metric either side lacks", () => {
    const nulls = {
      crash_free_devices: null,
      crash_rate: null,
      update_failure_rate: null,
      print_success_rate: null,
      p95_frame_ms: null,
      p95_rss_kb: null,
    };
    expect(warned(row(nulls), row())).toEqual([]);
    expect(warned(row(), row(nulls))).toEqual([]);
    expect(compareRelease(row(nulls), row(nulls))).toHaveLength(6);
  });
});

describe("pickCandidate", () => {
  it("picks the newest prerelease of VERSION.txt's release", () => {
    const seen = ["1.0.3", "1.0.4", "1.1.0-beta.9", "1.1.0-beta.10", "1.1.0-beta.2", "1.2.0-beta.1"];
    expect(pickCandidate("1.1.0\n", seen)).toBe("1.1.0-beta.10");
    expect(pickCandidate("1.1.0-beta.11", seen)).toBe("1.1.0-beta.10");
  });

  it("ignores other releases and stable builds, and returns null when none was seen", () => {
    expect(pickCandidate("1.1.0", ["1.0.3", "1.0.4-beta.1", "1.1.0"])).toBeNull();
    expect(pickCandidate("1.1.0", [])).toBeNull();
  });

  it("tolerates a v prefix", () => {
    expect(pickCandidate("1.1.0", ["v1.1.0-beta.3"])).toBe("v1.1.0-beta.3");
  });
});

describe("formatTable", () => {
  it("names both versions and marks warnings", () => {
    const c = row({ version: "1.1.0-beta.4", crash_free_devices: 0.9 });
    const out = formatTable(c, row(), compareRelease(c, row()));
    expect(out).toContain("1.1.0-beta.4");
    expect(out).toContain("1.0.3");
    expect(out).toMatch(/WARN.*crash-free devices/);
  });
});
