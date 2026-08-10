#!/usr/bin/env node
// Mock Obsidian Board — exercises BOTH firmware HTTP APIs without hardware, so the full app flow
// (onboarding + the live dashboard) runs in a simulator/emulator.
//
// Implements the contract in docs/app-control.md:
//
//   Provisioning (firmware: components/provisioning/prov_portal.c)
//     GET  /api/info        -> { deviceId, model, apSsid }            (AP-mode identity)
//     GET  /api/scan        -> { networks: [{ ssid, rssi, secure }] }
//     POST /api/provision   (x-www-form-urlencoded: ssid, password, vault_url?) -> 202 | 4xx
//     GET  /api/status      -> { state, ssid?, reason? }
//
//   Control (firmware: components/device_api)
//     GET  /api/info          -> { deviceId, model, fw, ip }          (STA-mode identity)
//     GET  /api/state         -> the live snapshot
//     POST /api/refresh       -> poll the vault source now
//     POST /api/page          { page: 0..3 }
//     POST /api/vault         { url }
//     POST /api/display/test  -> "run" the panel sweep
//
// It is not a stub: when a vault URL is set, this really fetches it and summarises it exactly as
// the firmware's device_api_json.c would, including the three distinct failure codes. So pointing
// it at `python3 tools/mock_vault_server.py` exercises the whole chain the real board walks —
// producer, transport, parse, summary — with only the panel missing.
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

// Firmware limits (components/provisioning/prov_config.h).
const SSID_MAX_LEN = 32
const PASS_MAX_LEN = 64
const URL_MAX_LEN = 128
const PAGE_COUNT = 4
const POLL_SECONDS = 300

// The firmware's page titles (components/vault_core/include/ui_strings.h). The app shows this
// string as the ground truth for what is on the glass, so the mock has to serve the real ones.
const PAGE_TITLES = ['볼트 통계', '링크 그래프', '에이전트', '최근 노트']

// ---- Provisioning state ----
const prov = { state: 'idle', ssid: undefined, reason: undefined }
const INFO_AP = { deviceId: '9F3A', model: 'Obsidian Board', apSsid: 'Obsidian Board-9F3A' }
const NETWORKS = [
  { ssid: 'Home 5G', rssi: -48, secure: true },
  { ssid: 'Home 2.4G', rssi: -60, secure: true },
  { ssid: 'CoffeeShop Guest', rssi: -72, secure: false },
  { ssid: 'Home WiFi', rssi: -55, secure: true },
]

// ---- The built-in demo snapshot ----
// Mirrors components/vault_core/vault_mock.c, which the board renders when no URL is configured.
// Only the fields /api/state summarises are kept — this mock has no panel to draw the rest on.
const DEMO = {
  valid: true,
  demo: true,
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
}

// ---- Board state ----
const board = {
  page: 0,
  vaultUrl: '',
  vault: { ...DEMO },
  lastResult: 'no_url',
  // Epoch ms of the last SUCCESSFUL poll. null means none has ever succeeded, which /api/state
  // reports as ageSeconds -1 — a different fact from "0 seconds ago".
  lastOkAt: null,
  // Fake panel timings in the range the real 648x480 UC8179 lands in, so the dashboard's panel
  // card has something plausible to show. Zero would mean "not measured since boot".
  partialChain: 0,
  fullRefreshMs: 0,
  partialRefreshMs: 0,
}

// Mirrors prov_validate_vault_url() (components/provisioning/prov_config.c).
function validVaultUrl(url) {
  if (url === undefined || url === null || url === '') return true
  if (Buffer.byteLength(url, 'utf8') > URL_MAX_LEN) return false
  let rest
  if (url.startsWith('http://')) rest = url.slice(7)
  else if (url.startsWith('https://')) rest = url.slice(8)
  else return false
  return rest.length > 0 && !rest.startsWith('/')
}

// Summarise a parsed snapshot the way device_api_json.c does. Returns null if the payload is not
// a vault snapshot — the same judgement vault_parse.c makes (an object with no vault content in
// it is a rejection, not an empty vault).
function summarise(json) {
  if (json === null || typeof json !== 'object' || Array.isArray(json)) return null
  const stats = json.stats ?? {}
  const agents = Array.isArray(json.agents) ? json.agents : []
  const nodes = Array.isArray(json.graph?.nodes) ? json.graph.nodes : []
  const recent = Array.isArray(json.recent) ? json.recent : []
  const inbox = Array.isArray(json.inbox) ? json.inbox : []
  const notes = num(stats.notes)
  if (notes === 0 && agents.length === 0 && nodes.length === 0 && recent.length === 0 && inbox.length === 0) {
    return null
  }
  return {
    valid: true,
    demo: false,
    name: String(json.vault ?? ''),
    generatedAt: String(json.generated_at ?? ''),
    notes,
    links: num(stats.links),
    orphans: num(stats.orphans),
    tags: num(stats.tags),
    addedToday: num(stats.added_today),
    added7d: num(stats.added_7d),
    agents: agents.length,
    agentsRunning: agents.filter((a) => a?.state === 'running').length,
    recent: recent.length,
    inbox: num(json.inbox_total) || inbox.length,
  }
}

function num(v) {
  const n = Number(v)
  return Number.isFinite(n) ? Math.trunc(n) : 0
}

// One poll of the configured source, with the firmware's failure taxonomy: `transport` for
// DNS/connect/timeout, `http_status` for a non-2xx, `bad_payload` for a 2xx that is not a
// snapshot. A failure leaves the previous snapshot in place — blanking the board is the one
// failure a user actually notices.
async function pollVault() {
  if (!board.vaultUrl) {
    board.vault = { ...DEMO }
    board.lastResult = 'no_url'
    return
  }
  let res
  try {
    res = await fetch(board.vaultUrl, { signal: AbortSignal.timeout(8000) })
  } catch (e) {
    board.lastResult = 'transport'
    console.log(`   !! transport: ${e.message}`)
    return
  }
  if (res.status < 200 || res.status > 299) {
    board.lastResult = 'http_status'
    console.log(`   !! http_status: ${res.status}`)
    return
  }
  let json
  try {
    json = await res.json()
  } catch {
    board.lastResult = 'bad_payload'
    console.log('   !! bad_payload: not JSON')
    return
  }
  const summary = summarise(json)
  if (!summary) {
    board.lastResult = 'bad_payload'
    console.log('   !! bad_payload: JSON, but not a vault snapshot')
    return
  }
  board.vault = summary
  board.lastResult = 'ok'
  board.lastOkAt = Date.now()
  console.log(`   -> polled ${board.vaultUrl}: ${summary.notes} notes, ${summary.agents} agents`)
}

// Pretend to refresh the panel, recording a timing in the range the real panel lands in. The
// firmware promotes a partial to a full refresh at its chain cap; this mirrors that so the
// dashboard's "partials since full" counter behaves the way the board's does.
function fakeRefresh(kind) {
  if (kind === 'full') {
    board.fullRefreshMs = 3900 + Math.round(Math.random() * 500)
    board.partialChain = 0
  } else {
    board.partialRefreshMs = 700 + Math.round(Math.random() * 200)
    board.partialChain += 1
  }
}

function state() {
  return {
    deviceId: '9F3A',
    model: 'Obsidian Board',
    fw: '0.1.0',
    ip: `127.0.0.1:${PORT}`,
    page: board.page,
    pageTitle: PAGE_TITLES[board.page] ?? '',
    vault: { ...board.vault },
    source: {
      url: board.vaultUrl,
      lastResult: board.lastResult,
      pollSeconds: POLL_SECONDS,
      ageSeconds: board.lastOkAt === null ? -1 : Math.round((Date.now() - board.lastOkAt) / 1000),
      // The firmware badges what is on the glass as old once a couple of polls have failed in a
      // row; approximate that with twice the interval since the last success.
      stale: board.lastOkAt !== null && Date.now() - board.lastOkAt > POLL_SECONDS * 2000,
    },
    battery: { present: true, percent: 84, millivolts: 4012 },
    panel: {
      partialChain: board.partialChain,
      fullRefreshMs: board.fullRefreshMs,
      partialRefreshMs: board.partialRefreshMs,
    },
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

const server = http.createServer(async (req, res) => {
  const { method, url } = req
  console.log(`${new Date().toISOString().slice(11, 19)}  ${method} ${url}`)

  // ---- shared / provisioning GETs ----
  if (method === 'GET' && url === '/api/info') {
    // Serve the STA-mode identity (the control API also exposes /api/info). apSsid is included so
    // the onboarding probe is happy when this mock stands in for AP mode too.
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
    const form = parseForm(await readBody(req))
    const { ssid = '', password = '', vault_url: vaultUrl = '' } = form
    if (ssid.length === 0) return sendJson(res, 400, { ok: false, error: 'ssid_empty' })
    if (ssid.length > SSID_MAX_LEN) return sendJson(res, 400, { ok: false, error: 'ssid_too_long' })
    if (password.length > PASS_MAX_LEN) return sendJson(res, 400, { ok: false, error: 'pass_too_long' })
    if (!validVaultUrl(vaultUrl)) return sendJson(res, 400, { ok: false, error: 'vault_url_invalid' })

    prov.state = 'connecting'
    prov.ssid = ssid
    prov.reason = undefined
    console.log(
      `   -> connecting to "${ssid}" (password ${password ? 'set' : 'empty'}, vault_url "${vaultUrl}")`,
    )

    // Provisioning REWRITES the whole config on the real board, so an absent field clears the URL.
    board.vaultUrl = vaultUrl

    setTimeout(async () => {
      if (password === 'wrong') {
        prov.state = 'failed'
        prov.reason = 'auth_failed'
        console.log('   -> connect test FAILED (auth_failed)')
        return
      }
      prov.state = 'connected'
      console.log('   -> connect test OK')
      await pollVault()
      fakeRefresh('full')
    }, CONNECT_MS)

    return sendJson(res, 202, { ok: true, state: 'connecting' })
  }

  // ---- control ----
  if (method === 'GET' && url === '/api/state') {
    return sendJson(res, 200, state())
  }

  if (method === 'POST' && url === '/api/refresh') {
    const before = JSON.stringify(board.vault)
    await pollVault()
    // The board only touches the panel when the snapshot actually changed — that is the whole
    // point of the content hash, so the mock honours it rather than counting a refresh every time.
    if (JSON.stringify(board.vault) !== before) fakeRefresh('full')
    return sendJson(res, 200, { ok: true })
  }

  if (method === 'POST' && url === '/api/page') {
    let body
    try {
      body = JSON.parse(await readBody(req))
    } catch {
      return sendJson(res, 400, { ok: false, error: 'bad_json' })
    }
    if (typeof body?.page !== 'number') return sendJson(res, 400, { ok: false, error: 'bad_json' })
    if (body.page < 0 || body.page >= PAGE_COUNT) {
      return sendJson(res, 400, { ok: false, error: 'page_range' })
    }
    board.page = body.page
    fakeRefresh('full') // a page change is always a full refresh
    return sendJson(res, 200, { ok: true })
  }

  if (method === 'POST' && url === '/api/vault') {
    let body
    try {
      body = JSON.parse(await readBody(req))
    } catch {
      return sendJson(res, 400, { ok: false, error: 'bad_json' })
    }
    if (typeof body?.url !== 'string') return sendJson(res, 400, { ok: false, error: 'bad_json' })
    if (!validVaultUrl(body.url)) return sendJson(res, 400, { ok: false, error: 'vault_url_invalid' })
    board.vaultUrl = body.url
    if (!body.url) board.lastOkAt = null // back to the demo snapshot; nothing has been fetched
    await pollVault()
    fakeRefresh('full')
    return sendJson(res, 200, { ok: true })
  }

  if (method === 'POST' && url === '/api/display/test') {
    console.log('   -> panel self-test sweep (the real board is busy for ~a minute here)')
    fakeRefresh('full')
    return sendJson(res, 200, { ok: true })
  }

  sendJson(res, 404, { ok: false, error: 'not_found' })
})

// Poll on the board's own schedule too, so a dashboard left open sees the age tick and reset.
setInterval(() => {
  if (board.vaultUrl) pollVault()
}, POLL_SECONDS * 1000)

server.listen(PORT, () => {
  console.log(`mock Obsidian Board listening on http://localhost:${PORT}`)
  console.log(`  EXPO_PUBLIC_ESP32_BASE_URL=http://localhost:${PORT} npx expo start`)
  console.log('  no vault URL set yet — serving the built-in demo snapshot')
  console.log('  set one with:  curl -X POST http://localhost:%d/api/vault -d \'{"url":"..."}\'', PORT)
})
