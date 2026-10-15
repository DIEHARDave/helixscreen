// SPDX-License-Identifier: GPL-3.0-or-later
// The weekly report snapshot stored in R2 and served to the dashboard. Types only,
// plus the storage key, so the dashboard can import it without Worker runtime types.

import type { HealthPayload } from '../health'
import type { Finding } from './findings'
import type { IssueOutcome } from './issues'

export interface WeeklyReport {
  /** The ISO week the report covers, e.g. 2026-W42. */
  week: string
  generated_at: string
  health: HealthPayload
  findings: Finding[]
  issues: IssueOutcome[]
}

export const reportKey = (week: string): string => `reports/${week}.json`
