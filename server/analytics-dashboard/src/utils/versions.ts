// SPDX-License-Identifier: GPL-3.0-or-later
import { compareVersions } from './chart'

/** Shared row shape for every grouped distribution. */
export interface Grouped {
  /** Stable id: the raw value, or 'line:0.99', 'betas:1.1.0', 'model:<canonical key>', 'set:PETG + PLA'. */
  key: string
  /** What the chart shows. */
  label: string
  /** Summed over members. */
  count: number
  /** Raw values to send as a filter, in input order. */
  members: string[]
  /** A collapsed version line or beta group. */
  expandable: boolean
}

interface Parsed {
  raw: string
  count: number
  core: string
  line: string
  prerelease: boolean
}

function parse(row: { name: string; count: number }): Parsed {
  const noBuild = row.name.replace(/^v/, '').split('+')[0]
  const dash = noBuild.indexOf('-')
  const core = dash < 0 ? noBuild : noBuild.slice(0, dash)
  const line = core.split('.').slice(0, 2).join('.')
  return { raw: row.name, count: row.count, core, line, prerelease: dash >= 0 }
}

function groupOf(key: string, label: string, parts: Parsed[], expandable: boolean): Grouped {
  return {
    key,
    label,
    count: parts.reduce((sum, p) => sum + p.count, 0),
    members: parts.map((p) => p.raw),
    expandable
  }
}

/**
 * Collapses a long version list into something readable. The `keepLines`
 * newest major.minor lines are shown version by version; older lines fold to
 * one "<x.y>.x (<n> versions)" row. In a kept line, prereleases whose release
 * is not in `rows` fold to one "<x.y.z> betas (<n>)" row. Keys in `expanded`
 * render their members individually. Output is newest first; `n` counts
 * members, not devices.
 */
export function groupVersions(
  rows: { name: string; count: number }[],
  expanded: ReadonlySet<string>,
  keepLines = 2
): Grouped[] {
  const parsed = rows.map(parse).sort((a, b) => compareVersions(b.raw, a.raw))
  const lines: string[] = []
  for (const p of parsed) if (!lines.includes(p.line)) lines.push(p.line)
  const kept = new Set(lines.slice(0, keepLines))
  const releases = new Set(parsed.filter((p) => !p.prerelease).map((p) => p.core))

  const out: Grouped[] = []
  const emitted = new Set<string>()
  const individual = (p: Parsed) =>
    out.push({ key: p.raw, label: p.raw, count: p.count, members: [p.raw], expandable: false })

  const inputOrder = (members: Parsed[]) =>
    members.slice().sort((a, b) => rows.findIndex((r) => r.name === a.raw) - rows.findIndex((r) => r.name === b.raw))

  for (const p of parsed) {
    if (kept.has(p.line)) {
      if (p.prerelease && !releases.has(p.core)) {
        const key = `betas:${p.core}`
        if (expanded.has(key)) {
          individual(p)
        } else if (!emitted.has(key)) {
          emitted.add(key)
          const members = inputOrder(parsed.filter((q) => q.prerelease && q.core === p.core))
          out.push(groupOf(key, `${p.core} betas (${members.length})`, members, true))
        }
      } else {
        individual(p)
      }
    } else {
      const key = `line:${p.line}`
      if (expanded.has(key)) {
        individual(p)
      } else if (!emitted.has(key)) {
        emitted.add(key)
        const members = inputOrder(parsed.filter((q) => q.line === p.line))
        out.push(groupOf(key, `${p.line}.x (${members.length} versions)`, members, true))
      }
    }
  }
  return out
}

/** One row per raw value, for a dimension that needs no grouping (platforms, boards). */
export function ungrouped(rows: { name: string; count: number }[]): Grouped[] {
  return rows.map((r) => ({ key: r.name, label: r.name, count: r.count, members: [r.name], expandable: false }))
}
