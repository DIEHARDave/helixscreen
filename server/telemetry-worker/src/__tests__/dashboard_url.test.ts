// SPDX-License-Identifier: GPL-3.0-or-later
import { describe, expect, it } from 'vitest'
import {
  applyDrill,
  buildDashQuery,
  legacyRedirect,
  parseDashQuery,
  type DashQuery
} from '../../../analytics-dashboard/src/utils/urlState'

const FULL = {
  range: '90d',
  platform: 'pi,ad5m',
  version: '1.0.5,1.0.4',
  model: 'Voron 2.4,Prusa MK4',
  board: 'btt-octopus',
  expand: 'line:0.99,betas:1.1.0'
}

describe('parseDashQuery / buildDashQuery', () => {
  it('round-trips a query carrying every key', () => {
    const parsed = parseDashQuery(FULL)
    expect(parsed).toEqual({
      range: '90d',
      filters: {
        platform: ['pi', 'ad5m'],
        version: ['1.0.5', '1.0.4'],
        model: ['Voron 2.4', 'Prusa MK4'],
        board: ['btt-octopus']
      },
      expand: ['line:0.99', 'betas:1.1.0']
    })
    expect(buildDashQuery(parsed)).toEqual(FULL)
  })

  it('omits empty keys but always keeps range', () => {
    const out = buildDashQuery(parseDashQuery({}))
    expect(out).toEqual({ range: '30d' })
  })

  it('trims, drops empty items and ignores unknown keys', () => {
    const parsed = parseDashQuery({ platform: ' pi , ,ad5m,', bogus: 'x' })
    expect(parsed.filters.platform).toEqual(['pi', 'ad5m'])
    expect(Object.keys(buildDashQuery(parsed)).sort()).toEqual(['platform', 'range'])
  })

  it('falls back to 30d for a malformed range', () => {
    for (const bad of ['abc', '30', 'd30', '30d;', '-7d', '']) {
      expect(parseDashQuery({ range: bad }).range, bad).toBe('30d')
    }
    expect(parseDashQuery({ range: '7d' }).range).toBe('7d')
  })

  it('reads an array-valued query key as its first entry and null as absent', () => {
    expect(parseDashQuery({ range: ['7d', '90d'] }).range).toBe('7d')
    expect(parseDashQuery({ model: null }).filters.model).toEqual([])
  })
})

describe('applyDrill', () => {
  const base = (): DashQuery => parseDashQuery({ range: '7d', model: 'Old', platform: 'pi' })

  it('replaces that key and leaves the others alone', () => {
    const out = applyDrill(base(), 'model', ['Voron 2.4'])
    expect(out.filters.model).toEqual(['Voron 2.4'])
    expect(out.filters.platform).toEqual(['pi'])
    expect(out.range).toBe('7d')
  })

  it('keeps every member of a group and dedupes', () => {
    const members = ['0.99.118', '0.99.30', '0.99.118', '0.99.7']
    const out = applyDrill(base(), 'version', members)
    expect(out.filters.version).toEqual(['0.99.118', '0.99.30', '0.99.7'])
    expect(buildDashQuery(out).version).toBe('0.99.118,0.99.30,0.99.7')
  })

  it('does not mutate its input', () => {
    const s = base()
    applyDrill(s, 'model', ['X'])
    expect(s.filters.model).toEqual(['Old'])
  })
})

describe('legacyRedirect', () => {
  const MAP: Record<string, string> = {
    '/adoption': '/fleet',
    '/hardware': '/fleet',
    '/memory': '/stability',
    '/crashes': '/stability',
    '/reliability': '/stability',
    '/engagement': '/usage',
    '/features': '/usage',
    '/ux': '/usage'
  }

  it('maps every legacy path', () => {
    for (const [from, to] of Object.entries(MAP)) expect(legacyRedirect(from), from).toBe(to)
  })

  it('returns null for current paths', () => {
    for (const p of ['/fleet', '/stability', '/usage', '/', '/devices', '/device/abc']) {
      expect(legacyRedirect(p), p).toBeNull()
    }
  })
})
