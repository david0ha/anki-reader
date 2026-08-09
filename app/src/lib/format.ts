// Pure display formatters for the dashboard. Kept tiny and testable so price/percent rendering
// is consistent everywhere and never throws on the device's loosely-typed numbers.

/** Page index → human label, matching the firmware's view order. */
export const PAGE_LABELS = ['Home', 'Chart', 'News', 'Metrics'] as const

export function pageLabel(page: number): string {
  return PAGE_LABELS[page] ?? `Page ${page}`
}

/** Price with a sensible number of decimals (2 by default). Returns '—' for non-finite. */
export function formatPrice(value: number, decimals = 2): string {
  if (!Number.isFinite(value)) return '—'
  return value.toFixed(decimals)
}

/** Signed change, e.g. "+1.20" / "-0.34". */
export function formatChange(value: number, decimals = 2): string {
  if (!Number.isFinite(value)) return '—'
  const sign = value > 0 ? '+' : ''
  return `${sign}${value.toFixed(decimals)}`
}

/** Signed percent, e.g. "+0.60%" / "-1.10%". */
export function formatPercent(value: number, decimals = 2): string {
  if (!Number.isFinite(value)) return '—'
  const sign = value > 0 ? '+' : ''
  return `${sign}${value.toFixed(decimals)}%`
}

export type Direction = 'up' | 'down' | 'flat'

export function direction(change: number): Direction {
  if (!Number.isFinite(change) || change === 0) return 'flat'
  return change > 0 ? 'up' : 'down'
}

/** "12s" / "3m" / "1h" age label; "—" when never fetched (ageSec < 0). */
export function formatAge(ageSec: number): string {
  if (!Number.isFinite(ageSec) || ageSec < 0) return '—'
  if (ageSec < 60) return `${Math.round(ageSec)}s`
  if (ageSec < 3600) return `${Math.round(ageSec / 60)}m`
  return `${Math.round(ageSec / 3600)}h`
}
