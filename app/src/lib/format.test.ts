import { describe, it, expect } from '@jest/globals'
import {
  GRADE_LABELS,
  SCREEN_LABELS,
  fetchResultLabel,
  fetchResultMessage,
  fetchResultTone,
  formatAge,
  formatCount,
  formatDifficulty,
  formatDue,
  formatInterval,
  formatMs,
  formatStability,
  formatStreak,
  formatTrack,
  fsrsStateLabel,
  fsrsStateTone,
  gradeLabel,
  screenLabel,
  trackFraction,
} from './format'

describe('screenLabel', () => {
  it('names the five screens exactly as the board does', () => {
    // These come from kanji_screen_title() / ui_strings.h. The phone and the glass must not have
    // two names for one screen.
    expect([...SCREEN_LABELS]).toEqual(['문제', '정답', '설명', '댓글', 'FSRS'])
    expect(screenLabel(0)).toBe('문제')
    expect(screenLabel(4)).toBe('FSRS')
  })

  it('falls back for an out-of-range screen rather than rendering undefined', () => {
    expect(screenLabel(7)).toBe('Screen 7')
    expect(screenLabel(-1)).toBe('Screen -1')
  })
})

describe('gradeLabel', () => {
  it('indexes the dock from 1, as kanji_grade_t does', () => {
    // KANJI_GRADE_AGAIN is 1 (py-fsrs Rating.Again), not 0. Off by one here would label every
    // rating as the next one along.
    expect([...GRADE_LABELS]).toEqual(['다시', '어려움', '보통', '쉬움'])
    expect(gradeLabel(1)).toBe('다시')
    expect(gradeLabel(3)).toBe('보통')
    expect(gradeLabel(4)).toBe('쉬움')
  })

  it('renders an em dash for a value outside the four ratings', () => {
    expect(gradeLabel(0)).toBe('—')
    expect(gradeLabel(5)).toBe('—')
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

describe('formatTrack / trackFraction', () => {
  it('renders the place in today’s queue', () => {
    expect(formatTrack(35, 60)).toBe('35 of 60')
    expect(trackFraction(35, 60)).toBeCloseTo(35 / 60)
  })

  it('refuses to divide by an empty queue', () => {
    expect(formatTrack(0, 0)).toBe('—')
    expect(trackFraction(0, 0)).toBe(0)
  })

  it('clamps a track past its own total', () => {
    // The parser clamps this too, but a bar drawn past its track reads as a rendering bug.
    expect(trackFraction(90, 60)).toBe(1)
  })
})

describe('formatStreak', () => {
  it('agrees with itself about singular and plural', () => {
    expect(formatStreak(12)).toBe('12 days')
    expect(formatStreak(1)).toBe('1 day')
    expect(formatStreak(0)).toBe('0 days')
  })
})

describe('formatDue', () => {
  it('passes the proxy’s wording through — the board has no clock to re-derive it', () => {
    expect(formatDue('9일 뒤')).toBe('9일 뒤')
    expect(formatDue('곧')).toBe('곧')
  })

  it('says "not scheduled" for the empty span rather than showing a blank', () => {
    expect(formatDue('')).toBe('not scheduled')
    expect(formatDue('   ')).toBe('not scheduled')
  })
})

describe('formatStability / formatDifficulty', () => {
  it('treats -1 as "not scheduled yet", not as a number', () => {
    // A card with no stability and a card with a same-day interval are different states. Printing
    // 0 for the first claims the scheduler knows something it does not.
    expect(formatStability(-1)).toBe('—')
    expect(formatDifficulty(-1)).toBe('—')
  })

  it('keeps a genuine zero', () => {
    expect(formatStability(0)).toBe('0 days')
    expect(formatDifficulty(0)).toBe('0%')
  })

  it('renders ordinary values', () => {
    expect(formatStability(9)).toBe('9 days')
    expect(formatStability(1)).toBe('1 day')
    expect(formatDifficulty(47)).toBe('47%')
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

describe('FSRS state rendering', () => {
  it('names every wire state', () => {
    expect(fsrsStateLabel('new')).toBe('New card')
    expect(fsrsStateLabel('learning')).toBe('Learning')
    expect(fsrsStateLabel('review')).toBe('Review')
    expect(fsrsStateLabel('relearning')).toBe('Relearning')
    expect(fsrsStateLabel('unknown')).toBe('Unknown')
  })

  it('flags only the state the learner has actually forgotten', () => {
    // A new card is the ordinary start of a card's life, not a problem to surface.
    expect(fsrsStateTone('new')).toBe('neutral')
    expect(fsrsStateTone('learning')).toBe('neutral')
    expect(fsrsStateTone('review')).toBe('up')
    expect(fsrsStateTone('relearning')).toBe('warn')
    expect(fsrsStateTone('unknown')).toBe('warn')
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
    // A board with no URL is a complete product showing its demo card, not a failure.
    expect(fetchResultTone('no_url')).toBe('neutral')
    expect(fetchResultTone('ok')).toBe('up')
    expect(fetchResultTone('transport')).toBe('down')
    expect(fetchResultTone('http_status')).toBe('down')
    expect(fetchResultTone('bad_payload')).toBe('down')
    expect(fetchResultTone('unknown')).toBe('warn')
  })
})
