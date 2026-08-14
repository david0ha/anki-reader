// Pure display formatters for the dashboard. Kept tiny and testable so the same number is
// rendered the same way everywhere and nothing throws on the board's loosely-typed JSON.

import type { FsrsState, StudyFetchResult } from './esp32'

/**
 * The five screens the board can be on, named as the board itself names them.
 *
 * These are Korean because the panel is: `kanji_screen_title()` prints exactly these strings in
 * the footer, and the phone showing "Answer" while the glass shows 정답 would leave the user
 * translating between two names for one screen.
 */
export const SCREEN_LABELS = ['문제', '정답', '설명', '댓글', 'FSRS'] as const

export function screenLabel(screen: number): string {
  return SCREEN_LABELS[screen] ?? `Screen ${screen}`
}

/**
 * The four FSRS ratings, in dock order, as the grade dock prints them. Indexed by the wire value
 * of `kanji_grade_t`, which starts at 1 (py-fsrs `Rating.Again`) — not at 0.
 */
export const GRADE_LABELS = ['다시', '어려움', '보통', '쉬움'] as const

export function gradeLabel(grade: number): string {
  return GRADE_LABELS[grade - 1] ?? '—'
}

/** Thousands-separated count. Returns '—' for non-finite. */
export function formatCount(value: number): string {
  if (!Number.isFinite(value)) return '—'
  return Math.round(value).toLocaleString('en-US')
}

/** Position in today's queue, e.g. "35 of 60". '—' before the board has a queue at all. */
export function formatTrack(track: number, total: number): string {
  if (!Number.isFinite(track) || !Number.isFinite(total) || total <= 0) return '—'
  return `${Math.round(track)} of ${Math.round(total)}`
}

/** How far through today's queue, 0..1. Zero when there is no queue, so a bar can always render. */
export function trackFraction(track: number, total: number): number {
  if (!Number.isFinite(track) || !Number.isFinite(total) || total <= 0) return 0
  return Math.max(0, Math.min(1, track / total))
}

/** Days in a row, e.g. "12 days" / "1 day". */
export function formatStreak(days: number): string {
  if (!Number.isFinite(days) || days < 0) return '—'
  const n = Math.round(days)
  return n === 1 ? '1 day' : `${n} days`
}

/**
 * When the card is next due.
 *
 * The board has no RTC, so this is a Korean span the proxy already worded against its own clock
 * ("9일 뒤", "곧"). It is passed through verbatim — re-deriving it here would need a timestamp
 * the contract deliberately never sends. An empty string means the scheduler has never seen this
 * card, which is a different fact from "due now".
 */
export function formatDue(due: string): string {
  const s = (due ?? '').trim()
  return s.length > 0 ? s : 'not scheduled'
}

/**
 * FSRS stability in days.
 *
 * -1 is the contract's "not scheduled yet" and must not render as a number: a card whose
 * stability is unknown and one whose interval rounds to zero days are different states, and only
 * the second of them is worth printing a 0 for.
 */
export function formatStability(days: number): string {
  if (!Number.isFinite(days) || days < 0) return '—'
  const n = Math.round(days)
  return n === 1 ? '1 day' : `${n} days`
}

/** FSRS difficulty as a percentage. -1 is "not scheduled yet", the same as stability. */
export function formatDifficulty(pct: number): string {
  if (!Number.isFinite(pct) || pct < 0) return '—'
  return `${Math.round(pct)}%`
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

export type Tone = 'up' | 'down' | 'warn' | 'neutral'

/** The scheduler state in English. The board sends the wire word, not its own Korean label. */
export function fsrsStateLabel(state: FsrsState): string {
  switch (state) {
    case 'new':
      return 'New card'
    case 'learning':
      return 'Learning'
    case 'review':
      return 'Review'
    case 'relearning':
      return 'Relearning'
    default:
      return 'Unknown'
  }
}

/**
 * Colour for the scheduler state. `relearning` is the only warning: it is the state a card lands
 * in after the learner has forgotten it, and it is the one worth noticing on a glance at the
 * phone. `new` is neutral — an unseen card is the normal start of every card's life.
 */
export function fsrsStateTone(state: FsrsState): Tone {
  switch (state) {
    case 'review':
      return 'up'
    case 'relearning':
      return 'warn'
    case 'new':
    case 'learning':
      return 'neutral'
    default:
      return 'warn'
  }
}

/** A sentence for each `source.lastResult`, saying what to go and check. */
export function fetchResultMessage(result: StudyFetchResult): string {
  switch (result) {
    case 'ok':
      return 'Last poll succeeded.'
    case 'no_url':
      return 'No study URL set — the board is showing its demo card.'
    case 'transport':
      return 'Couldn’t reach that address. Is the machine running kanji_server.py awake and on this network?'
    case 'http_status':
      return 'The server answered, but with an error. Check the path in the address.'
    case 'bad_payload':
      return 'The server answered with something that isn’t a study card.'
    default:
      return 'The board reported a result this app doesn’t recognise.'
  }
}

/** Short status word for the chip beside the deck name. */
export function fetchResultLabel(result: StudyFetchResult): string {
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

/**
 * Chip colour for a fetch result. `no_url` is deliberately neutral, not a warning: a board with no
 * URL is a complete, working product showing its demo card, not a broken one.
 */
export function fetchResultTone(result: StudyFetchResult): Tone {
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
