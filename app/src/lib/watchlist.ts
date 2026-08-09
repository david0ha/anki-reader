// Pure watchlist helpers mirroring the firmware's prov_tickers_parse / watchlist rules
// (docs/app-control.md): symbols are [A-Z0-9.-], <=12 chars, 1..16 symbols, de-duplicated.
// Kept separate from the screens so validation/normalization is unit-testable and identical
// between the onboarding "tickers" field and the live watchlist editor.

import { SYMBOL_RE, TICKER_MAX_LEN, WATCHLIST_MAX } from './esp32'

export { TICKER_MAX_LEN, WATCHLIST_MAX } from './esp32'

/** Uppercase + trim one raw symbol the user typed. Does not validate. */
export function normalizeSymbol(raw: string): string {
  return raw.trim().toUpperCase()
}

/** Whether a single normalized symbol is acceptable to the firmware. */
export function isValidSymbol(symbol: string): boolean {
  return symbol.length > 0 && symbol.length <= TICKER_MAX_LEN && SYMBOL_RE.test(symbol)
}

export type WatchlistError = 'empty' | 'too_many' | 'invalid_symbol'

export interface WatchlistResult {
  ok: boolean
  /** Normalized, de-duplicated symbol list when ok. */
  symbols?: string[]
  error?: WatchlistError
  /** The offending symbol for an invalid_symbol error. */
  offending?: string
}

/**
 * Validate + normalize a list of symbols (already split into individual entries). Uppercases,
 * drops blanks, de-duplicates preserving first-seen order, and enforces the count/charset limits.
 */
export function validateWatchlist(input: string[]): WatchlistResult {
  const out: string[] = []
  const seen = new Set<string>()
  for (const raw of input) {
    const sym = normalizeSymbol(raw)
    if (sym.length === 0) continue
    if (!isValidSymbol(sym)) return { ok: false, error: 'invalid_symbol', offending: sym }
    if (seen.has(sym)) continue
    seen.add(sym)
    out.push(sym)
  }
  if (out.length === 0) return { ok: false, error: 'empty' }
  if (out.length > WATCHLIST_MAX) return { ok: false, error: 'too_many' }
  return { ok: true, symbols: out }
}

/** Split a free-text "AAPL, TSLA  MSFT" field (used by onboarding) into raw entries. */
export function splitTickerText(text: string): string[] {
  return text.split(/[\s,]+/).filter((s) => s.length > 0)
}

/** Validate a free-text tickers field; '' is allowed (means "leave watchlist unchanged"). */
export function validateTickerText(text: string): WatchlistResult & { empty?: boolean } {
  const parts = splitTickerText(text)
  if (parts.length === 0) return { ok: true, symbols: [], empty: true }
  return validateWatchlist(parts)
}

/** Human-readable message for a watchlist validation failure. */
export function watchlistErrorMessage(r: WatchlistResult): string {
  switch (r.error) {
    case 'empty':
      return 'Add at least one symbol.'
    case 'too_many':
      return `Too many symbols (max ${WATCHLIST_MAX}).`
    case 'invalid_symbol':
      return `“${r.offending}” isn’t a valid symbol (use A–Z, 0–9, “.”, “-”, up to ${TICKER_MAX_LEN} chars).`
    default:
      return 'That watchlist isn’t valid.'
  }
}
