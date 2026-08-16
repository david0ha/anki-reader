import { describe, it, expect } from '@jest/globals'
import { spawn, spawnSync } from 'node:child_process'
import { readFileSync } from 'node:fs'
import { createServer, request as httpRequest, type Server } from 'node:http'
import type { AddressInfo } from 'node:net'
import path from 'node:path'
import { SCREEN_COUNT, createEsp32Client } from './esp32'

// A fake `fetch` that replays a queue of responses (or throws a queued Error to simulate the
// SoftAP dropping). Records every call so we can assert URLs/methods/bodies.
type Reply = { ok?: boolean; status?: number; body?: unknown; jsonThrows?: boolean } | Error

function fakeFetch(replies: Reply[]) {
  const calls: Array<{ url: string; init?: RequestInit }> = []
  let i = 0
  const fetchImpl = (async (url: string, init?: RequestInit) => {
    calls.push({ url: String(url), init })
    const r = replies[Math.min(i, replies.length - 1)]
    i++
    if (r instanceof Error) throw r
    return {
      ok: r.ok ?? true,
      status: r.status ?? 200,
      json: async () => {
        if (r.jsonThrows) throw new SyntaxError('Unexpected token in JSON')
        return r.body
      },
    } as unknown as Response
  }) as unknown as typeof fetch
  return { fetchImpl, calls }
}

// Controllable clock so waitForConnected's polling is deterministic and instant.
function fakeClock() {
  let t = 0
  return {
    now: () => t,
    sleep: async (ms: number) => {
      t += ms
    },
  }
}

const BASE = 'http://192.168.4.1'
const MOCK = path.resolve(__dirname, '../../scripts/mock-esp32.js')
const FIXTURE = path.resolve(
  __dirname,
  '../../../components/vault_core/test/host/fixtures/kanji.json',
)

function client(replies: Reply[], extra: Record<string, unknown> = {}) {
  const f = fakeFetch(replies)
  return { ...f, client: createEsp32Client({ baseUrl: BASE, fetchImpl: f.fetchImpl, ...extra }) }
}

function runMock(mode: string, input?: unknown) {
  return spawnSync(process.execPath, [MOCK, mode], {
    input: input === undefined ? '' : JSON.stringify(input),
    encoding: 'utf8',
    timeout: 2000,
  })
}

// =====================================================================================
// Provisioning surface
// =====================================================================================

describe('esp32 client — getInfo', () => {
  it('parses device identity and trims the base URL', async () => {
    const { fetchImpl, calls } = fakeFetch([
      { body: { deviceId: '9F3A', model: 'AnkiReader', apSsid: 'AnkiReader-AB12' } },
    ])
    const c = createEsp32Client({ baseUrl: 'http://192.168.4.1/', fetchImpl })
    const info = await c.getInfo()
    expect(info).toEqual({
      deviceId: '9F3A',
      model: 'AnkiReader',
      // The setup AP is `<model>-<deviceId>`, so the network the learner joins, the model this
      // probe reads back and the mDNS host are one name and cannot drift apart.
      apSsid: 'AnkiReader-AB12',
      fw: '',
      ip: '',
    })
    expect(calls[0].url).toBe('http://192.168.4.1/api/info')
  })

  it('parses the STA-mode info (fw + ip present, apSsid empty)', async () => {
    const { client: c } = client([
      { body: { deviceId: '9F3A', model: 'AnkiReader', fw: '0.1.0', ip: '192.168.0.42' } },
    ])
    expect(await c.getInfo()).toEqual({
      deviceId: '9F3A',
      model: 'AnkiReader',
      apSsid: '',
      fw: '0.1.0',
      ip: '192.168.0.42',
    })
  })

  it('defaults missing fields to empty strings', async () => {
    const { client: c } = client([{ body: {} }])
    expect(await c.getInfo()).toEqual({ deviceId: '', model: '', apSsid: '', fw: '', ip: '' })
  })

  it('tolerates a null JSON body', async () => {
    const { client: c } = client([{ body: null }])
    expect(await c.getInfo()).toEqual({ deviceId: '', model: '', apSsid: '', fw: '', ip: '' })
  })

  it('rejects with http_error (carrying status) on a non-ok response', async () => {
    const { client: c } = client([{ ok: false, status: 500, body: {} }])
    await expect(c.getInfo()).rejects.toMatchObject({ code: 'http_error', status: 500 })
  })
})

describe('esp32 client — scanNetworks', () => {
  it('maps secure->secured, coerces rssi, and drops empty SSIDs', async () => {
    const { client: c } = client([
      {
        body: {
          networks: [
            { ssid: 'Home', rssi: -54, secure: true },
            { ssid: 'Cafe', rssi: -77, secure: false },
            { ssid: '', rssi: -90, secure: true },
          ],
        },
      },
    ])
    expect(await c.scanNetworks()).toEqual([
      { ssid: 'Home', rssi: -54, secured: true },
      { ssid: 'Cafe', rssi: -77, secured: false },
    ])
  })

  it('returns [] when the payload has no networks array', async () => {
    const { client: c } = client([{ body: {} }])
    expect(await c.scanNetworks()).toEqual([])
  })

  it('rejects with http_error on a non-ok response', async () => {
    const { client: c } = client([{ ok: false, status: 503, body: {} }])
    await expect(c.scanNetworks()).rejects.toMatchObject({ code: 'http_error' })
  })
})

describe('esp32 client — provision', () => {
  it('POSTs url-encoded ssid+password+study_url and resolves on 202', async () => {
    const { client: c, calls } = client([{ status: 202, body: { ok: true, state: 'connecting' } }])
    await c.provision('My Wi-Fi', 'p@ss&w/rd', 'http://mac.local:8123/kanji.json')
    expect(calls[0].url).toBe(`${BASE}/api/provision`)
    expect(calls[0].init?.method).toBe('POST')
    expect((calls[0].init?.headers as Record<string, string>)['Content-Type']).toBe(
      'application/x-www-form-urlencoded',
    )
    expect(calls[0].init?.body).toBe(
      'ssid=My%20Wi-Fi&password=p%40ss%26w%2Frd' +
        '&study_url=http%3A%2F%2Fmac.local%3A8123%2Fkanji.json',
    )
  })

  it('still sends an empty study_url when none was given', async () => {
    // Provisioning REWRITES the whole stored config on the board, so omitting the field would
    // clear the URL regardless. Sending '' states that intent instead of relying on it.
    const { client: c, calls } = client([{ status: 202, body: { ok: true } }])
    await c.provision('Home', 'pw')
    expect(calls[0].init?.body).toBe('ssid=Home&password=pw&study_url=')
  })

  it('throws an Esp32Error carrying the firmware error code on 400', async () => {
    const { client: c } = client([{ ok: false, status: 400, body: { ok: false, error: 'pass_too_long' } }])
    await expect(c.provision('Home', 'x')).rejects.toMatchObject({
      name: 'Esp32Error',
      code: 'pass_too_long',
    })
  })

  it('surfaces study_url_invalid from the provisioning endpoint', async () => {
    const { client: c } = client([
      { ok: false, status: 400, body: { ok: false, error: 'study_url_invalid' } },
    ])
    await expect(c.provision('Home', 'pw', 'ftp://nope')).rejects.toMatchObject({
      code: 'study_url_invalid',
    })
  })

  it('falls back to http_error when the 4xx body lacks an error field', async () => {
    const { client: c } = client([{ ok: false, status: 400, body: { ok: false } }])
    await expect(c.provision('Home', 'x')).rejects.toMatchObject({ code: 'http_error' })
  })

  it('falls back to http_error when the error body is not JSON (e.g. 413 plain text)', async () => {
    const { client: c } = client([{ ok: false, status: 413, jsonThrows: true }])
    await expect(c.provision('Home', 'x')).rejects.toMatchObject({ code: 'http_error', status: 413 })
  })

  it('maps a thrown fetch (AP dropped) to a network_error', async () => {
    const { client: c } = client([new TypeError('Network request failed')])
    await expect(c.provision('Home', 'x')).rejects.toMatchObject({ code: 'network_error' })
  })
})

describe('esp32 client — getStatus parsing', () => {
  it('defaults a missing state to idle', async () => {
    const { client: c } = client([{ body: {} }])
    expect(await c.getStatus()).toEqual({ state: 'idle', ssid: undefined, reason: undefined })
  })

  it('drops non-string ssid/reason', async () => {
    const { client: c } = client([{ body: { state: 'connecting', ssid: 123, reason: null } }])
    expect(await c.getStatus()).toEqual({ state: 'connecting', ssid: undefined, reason: undefined })
  })
})

describe('esp32 client — waitForConnected', () => {
  it('resolves connected once the board reports it', async () => {
    const clock = fakeClock()
    const { client: c } = client(
      [
        { body: { state: 'connecting' } },
        { body: { state: 'connecting' } },
        { body: { state: 'connected', ssid: 'Home' } },
      ],
      { now: clock.now, sleep: clock.sleep },
    )
    const res = await c.waitForConnected({ intervalMs: 1000, timeoutMs: 45000 })
    expect(res.outcome).toBe('connected')
    expect(res.ssid).toBe('Home')
  })

  it('resolves failed with the firmware reason', async () => {
    const clock = fakeClock()
    const { client: c } = client([{ body: { state: 'failed', ssid: 'Home', reason: 'auth_failed' } }], {
      now: clock.now,
      sleep: clock.sleep,
    })
    const res = await c.waitForConnected()
    expect(res.outcome).toBe('failed')
    expect(res.reason).toBe('auth_failed')
  })

  it('tolerates transient fetch failures (channel-hop AP drop) then succeeds', async () => {
    const clock = fakeClock()
    const { client: c } = client(
      [
        new TypeError('Network request failed'),
        new TypeError('Network request failed'),
        { body: { state: 'connected', ssid: 'Home' } },
      ],
      { now: clock.now, sleep: clock.sleep },
    )
    const res = await c.waitForConnected({ intervalMs: 1000 })
    expect(res.outcome).toBe('connected')
  })

  it('gives up with outcome=timeout once the overall deadline passes', async () => {
    const clock = fakeClock()
    const { client: c } = client([{ body: { state: 'connecting' } }], {
      now: clock.now,
      sleep: clock.sleep,
    })
    const res = await c.waitForConnected({ intervalMs: 1000, timeoutMs: 5000 })
    expect(res.outcome).toBe('timeout')
  })

  it('carries the last observed status on timeout and polls the expected number of times', async () => {
    const clock = fakeClock()
    const { client: c, calls } = client([{ body: { state: 'connecting', ssid: 'Home' } }], {
      now: clock.now,
      sleep: clock.sleep,
    })
    const res = await c.waitForConnected({ intervalMs: 1000, timeoutMs: 3000 })
    expect(res.outcome).toBe('timeout')
    expect(res.ssid).toBe('Home') // proves it returns the last status, not the {state:'connecting'} seed
    expect(calls.length).toBe(3) // polls at t=0,1000,2000 then exits at t=3000
  })
})

// =====================================================================================
// Control surface
// =====================================================================================

describe('esp32 client — getState', () => {
  // The documented payload from docs/app-control.md, verbatim.
  const FULL = {
    deviceId: '1A2B',
    model: 'AnkiReader',
    fw: '0.1.0',
    ip: '192.168.0.42',
    screen: 1,
    screenTitle: '정답',
    revealed: true,
    grade: 3,
    card: {
      valid: true,
      demo: false,
      front: '会う',
      reading: 'あう',
      meaning: '만나다',
      fsrsState: 'review',
      due: '9일 뒤',
      reps: 5,
      lapses: 1,
      stabilityDays: 9,
      difficultyPct: 47,
    },
    session: {
      deck: 'JLPT N5 Vocabulary',
      streak: 12,
      reviewedToday: 34,
      leftNew: 7,
      leftReview: 18,
      track: 35,
      trackTotal: 60,
      complete: false,
    },
    source: {
      url: 'http://mac.local:8123/kanji.json',
      lastResult: 'ok',
      pollSeconds: 300,
      ageSeconds: 42,
      stale: false,
    },
    battery: { present: true, percent: 84, millivolts: 4012 },
    panel: { partialChain: 3, fullRefreshMs: 4120, partialRefreshMs: 780 },
  }

  it('parses the documented payload', async () => {
    const { client: c, calls } = client([{ body: FULL }])
    const s = await c.getState()
    expect(calls[0].url).toBe(`${BASE}/api/state`)
    expect(s.deviceId).toBe('1A2B')
    expect(s.screen).toBe(1)
    expect(s.screenTitle).toBe('정답')
    expect(s.revealed).toBe(true)
    expect(s.grade).toBe(3)
    expect(s.card.front).toBe('会う')
    expect(s.card.fsrsState).toBe('review')
    expect(s.card.stabilityDays).toBe(9)
    expect(s.session.deck).toBe('JLPT N5 Vocabulary')
    expect(s.session.leftReview).toBe(18)
    expect(s.source.lastResult).toBe('ok')
    expect(s.source.ageSeconds).toBe(42)
    expect(s.battery).toEqual({ present: true, percent: 84, millivolts: 4012 })
    expect(s.panel).toEqual({ partialChain: 3, fullRefreshMs: 4120, partialRefreshMs: 780 })
  })

  it('renders an empty object as an empty session, not a crash', async () => {
    const { client: c } = client([{ body: {} }])
    const s = await c.getState()
    expect(s.screen).toBe(0)
    expect(s.card.valid).toBe(false)
    expect(s.session.reviewedToday).toBe(0)
    expect(s.battery.present).toBe(false)
    expect(s.panel.fullRefreshMs).toBe(0)
  })

  it('defaults a MISSING ageSeconds to -1, not 0', async () => {
    // -1 is "no poll has ever succeeded". Defaulting to 0 would draw a board that had just
    // synced when in fact it never has — the single most misleading thing this parser could do.
    const { client: c } = client([{ body: { source: { url: 'http://x/', lastResult: 'transport' } } }])
    const s = await c.getState()
    expect(s.source.ageSeconds).toBe(-1)
  })

  it('preserves an explicit ageSeconds of 0', async () => {
    const { client: c } = client([{ body: { source: { ageSeconds: 0 } } }])
    expect((await c.getState()).source.ageSeconds).toBe(0)
  })

  it('defaults MISSING stability/difficulty to -1 and preserves an explicit 0', async () => {
    // The firmware's own "not scheduled yet". A card whose stability is unknown and one whose
    // interval rounds to zero days are different states; only the second may print a number.
    const { client: c } = client([{ body: { card: { valid: true } } }])
    const unscheduled = (await c.getState()).card
    expect(unscheduled.stabilityDays).toBe(-1)
    expect(unscheduled.difficultyPct).toBe(-1)

    const { client: d } = client([
      { body: { card: { valid: true, stabilityDays: 0, difficultyPct: 0 } } },
    ])
    const sameDay = (await d.getState()).card
    expect(sameDay.stabilityDays).toBe(0)
    expect(sameDay.difficultyPct).toBe(0)
  })

  it('maps an unrecognised lastResult to "unknown" rather than passing it through', async () => {
    // A future firmware may add a code. The UI switches on this value, so an unknown one has to
    // land in a case the UI already handles.
    const { client: c } = client([{ body: { source: { lastResult: 'quantum_failure' } } }])
    expect((await c.getState()).source.lastResult).toBe('unknown')
  })

  it('keeps every documented lastResult value', async () => {
    for (const r of ['ok', 'no_url', 'transport', 'http_status', 'bad_payload']) {
      const { client: c } = client([{ body: { source: { lastResult: r } } }])
      expect((await c.getState()).source.lastResult).toBe(r)
    }
  })

  it('maps an unrecognised fsrsState to "unknown", and keeps the four wire words', async () => {
    for (const s of ['new', 'learning', 'review', 'relearning']) {
      const { client: c } = client([{ body: { card: { fsrsState: s } } }])
      expect((await c.getState()).card.fsrsState).toBe(s)
    }
    const { client: c } = client([{ body: { card: { fsrsState: '복습' } } }])
    // The board sends the wire word, not its own Korean label. Anything else is a firmware the
    // app was not built against, and must land in a case the UI already renders.
    expect((await c.getState()).card.fsrsState).toBe('unknown')
  })

  it('coerces garbage numbers to 0 instead of NaN', async () => {
    const { client: c } = client([
      {
        body: {
          screen: 'two',
          session: { reviewedToday: 'many', leftNew: null },
          battery: { percent: {} },
        },
      },
    ])
    const s = await c.getState()
    expect(s.screen).toBe(0)
    expect(s.session.reviewedToday).toBe(0)
    expect(s.session.leftNew).toBe(0)
    expect(s.battery.percent).toBe(0)
  })

  it('rejects with http_error on a non-ok response', async () => {
    const { client: c } = client([{ ok: false, status: 500, body: {} }])
    await expect(c.getState()).rejects.toMatchObject({ code: 'http_error', status: 500 })
  })
})

describe('esp32 client — setScreen', () => {
  it('POSTs the screen index as JSON', async () => {
    const { client: c, calls } = client([{ body: { ok: true } }])
    await c.setScreen(4)
    expect(calls[0].url).toBe(`${BASE}/api/screen`)
    expect(calls[0].init?.method).toBe('POST')
    expect((calls[0].init?.headers as Record<string, string>)['Content-Type']).toBe('application/json')
    expect(calls[0].init?.body).toBe('{"screen":4}')
  })

  it('throws screen_range on a 400 body', async () => {
    const { client: c } = client([{ ok: false, status: 400, body: { ok: false, error: 'screen_range' } }])
    await expect(c.setScreen(SCREEN_COUNT)).rejects.toMatchObject({ code: 'screen_range', status: 400 })
  })
})

describe('esp32 client — setStudyUrl', () => {
  it('POSTs the url as JSON', async () => {
    const { client: c, calls } = client([{ body: { ok: true } }])
    await c.setStudyUrl('http://mac.local:8123/kanji.json')
    expect(calls[0].url).toBe(`${BASE}/api/study`)
    expect(calls[0].init?.body).toBe('{"url":"http://mac.local:8123/kanji.json"}')
  })

  it('sends an empty string through — that is the "use the demo card" request', async () => {
    const { client: c, calls } = client([{ body: { ok: true } }])
    await c.setStudyUrl('')
    expect(calls[0].init?.body).toBe('{"url":""}')
  })

  it('throws study_url_invalid on a 400 body', async () => {
    const { client: c } = client([
      { ok: false, status: 400, body: { ok: false, error: 'study_url_invalid' } },
    ])
    await expect(c.setStudyUrl('nope')).rejects.toMatchObject({ code: 'study_url_invalid' })
  })
})

describe('esp32 client — refresh and displayTest', () => {
  it('POSTs /api/refresh with no body', async () => {
    const { client: c, calls } = client([{ body: { ok: true } }])
    await c.refresh()
    expect(calls[0].url).toBe(`${BASE}/api/refresh`)
    expect(calls[0].init?.method).toBe('POST')
    expect(calls[0].init?.body).toBeUndefined()
  })

  it('POSTs /api/display/test with no body', async () => {
    const { client: c, calls } = client([{ body: { ok: true } }])
    await c.displayTest()
    expect(calls[0].url).toBe(`${BASE}/api/display/test`)
    expect(calls[0].init?.body).toBeUndefined()
  })

  it('surfaces busy — the board is mid-refresh and the command was not queued', async () => {
    const { client: c } = client([{ ok: false, status: 400, body: { ok: false, error: 'busy' } }])
    await expect(c.refresh()).rejects.toMatchObject({ code: 'busy' })
    const { client: d } = client([{ ok: false, status: 400, body: { ok: false, error: 'busy' } }])
    await expect(d.displayTest()).rejects.toMatchObject({ code: 'busy' })
  })

  it('maps a dropped connection to network_error', async () => {
    const { client: c } = client([new TypeError('Network request failed')])
    await expect(c.refresh()).rejects.toMatchObject({ code: 'network_error' })
  })
})

// =====================================================================================
// The companion mock as a stand-in for the firmware's parser
// =====================================================================================

describe('companion mock — the study-card contract', () => {
  it('normalizes every visible field with the firmware capacities and list caps', () => {
    const result = runMock('--fingerprint', {
      session: {
        deck: 'D'.repeat(60),
        level: 'N5',
        streak: -3,
        reviewed_today: 1e9,
        left_new: 7.9,
        left_review: 18,
        retry: 2,
        track: 90,
        track_total: 60,
        complete: false,
      },
      card: {
        id: 'f00c539e-23f9-4294-bee1-c642189b105f',
        front: '会',
        reading: 'あ'.repeat(30),
        senses: ['만나다', '   ', '대면하다', '우연히 만나다', 'overflow'],
        examples: [
          { text: '出会う', reading: 'であう', gloss: '우연히 만나다' },
          { text: '  ', reading: 'x', gloss: 'y' },
          { text: '出会い', reading: 'であい', gloss: '만남' },
          { text: '会합', reading: '', gloss: '' },
          { text: 'overflow', reading: '', gloss: '' },
        ],
        comments: [{ author: '유키', body: '조사는 に', likes: -1 }],
        comment_total: 0,
        fsrs: { state: 'new', reps: 0, lapses: 0 },
        preview: { again: '10분 뒤', good: '9일 뒤' },
      },
    })

    expect(result.status).toBe(0)
    const normalized = JSON.parse(result.stdout)
    // 'D' is one byte, so the 40-byte deck buffer holds 39 of them.
    expect(normalized.session.deck).toHaveLength(39)
    expect(normalized.session.streak).toBe(0) // a negative counter clamps to zero
    expect(normalized.session.reviewed_today).toBe(9999) // COUNT_MAX: past this the digits are noise
    expect(normalized.session.left_new).toBe(7) // truncated, not rounded
    expect(normalized.session.track).toBe(60) // "TRK 90/60" is a producer bug printed at 14 px
    // かな is three bytes each, so a 64-byte reading buffer holds 21 of them.
    expect(normalized.card.reading).toHaveLength(21)
    expect(normalized.card.senses).toEqual(['만나다', '대면하다', '우연히 만나다'])
    expect(normalized.card.examples.map((e: { text: string }) => e.text)).toEqual([
      '出会う',
      '出会い',
      '会합',
    ])
    expect(normalized.card.comments[0].likes).toBe(0)
    // A total below the number of comments actually shown is arithmetic nobody should read past.
    expect(normalized.card.comment_total).toBe(1)
    // Absent FSRS numbers are "not scheduled yet", which is -1 and not 0.
    expect(normalized.card.fsrs.stability_days).toBe(-1)
    expect(normalized.card.fsrs.difficulty_pct).toBe(-1)
    expect(normalized.card.preview).toEqual(['10분 뒤', '', '9일 뒤', ''])
  })

  it('excludes the card id from the fingerprint — it is routing, not pixels', () => {
    // The same card re-served under a new study_card_id must not cost a full-panel flash.
    const fixture = JSON.parse(readFileSync(FIXTURE, 'utf8'))
    const original = runMock('--fingerprint', fixture)
    const reissued = runMock('--fingerprint', {
      ...fixture,
      card: { ...fixture.card, id: '00000000-0000-0000-0000-000000000000' },
    })
    expect(original.status).toBe(0)
    expect(reissued.stdout).toBe(original.stdout)
  })

  it('rejects exactly what the firmware rejects', () => {
    for (const payload of [
      // Neither a session nor a card: an error envelope or a captive-portal page, not a payload.
      { v: 1 },
      { v: 1, card: { front: '   ' } },
      // The root must be an object.
      [1, 2, 3],
      'a string',
    ]) {
      expect(runMock('--fingerprint', payload).status).toBe(1)
    }
  })

  it('keeps a session with no card — that is the completion screen, not a failure', () => {
    const result = runMock('--summarise', {
      session: { deck: 'JLPT N5 Vocabulary', streak: 12, reviewed_today: 34, complete: true },
    })
    expect(result.status).toBe(0)
    expect(JSON.parse(result.stdout)).toMatchObject({
      card: { valid: false, front: '', stabilityDays: -1 },
      session: { deck: 'JLPT N5 Vocabulary', streak: 12, reviewedToday: 34, complete: true },
    })
  })

  it('summarises a card the way device_api_json.c does', () => {
    const result = runMock('--summarise', JSON.parse(readFileSync(FIXTURE, 'utf8')))
    expect(result.status).toBe(0)
    expect(JSON.parse(result.stdout)).toEqual({
      card: {
        valid: true,
        demo: false,
        front: '会う',
        reading: 'あう',
        // The panel shows three senses; the phone gets the one that names the card.
        meaning: '만나다',
        fsrsState: 'review',
        due: '9일 뒤',
        reps: 5,
        lapses: 1,
        stabilityDays: 9,
        difficultyPct: 47,
      },
      session: {
        deck: 'JLPT N5 Vocabulary',
        streak: 12,
        reviewedToday: 34,
        leftNew: 7,
        leftReview: 18,
        track: 35,
        trackTotal: 60,
        complete: false,
      },
    })
  })

  it('serves the same demo card as kanji_mock.c and tools/mock_kanji_server.py', () => {
    // The fixture is what test_kanji_mock.c compares the C demo card against, so agreeing with it
    // by fingerprint is what keeps three copies of one card from drifting apart.
    const canonical = runMock('--fingerprint', JSON.parse(readFileSync(FIXTURE, 'utf8')))
    const builtIn = runMock('--demo-fingerprint')
    expect(canonical.status).toBe(0)
    expect(builtIn.status).toBe(0)
    expect(builtIn.stdout).toBe(canonical.stdout)
  })
})

describe('companion mock — as a running board', () => {
  it('records a full refresh when a scheduled poll changes the card', async () => {
    let sourceRequests = 0
    const source = createServer((_req, res) => {
      sourceRequests++
      res.writeHead(200, { 'Content-Type': 'application/json' })
      res.end(JSON.stringify(cardPayload(`会${sourceRequests}`)))
    })
    const reservation = createServer()
    let mock: ReturnType<typeof spawn> | undefined
    try {
      const sourcePort = await listenOnRandomPort(source)
      const mockPort = await listenOnRandomPort(reservation)
      await closeServer(reservation)
      mock = spawn(process.execPath, [MOCK], {
        env: { ...process.env, PORT: String(mockPort), POLL_SECONDS: '0.05' },
        stdio: ['ignore', 'pipe', 'pipe'],
      })
      let output = ''
      mock.stdout?.on('data', (chunk) => {
        output += String(chunk)
      })
      expect(await eventually(() => output.includes('mock AnkiReader listening'), 1000)).toBe(true)

      const baseUrl = `http://127.0.0.1:${mockPort}`
      const setResponse = await requestJson(`${baseUrl}/api/study`, 'POST', {
        url: `http://127.0.0.1:${sourcePort}/kanji.json`,
      })
      expect({ ...setResponse, output }).toMatchObject({ status: 200 })
      const initial = (await requestJson(`${baseUrl}/api/state`)).body
      expect(initial.card.valid).toBe(true)
      expect(initial.card.demo).toBe(false)
      const firstRefreshMs = initial.panel.fullRefreshMs

      expect(
        await eventually(async () => {
          const current = (await requestJson(`${baseUrl}/api/state`)).body
          return sourceRequests > 1 && current.panel.fullRefreshMs !== firstRefreshMs
        }, 1500),
      ).toBe(true)
    } finally {
      mock?.kill()
      await closeServer(source)
      if (reservation.listening) await closeServer(reservation)
    }
  }, 5000)

  it('drives the same nav state a button press does', async () => {
    const source = createServer((_req, res) => {
      res.writeHead(200, { 'Content-Type': 'application/json' })
      res.end(JSON.stringify(cardPayload('会う')))
    })
    const reservation = createServer()
    let mock: ReturnType<typeof spawn> | undefined
    try {
      const sourcePort = await listenOnRandomPort(source)
      const mockPort = await listenOnRandomPort(reservation)
      await closeServer(reservation)
      mock = spawn(process.execPath, [MOCK], {
        env: { ...process.env, PORT: String(mockPort), POLL_SECONDS: '300' },
        stdio: ['ignore', 'pipe', 'pipe'],
      })
      let output = ''
      mock.stdout?.on('data', (chunk) => {
        output += String(chunk)
      })
      expect(await eventually(() => output.includes('mock AnkiReader listening'), 1000)).toBe(true)

      const baseUrl = `http://127.0.0.1:${mockPort}`
      await requestJson(`${baseUrl}/api/study`, 'POST', {
        url: `http://127.0.0.1:${sourcePort}/kanji.json`,
      })

      // 정답 reveals the answer; 문제 hides it again. The screen is derived from that state, so
      // the two can never disagree about which of the five is up.
      await requestJson(`${baseUrl}/api/screen`, 'POST', { screen: 1 })
      let state = (await requestJson(`${baseUrl}/api/state`)).body
      expect(state).toMatchObject({ screen: 1, screenTitle: '정답', revealed: true, grade: 3 })

      await requestJson(`${baseUrl}/api/screen`, 'POST', { screen: 4 })
      state = (await requestJson(`${baseUrl}/api/state`)).body
      expect(state).toMatchObject({ screen: 4, screenTitle: 'FSRS' })

      await requestJson(`${baseUrl}/api/screen`, 'POST', { screen: 0 })
      state = (await requestJson(`${baseUrl}/api/state`)).body
      expect(state).toMatchObject({ screen: 0, screenTitle: '문제', revealed: false })

      const rejected = await requestJson(`${baseUrl}/api/screen`, 'POST', { screen: SCREEN_COUNT })
      expect(rejected).toMatchObject({ status: 400, body: { error: 'screen_range' } })
    } finally {
      mock?.kill()
      await closeServer(source)
      if (reservation.listening) await closeServer(reservation)
    }
  }, 5000)

  it('discards an old source response that finishes after the study URL changes', async () => {
    let oldRequests = 0
    const oldSource = createServer((_req, res) => {
      oldRequests++
      setTimeout(() => {
        res.writeHead(200, { 'Content-Type': 'application/json' })
        res.end(JSON.stringify(cardPayload('古い')))
      }, 150)
    })
    const newSource = createServer((_req, res) => {
      res.writeHead(200, { 'Content-Type': 'application/json' })
      res.end(JSON.stringify(cardPayload('新しい')))
    })
    const reservation = createServer()
    let mock: ReturnType<typeof spawn> | undefined
    try {
      const oldPort = await listenOnRandomPort(oldSource)
      const newPort = await listenOnRandomPort(newSource)
      const mockPort = await listenOnRandomPort(reservation)
      await closeServer(reservation)
      mock = spawn(process.execPath, [MOCK], {
        env: { ...process.env, PORT: String(mockPort), POLL_SECONDS: '300' },
        stdio: ['ignore', 'pipe', 'pipe'],
      })
      let output = ''
      mock.stdout?.on('data', (chunk) => {
        output += String(chunk)
      })
      expect(await eventually(() => output.includes('mock AnkiReader listening'), 1000)).toBe(true)

      const baseUrl = `http://127.0.0.1:${mockPort}`
      const oldPost = requestJson(`${baseUrl}/api/study`, 'POST', {
        url: `http://127.0.0.1:${oldPort}/kanji.json`,
      })
      expect(await eventually(() => oldRequests > 0, 1000)).toBe(true)
      await requestJson(`${baseUrl}/api/study`, 'POST', {
        url: `http://127.0.0.1:${newPort}/kanji.json`,
      })
      await oldPost

      const state = (await requestJson(`${baseUrl}/api/state`)).body
      expect(state.card.front).toBe('新しい')
      expect(state.source.url).toBe(`http://127.0.0.1:${newPort}/kanji.json`)
    } finally {
      mock?.kill()
      await closeServer(oldSource)
      await closeServer(newSource)
      if (reservation.listening) await closeServer(reservation)
    }
  }, 5000)
})

function cardPayload(front: string) {
  return {
    v: 1,
    session: {
      deck: 'JLPT N5 Vocabulary',
      level: 'N5',
      streak: 12,
      reviewed_today: 34,
      left_new: 7,
      left_review: 18,
      track: 35,
      track_total: 60,
      complete: false,
    },
    card: {
      id: 'f00c539e-23f9-4294-bee1-c642189b105f',
      front,
      reading: 'あう',
      level: 'N5',
      senses: ['만나다'],
      fsrs: { state: 'review', state_label: '복습', due: '9일 뒤', reps: 5, lapses: 1, stability_days: 9, difficulty_pct: 47 },
      preview: { again: '10분 뒤', hard: '4일 뒤', good: '9일 뒤', easy: '21일 뒤' },
    },
  }
}

function listenOnRandomPort(server: Server): Promise<number> {
  return new Promise((resolve, reject) => {
    server.once('error', reject)
    server.listen(0, '127.0.0.1', () => resolve((server.address() as AddressInfo).port))
  })
}

function closeServer(server: Server): Promise<void> {
  if (!server.listening) return Promise.resolve()
  return new Promise((resolve, reject) => server.close((error) => (error ? reject(error) : resolve())))
}

async function eventually(check: () => boolean | Promise<boolean>, timeoutMs: number): Promise<boolean> {
  const deadline = Date.now() + timeoutMs
  while (Date.now() < deadline) {
    if (await check()) return true
    await new Promise((resolve) => setTimeout(resolve, 20))
  }
  return false
}

function requestJson(
  rawUrl: string,
  method = 'GET',
  value?: unknown,
): Promise<{ status: number; body: any }> {
  return new Promise((resolve, reject) => {
    const url = new URL(rawUrl)
    const body = value === undefined ? '' : JSON.stringify(value)
    const req = httpRequest(
      {
        hostname: url.hostname,
        port: url.port,
        path: url.pathname,
        method,
        headers: body
          ? { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(body) }
          : {},
      },
      (res) => {
        let data = ''
        res.setEncoding('utf8')
        res.on('data', (chunk) => {
          data += chunk
        })
        res.on('end', () => resolve({ status: res.statusCode ?? 0, body: JSON.parse(data) }))
      },
    )
    req.on('error', reject)
    req.end(body)
  })
}
