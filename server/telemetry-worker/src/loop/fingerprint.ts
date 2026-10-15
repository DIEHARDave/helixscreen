// SPDX-License-Identifier: GPL-3.0-or-later
// Stable identity for a finding, embedded in its GitHub issue body so a later run
// finds the same issue again instead of opening a second one.

export type FindingKind =
  | 'crash_cluster'
  | 'error_code'
  | 'update_failure'
  | 'release_regression'
  | 'data_health'
  | 'printer'

const MARKER_RE = /<!--\s*telemetry-fp:\s*(.+?)\s*-->/

/** "<kind>:" followed by the parts lowercased and trimmed, joined with "|". */
export function fingerprint(kind: FindingKind, parts: string[]): string {
  return `${kind}:${parts.map((p) => p.trim().toLowerCase()).join('|')}`
}

export function markerFor(fp: string): string {
  return `<!-- telemetry-fp: ${fp} -->`
}

/** The fingerprint stored in an issue body by markerFor, or null when there is none. */
export function fingerprintInBody(body: string): string | null {
  const m = MARKER_RE.exec(body)
  return m ? m[1] : null
}
