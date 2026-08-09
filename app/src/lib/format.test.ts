import { describe, it, expect } from '@jest/globals'
import {
  direction,
  formatAge,
  formatChange,
  formatPercent,
  formatPrice,
  pageLabel,
} from './format'

describe('pageLabel', () => {
  it('maps known pages', () => {
    expect(pageLabel(0)).toBe('Home')
    expect(pageLabel(3)).toBe('Metrics')
  })
  it('falls back for unknown pages', () => {
    expect(pageLabel(9)).toBe('Page 9')
  })
})

describe('formatPrice', () => {
  it('fixes to 2 decimals by default', () => {
    expect(formatPrice(201.5)).toBe('201.50')
  })
  it('returns an em-dash for non-finite', () => {
    expect(formatPrice(NaN)).toBe('—')
  })
})

describe('formatChange / formatPercent', () => {
  it('prefixes a + on gains and nothing on losses', () => {
    expect(formatChange(1.2)).toBe('+1.20')
    expect(formatChange(-0.34)).toBe('-0.34')
    expect(formatPercent(0.6)).toBe('+0.60%')
    expect(formatPercent(-1.1)).toBe('-1.10%')
  })
  it('handles zero without a sign', () => {
    expect(formatChange(0)).toBe('0.00')
  })
})

describe('direction', () => {
  it('classifies up/down/flat', () => {
    expect(direction(2)).toBe('up')
    expect(direction(-2)).toBe('down')
    expect(direction(0)).toBe('flat')
    expect(direction(NaN)).toBe('flat')
  })
})

describe('formatAge', () => {
  it('renders seconds, minutes, hours', () => {
    expect(formatAge(12)).toBe('12s')
    expect(formatAge(90)).toBe('2m')
    expect(formatAge(7200)).toBe('2h')
  })
  it('returns an em-dash when never fetched', () => {
    expect(formatAge(-1)).toBe('—')
  })
})
