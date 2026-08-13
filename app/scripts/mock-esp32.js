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
//     POST /api/page          { page: 0 }
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
//   node scripts/mock-esp32.js --fingerprint # normalize one artwork object from stdin and exit
//   node scripts/mock-esp32.js --tarot-fingerprint # normalize one daily_tarot object and exit
//   node scripts/mock-esp32.js --tarot-visible-fingerprint # hash its pixel-driving fields
//   node scripts/mock-esp32.js --summarise   # validate/summarize one schema-3 snapshot from stdin
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
const POLL_SECONDS = Number(process.env.POLL_SECONDS || 300)

// The control API retains one page title for compatibility with existing companion clients.
const PAGE_TITLES = ['Artwork']

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
const DEMO_ARTWORK = {
  headline: ['기억한 것은', '남아 있다.'],
  definition: {
    headword: '우연한 연결',
    meta: '명사',
    lines: ['서로 멀리 있던 생각이 만나', '새로운 방향을 만드는 순간.'],
  },
  note: {
    title: '우연한 연결',
    path: '00 Daily/2026-08-13.md',
    backlink_total: 6,
    backlinks: ['아이디어', 'MOC/연구', '프로젝트/보드'],
  },
  graph: {
    nodes: [
      { id: 0, title: '우연한\n연결', slot: 0 },
      { id: 1, title: '아이디어', slot: 1 },
      { id: 2, title: 'MOC/연구', slot: 2 },
      { id: 3, title: '프로젝트/보드', slot: 3 },
      { id: 4, title: '논문', slot: 4 },
      { id: 5, title: 'ESP32', slot: 5 },
    ],
    edges: [[0, 1], [0, 2], [0, 3], [0, 4], [0, 5], [1, 2], [2, 4], [3, 5]],
  },
}
const DEMO_TAROT = {
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

// ---- Board state ----
const board = {
  page: 0,
  vaultUrl: '',
  sourceGeneration: 0,
  vault: { ...DEMO },
  tarotFingerprint: '',
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

// Summarise a parsed snapshot the way device_api_json.c does. Schema 3 requires a drawable
// normalized artwork object; a rejection preserves the last image on the real panel.
function summarise(json) {
  if (json === null || typeof json !== 'object' || Array.isArray(json)) return null
  if (json.schema !== 3) return null
  const stats = json.stats ?? {}
  const agents = Array.isArray(json.agents) ? json.agents : []
  const nodes = Array.isArray(json.graph?.nodes) ? json.graph.nodes : []
  const recent = Array.isArray(json.recent) ? json.recent : []
  const inbox = Array.isArray(json.inbox) ? json.inbox : []
  const notes = num(stats.notes)
  const normalizedArtwork = normalizeArtwork(json.artwork)
  const hasProse = normalizedArtwork.headline.length > 0 ||
    normalizedArtwork.definition.headword.length > 0 ||
    normalizedArtwork.definition.lines.length > 0
  if (!normalizedArtwork.note.title || !hasProse) return null
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

function utf8Text(value, maxBytes) {
  if (typeof value !== 'string') return ''
  let result = ''
  let bytes = 0
  for (const glyph of value) {
    if (glyph === '\0') break
    const glyphBytes = Buffer.byteLength(glyph, 'utf8')
    if (bytes + glyphBytes > maxBytes) break
    result += glyph
    bytes += glyphBytes
  }
  return result
}

// Normalize only fields consumed by ui_artwork.c. Keeping the same caps and graph rules as the C
// parser makes JSON.stringify of this object a companion-side visible-content fingerprint.
function normalizeArtwork(artwork) {
  const wireInt = (value, fallback = 0) =>
    typeof value === 'number' && Number.isFinite(value) ? Math.trunc(value) : fallback
  const exactInt = (value) => Number.isInteger(value) && value >= -2147483648 && value <= 2147483647
  const unsignedWireInt = (value) => Math.max(0, Math.min(2147483000, wireInt(value)))
  const text = utf8Text
  const lines = (value, cap) => {
    const result = []
    for (const line of Array.isArray(value) ? value : []) {
      if (result.length >= cap) break
      const normalized = text(line, 127)
      if (normalized) result.push(normalized)
    }
    return result
  }
  const definition = artwork?.definition && typeof artwork.definition === 'object' &&
    !Array.isArray(artwork.definition) ? artwork.definition : {}
  const note = artwork?.note && typeof artwork.note === 'object' && !Array.isArray(artwork.note)
    ? artwork.note : {}
  const backlinks = []
  for (const backlink of Array.isArray(note.backlinks) ? note.backlinks : []) {
    if (backlinks.length >= 3) break
    const normalized = text(backlink, 63)
    if (normalized) backlinks.push(normalized)
  }
  const nodes = []
  const wireIds = []
  for (const node of Array.isArray(artwork?.graph?.nodes) ? artwork.graph.nodes : []) {
    if (nodes.length >= 6) break
    const title = text(node?.title, 63)
    if (!title) continue
    if (!exactInt(node.id) || wireIds.includes(node.id)) continue
    const slot = exactInt(node.slot) && node.slot >= 0 && node.slot < 6 ? node.slot : null
    wireIds.push(node.id)
    nodes.push({ title, slot })
  }
  const usedSlots = new Set()
  for (const node of nodes) {
    if (node.slot !== null && !usedSlots.has(node.slot)) usedSlots.add(node.slot)
    else node.slot = null
  }
  if (nodes.length > 0 && !usedSlots.has(0)) {
    let focus = nodes.findIndex((node) => node.slot === null)
    if (focus < 0) {
      focus = 0
      usedSlots.delete(nodes[focus].slot)
    }
    nodes[focus].slot = 0
    usedSlots.add(0)
  }
  for (const node of nodes) {
    if (node.slot !== null) continue
    let slot = 0
    while (usedSlots.has(slot)) slot++
    node.slot = slot
    usedSlots.add(slot)
  }
  const edgeKeys = new Set()
  for (const edge of Array.isArray(artwork?.graph?.edges) ? artwork.graph.edges : []) {
    if (!Array.isArray(edge) || edge.length !== 2 ||
        !exactInt(edge[0]) || !exactInt(edge[1])) continue
    const a = wireIds.indexOf(edge[0])
    const b = wireIds.indexOf(edge[1])
    if (a < 0 || b < 0 || a === b) continue
    edgeKeys.add(`${Math.min(a, b)},${Math.max(a, b)}`)
  }
  const edges = [...edgeKeys]
    .map((key) => key.split(',').map(Number))
    .sort(([a1, b1], [a2, b2]) => a1 - a2 || b1 - b2)
    .slice(0, 8)
  return {
    headline: lines(artwork?.headline, 2),
    definition: {
      headword: text(definition.headword, 63),
      meta: text(definition.meta, 31),
      lines: lines(definition.lines, 2),
    },
    note: {
      title: text(note.title, 63),
      path: text(note.path, 127),
      backlinkTotal: Math.max(unsignedWireInt(note.backlink_total), backlinks.length),
      backlinks,
    },
    nodes,
    edges,
  }
}

function artworkFingerprint(artwork) {
  return JSON.stringify(normalizeArtwork(artwork))
}

function validTarotDate(value) {
  if (typeof value !== 'string' || !/^\d{4}-\d{2}-\d{2}$/.test(value)) return false
  const [year, month, day] = value.split('-').map(Number)
  if (year < 1 || month < 1 || month > 12) return false
  const leap = year % 400 === 0 || (year % 4 === 0 && year % 100 !== 0)
  const days = [0, 31, leap ? 29 : 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]
  return day >= 1 && day <= days[month]
}

function validTarotCardId(value) {
  if (typeof value !== 'string') return false
  const match = /^(major|cups|pentacles|swords|wands)-(\d{2})$/.exec(value)
  if (!match) return false
  const number = Number(match[2])
  return match[1] === 'major' ? number >= 0 && number <= 21 : number >= 1 && number <= 14
}

// This is the exact render contract used by user_app.cpp: malformed tarot never replaces the
// last-good pixels, and the fingerprint contains only content visible on the tarot composition.
function normalizeDailyTarot(tarot) {
  if (!tarot || typeof tarot !== 'object' || Array.isArray(tarot) ||
      !validTarotDate(tarot.date) || tarot.timezone !== 'Asia/Seoul' ||
      !validTarotCardId(tarot.card_id) || tarot.orientation !== 'upright' ||
      !Number.isInteger(tarot.copy_version) || tarot.copy_version <= 0 ||
      tarot.copy_version > 2147483647) return null

  const normalized = {
    date: tarot.date,
    timezone: tarot.timezone,
    card_id: tarot.card_id,
    orientation: tarot.orientation,
    copy_version: tarot.copy_version,
  }
  for (const key of ['headline', 'flow', 'caution', 'action']) {
    const source = tarot[key]
    if (!Array.isArray(source) || source.length < 1 || source.length > 2) return null
    const lines = source.map((line) => utf8Text(line, 127))
    if (lines.some((line) => !line || /[\u0000-\u001f]/.test(line))) return null
    normalized[key] = lines
  }
  return normalized
}

function tarotFingerprint(tarot, demo = false) {
  const normalized = normalizeDailyTarot(tarot)
  if (!normalized) return null
  return JSON.stringify({
    demo,
    date: normalized.date,
    card_id: normalized.card_id,
    headline: normalized.headline,
    flow: normalized.flow,
    caution: normalized.caution,
    action: normalized.action,
  })
}

if (process.argv[2] === '--demo-fingerprint') {
  process.stdout.write(artworkFingerprint(DEMO_ARTWORK))
  return
}

if (process.argv[2] === '--demo-tarot-fingerprint') {
  process.stdout.write(tarotFingerprint(DEMO_TAROT, true))
  return
}

if (process.argv[2] === '--fingerprint' || process.argv[2] === '--tarot-fingerprint' ||
    process.argv[2] === '--tarot-visible-fingerprint' || process.argv[2] === '--summarise') {
  const command = process.argv[2]
  let input = ''
  process.stdin.setEncoding('utf8')
  process.stdin.on('data', (chunk) => { input += chunk })
  process.stdin.on('end', () => {
    try {
      const json = JSON.parse(input)
      const output = command === '--fingerprint' ? normalizeArtwork(json) :
        command === '--tarot-fingerprint' ? normalizeDailyTarot(json) :
          command === '--tarot-visible-fingerprint' ? tarotFingerprint(json) : summarise(json)
      if (output === null) {
        process.exitCode = 1
        return
      }
      process.stdout.write(typeof output === 'string' ? output : JSON.stringify(output))
    } catch {
      process.exitCode = 1
    }
  })
  return
}

// One poll of the configured source, with the firmware's failure taxonomy: `transport` for
// DNS/connect/timeout, `http_status` for a non-2xx, `bad_payload` for a 2xx that is not a
// snapshot. A failure leaves the previous snapshot in place — blanking the board is the one
// failure a user actually notices.
async function pollVault() {
  const sourceUrl = board.vaultUrl
  const sourceGeneration = board.sourceGeneration
  const sourceIsCurrent = () =>
    sourceGeneration === board.sourceGeneration && sourceUrl === board.vaultUrl
  if (!sourceUrl) {
    board.vault = { ...DEMO }
    board.tarotFingerprint = tarotFingerprint(DEMO_TAROT, true)
    board.lastResult = 'no_url'
    return
  }
  let res
  try {
    res = await fetch(sourceUrl, { signal: AbortSignal.timeout(8000) })
  } catch (e) {
    if (!sourceIsCurrent()) {
      console.log(`   -> discarded stale failure from ${sourceUrl}`)
      return
    }
    board.lastResult = 'transport'
    console.log(`   !! transport: ${e.message}`)
    return
  }
  if (!sourceIsCurrent()) {
    console.log(`   -> discarded stale response from ${sourceUrl}`)
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
    if (!sourceIsCurrent()) {
      console.log(`   -> discarded stale failure from ${sourceUrl}`)
      return
    }
    board.lastResult = 'bad_payload'
    console.log('   !! bad_payload: not JSON')
    return
  }
  if (!sourceIsCurrent()) {
    console.log(`   -> discarded stale response from ${sourceUrl}`)
    return
  }
  const summary = summarise(json)
  const fingerprint = tarotFingerprint(json.daily_tarot)
  if (!summary || !fingerprint) {
    board.lastResult = 'bad_payload'
    console.log('   !! bad_payload: JSON, but not a vault snapshot')
    return
  }
  board.vault = summary
  board.tarotFingerprint = fingerprint
  board.lastResult = 'ok'
  board.lastOkAt = Date.now()
  console.log(`   -> polled ${sourceUrl}: ${summary.notes} notes, ${summary.agents} agents`)
}

board.tarotFingerprint = tarotFingerprint(DEMO_TAROT, true)

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
      // The API reports an old last-good snapshot after a couple of failed polls. The artwork has
      // no status badge; the companion dashboard surfaces this operational state.
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
    board.sourceGeneration++

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
    const before = board.tarotFingerprint
    await pollVault()
    // The board only touches the panel when the snapshot actually changed — that is the whole
    // point of the content hash, so the mock honours it rather than counting a refresh every time.
    if (board.tarotFingerprint !== before) fakeRefresh('full')
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
    if (!Number.isInteger(body.page) || body.page !== 0) {
      return sendJson(res, 400, { ok: false, error: 'page_range' })
    }
    board.page = body.page
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
    const before = board.tarotFingerprint
    board.vaultUrl = body.url
    board.sourceGeneration++
    if (!body.url) board.lastOkAt = null // back to the demo snapshot; nothing has been fetched
    await pollVault()
    if (board.tarotFingerprint !== before) fakeRefresh('full')
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
setInterval(async () => {
  if (!board.vaultUrl) return
  const before = board.tarotFingerprint
  await pollVault()
  if (board.tarotFingerprint !== before) fakeRefresh('full')
}, POLL_SECONDS * 1000)

server.listen(PORT, () => {
  console.log(`mock Obsidian Board listening on http://localhost:${PORT}`)
  console.log(`  EXPO_PUBLIC_ESP32_BASE_URL=http://localhost:${PORT} npx expo start`)
  console.log('  no vault URL set yet — serving the built-in demo snapshot')
  console.log('  set one with:  curl -X POST http://localhost:%d/api/vault -d \'{"url":"..."}\'', PORT)
})
