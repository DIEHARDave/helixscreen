// SPDX-License-Identifier: GPL-3.0-or-later
import { describe, it, expect } from 'vitest'
import * as T from '../loop/thresholds'
import { fingerprint, markerFor, fingerprintInBody } from '../loop/fingerprint'
import { isoWeek, previousIsoWeek, isMonday } from '../loop/week'

const d = (s: string) => new Date(`${s}T12:00:00Z`)

describe('thresholds', () => {
  it('holds the spec values', () => {
    expect(T.CLUSTER_ALERT_DEVICES).toBe(3)
    expect(T.CLUSTER_ISSUE_DEVICES).toBe(3)
    expect(T.ERROR_ISSUE_DEVICES).toBe(5)
    expect(T.UPDATE_FAILURE_ISSUE_DEVICES).toBe(3)
    expect(T.RELEASE_REGRESSION_MIN_DEVICES).toBe(20)
    expect(T.RELEASE_REGRESSION_CRASH_FREE_DROP).toBe(0.02)
    expect(T.PRINTER_ISSUE_DEVICES).toBe(5)
    expect(T.NEW_ISSUES_PER_RUN).toBe(5)
    expect(T.ALERT_REPEAT_DAYS).toBe(30)
  })
})

describe('isoWeek', () => {
  it('handles year boundaries', () => {
    expect(isoWeek(d('2026-01-01'))).toBe('2026-W01')
    expect(isoWeek(d('2027-01-01'))).toBe('2026-W53')
    expect(isoWeek(d('2026-12-28'))).toBe('2026-W53')
    expect(isoWeek(d('2026-10-19'))).toBe('2026-W43')
    expect(isoWeek(d('2026-10-18'))).toBe('2026-W42')
  })
  it('previousIsoWeek steps back one week, across years', () => {
    expect(previousIsoWeek(d('2026-10-19'))).toBe('2026-W42')
    expect(previousIsoWeek(d('2026-10-21'))).toBe('2026-W42')
    expect(previousIsoWeek(d('2026-01-01'))).toBe('2025-W52')
  })
  it('isMonday is UTC', () => {
    expect(isMonday(d('2026-10-19'))).toBe(true)
    expect(isMonday(d('2026-10-20'))).toBe(false)
    expect(isMonday(new Date('2026-10-19T00:00:00Z'))).toBe(true)
    expect(isMonday(new Date('2026-10-18T23:59:59Z'))).toBe(false)
  })
})

describe('fingerprint', () => {
  it('is case and whitespace insensitive and order preserving', () => {
    expect(fingerprint('crash_cluster', [' SIGSEGV ', 'Foo::Bar'])).toBe('crash_cluster:sigsegv|foo::bar')
    expect(fingerprint('crash_cluster', ['a', 'b'])).not.toBe(fingerprint('crash_cluster', ['b', 'a']))
    expect(fingerprint('error_code', ['X', 'y'])).toBe(fingerprint('error_code', [' x', 'Y ']))
  })
  it('is distinct across kinds', () => {
    expect(fingerprint('crash_cluster', ['a'])).not.toBe(fingerprint('error_code', ['a']))
  })
  it('round-trips through a marker', () => {
    const fp = fingerprint('printer', ['Voron 2.4', '350'])
    const body = `Some text\n\n${markerFor(fp)}\n`
    expect(markerFor(fp)).toBe(`<!-- telemetry-fp: ${fp} -->`)
    expect(fingerprintInBody(body)).toBe(fp)
  })
  it('returns null without a marker', () => {
    expect(fingerprintInBody('no marker here')).toBeNull()
    expect(fingerprintInBody('')).toBeNull()
  })
})
