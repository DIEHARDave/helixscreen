// SPDX-License-Identifier: GPL-3.0-or-later
// Files, updates and reopens one GitHub issue per issue-worthy finding, keyed by the
// fingerprint marker in the issue body.

import type { Finding } from './findings'
import { markerFor, type FindingKind } from './fingerprint'
import type { IssueTracker } from './types'
import type { LoopState } from './state'
import { NEW_ISSUES_PER_RUN } from './thresholds'

export interface IssueOutcome {
  fingerprint: string
  action: 'created' | 'commented' | 'reopened' | 'unchanged' | 'held'
  issue?: { number: number; url: string }
  /**
   * What went wrong: why a finding was held when GitHub failed (absent when the per-run
   * cap held it), or what is wrong with an issue that was created.
   */
  error?: string
}

/** Every issue a producer files carries it; lookups list by it. */
export const TELEMETRY_LABEL = 'telemetry'

const LABELS: Partial<Record<FindingKind, string>> = {
  crash_cluster: 'crash',
  error_code: 'error-code',
  update_failure: 'update-failure',
  printer: 'printer-db',
}

/** Every label a telemetry issue can carry, telemetry first. */
export const ALL_ISSUE_LABELS: readonly string[] = [TELEMETRY_LABEL, ...Object.values(LABELS)]

export function issueLabels(kind: FindingKind): string[] {
  const label = LABELS[kind]
  return label ? [TELEMETRY_LABEL, label] : [TELEMETRY_LABEL]
}

/** An issue without the label is invisible to the next run's lookup, which would file it again. */
const UNLABELLED = `created without the ${TELEMETRY_LABEL} label; add it by hand or the next run files a duplicate`

function countsTable(f: Finding): string {
  const rows = Object.entries(f.summary).map(([k, v]) => `| ${k} | ${v} |`)
  return ['| Field | Value |', '| --- | --- |', ...rows].join('\n')
}

/** A dashboard route under the configured base URL, whether or not the base ends in "/". */
export function dashboardLink(dashboardUrl: string, path: string): string {
  return `${dashboardUrl.replace(/\/+$/, '')}${path}`
}

/** Marker first, then the counts, the dashboard link and the ISO week the counts cover. */
export function issueBody(f: Finding, dashboardUrl: string, week: string): string {
  return [
    markerFor(f.fingerprint),
    '',
    `Seen in telemetry for ${week}.`,
    '',
    countsTable(f),
    '',
    `Dashboard: ${dashboardLink(dashboardUrl, f.dashboardPath)}`,
  ].join('\n')
}

function commentBody(f: Finding, dashboardUrl: string, week: string, returned: boolean): string {
  const lead = returned ? `This returned in ${week}.` : `Still seen in ${week}.`
  return [lead, '', countsTable(f), '', `Dashboard: ${dashboardLink(dashboardUrl, f.dashboardPath)}`].join('\n')
}

/**
 * Applies every issue-bearing finding to the tracker. Mutates `state.issueComments`
 * (the caller saves it) unless `dryRun`, which reports the same outcomes and calls
 * nothing that writes. A tracker error holds that finding and moves on.
 */
export async function syncIssues(
  findings: Finding[],
  tracker: IssueTracker,
  state: LoopState,
  week: string,
  dashboardUrl: string,
  dryRun: boolean,
): Promise<IssueOutcome[]> {
  const outcomes: IssueOutcome[] = []
  let created = 0
  const remember = (fp: string): void => {
    if (!dryRun) state.issueComments[fp] = week
  }

  for (const f of findings) {
    if (!f.actions.includes('issue')) continue
    const fp = f.fingerprint
    try {
      const existing = await tracker.findByFingerprint(fp)
      if (existing) {
        const ref = { number: existing.number, url: existing.url }
        if (existing.state === 'closed') {
          if (!dryRun) {
            await tracker.reopen(existing.number)
            await tracker.comment(existing.number, commentBody(f, dashboardUrl, week, true))
          }
          remember(fp)
          outcomes.push({ fingerprint: fp, action: 'reopened', issue: ref })
        } else if (state.issueComments[fp] === week) {
          outcomes.push({ fingerprint: fp, action: 'unchanged', issue: ref })
        } else {
          if (!dryRun) await tracker.comment(existing.number, commentBody(f, dashboardUrl, week, false))
          remember(fp)
          outcomes.push({ fingerprint: fp, action: 'commented', issue: ref })
        }
      } else if (created >= NEW_ISSUES_PER_RUN) {
        outcomes.push({ fingerprint: fp, action: 'held' })
      } else {
        created++
        if (dryRun) {
          outcomes.push({ fingerprint: fp, action: 'created' })
        } else {
          const made = await tracker.create(f.title, issueBody(f, dashboardUrl, week), issueLabels(f.kind))
          remember(fp)
          const outcome: IssueOutcome = { fingerprint: fp, action: 'created', issue: { number: made.number, url: made.url } }
          if (!made.labels.includes(TELEMETRY_LABEL)) {
            console.error(`[loop] ${new Date().toISOString()} issue #${made.number} ${UNLABELLED}`)
            outcome.error = UNLABELLED
          }
          outcomes.push(outcome)
        }
      }
    } catch (err) {
      const error = err instanceof Error ? err.message : String(err)
      console.warn(`[loop] ${new Date().toISOString()} issue sync failed for ${fp}: ${error}`)
      outcomes.push({ fingerprint: fp, action: 'held', error })
    }
  }
  return outcomes
}
