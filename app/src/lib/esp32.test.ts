import { describe, it, expect } from '@jest/globals'
import { createEsp32Client } from './esp32'

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
    page: 2,
    pageTitle: '에이전트',
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
    expect(s.page).toBe(2)
    // The board's page title is Korean; it must survive as-is, not be mangled or dropped.
    expect(s.pageTitle).toBe('에이전트')
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

  it('rejects with http_error on a non-ok response', async () => {
    const { client: c } = client([{ ok: false, status: 500, body: {} }])
    await expect(c.getState()).rejects.toMatchObject({ code: 'http_error', status: 500 })
  })
})

describe('esp32 client — setPage', () => {
  it('POSTs the page as JSON', async () => {
    const { client: c, calls } = client([{ body: { ok: true } }])
    await c.setPage(3)
    expect(calls[0].url).toBe(`${BASE}/api/page`)
    expect(calls[0].init?.method).toBe('POST')
    expect((calls[0].init?.headers as Record<string, string>)['Content-Type']).toBe('application/json')
    expect(calls[0].init?.body).toBe('{"page":3}')
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
