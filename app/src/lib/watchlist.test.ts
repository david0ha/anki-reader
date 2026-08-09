import { describe, it, expect } from '@jest/globals'
import {
  isValidSymbol,
  normalizeSymbol,
  splitTickerText,
  validateTickerText,
  validateWatchlist,
  watchlistErrorMessage,
} from './watchlist'

describe('normalizeSymbol / isValidSymbol', () => {
  it('uppercases and trims', () => {
    expect(normalizeSymbol('  aapl ')).toBe('AAPL')
  })

  it('accepts A-Z 0-9 . - up to 12 chars', () => {
    expect(isValidSymbol('BRK.B')).toBe(true)
    expect(isValidSymbol('BTC-USD')).toBe(true)
    expect(isValidSymbol('A')).toBe(true)
    expect(isValidSymbol('ABCDEFGHIJKL')).toBe(true) // exactly 12
  })

  it('rejects empty, too long, and bad characters', () => {
    expect(isValidSymbol('')).toBe(false)
    expect(isValidSymbol('ABCDEFGHIJKLM')).toBe(false) // 13
    expect(isValidSymbol('AA PL')).toBe(false)
    expect(isValidSymbol('aapl')).toBe(false) // lowercase isn't normalized here
    expect(isValidSymbol('A$B')).toBe(false)
  })
})

describe('validateWatchlist', () => {
  it('uppercases, de-duplicates (first-seen order), and drops blanks', () => {
    expect(validateWatchlist(['aapl', '', 'TSLA', 'aapl', '  msft '])).toEqual({
      ok: true,
      symbols: ['AAPL', 'TSLA', 'MSFT'],
    })
  })

  it('rejects an all-blank list as empty', () => {
    expect(validateWatchlist(['', '   '])).toMatchObject({ ok: false, error: 'empty' })
  })

  it('reports the offending invalid symbol', () => {
    expect(validateWatchlist(['AAPL', 'BAD!'])).toMatchObject({
      ok: false,
      error: 'invalid_symbol',
      offending: 'BAD!',
    })
  })

  it('rejects more than 16 symbols', () => {
    const many = Array.from({ length: 17 }, (_, i) => `S${i}`)
    expect(validateWatchlist(many)).toMatchObject({ ok: false, error: 'too_many' })
  })

  it('accepts exactly 16 symbols', () => {
    const sixteen = Array.from({ length: 16 }, (_, i) => `S${i}`)
    expect(validateWatchlist(sixteen).ok).toBe(true)
  })
})

describe('splitTickerText / validateTickerText', () => {
  it('splits on commas and whitespace', () => {
    expect(splitTickerText('AAPL, TSLA  MSFT\nNVDA')).toEqual(['AAPL', 'TSLA', 'MSFT', 'NVDA'])
  })

  it('treats empty text as a valid no-op (empty=true)', () => {
    expect(validateTickerText('   ')).toMatchObject({ ok: true, empty: true, symbols: [] })
  })

  it('validates non-empty text via validateWatchlist', () => {
    expect(validateTickerText('aapl tsla')).toEqual({ ok: true, symbols: ['AAPL', 'TSLA'] })
    expect(validateTickerText('aapl BAD!')).toMatchObject({ ok: false, error: 'invalid_symbol' })
  })
})

describe('watchlistErrorMessage', () => {
  it('produces a message per error code', () => {
    expect(watchlistErrorMessage({ ok: false, error: 'empty' })).toMatch(/at least one/i)
    expect(watchlistErrorMessage({ ok: false, error: 'too_many' })).toMatch(/max 16/i)
    expect(watchlistErrorMessage({ ok: false, error: 'invalid_symbol', offending: 'X$' })).toMatch(/X\$/)
  })
})
