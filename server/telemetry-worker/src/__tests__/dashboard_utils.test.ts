// SPDX-License-Identifier: GPL-3.0-or-later
import { afterEach, describe, expect, it, vi } from 'vitest'
import { groupVersions } from '../../../analytics-dashboard/src/utils/versions'
import { canonicalModelKey, canonicalModels, isFilterable } from '../../../analytics-dashboard/src/utils/models'
import { groupMaterialSets, materialSet } from '../../../analytics-dashboard/src/utils/materials'
import { barChartHeight, seriesColor, todayUtc, trimPartialDays } from '../../../analytics-dashboard/src/utils/series'

const rows = (names: string[]) => names.map((name, i) => ({ name, count: i + 1 }))
const LIVE = ['1.1.0-beta.9', '1.1.0-beta.8', '1.0.5', '1.0.4', '1.0.0', '0.99.118', '0.99.30']

describe('groupVersions', () => {
  it('collapses betas of an absent release and old lines', () => {
    const out = groupVersions(rows(LIVE), new Set())
    expect(out.map((g) => g.label)).toEqual([
      '1.1.0 betas (2)',
      '1.0.5',
      '1.0.4',
      '1.0.0',
      '0.99.x (2 versions)'
    ])
    expect(out[0].key).toBe('betas:1.1.0')
    expect(out[0].members).toEqual(['1.1.0-beta.9', '1.1.0-beta.8'])
    expect(out[0].expandable).toBe(true)
    expect(out[4]).toMatchObject({ key: 'line:0.99', expandable: true, count: 6 + 7, members: ['0.99.118', '0.99.30'] })
  })

  it('shows betas individually once the release is present', () => {
    const out = groupVersions(rows([...LIVE, '1.1.0']), new Set())
    expect(out.map((g) => g.label)).toEqual([
      '1.1.0',
      '1.1.0-beta.9',
      '1.1.0-beta.8',
      '1.0.5',
      '1.0.4',
      '1.0.0',
      '0.99.x (2 versions)'
    ])
  })

  it('expands a collapsed line to its members, newest first', () => {
    const out = groupVersions(rows(LIVE), new Set(['line:0.99']))
    expect(out.slice(-2).map((g) => [g.label, g.expandable])).toEqual([
      ['0.99.118', false],
      ['0.99.30', false]
    ])
  })

  it('expands a beta group', () => {
    const out = groupVersions(rows(LIVE), new Set(['betas:1.1.0']))
    expect(out.slice(0, 2).map((g) => g.label)).toEqual(['1.1.0-beta.9', '1.1.0-beta.8'])
  })

  it('returns nothing for no rows', () => {
    expect(groupVersions([], new Set())).toEqual([])
  })
})

describe('canonicalModels', () => {
  it('folds case, separators and a trailing parenthetical', () => {
    const keys = ['creality_ender_3_v3_ke', 'Creality Ender 3 V3 KE', 'Creality Ender-3 V3 KE'].map(canonicalModelKey)
    expect(new Set(keys).size).toBe(1)
    expect(canonicalModelKey('FlashForge Adventurer 5M Pro (ForgeX)')).toBe(
      canonicalModelKey('FlashForge Adventurer 5M Pro')
    )
    expect(canonicalModelKey('K1')).toBe(canonicalModelKey('Creality K1'))
  })

  it('labels with the biggest spelling and keeps every member', () => {
    const out = canonicalModels([
      { name: 'K1', count: 3 },
      { name: 'Creality K1', count: 10 },
      { name: 'Unknown', count: 50 },
      { name: '', count: 1 }
    ])
    expect(out.map((g) => g.label)).toEqual(['Unknown', 'Creality K1', ''])
    expect(out[1]).toMatchObject({ key: 'model:creality k1', count: 13, members: ['K1', 'Creality K1'] })
  })
})

describe('isFilterable', () => {
  it('rejects values the worker would alter or split', () => {
    expect(isFilterable("CdO - Cubo dell'Odio")).toBe(false)
    expect(isFilterable('Voron 2.4')).toBe(true)
    expect(isFilterable('a,b')).toBe(false)
  })
})

describe('materialSet', () => {
  it('normalizes lists', () => {
    expect(materialSet('PLA;PLA;PETG;PLA')).toEqual({ set: 'PETG + PLA', materials: ['PETG', 'PLA'], multi: true })
    expect(materialSet('pla ')).toEqual({ set: 'PLA', materials: ['PLA'], multi: false })
    expect(materialSet('')).toEqual({ set: '', materials: [], multi: false })
    expect(materialSet('PLA;;PLA').set).toBe('PLA')
  })
})

describe('groupMaterialSets', () => {
  it('merges spellings with a count-weighted rate', () => {
    const out = groupMaterialSets([
      { type: 'PLA;PLA', count: 72, success_rate: 0.9 },
      { type: 'PLA', count: 28, success_rate: 0.5 }
    ])
    expect(out).toHaveLength(1)
    expect(out[0]).toMatchObject({ key: 'set:PLA', label: 'PLA', count: 100, members: ['PLA;PLA', 'PLA'] })
    expect(out[0].success_rate).toBeCloseTo(0.788, 3)
  })
})

describe('trimPartialDays', () => {
  const day = (date: string) => ({ date })
  it('drops the first day and today', () => {
    const s = ['2026-10-10', '2026-10-11', '2026-10-12', '2026-10-13'].map(day)
    expect(trimPartialDays(s, '2026-10-13').map((p) => p.date)).toEqual(['2026-10-11', '2026-10-12'])
  })
  it('drops only the first day when today is absent', () => {
    const s = ['2026-10-10', '2026-10-11', '2026-10-12'].map(day)
    expect(trimPartialDays(s, '2026-10-20').map((p) => p.date)).toEqual(['2026-10-11', '2026-10-12'])
  })
  it('leaves nothing for one or two days', () => {
    expect(trimPartialDays(['2026-10-10', '2026-10-11'].map(day), '2026-10-11')).toEqual([])
    expect(trimPartialDays(['2026-10-10', '2026-10-11'].map(day), '2026-10-20')).toEqual([])
  })
})

describe('seriesColor and barChartHeight', () => {
  it('cycles a 12 color palette', () => {
    expect(seriesColor(0)).not.toBe(seriesColor(1))
    expect(seriesColor(12)).toBe(seriesColor(0))
  })
  it('sizes bar charts per row with a floor', () => {
    expect(barChartHeight(5)).toBe(300)
    expect(barChartHeight(56)).toBe(1292)
  })
})

describe('todayUtc', () => {
  afterEach(() => vi.useRealTimers())
  it('formats the UTC date', () => {
    vi.useFakeTimers()
    vi.setSystemTime(new Date('2026-10-15T23:59:59Z'))
    expect(todayUtc()).toBe('2026-10-15')
  })
})
