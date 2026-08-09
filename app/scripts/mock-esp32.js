#!/usr/bin/env node
// Mock ESP32 Ticker Board server — exercises BOTH firmware HTTP APIs without hardware, so you can
// run the full app flow (onboarding + live control dashboard) in a simulator/emulator.
//
// Implements the contract in docs/app-control.md:
//
//   Provisioning (firmware: components/provisioning/prov_portal.c)
//     GET  /api/info        -> { deviceId, model, apSsid }            (AP-mode identity)
//     GET  /api/scan        -> { networks: [{ ssid, rssi, secure }] }
//     POST /api/provision   (x-www-form-urlencoded: ssid, password, tickers?, finnhub_key?, fmp_key?, econ_url?) -> 202 | 4xx
//     GET  /api/status      -> { state, ssid?, reason? }
//
//   Stock control (firmware: components/stock_api)
//     GET  /api/info            -> { deviceId, model, fw, ip }        (STA-mode identity)
//     GET  /api/stock/state     -> live snapshot (drifting quotes + keys presence)
//     POST /api/stock/select    { index } | { symbol }
//     POST /api/stock/page      { page }
//     POST /api/stock/econ      { mode, week? }
//     POST /api/stock/refresh   { all? }
//     POST /api/stock/watchlist { tickers: string[] | string }
//     POST /api/stock/keys      { finnhubKey?, fmpKey?, econUrl? }    (presence only; values not stored)
//     POST /api/stock/location  { location }                         ('' turns the weather widget off)
//
// Usage:
//   node scripts/mock-esp32.js               # listens on http://localhost:8080
//   PORT=9000 node scripts/mock-esp32.js     # custom port
// Then point the app at it (the iOS simulator / Android emulator can reach the host):
//   EXPO_PUBLIC_ESP32_BASE_URL=http://localhost:8080 npx expo start
//   (Android emulator: use http://10.0.2.2:8080)
//
// Provisioning test knobs:
//   - password "wrong"   -> connect test ends in state=failed (reason auth_failed)
//   - anything else      -> state=connected after ~3s
//   - CONNECT_MS=8000    -> override the connecting->connected/failed delay

const http = require('http')

const PORT = Number(process.env.PORT || 8080)
const CONNECT_MS = Number(process.env.CONNECT_MS || 3000)

// Firmware watchlist rules (mirrors prov_tickers_parse).
const TICKER_MAX_LEN = 12
const WATCHLIST_MAX = 16
const SYMBOL_RE = /^[A-Z0-9.-]+$/

// ---- Provisioning state ----
const prov = { state: 'idle', ssid: undefined, reason: undefined }
const INFO_AP = { deviceId: '9F3A', model: 'Ticker Board', apSsid: 'Ticker Board-9F3A' }
const NETWORKS = [
  { ssid: 'Home 5G', rssi: -48, secure: true },
  { ssid: 'Home 2.4G', rssi: -60, secure: true },
  { ssid: 'CoffeeShop Guest', rssi: -72, secure: false },
  { ssid: 'Home WiFi', rssi: -55, secure: true },
]

// ---- Stock-control state (mutable; quotes drift on each poll) ----
function makeTicker(symbol, base) {
  return { symbol, base, price: base, change: 0, percent: 0, ageSec: 0, valid: true, _born: Date.now() }
}
const stock = {
  index: 0,
  page: 0,
  econMode: false,
  econWeek: 0,
  refreshSeconds: 30,
  // Whether each data-source key/URL is configured. The mock (like the firmware) tracks presence
  // only — it never stores or returns the actual secret values.
  keys: { finnhub: false, fmp: false, econUrl: false },
  env: { valid: true, tempC: 24.3, humidity: 41, batteryValid: true, batteryV: 4.02, batteryPct: 88 },
  // Weather location (free-text, like the firmware) + the resolved current weather. Default: no
  // location set → weather invalid. Setting a location fakes a geocode + a drifting temperature.
  location: '',
  weather: { valid: false, tempC: 0, city: '' },
  tickers: [
    makeTicker('AAPL', 201.5),
    makeTicker('TSLA', 246.2),
    makeTicker('MSFT', 421.9),
    makeTicker('NVDA', 122.4),
  ],
}

// Random-walk each quote a little so the dashboard visibly updates as it polls.
function driftQuotes() {
  for (const t of stock.tickers) {
    const step = (Math.random() - 0.5) * (t.base * 0.004) // ±0.2%
    t.price = Math.max(0.01, t.price + step)
    t.change = t.price - t.base
    t.percent = (t.change / t.base) * 100
    t.ageSec = Math.round((Date.now() - t._born) / 1000) % 60
    t.valid = true
  }
  // Gently wander the sensors too.
  stock.env.tempC = +(stock.env.tempC + (Math.random() - 0.5) * 0.1).toFixed(2)
  stock.env.humidity = Math.min(99, Math.max(1, stock.env.humidity + (Math.random() - 0.5) * 0.4))
  // Drift the weather a touch as well so the dashboard chip visibly updates while it's valid.
  if (stock.weather.valid) {
    stock.weather.tempC = +(stock.weather.tempC + (Math.random() - 0.5) * 0.3).toFixed(1)
  }
}

// Fake the device's geocode + forecast: a non-empty location resolves to a "<Place>, KR" city and
// a plausible temperature; an empty location turns the weather widget off (matches the firmware).
function resolveWeather(loc) {
  const place = String(loc ?? '').trim()
  stock.location = place
  if (!place) {
    stock.weather = { valid: false, tempC: 0, city: '' }
    return
  }
  const city = place.includes(',') ? place : `${place}, KR`
  stock.weather = { valid: true, tempC: +(15 + Math.random() * 12).toFixed(1), city }
}

function stockState() {
  driftQuotes()
  return {
    model: 'Ticker Board',
    fw: '0.1.0',
    deviceId: '9F3A',
    index: stock.index,
    page: stock.page,
    econMode: stock.econMode,
    econWeek: stock.econWeek,
    refreshSeconds: stock.refreshSeconds,
    keys: { ...stock.keys },
    env: stock.env,
    location: stock.location,
    weather: { ...stock.weather },
    watchlist: stock.tickers.map((t) => ({
      symbol: t.symbol,
      valid: t.valid,
      price: +t.price.toFixed(2),
      change: +t.change.toFixed(2),
      percent: +t.percent.toFixed(2),
      ageSec: t.valid ? t.ageSec : -1,
    })),
  }
}

// ---- helpers ----
function sendJson(res, status, body) {
  res.writeHead(status, { 'Content-Type': 'application/json', Connection: 'close' })
  res.end(JSON.stringify(body))
}

function parseForm(body) {
  const out = {}
  for (const pair of body.split('&')) {
    if (!pair) continue
    const [k, v = ''] = pair.split('=')
    out[decodeURIComponent(k)] = decodeURIComponent(v.replace(/\+/g, ' '))
  }
  return out
}

function readBody(req) {
  return new Promise((resolve) => {
    let body = ''
    req.on('data', (c) => (body += c))
    req.on('end', () => resolve(body))
  })
}

// Normalize a watchlist payload (array | comma/space string) the SAME way the firmware does
// (stock_api.c api_watchlist_post + prov_tickers_parse):
//   - cap on the RAW token count (both forms) -> too_many_tickers
//   - silently DROP tokens that don't normalize ([A-Z0-9.-], <=12 chars) — NOT bad_json
//   - dedupe; empty result -> empty_watchlist
function parseTickers(raw) {
  const parts = (Array.isArray(raw) ? raw : String(raw ?? '').split(/[\s,]+/))
    .map((p) => String(p).trim())
    .filter((p) => p.length > 0)
  if (parts.length > WATCHLIST_MAX) return { error: 'too_many_tickers' }
  const out = []
  const seen = new Set()
  for (const p of parts) {
    const sym = p.toUpperCase()
    if (sym.length > TICKER_MAX_LEN || !SYMBOL_RE.test(sym)) continue // firmware drops, not errors
    if (seen.has(sym)) continue
    seen.add(sym)
    out.push(sym)
  }
  if (out.length === 0) return { error: 'empty_watchlist' }
  return { symbols: out }
}

// Update one key's PRESENCE from a form field (provisioning). `field` present in the form sets
// presence to (value !== '') — non-empty = set, empty = cleared; absent field = leave untouched.
function applyKey(presenceName, field, form) {
  if (!(field in form)) return
  stock.keys[presenceName] = form[field].length > 0
}

const server = http.createServer(async (req, res) => {
  const { method, url } = req
  console.log(`${new Date().toISOString().slice(11, 19)}  ${method} ${url}`)

  // ---- shared / provisioning GETs ----
  if (method === 'GET' && url === '/api/info') {
    // Serve the STA-mode identity (the stock API also exposes /api/info). Includes apSsid too so
    // the onboarding probe is happy when the mock stands in for AP mode.
    return sendJson(res, 200, { ...INFO_AP, fw: '0.1.0', ip: `127.0.0.1:${PORT}` })
  }
  if (method === 'GET' && url === '/api/scan') {
    return sendJson(res, 200, { networks: NETWORKS })
  }
  if (method === 'GET' && url === '/api/status') {
    return sendJson(res, 200, {
      state: prov.state,
      ...(prov.ssid ? { ssid: prov.ssid } : {}),
      ...(prov.reason ? { reason: prov.reason } : {}),
    })
  }

  if (method === 'POST' && url === '/api/provision') {
    const body = await readBody(req)
    const form = parseForm(body)
    const { ssid = '', password = '', tickers = '' } = form
    if (ssid.length === 0) return sendJson(res, 400, { ok: false, error: 'ssid_empty' })
    if (ssid.length > 32) return sendJson(res, 400, { ok: false, error: 'ssid_too_long' })
    if (password.length > 64) return sendJson(res, 400, { ok: false, error: 'pass_too_long' })

    prov.state = 'connecting'
    prov.ssid = ssid
    prov.reason = undefined
    console.log(`   -> connecting to "${ssid}" (password ${password ? 'set' : 'empty'}, tickers "${tickers}")`)

    // Optional data-source keys (NVS at provisioning time). A present field updates presence: a
    // non-empty value sets it, an empty value clears it (device reverts to its compiled default).
    // An omitted field leaves the current presence untouched.
    applyKey('finnhub', 'finnhub_key', form)
    applyKey('fmp', 'fmp_key', form)
    applyKey('econUrl', 'econ_url', form)

    // Optional weather location (NVS at provisioning time). A present field resolves it; an empty
    // value turns the widget off. An omitted field leaves the current location untouched.
    if ('location' in form) resolveWeather(form.location)

    // Apply the optional watchlist immediately (the firmware persists it during provisioning).
    if (tickers) {
      const parsed = parseTickers(tickers)
      if (parsed.symbols) {
        stock.tickers = parsed.symbols.map((s) => makeTicker(s, 100 + Math.random() * 300))
        stock.index = 0
      }
    }

    setTimeout(() => {
      if (password === 'wrong') {
        prov.state = 'failed'
        prov.reason = 'auth_failed'
        console.log('   -> FAILED (auth_failed)')
      } else {
        prov.state = 'connected'
        console.log('   -> CONNECTED (a real device would now reboot into station mode)')
      }
    }, CONNECT_MS)

    return sendJson(res, 202, { ok: true, state: 'connecting' })
  }

  // ---- stock control ----
  if (method === 'GET' && url === '/api/stock/state') {
    return sendJson(res, 200, stockState())
  }

  if (method === 'POST' && url === '/api/stock/select') {
    const body = await readBody(req)
    let j
    try {
      j = JSON.parse(body)
    } catch {
      return sendJson(res, 400, { ok: false, error: 'bad_json' })
    }
    if (typeof j.index === 'number') {
      if (j.index < 0 || j.index >= stock.tickers.length) {
        return sendJson(res, 400, { ok: false, error: 'index_range' })
      }
      stock.index = j.index
      return sendJson(res, 200, { ok: true })
    }
    if (typeof j.symbol === 'string') {
      const idx = stock.tickers.findIndex((t) => t.symbol === j.symbol.toUpperCase())
      if (idx < 0) return sendJson(res, 404, { ok: false, error: 'symbol_not_found' })
      stock.index = idx
      return sendJson(res, 200, { ok: true })
    }
    return sendJson(res, 400, { ok: false, error: 'bad_json' })
  }

  if (method === 'POST' && url === '/api/stock/page') {
    const body = await readBody(req)
    let j
    try {
      j = JSON.parse(body)
    } catch {
      return sendJson(res, 400, { ok: false, error: 'bad_json' })
    }
    if (typeof j.page !== 'number' || j.page < 0 || j.page > 3) {
      return sendJson(res, 400, { ok: false, error: 'page_range' })
    }
    stock.page = j.page
    return sendJson(res, 200, { ok: true })
  }

  if (method === 'POST' && url === '/api/stock/econ') {
    const body = await readBody(req)
    let j
    try {
      j = JSON.parse(body)
    } catch {
      return sendJson(res, 400, { ok: false, error: 'bad_json' })
    }
    stock.econMode = !!j.mode
    if (stock.econMode && typeof j.week === 'number') stock.econWeek = j.week
    if (!stock.econMode) stock.econWeek = 0
    return sendJson(res, 200, { ok: true })
  }

  if (method === 'POST' && url === '/api/stock/refresh') {
    const body = await readBody(req)
    try {
      JSON.parse(body || '{}')
    } catch {
      return sendJson(res, 400, { ok: false, error: 'bad_json' })
    }
    // "Refresh" simply re-bases the drift origin so quotes visibly jump.
    for (const t of stock.tickers) t._born = Date.now()
    return sendJson(res, 200, { ok: true })
  }

  if (method === 'POST' && url === '/api/stock/watchlist') {
    const body = await readBody(req)
    let j
    try {
      j = JSON.parse(body)
    } catch {
      return sendJson(res, 400, { ok: false, error: 'bad_json' })
    }
    const parsed = parseTickers(j.tickers)
    if (parsed.error) return sendJson(res, 400, { ok: false, error: parsed.error })
    // Preserve prices for symbols that stay on the list; seed new ones.
    const prev = new Map(stock.tickers.map((t) => [t.symbol, t]))
    stock.tickers = parsed.symbols.map((s) => prev.get(s) ?? makeTicker(s, 100 + Math.random() * 300))
    stock.index = Math.min(stock.index, stock.tickers.length - 1)
    console.log(`   -> watchlist set to [${parsed.symbols.join(', ')}]`)
    return sendJson(res, 200, { ok: true })
  }

  if (method === 'POST' && url === '/api/stock/keys') {
    const body = await readBody(req)
    let j
    try {
      j = JSON.parse(body)
    } catch {
      return sendJson(res, 400, { ok: false, error: 'bad_json' })
    }
    // Only the provided fields update presence (firmware semantics): a non-empty string sets the
    // key, an empty string clears it (revert to compiled default), an omitted field is untouched.
    if (typeof j.finnhubKey === 'string') stock.keys.finnhub = j.finnhubKey.length > 0
    if (typeof j.fmpKey === 'string') stock.keys.fmp = j.fmpKey.length > 0
    if (typeof j.econUrl === 'string') stock.keys.econUrl = j.econUrl.length > 0
    console.log(`   -> keys now ${JSON.stringify(stock.keys)}`)
    return sendJson(res, 200, { ok: true })
  }

  if (method === 'POST' && url === '/api/stock/location') {
    const body = await readBody(req)
    let j
    try {
      j = JSON.parse(body)
    } catch {
      return sendJson(res, 400, { ok: false, error: 'bad_json' })
    }
    if (typeof j.location !== 'string') {
      return sendJson(res, 400, { ok: false, error: 'bad_json' })
    }
    resolveWeather(j.location)
    console.log(`   -> location set to "${stock.location}" (weather ${stock.weather.valid ? stock.weather.city : 'off'})`)
    return sendJson(res, 200, { ok: true })
  }

  // Econ JSON is optional in the contract; advertise it as not-implemented.
  if (method === 'GET' && url === '/api/econ') {
    return sendJson(res, 404, { ok: false, error: 'not_implemented' })
  }

  sendJson(res, 404, { ok: false, error: 'not_found' })
})

server.listen(PORT, () => {
  console.log(`Mock Ticker Board on http://localhost:${PORT}`)
  console.log(`  point the app at it:  EXPO_PUBLIC_ESP32_BASE_URL=http://localhost:${PORT}`)
  console.log('  provisioning tip: enter password "wrong" to test the failure path.')
  console.log('  control: GET /api/stock/state drifts quotes; POST /api/stock/* mutates state.\n')
})
