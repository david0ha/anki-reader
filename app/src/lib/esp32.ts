// Client for the ESP32 Ticker Board's two HTTP/JSON APIs (firmware:
// components/provisioning/prov_portal.c + components/stock_api). See docs/app-control.md for the
// full contract — this file is the source-of-truth TypeScript mirror of it.
//
// [1] Provisioning (SoftAP, http://192.168.4.1): join the device's setup AP first.
//   GET  /api/info       -> { deviceId, model, apSsid }
//   GET  /api/scan       -> { networks: [{ ssid, rssi, secure }] }
//   POST /api/provision  (x-www-form-urlencoded: ssid, password, tickers?, finnhub_key?, fmp_key?, econ_url?) -> 202 | 4xx
//   GET  /api/status     -> { state: idle|connecting|connected|failed, ssid?, reason? }
//
// [2] Stock control (STA, http://tickerboard.local or device IP): same home Wi-Fi.
//   GET  /api/info            -> { deviceId, model, fw, ip }
//   GET  /api/stock/state     -> StockState snapshot (polled by the dashboard)
//   POST /api/stock/select    { index } | { symbol }
//   POST /api/stock/page      { page }
//   POST /api/stock/econ      { mode, week? }
//   POST /api/stock/refresh   { all? }
//   POST /api/stock/watchlist { tickers: string[] | string }
//   POST /api/stock/keys      { finnhubKey?, fmpKey?, econUrl? }
//   POST /api/stock/location  { location }   // free-text place; '' turns the weather widget off
//
// Every function takes an injectable fetch/clock so it can be unit-tested without a device.

// ---------------------------------------------------------------------------
// Response types (one interface per documented payload).
// ---------------------------------------------------------------------------

export interface DeviceInfo {
  deviceId: string
  model: string
  /** Only present over the SoftAP (provisioning). Empty string in STA mode. */
  apSsid: string
  /** Firmware version — present over STA (GET /api/info), '' over the AP. */
  fw: string
  /** Station IP — present over STA, '' over the AP. */
  ip: string
}

export interface ScanNetwork {
  ssid: string
  rssi: number
  secured: boolean
}

export type ProvisionState = 'idle' | 'connecting' | 'connected' | 'failed'

export interface ProvisionStatus {
  state: ProvisionState
  ssid?: string
  // Failure reason from GET /api/status when state==='failed' (e.g. 'auth_failed',
  // 'save_failed', 'internal_error'). Kept as a free string since the app only displays it.
  reason?: string
}

/** One watchlist slot in GET /api/stock/state. valid=false / ageSec=-1 → never fetched yet. */
export interface StockTicker {
  symbol: string
  valid: boolean
  price: number
  change: number
  percent: number
  /** Seconds since the last successful fetch for this slot; -1 if never received. */
  ageSec: number
}

/**
 * Whether each data-source credential/URL is configured on the device (GET /api/stock/state).
 * The firmware reports presence only — it NEVER exposes the stored secret values. An unset key
 * means the device falls back to its compiled-in (Kconfig) default.
 */
export interface StockKeys {
  finnhub: boolean
  fmp: boolean
  econUrl: boolean
}

/** Environment block (sensors + battery) in GET /api/stock/state. */
export interface StockEnv {
  valid: boolean
  tempC: number
  humidity: number
  batteryValid: boolean
  batteryV: number
  batteryPct: number
}

/**
 * Resolved current weather for the configured location (GET /api/stock/state). valid=false until
 * the device's first forecast lands (or when no location is set). `city` is the geocoded place name
 * the device resolved (e.g. "Seoul, KR"); `tempC` is the current temperature in °C.
 */
export interface StockWeather {
  valid: boolean
  tempC: number
  city: string
}

/** The live snapshot the dashboard polls (GET /api/stock/state). */
export interface StockState {
  model: string
  fw: string
  deviceId: string
  /** Watchlist index currently shown on the device screen. */
  index: number
  /** On-screen view: 0=home 1=chart 2=news 3=metrics. */
  page: number
  /** Economic-calendar overlay enabled. */
  econMode: boolean
  /** Week offset for the econ overlay (0 = current). */
  econWeek: number
  /** Seconds between the device's automatic quote refreshes. */
  refreshSeconds: number
  /** Which data-source keys/URL are configured (presence only — never the secret values). */
  keys: StockKeys
  env: StockEnv
  /** Configured weather location (free-text place, e.g. "Seoul"). Empty string = weather off. */
  location: string
  /** Resolved current weather for `location` (valid=false until the first forecast lands). */
  weather: StockWeather
  watchlist: StockTicker[]
}

/** Optional data-source credentials the app can set at provisioning time or update live. */
export interface StockKeyInput {
  /** Finnhub API key (stock quotes). Empty string clears it back to the compiled default. */
  finnhubKey?: string
  /** FMP API key / econ-proxy token (economic calendar). Empty string clears it. */
  fmpKey?: string
  /** Economic-calendar base URL (FMP direct or a self-hosted proxy). Empty string clears it. */
  econUrl?: string
}

// ---------------------------------------------------------------------------
// Errors. Codes from both API surfaces in docs/app-control.md, plus client-side ones.
// ---------------------------------------------------------------------------

export type Esp32ErrorCode =
  // POST /api/provision (4xx body `error`)
  | 'ssid_empty'
  | 'ssid_too_long'
  | 'pass_too_long'
  | 'too_large'
  | 'read_error'
  // POST /api/stock/* (4xx body `error`)
  | 'bad_json'
  | 'index_range'
  | 'page_range'
  | 'symbol_not_found'
  | 'empty_watchlist'
  | 'too_many_tickers'
  // Client-side
  | 'http_error'
  | 'network_error'

export class Esp32Error extends Error {
  code: Esp32ErrorCode
  /** HTTP status of the failed response, when there was one. */
  status?: number
  constructor(code: Esp32ErrorCode, message?: string, status?: number) {
    super(message ?? code)
    this.name = 'Esp32Error'
    this.code = code
    this.status = status
  }
}

// ---------------------------------------------------------------------------
// Client.
// ---------------------------------------------------------------------------

export interface Esp32ClientOptions {
  /** Base URL of the device. Defaults to EXPO_PUBLIC_ESP32_BASE_URL or 192.168.4.1. */
  baseUrl?: string
  /** Injectable fetch (RN global by default). */
  fetchImpl?: typeof fetch
  /** Per-request timeout in ms (RN fetch has none by default). */
  timeoutMs?: number
  /** Injectable clock for waitForConnected (defaults to Date.now / setTimeout). */
  now?: () => number
  sleep?: (ms: number) => Promise<void>
}

export interface WaitForConnectedOptions {
  /** Overall budget before giving up with outcome 'timeout'. */
  timeoutMs?: number
  /** Delay between status polls. */
  intervalMs?: number
}

export interface WaitForConnectedResult extends ProvisionStatus {
  outcome: 'connected' | 'failed' | 'timeout'
}

/** Validation constants mirroring the firmware's prov_tickers_parse / watchlist rules. */
export const TICKER_MAX_LEN = 12
export const WATCHLIST_MAX = 16
export const SYMBOL_RE = /^[A-Z0-9.-]+$/

/**
 * The firmware's compiled-in default econ-calendar base URL (CONFIG_STOCK_ECON_BASE_URL). Shown as
 * the placeholder when the user can optionally override it, so they see the expected URL shape.
 */
export const DEFAULT_ECON_URL = 'https://financialmodelingprep.com/stable/economic-calendar'

const DEFAULT_BASE_URL = process.env.EXPO_PUBLIC_ESP32_BASE_URL || 'http://192.168.4.1'
const DEFAULT_TIMEOUT_MS = 8000
// The connect test briefly hops the SoftAP to the home AP's channel, dropping the phone for a
// few seconds; poll generously so we ride through the gap and still catch the 'connected' read
// before the device reboots out of AP mode.
const DEFAULT_WAIT_TIMEOUT_MS = 45000
const DEFAULT_POLL_INTERVAL_MS = 1500

// Coercers — the device JSON is trusted but we defensively normalize so a missing/garbage field
// never crashes a render. Mirrors masterham's String()/Number()/Boolean() defaulting.
function asNum(v: unknown, fallback = 0): number {
  const n = Number(v)
  return Number.isFinite(n) ? n : fallback
}
function asBool(v: unknown): boolean {
  return Boolean(v)
}
function asStr(v: unknown): string {
  return v == null ? '' : String(v)
}

function parseTicker(raw: Record<string, unknown>): StockTicker {
  return {
    symbol: asStr(raw.symbol),
    valid: asBool(raw.valid),
    price: asNum(raw.price),
    change: asNum(raw.change),
    percent: asNum(raw.percent),
    ageSec: asNum(raw.ageSec, -1),
  }
}

function parseEnv(raw: Record<string, unknown> | undefined): StockEnv {
  const e = raw ?? {}
  return {
    valid: asBool(e.valid),
    tempC: asNum(e.tempC),
    humidity: asNum(e.humidity),
    batteryValid: asBool(e.batteryValid),
    batteryV: asNum(e.batteryV),
    batteryPct: asNum(e.batteryPct),
  }
}

function parseKeys(raw: Record<string, unknown> | undefined): StockKeys {
  const k = raw ?? {}
  return {
    finnhub: asBool(k.finnhub),
    fmp: asBool(k.fmp),
    econUrl: asBool(k.econUrl),
  }
}

function parseWeather(raw: Record<string, unknown> | undefined): StockWeather {
  const w = raw ?? {}
  return {
    valid: asBool(w.valid),
    tempC: asNum(w.tempC),
    city: asStr(w.city),
  }
}

export function createEsp32Client(opts: Esp32ClientOptions = {}) {
  const baseUrl = (opts.baseUrl ?? DEFAULT_BASE_URL).replace(/\/+$/, '')
  const doFetch = opts.fetchImpl ?? fetch
  const timeoutMs = opts.timeoutMs ?? DEFAULT_TIMEOUT_MS
  const now = opts.now ?? (() => Date.now())
  const sleep = opts.sleep ?? ((ms: number) => new Promise<void>((r) => setTimeout(r, ms)))

  async function request(path: string, init?: RequestInit): Promise<Response> {
    const controller = new AbortController()
    const timer = setTimeout(() => controller.abort(), timeoutMs)
    try {
      return await doFetch(`${baseUrl}${path}`, { ...init, signal: controller.signal })
    } catch (e) {
      throw new Esp32Error('network_error', e instanceof Error ? e.message : 'network error')
    } finally {
      clearTimeout(timer)
    }
  }

  async function getJson(path: string, label: string): Promise<Record<string, unknown>> {
    const res = await request(path)
    if (!res.ok) {
      throw new Esp32Error('http_error', `${label} responded ${res.status}`, res.status)
    }
    return ((await res.json()) ?? {}) as Record<string, unknown>
  }

  // Shared POST helper for the JSON control endpoints. Resolves on a 2xx body of {ok:true},
  // otherwise reads the firmware's {ok:false,error:<code>} and throws a typed Esp32Error
  // (falling back to http_error for non-JSON / fieldless error bodies).
  async function postJson(path: string, body: unknown, label: string): Promise<void> {
    const res = await request(path, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    })
    if (res.ok) return
    let code: Esp32ErrorCode = 'http_error'
    try {
      const j = (await res.json()) as { error?: string }
      if (j && typeof j.error === 'string') code = j.error as Esp32ErrorCode
    } catch {
      // non-JSON error body — keep http_error
    }
    throw new Esp32Error(code, `${label} responded ${res.status}`, res.status)
  }

  // ----- Provisioning (SoftAP) -----

  async function getInfo(): Promise<DeviceInfo> {
    const j = await getJson('/api/info', 'info')
    return {
      deviceId: asStr(j.deviceId),
      model: asStr(j.model),
      apSsid: asStr(j.apSsid),
      fw: asStr(j.fw),
      ip: asStr(j.ip),
    }
  }

  async function scanNetworks(): Promise<ScanNetwork[]> {
    const j = await getJson('/api/scan', 'scan')
    const raw = Array.isArray(j.networks) ? (j.networks as Array<Record<string, unknown>>) : []
    return raw
      .map((n) => ({ ssid: asStr(n.ssid), rssi: asNum(n.rssi), secured: asBool(n.secure) }))
      .filter((n) => n.ssid.length > 0)
  }

  // POST the home-Wi-Fi credentials (and optional comma/space-separated watchlist, plus optional
  // data-source keys/URL) as a url-encoded form, matching the firmware's HTML /save path. The
  // firmware reads `finnhub_key`, `fmp_key` and `econ_url` (all optional) into NVS at provisioning
  // time. Returns once the device has accepted them (202); the caller then polls waitForConnected.
  async function provision(
    ssid: string,
    password: string,
    tickers?: string,
    opts?: StockKeyInput & {
      /** Weather location (free-text place, e.g. "Seoul"). Empty/omitted leaves it unset. */
      location?: string
    },
  ): Promise<void> {
    let body = `ssid=${encodeURIComponent(ssid)}&password=${encodeURIComponent(password)}`
    if (tickers != null && tickers.length > 0) {
      body += `&tickers=${encodeURIComponent(tickers)}`
    }
    // Keys are optional. Only append a field the caller actually provided (undefined = leave the
    // device's existing/default value). An empty string is meaningful — it clears the key — so it
    // is sent through; the firmware treats blank as "fall back to the compiled default".
    if (opts?.finnhubKey != null) body += `&finnhub_key=${encodeURIComponent(opts.finnhubKey)}`
    if (opts?.fmpKey != null) body += `&fmp_key=${encodeURIComponent(opts.fmpKey)}`
    if (opts?.econUrl != null) body += `&econ_url=${encodeURIComponent(opts.econUrl)}`
    // Weather location is optional too; same "send what was provided" semantics as the keys above.
    if (opts?.location != null) body += `&location=${encodeURIComponent(opts.location)}`
    const res = await request('/api/provision', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body,
    })
    if (res.ok) return
    let code: Esp32ErrorCode = 'http_error'
    try {
      const j = (await res.json()) as { error?: string }
      if (j && typeof j.error === 'string') code = j.error as Esp32ErrorCode
    } catch {
      // non-JSON error body — keep http_error
    }
    throw new Esp32Error(code, `provision responded ${res.status}`, res.status)
  }

  async function getStatus(): Promise<ProvisionStatus> {
    const j = await getJson('/api/status', 'status')
    const state = (j.state as ProvisionState) ?? 'idle'
    return {
      state,
      ssid: typeof j.ssid === 'string' ? j.ssid : undefined,
      reason: typeof j.reason === 'string' ? j.reason : undefined,
    }
  }

  // Poll /api/status until the device reports connected/failed, or the overall budget elapses.
  // Transient fetch failures are tolerated (the SoftAP drops momentarily during the connect
  // test's channel hop, and disappears entirely once the device reboots into station mode after
  // a confirmed join) — so a 'connected' read is terminal success and we never require the AP to
  // stay reachable to the end.
  async function waitForConnected(
    options: WaitForConnectedOptions = {},
  ): Promise<WaitForConnectedResult> {
    const budget = options.timeoutMs ?? DEFAULT_WAIT_TIMEOUT_MS
    const interval = options.intervalMs ?? DEFAULT_POLL_INTERVAL_MS
    const deadline = now() + budget
    let last: ProvisionStatus = { state: 'connecting' }
    while (now() < deadline) {
      try {
        const st = await getStatus()
        last = st
        if (st.state === 'connected') return { ...st, outcome: 'connected' }
        if (st.state === 'failed') return { ...st, outcome: 'failed' }
      } catch {
        // transient — keep polling across the AP drop
      }
      await sleep(interval)
    }
    return { ...last, outcome: 'timeout' }
  }

  // ----- Stock control (STA) -----

  // The live snapshot. Defensively coerced so a malformed/partial payload renders as
  // "loading" slots rather than crashing the dashboard.
  async function getState(): Promise<StockState> {
    const j = await getJson('/api/stock/state', 'state')
    const watchlist = Array.isArray(j.watchlist)
      ? (j.watchlist as Array<Record<string, unknown>>).map(parseTicker)
      : []
    return {
      model: asStr(j.model),
      fw: asStr(j.fw),
      deviceId: asStr(j.deviceId),
      index: asNum(j.index),
      page: asNum(j.page),
      econMode: asBool(j.econMode),
      econWeek: asNum(j.econWeek),
      refreshSeconds: asNum(j.refreshSeconds),
      keys: parseKeys(j.keys as Record<string, unknown> | undefined),
      env: parseEnv(j.env as Record<string, unknown> | undefined),
      location: asStr(j.location),
      weather: parseWeather(j.weather as Record<string, unknown> | undefined),
      watchlist,
    }
  }

  // Switch the on-screen ticker. Pass either a watchlist index or a symbol (the firmware
  // resolves the symbol to an index); symbol_not_found / index_range are surfaced as typed errors.
  async function select(target: { index: number } | { symbol: string }): Promise<void> {
    return postJson('/api/stock/select', target, 'select')
  }

  // Switch the on-screen view (0=home 1=chart 2=news 3=metrics).
  async function setPage(page: number): Promise<void> {
    return postJson('/api/stock/page', { page }, 'page')
  }

  // Toggle the economic-calendar overlay. When enabling, an optional week offset (0=current)
  // navigates between weeks; week is ignored when disabling.
  async function setEcon(mode: boolean, week?: number): Promise<void> {
    const body = mode ? { mode, week: week ?? 0 } : { mode: false }
    return postJson('/api/stock/econ', body, 'econ')
  }

  // Force a quote re-fetch for the current ticker, or all of them when `all` is true.
  async function refresh(all = false): Promise<void> {
    return postJson('/api/stock/refresh', { all }, 'refresh')
  }

  // Replace the watchlist (NVS-persisted, applied live without a reboot). Accepts an array or a
  // comma/space-separated string; the firmware normalizes via prov_tickers_parse.
  async function setWatchlist(tickers: string[] | string): Promise<void> {
    return postJson('/api/stock/watchlist', { tickers }, 'watchlist')
  }

  // Update the data-source keys/URL live (NVS-persisted, re-fetches immediately). Sends ONLY the
  // fields the caller provided — an omitted field is left untouched on the device. An empty-string
  // value is a valid "clear this key" request (the device falls back to its compiled default) and
  // is sent through. The firmware never echoes the stored values back; success is 200 {ok:true}.
  async function setKeys(keys: StockKeyInput): Promise<void> {
    const body: Record<string, string> = {}
    if (keys.finnhubKey != null) body.finnhubKey = keys.finnhubKey
    if (keys.fmpKey != null) body.fmpKey = keys.fmpKey
    if (keys.econUrl != null) body.econUrl = keys.econUrl
    return postJson('/api/stock/keys', body, 'keys')
  }

  // Set the weather location live (NVS-persisted, the device re-geocodes via Open-Meteo with no
  // reboot). An empty string is valid — it turns the weather widget off — and is sent through.
  // Success is 200 {ok:true}; a bad body is surfaced as a typed Esp32Error ('bad_json').
  async function setLocation(location: string): Promise<void> {
    return postJson('/api/stock/location', { location }, 'location')
  }

  return {
    baseUrl,
    // provisioning
    getInfo,
    scanNetworks,
    provision,
    getStatus,
    waitForConnected,
    // stock control
    getState,
    select,
    setPage,
    setEcon,
    refresh,
    setWatchlist,
    setKeys,
    setLocation,
  }
}

export type Esp32Client = ReturnType<typeof createEsp32Client>

/** Default client bound to EXPO_PUBLIC_ESP32_BASE_URL (or 192.168.4.1). */
export const esp32: Esp32Client = createEsp32Client()
