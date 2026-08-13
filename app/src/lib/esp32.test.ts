import { describe, it, expect } from '@jest/globals'
import { spawn, spawnSync } from 'node:child_process'
import { readFileSync } from 'node:fs'
import { createServer, request as httpRequest, type Server } from 'node:http'
import type { AddressInfo } from 'node:net'
import path from 'node:path'
import { PAGE_COUNT, createEsp32Client } from './esp32'

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

const VALID_TAROT = {
  date: '2026-08-13',
  timezone: 'Asia/Seoul',
  card_id: 'major-02',
  orientation: 'upright',
  copy_version: 1,
  headline: ['고요히 살피면', '속뜻이 보인다'],
  flow: ['두 기둥 사이 장막이', '감춰진 단서를 품고 있다'],
  caution: ['모호한 느낌을', '사실로 단정하지 않는다'],
  action: ['답하기 전에', '침묵 속에서 한 번 읽는다'],
}

function client(replies: Reply[], extra: Record<string, unknown> = {}) {
  const f = fakeFetch(replies)
  return { ...f, client: createEsp32Client({ baseUrl: BASE, fetchImpl: f.fetchImpl, ...extra }) }
}

// =====================================================================================
// Provisioning surface
// =====================================================================================

describe('esp32 client — getInfo', () => {
  it('parses device identity and trims the base URL', async () => {
    const { fetchImpl, calls } = fakeFetch([
      { body: { deviceId: '9F3A', model: 'Obsidian Board', apSsid: 'Obsidian Board-AB12' } },
    ])
    const c = createEsp32Client({ baseUrl: 'http://192.168.4.1/', fetchImpl })
    const info = await c.getInfo()
    expect(info).toEqual({
      deviceId: '9F3A',
      model: 'Obsidian Board',
      apSsid: 'Obsidian Board-AB12',
      fw: '',
      ip: '',
    })
    expect(calls[0].url).toBe('http://192.168.4.1/api/info')
  })

  it('parses the STA-mode info (fw + ip present, apSsid empty)', async () => {
    const { client: c } = client([
      { body: { deviceId: '9F3A', model: 'Obsidian Board', fw: '0.1.0', ip: '192.168.0.42' } },
    ])
    expect(await c.getInfo()).toEqual({
      deviceId: '9F3A',
      model: 'Obsidian Board',
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
  it('POSTs url-encoded ssid+password+vault_url and resolves on 202', async () => {
    const { client: c, calls } = client([{ status: 202, body: { ok: true, state: 'connecting' } }])
    await c.provision('My Wi-Fi', 'p@ss&w/rd', 'http://mac.local:8123/vault.json')
    expect(calls[0].url).toBe(`${BASE}/api/provision`)
    expect(calls[0].init?.method).toBe('POST')
    expect((calls[0].init?.headers as Record<string, string>)['Content-Type']).toBe(
      'application/x-www-form-urlencoded',
    )
    expect(calls[0].init?.body).toBe(
      'ssid=My%20Wi-Fi&password=p%40ss%26w%2Frd' +
        '&vault_url=http%3A%2F%2Fmac.local%3A8123%2Fvault.json',
    )
  })

  it('still sends an empty vault_url when none was given', async () => {
    // Provisioning REWRITES the whole stored config on the board, so omitting the field would
    // clear the URL regardless. Sending '' states that intent instead of relying on it.
    const { client: c, calls } = client([{ status: 202, body: { ok: true } }])
    await c.provision('Home', 'pw')
    expect(calls[0].init?.body).toBe('ssid=Home&password=pw&vault_url=')
  })

  it('throws an Esp32Error carrying the firmware error code on 400', async () => {
    const { client: c } = client([{ ok: false, status: 400, body: { ok: false, error: 'pass_too_long' } }])
    await expect(c.provision('Home', 'x')).rejects.toMatchObject({
      name: 'Esp32Error',
      code: 'pass_too_long',
    })
  })

  it('surfaces vault_url_invalid from the provisioning endpoint', async () => {
    const { client: c } = client([
      { ok: false, status: 400, body: { ok: false, error: 'vault_url_invalid' } },
    ])
    await expect(c.provision('Home', 'pw', 'ftp://nope')).rejects.toMatchObject({
      code: 'vault_url_invalid',
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
    model: 'Obsidian Board',
    fw: '0.1.0',
    ip: '192.168.0.42',
    page: 0,
    pageTitle: 'Artwork',
    vault: {
      valid: true,
      demo: false,
      name: 'second-brain',
      generatedAt: '21:04',
      notes: 1428,
      links: 3910,
      orphans: 37,
      tags: 212,
      addedToday: 6,
      added7d: 41,
      agents: 5,
      agentsRunning: 2,
      recent: 8,
      inbox: 11,
    },
    source: {
      url: 'http://mac.local:8123/vault.json',
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
    expect(s.page).toBe(0)
    expect(s.pageTitle).toBe('Artwork')
    expect(s.vault.notes).toBe(1428)
    expect(s.vault.agentsRunning).toBe(2)
    expect(s.source.lastResult).toBe('ok')
    expect(s.source.ageSeconds).toBe(42)
    expect(s.battery).toEqual({ present: true, percent: 84, millivolts: 4012 })
    expect(s.panel).toEqual({ partialChain: 3, fullRefreshMs: 4120, partialRefreshMs: 780 })
  })

  it('renders an empty object as zeros, not a crash', async () => {
    const { client: c } = client([{ body: {} }])
    const s = await c.getState()
    expect(s.vault.notes).toBe(0)
    expect(s.vault.valid).toBe(false)
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

  it('coerces garbage numbers to 0 instead of NaN', async () => {
    const { client: c } = client([
      { body: { page: 'two', vault: { notes: 'many', links: null }, battery: { percent: {} } } },
    ])
    const s = await c.getState()
    expect(s.page).toBe(0)
    expect(s.vault.notes).toBe(0)
    expect(s.vault.links).toBe(0)
    expect(s.battery.percent).toBe(0)
  })

  it('normalizes legacy page indices to the single Artwork page', async () => {
    const { client: c } = client([{ body: { page: 3, pageTitle: '최근 노트' } }])
    const s = await c.getState()
    expect(PAGE_COUNT).toBe(1)
    expect(s.page).toBe(0)
    expect(s.pageTitle).toBe('Artwork')
  })

  it('rejects with http_error on a non-ok response', async () => {
    const { client: c } = client([{ ok: false, status: 500, body: {} }])
    await expect(c.getState()).rejects.toMatchObject({ code: 'http_error', status: 500 })
  })
})

describe('companion mock — schema-3 render fingerprints', () => {
  it('normalizes exactly the daily-tarot fields rendered by firmware', () => {
    const script = path.resolve(__dirname, '../../scripts/mock-esp32.js')
    const result = spawnSync(process.execPath, [script, '--tarot-fingerprint'], {
      input: JSON.stringify(VALID_TAROT),
      encoding: 'utf8',
      timeout: 1000,
    })

    expect(result.status).toBe(0)
    expect(JSON.parse(result.stdout)).toEqual(VALID_TAROT)
  })

  it('rejects tarot that firmware rejects, including reversed cards and a third copy row', () => {
    const script = path.resolve(__dirname, '../../scripts/mock-esp32.js')
    for (const tarot of [
      { ...VALID_TAROT, orientation: 'reversed' },
      { ...VALID_TAROT, flow: ['one', 'two', 'three'] },
      { ...VALID_TAROT, date: '2026-02-30' },
      { ...VALID_TAROT, card_id: 'major-22' },
      { ...VALID_TAROT, action: ['two\nrows'] },
      { ...VALID_TAROT, action: ['two\tcolumns'] },
    ]) {
      const result = spawnSync(process.execPath, [script, '--tarot-fingerprint'], {
        input: JSON.stringify(tarot),
        encoding: 'utf8',
        timeout: 1000,
      })
      expect(result.status).toBe(1)
    }
  })

  it('does not refresh pixels for a copy-version-only metadata change', () => {
    const script = path.resolve(__dirname, '../../scripts/mock-esp32.js')
    const run = (tarot: unknown) => spawnSync(
      process.execPath, [script, '--tarot-visible-fingerprint'], {
        input: JSON.stringify(tarot), encoding: 'utf8', timeout: 1000,
      },
    )
    const first = run(VALID_TAROT)
    const versionOnly = run({ ...VALID_TAROT, copy_version: 2 })
    expect(first.status).toBe(0)
    expect(versionOnly.status).toBe(0)
    expect(versionOnly.stdout).toBe(first.stdout)
  })

  it('normalizes every visible field with the firmware capacities and graph rules', () => {
    const artwork = {
      headline: ['h1', 'h2', 'headline overflow'],
      definition: {
        headword: 'word',
        meta: 'noun',
        lines: ['d1', 'd2', 'definition overflow'],
      },
      note: {
        title: 'focus',
        path: '00/focus.md',
        backlink_total: -5,
        backlinks: ['b1', '', 'b2', 'b3', 'backlink overflow'],
      },
      graph: {
        nodes: [
          { id: 2147483648, title: 'out-of-range id', slot: 0 },
          { id: 10, title: 'focus', slot: 5 },
          { id: 20, title: 'top', slot: 1 },
          { id: 30, title: 'duplicate slot', slot: 1 },
          { id: 40, title: 'invalid slot', slot: 9 },
          { id: 50, title: 'lower left', slot: 4 },
          { id: 60, title: 'lower right', slot: 5 },
          { id: 70, title: 'node overflow', slot: 1 },
        ],
        edges: [
          [10, 20], [20, 10], [10, 10], [10, 999],
          [10, 30], [10, 40], [10, 50], [10, 60],
          [20, 30], [30, 50], [40, 60], [20, 50],
        ],
      },
    }
    const script = path.resolve(__dirname, '../../scripts/mock-esp32.js')
    const result = spawnSync(process.execPath, [script, '--fingerprint'], {
      input: JSON.stringify(artwork),
      encoding: 'utf8',
      timeout: 1000,
    })

    expect(result.status).toBe(0)
    expect(JSON.parse(result.stdout)).toEqual({
      headline: ['h1', 'h2'],
      definition: { headword: 'word', meta: 'noun', lines: ['d1', 'd2'] },
      note: {
        title: 'focus',
        path: '00/focus.md',
        backlinkTotal: 3,
        backlinks: ['b1', 'b2', 'b3'],
      },
      nodes: [
        { title: 'focus', slot: 5 },
        { title: 'top', slot: 1 },
        { title: 'duplicate slot', slot: 0 },
        { title: 'invalid slot', slot: 2 },
        { title: 'lower left', slot: 4 },
        { title: 'lower right', slot: 3 },
      ],
      edges: [[0, 1], [0, 2], [0, 3], [0, 4], [0, 5], [1, 2], [1, 4], [2, 4]],
    })
  })

  it('preserves a declared focus and canonicalizes edges independently of wire order', () => {
    const script = path.resolve(__dirname, '../../scripts/mock-esp32.js')
    const run = (artwork: unknown) => {
      const result = spawnSync(process.execPath, [script, '--fingerprint'], {
        input: JSON.stringify(artwork),
        encoding: 'utf8',
        timeout: 1000,
      })
      expect(result.status).toBe(0)
      return JSON.parse(result.stdout)
    }
    const nodes = [
      { id: 10, title: 'left', slot: 2 },
      { id: 20, title: 'focus', slot: 0 },
      { id: 30, title: 'right', slot: 3 },
      { id: 40, title: 'lower', slot: 4 },
    ]
    const base = {
      headline: ['thought'],
      note: { title: 'focus' },
      graph: {
        nodes,
        edges: [[40, 30], [20, 10], [30, 10], [10, 20], [20, 40], [30, 20]],
      },
    }
    const reordered = {
      ...base,
      graph: { nodes, edges: [...base.graph.edges].reverse().map(([a, b]) => [b, a]) },
    }

    expect(run(base)).toEqual(run(reordered))
    expect(run(base).nodes).toEqual([
      { title: 'left', slot: 2 },
      { title: 'focus', slot: 0 },
      { title: 'right', slot: 3 },
      { title: 'lower', slot: 4 },
    ])
    expect(run(base).edges).toEqual([[0, 1], [0, 2], [1, 2], [1, 3], [2, 3]])
  })

  it('uses the firmware UTF-8 byte caps and excludes remapped wire ids', () => {
    const script = path.resolve(__dirname, '../../scripts/mock-esp32.js')
    const run = (artwork: unknown) => {
      const result = spawnSync(process.execPath, [script, '--fingerprint'], {
        input: JSON.stringify(artwork),
        encoding: 'utf8',
        timeout: 1000,
      })
      expect(result.status).toBe(0)
      return JSON.parse(result.stdout)
    }
    const base = {
      headline: ['x'.repeat(140)],
      definition: {
        headword: '가'.repeat(30),
        meta: 'm'.repeat(40),
        lines: ['d'.repeat(140)],
      },
      note: {
        title: 't'.repeat(80),
        path: 'p'.repeat(140),
        backlink_total: 1,
        backlinks: ['b'.repeat(80)],
      },
      graph: {
        nodes: [{ id: 10, title: 'n'.repeat(80), slot: 0 }, { id: 20, title: 'related', slot: 1 }],
        edges: [[10, 20]],
      },
    }
    const remapped = {
      ...base,
      graph: {
        nodes: [{ id: 101, title: 'n'.repeat(80), slot: 0 }, { id: 202, title: 'related', slot: 1 }],
        edges: [[101, 202]],
      },
    }

    const normalized = run(base)
    expect(normalized).toEqual(run(remapped))
    expect(normalized.headline[0]).toHaveLength(127)
    expect(normalized.definition.headword).toBe('가'.repeat(21))
    expect(normalized.definition.meta).toHaveLength(31)
    expect(normalized.definition.lines[0]).toHaveLength(127)
    expect(normalized.note.title).toHaveLength(63)
    expect(normalized.note.path).toHaveLength(127)
    expect(normalized.note.backlinks[0]).toHaveLength(63)
    expect(normalized.nodes[0].title).toHaveLength(63)
  })

  it('mirrors C string truncation at an escaped NUL code point', () => {
    const script = path.resolve(__dirname, '../../scripts/mock-esp32.js')
    const result = spawnSync(process.execPath, [script, '--fingerprint'], {
      input: JSON.stringify({
        headline: ['before\0after'],
        definition: { headword: 'word\0hidden', lines: [] },
        note: { title: 'focus\0hidden', backlinks: ['back\0hidden'] },
        graph: {
          nodes: [
            { id: 1, title: '\0discarded', slot: 1 },
            { id: 2, title: 'node\0hidden', slot: 0 },
          ],
          edges: [[1, 2]],
        },
      }),
      encoding: 'utf8',
      timeout: 1000,
    })

    expect(result.status).toBe(0)
    expect(JSON.parse(result.stdout)).toMatchObject({
      headline: ['before'],
      definition: { headword: 'word' },
      note: { title: 'focus', backlinks: ['back'] },
      nodes: [{ title: 'node', slot: 0 }],
      edges: [],
    })
  })

  it('clamps backlink totals to the firmware non-negative integer range', () => {
    const script = path.resolve(__dirname, '../../scripts/mock-esp32.js')
    const result = spawnSync(process.execPath, [script, '--fingerprint'], {
      input: JSON.stringify({
        headline: ['thought'],
        definition: { headword: 'connection', lines: [] },
        note: { title: 'focus', backlink_total: 1e100, backlinks: [] },
      }),
      encoding: 'utf8',
      timeout: 1000,
    })

    expect(result.status).toBe(0)
    expect(JSON.parse(result.stdout).note.backlinkTotal).toBe(2147483000)
  })

  it('rejects a schema-2 snapshot before it can replace the last artwork', () => {
    const script = path.resolve(__dirname, '../../scripts/mock-esp32.js')
    const result = spawnSync(process.execPath, [script, '--summarise'], {
      input: JSON.stringify({
        schema: 2,
        artwork: { headline: ['legacy'], note: { title: 'legacy focus' } },
      }),
      encoding: 'utf8',
      timeout: 1000,
    })

    expect(result.status).toBe(1)
  })

  it('accepts drawable schema-3 artwork for the companion summary', () => {
    const script = path.resolve(__dirname, '../../scripts/mock-esp32.js')
    const result = spawnSync(process.execPath, [script, '--summarise'], {
      input: JSON.stringify({
        schema: 3,
        vault: 'second-brain',
        artwork: { headline: ['current'], note: { title: 'focus' } },
      }),
      encoding: 'utf8',
      timeout: 1000,
    })

    expect(result.status).toBe(0)
    expect(JSON.parse(result.stdout)).toMatchObject({ valid: true, name: 'second-brain' })
  })

  it('uses the canonical schema-3 fingerprint for its built-in demo artwork', () => {
    const script = path.resolve(__dirname, '../../scripts/mock-esp32.js')
    const fixture = JSON.parse(
      readFileSync(
        path.resolve(__dirname, '../../../components/vault_core/test/host/fixtures/vault.json'),
        'utf8',
      ),
    )
    const canonical = spawnSync(process.execPath, [script, '--fingerprint'], {
      input: JSON.stringify(fixture.artwork),
      encoding: 'utf8',
      timeout: 1000,
    })
    const builtIn = spawnSync(process.execPath, [script, '--demo-fingerprint'], {
      encoding: 'utf8',
      timeout: 1000,
    })

    expect(canonical.status).toBe(0)
    expect(builtIn.status).toBe(0)
    expect(builtIn.stdout).toBe(canonical.stdout)
  })

  it('records a full refresh when a scheduled poll changes the visible tarot', async () => {
    let sourceRequests = 0
    const source = createServer((_req, res) => {
      sourceRequests++
      res.writeHead(200, { 'Content-Type': 'application/json' })
      res.end(JSON.stringify({
        schema: 3,
        vault: 'second-brain',
        daily_tarot: { ...VALID_TAROT, headline: [`오늘의 흐름 ${sourceRequests}`] },
        artwork: {
          headline: ['legacy artwork stays fixed'],
          definition: { headword: 'connection', meta: 'noun', lines: ['one thought meets another'] },
          note: { title: 'focus', path: 'focus.md', backlink_total: 0, backlinks: [] },
          graph: { nodes: [{ id: 0, title: 'focus', slot: 0 }], edges: [] },
        },
      }))
    })
    const reservation = createServer()
    let mock: ReturnType<typeof spawn> | undefined
    try {
      const sourcePort = await listenOnRandomPort(source)
      const mockPort = await listenOnRandomPort(reservation)
      await closeServer(reservation)
      const script = path.resolve(__dirname, '../../scripts/mock-esp32.js')
      mock = spawn(process.execPath, [script], {
        env: { ...process.env, PORT: String(mockPort), POLL_SECONDS: '0.05' },
        stdio: ['ignore', 'pipe', 'pipe'],
      })
      let output = ''
      mock.stdout?.on('data', (chunk) => { output += String(chunk) })
      expect(await eventually(() => output.includes('mock Obsidian Board listening'), 1000)).toBe(true)

      const baseUrl = `http://127.0.0.1:${mockPort}`
      const setResponse = await requestJson(`${baseUrl}/api/vault`, 'POST', {
        url: `http://127.0.0.1:${sourcePort}/vault.json`,
      })
      expect({ ...setResponse, output }).toMatchObject({ status: 200 })
      const initial = (await requestJson(`${baseUrl}/api/state`)).body
      const firstRefreshMs = initial.panel.fullRefreshMs

      expect(await eventually(async () => {
        const current = (await requestJson(`${baseUrl}/api/state`)).body
        return sourceRequests > 1 && current.panel.fullRefreshMs !== firstRefreshMs
      }, 1500)).toBe(true)
    } finally {
      mock?.kill()
      await closeServer(source)
      if (reservation.listening) await closeServer(reservation)
    }
  }, 5000)

  it('discards an old source response that finishes after the vault URL changes', async () => {
    let oldRequests = 0
    const snapshot = (name: string) => ({
      schema: 3,
      vault: name,
      daily_tarot: { ...VALID_TAROT, headline: [name] },
      artwork: {
        headline: [name],
        note: { title: name },
        graph: { nodes: [{ id: 0, title: name, slot: 0 }], edges: [] },
      },
    })
    const oldSource = createServer((_req, res) => {
      oldRequests++
      setTimeout(() => {
        res.writeHead(200, { 'Content-Type': 'application/json' })
        res.end(JSON.stringify(snapshot('old source')))
      }, 150)
    })
    const newSource = createServer((_req, res) => {
      res.writeHead(200, { 'Content-Type': 'application/json' })
      res.end(JSON.stringify(snapshot('new source')))
    })
    const reservation = createServer()
    let mock: ReturnType<typeof spawn> | undefined
    try {
      const oldPort = await listenOnRandomPort(oldSource)
      const newPort = await listenOnRandomPort(newSource)
      const mockPort = await listenOnRandomPort(reservation)
      await closeServer(reservation)
      const script = path.resolve(__dirname, '../../scripts/mock-esp32.js')
      mock = spawn(process.execPath, [script], {
        env: { ...process.env, PORT: String(mockPort), POLL_SECONDS: '300' },
        stdio: ['ignore', 'pipe', 'pipe'],
      })
      let output = ''
      mock.stdout?.on('data', (chunk) => { output += String(chunk) })
      expect(await eventually(() => output.includes('mock Obsidian Board listening'), 1000)).toBe(true)

      const baseUrl = `http://127.0.0.1:${mockPort}`
      const oldPost = requestJson(`${baseUrl}/api/vault`, 'POST', {
        url: `http://127.0.0.1:${oldPort}/vault.json`,
      })
      expect(await eventually(() => oldRequests > 0, 1000)).toBe(true)
      await requestJson(`${baseUrl}/api/vault`, 'POST', {
        url: `http://127.0.0.1:${newPort}/vault.json`,
      })
      await oldPost

      const state = (await requestJson(`${baseUrl}/api/state`)).body
      expect(state.vault.name).toBe('new source')
      expect(state.source.url).toBe(`http://127.0.0.1:${newPort}/vault.json`)
    } finally {
      mock?.kill()
      await closeServer(oldSource)
      await closeServer(newSource)
      if (reservation.listening) await closeServer(reservation)
    }
  }, 5000)
})

function listenOnRandomPort(server: Server): Promise<number> {
  return new Promise((resolve, reject) => {
    server.once('error', reject)
    server.listen(0, '127.0.0.1', () => resolve((server.address() as AddressInfo).port))
  })
}

function closeServer(server: Server): Promise<void> {
  if (!server.listening) return Promise.resolve()
  return new Promise((resolve, reject) => server.close((error) => error ? reject(error) : resolve()))
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
    const req = httpRequest({
      hostname: url.hostname,
      port: url.port,
      path: url.pathname,
      method,
      headers: body ? { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(body) } : {},
    }, (res) => {
      let data = ''
      res.setEncoding('utf8')
      res.on('data', (chunk) => { data += chunk })
      res.on('end', () => resolve({ status: res.statusCode ?? 0, body: JSON.parse(data) }))
    })
    req.on('error', reject)
    req.end(body)
  })
}

describe('esp32 client — setPage', () => {
  it('POSTs the single compatibility page as JSON', async () => {
    const { client: c, calls } = client([{ body: { ok: true } }])
    await c.setPage(0)
    expect(calls[0].url).toBe(`${BASE}/api/page`)
    expect(calls[0].init?.method).toBe('POST')
    expect((calls[0].init?.headers as Record<string, string>)['Content-Type']).toBe('application/json')
    expect(calls[0].init?.body).toBe('{"page":0}')
  })

  it('throws page_range on a 400 body', async () => {
    const { client: c } = client([{ ok: false, status: 400, body: { ok: false, error: 'page_range' } }])
    await expect(c.setPage(9)).rejects.toMatchObject({ code: 'page_range', status: 400 })
  })
})

describe('esp32 client — setVaultUrl', () => {
  it('POSTs the url as JSON', async () => {
    const { client: c, calls } = client([{ body: { ok: true } }])
    await c.setVaultUrl('http://mac.local:8123/vault.json')
    expect(calls[0].url).toBe(`${BASE}/api/vault`)
    expect(calls[0].init?.body).toBe('{"url":"http://mac.local:8123/vault.json"}')
  })

  it('sends an empty string through — that is the "use demo data" request', async () => {
    const { client: c, calls } = client([{ body: { ok: true } }])
    await c.setVaultUrl('')
    expect(calls[0].init?.body).toBe('{"url":""}')
  })

  it('throws vault_url_invalid on a 400 body', async () => {
    const { client: c } = client([
      { ok: false, status: 400, body: { ok: false, error: 'vault_url_invalid' } },
    ])
    await expect(c.setVaultUrl('nope')).rejects.toMatchObject({ code: 'vault_url_invalid' })
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
