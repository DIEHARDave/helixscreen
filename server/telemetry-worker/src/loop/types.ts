// SPDX-License-Identifier: GPL-3.0-or-later
// The loop's three outside dependencies (GitHub, email, R2) as small interfaces. No
// Worker runtime types, so modules the dashboard imports can depend on them; the
// production adapters live in ports.ts.

import type { EmailMessage } from '../../../shared/email'

export interface IssueRef {
  number: number
  state: 'open' | 'closed'
  url: string
  body: string
  /** ISO week of the last telemetry comment. Filled from LoopState by the caller, never by the tracker. */
  lastTelemetryCommentWeek: string | null
}

export interface IssueTracker {
  /** Searches open and closed issues labelled telemetry for the fingerprint's marker. */
  findByFingerprint(fp: string): Promise<IssueRef | null>
  create(title: string, body: string, labels: string[]): Promise<IssueRef>
  comment(issue: number, body: string): Promise<void>
  reopen(issue: number): Promise<void>
}

export interface Mailer {
  send(message: EmailMessage): Promise<void>
}

export interface ObjectStore {
  get(key: string): Promise<string | null>
  put(key: string, value: string): Promise<void>
  list(prefix: string): Promise<string[]>
}
