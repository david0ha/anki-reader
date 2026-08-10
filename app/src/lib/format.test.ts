import { describe, it, expect } from '@jest/globals'
import {
  PAGE_LABELS,
  fetchResultLabel,
  fetchResultMessage,
  fetchResultTone,
  formatAge,
  formatCount,
  formatDelta,
  formatDensity,
  formatInterval,
  formatMs,
  formatRatio,
  pageLabel,
} from './format'

describe('pageLabel', () => {
  it('names the four pages in the firmware’s order', () => {
    expect([...PAGE_LABELS]).toEqual(['Stats', 'Graph', 'Agents', 'Notes'])
    expect(pageLabel(0)).toBe('Stats')
    expect(pageLabel(3)).toBe('Notes')
  })

  it('falls back for an out-of-range page rather than rendering undefined', () => {
    expect(pageLabel(7)).toBe('Page 7')
    expect(pageLabel(-1)).toBe('Page -1')
  })
})

describe('formatCount', () => {
  it('groups thousands', () => {
    expect(formatCount(1428)).toBe('1,428')
    expect(formatCount(0)).toBe('0')
    expect(formatCount(1000000)).toBe('1,000,000')
  })

  it('returns an em dash for non-finite input', () => {
    expect(formatCount(NaN)).toBe('—')
    expect(formatCount(Infinity)).toBe('—')
  })
})

describe('formatDelta', () => {
  it('signs a positive value and leaves zero unsigned', () => {
    expect(formatDelta(6)).toBe('+6')
    expect(formatDelta(0)).toBe('0')
    expect(formatDelta(-2)).toBe('-2')
  })
})

describe('formatRatio', () => {
  it('renders one decimal of percent', () => {
    expect(formatRatio(37, 1428)).toBe('2.6%')
    expect(formatRatio(0, 100)).toBe('0.0%')
  })

  it('refuses to divide by an empty vault', () => {
    // "0.0%" would claim an empty vault has a good orphan rate. It has none.
    expect(formatRatio(0, 0)).toBe('—')
    expect(formatRatio(5, -1)).toBe('—')
  })
})

describe('formatDensity', () => {
  it('renders links per note to one decimal', () => {
    expect(formatDensity(3910, 1428)).toBe('2.7')
  })

  it('refuses to divide by zero notes', () => {
    expect(formatDensity(0, 0)).toBe('—')
  })
})

describe('formatAge', () => {
  it('never reports a board that has never synced as fresh', () => {
    // The firmware sends -1 for "no poll has ever succeeded". Rendering that as "0s ago" is the
    // one mistake here that actively misinforms.
    expect(formatAge(-1)).toBe('never')
    expect(formatAge(NaN)).toBe('never')
  })

  it('scales the unit with the age', () => {
    expect(formatAge(0)).toBe('0s ago')
    expect(formatAge(42)).toBe('42s ago')
    expect(formatAge(180)).toBe('3m ago')
    expect(formatAge(7200)).toBe('2h ago')
    expect(formatAge(172800)).toBe('2d ago')
  })
})

describe('formatInterval', () => {
  it('renders the poll interval', () => {
    expect(formatInterval(300)).toBe('every 5m')
    expect(formatInterval(45)).toBe('every 45s')
    expect(formatInterval(90)).toBe('every 1.5m')
  })

  it('returns an em dash for a nonsensical interval', () => {
    expect(formatInterval(0)).toBe('—')
    expect(formatInterval(-5)).toBe('—')
  })
})

describe('formatMs', () => {
  it('treats zero as "not measured yet", not as an instant refresh', () => {
    // The firmware reports 0 until that kind of refresh has run once since boot. "0 ms" would
    // read as an impossibly fast e-Paper panel.
    expect(formatMs(0)).toBe('—')
  })

  it('switches to seconds above a second', () => {
    expect(formatMs(780)).toBe('780 ms')
    expect(formatMs(4120)).toBe('4.1 s')
  })
})

describe('fetch result rendering', () => {
  it('labels every documented result', () => {
    expect(fetchResultLabel('ok')).toBe('synced')
    expect(fetchResultLabel('no_url')).toBe('demo')
    expect(fetchResultLabel('transport')).toBe('unreachable')
    expect(fetchResultLabel('http_status')).toBe('server error')
    expect(fetchResultLabel('bad_payload')).toBe('bad payload')
    expect(fetchResultLabel('unknown')).toBe('unknown')
  })

  it('gives the three failures three different explanations', () => {
    // They point at three different mistakes, which is the whole reason the firmware keeps them
    // apart; collapsing them here would throw that away at the last step.
    const messages = new Set([
      fetchResultMessage('transport'),
      fetchResultMessage('http_status'),
      fetchResultMessage('bad_payload'),
    ])
    expect(messages.size).toBe(3)
  })

  it('does not colour an unconfigured board as broken', () => {
    // A board with no URL is a complete product showing demo data, not a failure.
    expect(fetchResultTone('no_url')).toBe('neutral')
    expect(fetchResultTone('ok')).toBe('up')
    expect(fetchResultTone('transport')).toBe('down')
    expect(fetchResultTone('http_status')).toBe('down')
    expect(fetchResultTone('bad_payload')).toBe('down')
    expect(fetchResultTone('unknown')).toBe('warn')
  })
})
