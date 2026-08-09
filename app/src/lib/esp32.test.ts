import { describe, it, expect } from '@jest/globals'
import { createEsp32Client, Esp32Error } from './esp32'

// A fake `fetch` that replays a queue of responses (or throws a queued Error to simulate the
// SoftAP dropping). Records every call so we can assert URLs/methods/bodies. Mirrors the
// masterham esp32.test.ts harness.
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
      { body: { deviceId: '9F3A', model: 'Ticker Board', apSsid: 'Ticker Board-AB12' } },
    ])
    const c = createEsp32Client({ baseUrl: 'http://192.168.4.1/', fetchImpl })
    const info = await c.getInfo()
    expect(info).toEqual({
      deviceId: '9F3A',
      model: 'Ticker Board',
      apSsid: 'Ticker Board-AB12',
      fw: '',
      ip: '',
    })
    expect(calls[0].url).toBe('http://192.168.4.1/api/info')
  })

  it('parses the STA-mode info (fw + ip present, apSsid empty)', async () => {
    const { client: c } = client([
      { body: { deviceId: '9F3A', model: 'Ticker Board', fw: '0.1.0', ip: '192.168.0.42' } },
    ])
    expect(await c.getInfo()).toEqual({
      deviceId: '9F3A',
      model: 'Ticker Board',
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
  it('POSTs url-encoded ssid+password and resolves on 202', async () => {
    const { client: c, calls } = client([{ status: 202, body: { ok: true, state: 'connecting' } }])
    await c.provision('My Wi-Fi', 'p@ss&w/rd')
    expect(calls[0].url).toBe(`${BASE}/api/provision`)
    expect(calls[0].init?.method).toBe('POST')
    expect((calls[0].init?.headers as Record<string, string>)['Content-Type']).toBe(
      'application/x-www-form-urlencoded',
    )
    expect(calls[0].init?.body).toBe('ssid=My%20Wi-Fi&password=p%40ss%26w%2Frd')
  })

  it('appends an url-encoded tickers field when provided', async () => {
    const { client: c, calls } = client([{ status: 202, body: { ok: true } }])
    await c.provision('Home', 'pw', 'AAPL, TSLA')
    expect(calls[0].init?.body).toBe('ssid=Home&password=pw&tickers=AAPL%2C%20TSLA')
  })

  it('omits the tickers field when empty', async () => {
    const { client: c, calls } = client([{ status: 202, body: { ok: true } }])
    await c.provision('Home', 'pw', '')
    expect(calls[0].init?.body).toBe('ssid=Home&password=pw')
  })

  it('appends url-encoded key fields when provided in opts', async () => {
    const { client: c, calls } = client([{ status: 202, body: { ok: true } }])
    await c.provision('Home', 'pw', 'AAPL', {
      finnhubKey: 'fh key/1',
      fmpKey: 'fmp&2',
      econUrl: 'http://proxy.local:8442/calendar',
    })
    expect(calls[0].init?.body).toBe(
      'ssid=Home&password=pw&tickers=AAPL' +
        '&finnhub_key=fh%20key%2F1&fmp_key=fmp%262' +
        '&econ_url=http%3A%2F%2Fproxy.local%3A8442%2Fcalendar',
    )
  })

  it('omits a key field that is undefined but sends an empty-string (clear) field', async () => {
    const { client: c, calls } = client([{ status: 202, body: { ok: true } }])
    await c.provision('Home', 'pw', undefined, { finnhubKey: 'k', fmpKey: '' })
    // fmp_key is sent (empty = clear); econ_url is omitted (undefined = leave untouched).
    expect(calls[0].init?.body).toBe('ssid=Home&password=pw&finnhub_key=k&fmp_key=')
  })

  it('appends an url-encoded location field when provided in opts', async () => {
    const { client: c, calls } = client([{ status: 202, body: { ok: true } }])
    await c.provision('Home', 'pw', undefined, { location: 'Paris, FR' })
    expect(calls[0].init?.body).toBe('ssid=Home&password=pw&location=Paris%2C%20FR')
  })

  it('omits the location field when not provided in opts', async () => {
    const { client: c, calls } = client([{ status: 202, body: { ok: true } }])
    await c.provision('Home', 'pw', undefined, { finnhubKey: 'k' })
    expect(calls[0].init?.body).toBe('ssid=Home&password=pw&finnhub_key=k')
  })

  it('throws an Esp32Error carrying the firmware error code on 400', async () => {
    const { client: c } = client([{ ok: false, status: 400, body: { ok: false, error: 'pass_too_long' } }])
    await expect(c.provision('Home', 'x')).rejects.toMatchObject({
      name: 'Esp32Error',
      code: 'pass_too_long',
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
  it('resolves connected once the device reports it', async () => {
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
    const { client: c } = client(
      [{ body: { state: 'failed', ssid: 'Home', reason: 'auth_failed' } }],
      { now: clock.now, sleep: clock.sleep },
    )
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
// Stock-control surface
// =====================================================================================

describe('esp32 client — getState', () => {
  const FULL = {
    model: 'Ticker Board',
    fw: '0.1.0',
    deviceId: '9F3A',
    index: 1,
    page: 2,
    econMode: true,
    econWeek: 1,
    refreshSeconds: 30,
    keys: { finnhub: true, fmp: false, econUrl: true },
    env: {
      valid: true,
      tempC: 24.3,
      humidity: 41.0,
      batteryValid: true,
      batteryV: 4.02,
      batteryPct: 88,
    },
    location: 'Seoul',
    weather: { valid: true, tempC: 21, city: 'Seoul, KR' },
    watchlist: [
      { symbol: 'AAPL', valid: true, price: 201.5, change: 1.2, percent: 0.6, ageSec: 12 },
      { symbol: 'TSLA', valid: false, price: 0, change: 0, percent: 0, ageSec: -1 },
    ],
  }

  it('parses a full snapshot exactly', async () => {
    const { client: c, calls } = client([{ body: FULL }])
    const st = await c.getState()
    expect(calls[0].url).toBe(`${BASE}/api/stock/state`)
    expect(st).toEqual(FULL)
  })

  it('defaults a missing env to an all-zero/invalid block', async () => {
    const { client: c } = client([{ body: { ...FULL, env: undefined } }])
    const st = await c.getState()
    expect(st.env).toEqual({
      valid: false,
      tempC: 0,
      humidity: 0,
      batteryValid: false,
      batteryV: 0,
      batteryPct: 0,
    })
  })

  it('returns an empty watchlist when the array is missing', async () => {
    const { client: c } = client([{ body: { ...FULL, watchlist: undefined } }])
    expect((await c.getState()).watchlist).toEqual([])
  })

  it('coerces a partial ticker (missing ageSec -> -1, missing numbers -> 0)', async () => {
    const { client: c } = client([{ body: { watchlist: [{ symbol: 'NVDA', valid: true }] } }])
    const st = await c.getState()
    expect(st.watchlist[0]).toEqual({
      symbol: 'NVDA',
      valid: true,
      price: 0,
      change: 0,
      percent: 0,
      ageSec: -1,
    })
  })

  it('parses the keys presence block', async () => {
    const { client: c } = client([
      { body: { ...FULL, keys: { finnhub: false, fmp: true, econUrl: false } } },
    ])
    expect((await c.getState()).keys).toEqual({ finnhub: false, fmp: true, econUrl: false })
  })

  it('defaults a missing keys block to all-false', async () => {
    const { client: c } = client([{ body: { ...FULL, keys: undefined } }])
    expect((await c.getState()).keys).toEqual({ finnhub: false, fmp: false, econUrl: false })
  })

  it('parses the location and resolved weather block', async () => {
    const { client: c } = client([
      { body: { ...FULL, location: 'Paris', weather: { valid: true, tempC: 14, city: 'Paris, FR' } } },
    ])
    const st = await c.getState()
    expect(st.location).toBe('Paris')
    expect(st.weather).toEqual({ valid: true, tempC: 14, city: 'Paris, FR' })
  })

  it('defaults a missing location to "" and a missing weather block to invalid/zero/empty', async () => {
    const { client: c } = client([{ body: { ...FULL, location: undefined, weather: undefined } }])
    const st = await c.getState()
    expect(st.location).toBe('')
    expect(st.weather).toEqual({ valid: false, tempC: 0, city: '' })
  })

  it('rejects with http_error on a non-ok response', async () => {
    const { client: c } = client([{ ok: false, status: 500, body: {} }])
    await expect(c.getState()).rejects.toMatchObject({ code: 'http_error' })
  })
})

describe('esp32 client — select', () => {
  it('POSTs an index body as JSON and resolves on 200 {ok:true}', async () => {
    const { client: c, calls } = client([{ body: { ok: true } }])
    await c.select({ index: 2 })
    expect(calls[0].url).toBe(`${BASE}/api/stock/select`)
    expect(calls[0].init?.method).toBe('POST')
    expect((calls[0].init?.headers as Record<string, string>)['Content-Type']).toBe('application/json')
    expect(calls[0].init?.body).toBe('{"index":2}')
  })

  it('POSTs a symbol body as JSON', async () => {
    const { client: c, calls } = client([{ body: { ok: true } }])
    await c.select({ symbol: 'TSLA' })
    expect(calls[0].init?.body).toBe('{"symbol":"TSLA"}')
  })

  it('throws symbol_not_found on a 404 body', async () => {
    const { client: c } = client([{ ok: false, status: 404, body: { ok: false, error: 'symbol_not_found' } }])
    await expect(c.select({ symbol: 'ZZZZ' })).rejects.toMatchObject({ code: 'symbol_not_found' })
  })

  it('throws index_range on a 400 body', async () => {
    const { client: c } = client([{ ok: false, status: 400, body: { ok: false, error: 'index_range' } }])
    await expect(c.select({ index: 99 })).rejects.toMatchObject({ code: 'index_range' })
  })
})

describe('esp32 client — setPage', () => {
  it('POSTs {page} and resolves', async () => {
    const { client: c, calls } = client([{ body: { ok: true } }])
    await c.setPage(1)
    expect(calls[0].url).toBe(`${BASE}/api/stock/page`)
    expect(calls[0].init?.body).toBe('{"page":1}')
  })

  it('throws page_range on a 400 body', async () => {
    const { client: c } = client([{ ok: false, status: 400, body: { ok: false, error: 'page_range' } }])
    await expect(c.setPage(7)).rejects.toMatchObject({ code: 'page_range' })
  })
})

describe('esp32 client — setEcon', () => {
  it('includes the week (defaulting to 0) when enabling', async () => {
    const { client: c, calls } = client([{ body: { ok: true } }])
    await c.setEcon(true)
    expect(calls[0].init?.body).toBe('{"mode":true,"week":0}')
  })

  it('passes through a non-zero week when enabling', async () => {
    const { client: c, calls } = client([{ body: { ok: true } }])
    await c.setEcon(true, 2)
    expect(calls[0].init?.body).toBe('{"mode":true,"week":2}')
  })

  it('omits the week when disabling', async () => {
    const { client: c, calls } = client([{ body: { ok: true } }])
    await c.setEcon(false)
    expect(calls[0].init?.body).toBe('{"mode":false}')
  })
})

describe('esp32 client — refresh', () => {
  it('POSTs {all:false} by default', async () => {
    const { client: c, calls } = client([{ body: { ok: true } }])
    await c.refresh()
    expect(calls[0].url).toBe(`${BASE}/api/stock/refresh`)
    expect(calls[0].init?.body).toBe('{"all":false}')
  })

  it('POSTs {all:true} when requested', async () => {
    const { client: c, calls } = client([{ body: { ok: true } }])
    await c.refresh(true)
    expect(calls[0].init?.body).toBe('{"all":true}')
  })
})

describe('esp32 client — setWatchlist', () => {
  it('POSTs an array body', async () => {
    const { client: c, calls } = client([{ body: { ok: true } }])
    await c.setWatchlist(['AAPL', 'TSLA'])
    expect(calls[0].url).toBe(`${BASE}/api/stock/watchlist`)
    expect(calls[0].init?.body).toBe('{"tickers":["AAPL","TSLA"]}')
  })

  it('POSTs a string body', async () => {
    const { client: c, calls } = client([{ body: { ok: true } }])
    await c.setWatchlist('AAPL,TSLA')
    expect(calls[0].init?.body).toBe('{"tickers":"AAPL,TSLA"}')
  })

  it('throws empty_watchlist on a 400 body', async () => {
    const { client: c } = client([{ ok: false, status: 400, body: { ok: false, error: 'empty_watchlist' } }])
    await expect(c.setWatchlist([])).rejects.toMatchObject({ code: 'empty_watchlist' })
  })

  it('throws too_many_tickers on a 400 body', async () => {
    const { client: c } = client([{ ok: false, status: 400, body: { ok: false, error: 'too_many_tickers' } }])
    await expect(c.setWatchlist(Array(20).fill('A'))).rejects.toMatchObject({ code: 'too_many_tickers' })
  })

  it('throws bad_json and carries the status when the body is malformed', async () => {
    const { client: c } = client([{ ok: false, status: 400, body: { ok: false, error: 'bad_json' } }])
    await expect(c.setWatchlist('')).rejects.toMatchObject({ code: 'bad_json', status: 400 })
  })

  it('maps a thrown fetch to network_error', async () => {
    const { client: c } = client([new TypeError('Network request failed')])
    await expect(c.setWatchlist(['AAPL'])).rejects.toMatchObject({ code: 'network_error' })
  })
})

describe('esp32 client — setKeys', () => {
  it('POSTs only the provided fields as JSON and resolves on 200 {ok:true}', async () => {
    const { client: c, calls } = client([{ body: { ok: true } }])
    await c.setKeys({ finnhubKey: 'abc', econUrl: 'http://proxy.local' })
    expect(calls[0].url).toBe(`${BASE}/api/stock/keys`)
    expect(calls[0].init?.method).toBe('POST')
    expect((calls[0].init?.headers as Record<string, string>)['Content-Type']).toBe('application/json')
    // fmpKey omitted (undefined); the two provided fields are present.
    expect(calls[0].init?.body).toBe('{"finnhubKey":"abc","econUrl":"http://proxy.local"}')
  })

  it('sends an empty string as a clear request (does not drop it)', async () => {
    const { client: c, calls } = client([{ body: { ok: true } }])
    await c.setKeys({ fmpKey: '' })
    expect(calls[0].init?.body).toBe('{"fmpKey":""}')
  })

  it('throws bad_json carrying the status on a 400 body', async () => {
    const { client: c } = client([{ ok: false, status: 400, body: { ok: false, error: 'bad_json' } }])
    await expect(c.setKeys({ finnhubKey: 'x' })).rejects.toMatchObject({ code: 'bad_json', status: 400 })
  })

  it('maps a thrown fetch to network_error', async () => {
    const { client: c } = client([new TypeError('Network request failed')])
    await expect(c.setKeys({ finnhubKey: 'x' })).rejects.toMatchObject({ code: 'network_error' })
  })
})

describe('esp32 client — setLocation', () => {
  it('POSTs {location} as JSON and resolves on 200 {ok:true}', async () => {
    const { client: c, calls } = client([{ body: { ok: true } }])
    await c.setLocation('Seoul')
    expect(calls[0].url).toBe(`${BASE}/api/stock/location`)
    expect(calls[0].init?.method).toBe('POST')
    expect((calls[0].init?.headers as Record<string, string>)['Content-Type']).toBe('application/json')
    expect(calls[0].init?.body).toBe('{"location":"Seoul"}')
  })

  it('sends an empty string (clear) as a valid request', async () => {
    const { client: c, calls } = client([{ body: { ok: true } }])
    await c.setLocation('')
    expect(calls[0].init?.body).toBe('{"location":""}')
  })

  it('throws bad_json carrying the status on a 400 body', async () => {
    const { client: c } = client([{ ok: false, status: 400, body: { ok: false, error: 'bad_json' } }])
    await expect(c.setLocation('!!')).rejects.toMatchObject({ code: 'bad_json', status: 400 })
  })

  it('maps a thrown fetch to network_error', async () => {
    const { client: c } = client([new TypeError('Network request failed')])
    await expect(c.setLocation('Seoul')).rejects.toMatchObject({ code: 'network_error' })
  })
})

// =====================================================================================
// Cross-cutting
// =====================================================================================

describe('esp32 client — base URL normalization', () => {
  it('strips multiple trailing slashes', async () => {
    const { fetchImpl, calls } = fakeFetch([{ body: {} }])
    const c = createEsp32Client({ baseUrl: 'http://192.168.4.1///', fetchImpl })
    await c.getInfo()
    expect(calls[0].url).toBe('http://192.168.4.1/api/info')
  })

  it('falls back to the default base URL when none is provided', async () => {
    const { fetchImpl, calls } = fakeFetch([{ body: {} }])
    const c = createEsp32Client({ fetchImpl })
    await c.getInfo()
    expect(calls[0].url).toBe('http://192.168.4.1/api/info')
  })
})

describe('Esp32Error', () => {
  it('exposes the code and is an Error', () => {
    const e = new Esp32Error('ssid_empty')
    expect(e).toBeInstanceOf(Error)
    expect(e.code).toBe('ssid_empty')
  })

  it('carries an optional HTTP status', () => {
    const e = new Esp32Error('http_error', 'boom', 502)
    expect(e.status).toBe(502)
  })
})
