// Pure display formatters for the dashboard. Kept tiny and testable so the same number is
// rendered the same way everywhere and nothing throws on the board's loosely-typed JSON.

import type { VaultFetchResult } from './esp32'

/**
 * Page index → the app's label, matching the firmware's page order (ui_vault.c).
 *
 * The board also reports its own `pageTitle`, in Korean, which is what is actually on the glass.
 * These are the app's English equivalents for the page switcher: a control the user presses should
 * read in the app's language, and the board's own title is shown separately next to the page.
 */
export const PAGE_LABELS = ['Stats', 'Graph', 'Agents', 'Notes'] as const

export function pageLabel(page: number): string {
  return PAGE_LABELS[page] ?? `Page ${page}`
}

/** Thousands-separated count. Returns '—' for non-finite. */
export function formatCount(value: number): string {
  if (!Number.isFinite(value)) return '—'
  return Math.round(value).toLocaleString('en-US')
}

/** Signed delta for the "+N today" lines, e.g. "+6" / "0". */
export function formatDelta(value: number): string {
  if (!Number.isFinite(value)) return '—'
  const n = Math.round(value)
  return n > 0 ? `+${n}` : String(n)
}

/**
 * One decimal of percent, e.g. "2.6%". `total` of zero yields '—' rather than NaN or a division
 * by zero — an empty vault has no orphan *rate*, and "0.0%" would claim it has a good one.
 */
export function formatRatio(part: number, total: number): string {
  if (!Number.isFinite(part) || !Number.isFinite(total) || total <= 0) return '—'
  return `${((part / total) * 100).toFixed(1)}%`
}

/** Links per note to one decimal — the vault's connectedness. '—' on an empty vault. */
export function formatDensity(links: number, notes: number): string {
  if (!Number.isFinite(links) || !Number.isFinite(notes) || notes <= 0) return '—'
  return (links / notes).toFixed(1)
}

/**
 * "12s" / "3m" / "1h ago" style age for `source.ageSeconds`.
 *
 * -1 is the board's "no poll has ever succeeded", which is a different fact from "0 seconds ago"
 * and must not render as one — a board that has never reached its server would otherwise look
 * freshly synced.
 */
export function formatAge(ageSec: number): string {
  if (!Number.isFinite(ageSec) || ageSec < 0) return 'never'
  if (ageSec < 60) return `${Math.round(ageSec)}s ago`
  if (ageSec < 3600) return `${Math.round(ageSec / 60)}m ago`
  if (ageSec < 86400) return `${Math.round(ageSec / 3600)}h ago`
  return `${Math.round(ageSec / 86400)}d ago`
}

/** Poll interval as "every 5m" / "every 45s". */
export function formatInterval(seconds: number): string {
  if (!Number.isFinite(seconds) || seconds <= 0) return '—'
  if (seconds < 60) return `every ${Math.round(seconds)}s`
  const m = seconds / 60
  return `every ${Number.isInteger(m) ? m : m.toFixed(1)}m`
}

/**
 * A measured panel timing. Zero means the firmware has not run that kind of refresh since boot,
 * which is "not measured yet" — printing "0 ms" would read as an impossibly fast panel.
 */
export function formatMs(ms: number): string {
  if (!Number.isFinite(ms) || ms <= 0) return '—'
  if (ms < 1000) return `${Math.round(ms)} ms`
  return `${(ms / 1000).toFixed(1)} s`
}

/** A sentence for each `source.lastResult`, saying what to go and check. */
export function fetchResultMessage(result: VaultFetchResult): string {
  switch (result) {
    case 'ok':
      return 'Last poll succeeded.'
    case 'no_url':
      return 'No vault URL set — the board is showing its demo snapshot.'
    case 'transport':
      return 'Couldn’t reach that address. Is the machine serving it awake and on this network?'
    case 'http_status':
      return 'The server answered, but with an error. Check the path in the address.'
    case 'bad_payload':
      return 'The server answered with something that isn’t a vault snapshot.'
    default:
      return 'The board reported a result this app doesn’t recognise.'
  }
}

/** Short status word for the chip beside the vault name. */
export function fetchResultLabel(result: VaultFetchResult): string {
  switch (result) {
    case 'ok':
      return 'synced'
    case 'no_url':
      return 'demo'
    case 'transport':
      return 'unreachable'
    case 'http_status':
      return 'server error'
    case 'bad_payload':
      return 'bad payload'
    default:
      return 'unknown'
  }
}

export type Tone = 'up' | 'down' | 'warn' | 'neutral'

/**
 * Chip colour for a fetch result. `no_url` is deliberately neutral, not a warning: a board with no
 * URL is a complete, working product showing its demo snapshot, not a broken one.
 */
export function fetchResultTone(result: VaultFetchResult): Tone {
  switch (result) {
    case 'ok':
      return 'up'
    case 'no_url':
      return 'neutral'
    case 'transport':
    case 'http_status':
    case 'bad_payload':
      return 'down'
    default:
      return 'warn'
  }
}
