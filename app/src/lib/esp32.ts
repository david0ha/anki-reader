// Client for the Obsidian Board's two HTTP/JSON APIs (firmware:
// components/provisioning/prov_portal.c + components/device_api). See docs/app-control.md and
// components/provisioning/README.md for the contract — this file is the TypeScript mirror of it,
// and the only place in the app that knows a field name.
//
// [1] Provisioning (SoftAP, http://192.168.4.1): join the board's setup AP first.
//   GET  /api/info       -> { deviceId, model, apSsid }
//   GET  /api/scan       -> { networks: [{ ssid, rssi, secure }] }
//   POST /api/provision  (x-www-form-urlencoded: ssid, password, vault_url?) -> 202 | 4xx
//   GET  /api/status     -> { state: idle|connecting|connected|failed, ssid?, reason? }
//
// [2] Control (STA, http://obsidianboard.local or the board's IP): same home Wi-Fi.
//   GET  /api/info          -> { deviceId, model, fw, ip }
//   GET  /api/state         -> DeviceState snapshot (polled by the dashboard)
//   POST /api/refresh       -> poll the vault source now
//   POST /api/page          { page: 0 } (compatibility no-op)
//   POST /api/vault         { url }      // '' switches the board to its built-in demo snapshot
//   POST /api/display/test  -> run the e-Paper self-test sweep
//
// Every function takes an injectable fetch/clock so it can be unit-tested without a board.

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

/**
 * How the board's last poll of the vault URL went (`source.lastResult`). These are the firmware's
 * own strings — the three failures are separate codes because they send the user to three
 * different places: `transport` is DNS/connect/timeout (is the PC awake?), `http_status` means the
 * server answered but not with a 2xx (is the path right?), and `bad_payload` means it answered 2xx
 * with something that is not a vault snapshot (is that a captive portal?).
 */
export type VaultFetchResult =
  | 'ok'
  | 'no_url'
  | 'transport'
  | 'http_status'
  | 'bad_payload'
  /** Anything the firmware might add later — rendered as-is rather than crashing the row. */
  | 'unknown'

const FETCH_RESULTS: readonly string[] = ['ok', 'no_url', 'transport', 'http_status', 'bad_payload']

/** The board's summary of the vault it is displaying (GET /api/state, `vault`). */
export interface VaultSummary {
  /** A snapshot has been parsed at least once. False on a board that has never had a good poll. */
  valid: boolean
  /** The board is showing its built-in demo snapshot (no vault URL configured). */
  demo: boolean
  name: string
  /** Clock time the snapshot was generated, as the producer reported it (e.g. "21:04"). */
  generatedAt: string
  notes: number
  links: number
  orphans: number
  tags: number
  addedToday: number
  added7d: number
  agents: number
  agentsRunning: number
  /** How many recent notes the source summary contains. */
  recent: number
  /** Total inbox items — may exceed what fits on the panel. */
  inbox: number
}

/** Where the board is fetching from and how that is going (GET /api/state, `source`). */
export interface VaultSource {
  /** Configured snapshot URL. Empty string = unconfigured, running on the demo snapshot. */
  url: string
  lastResult: VaultFetchResult
  pollSeconds: number
  /** Seconds since the last SUCCESSFUL poll; -1 when none has ever succeeded. */
  ageSeconds: number
  /** The board reports that the last good source snapshot is old; the companion may surface it. */
  stale: boolean
}

export interface BatteryInfo {
  present: boolean
  percent: number
  millivolts: number
}

/**
 * Measured panel timings (GET /api/state, `panel`). Not decoration: the refresh policy for the
 * 648x480 UC8179 is meant to be chosen from measurement rather than inherited from the 2.13" panel
 * this firmware forked from, and reading them off a phone beats holding a serial cable to a board
 * on a shelf. Zero means "not measured yet" — that refresh has not run since boot.
 */
export interface PanelInfo {
  /** Partial refreshes since the last full one. The firmware promotes to full at its cap. */
  partialChain: number
  fullRefreshMs: number
  partialRefreshMs: number
}

/** The live snapshot the dashboard polls (GET /api/state). */
export interface DeviceState {
  deviceId: string
  model: string
  fw: string
  ip: string
  /** Compatibility page index. The fixed artwork composition is always page 0. */
  page: number
  /** The board's title for the single composition. */
  pageTitle: string
  vault: VaultSummary
  source: VaultSource
  battery: BatteryInfo
  panel: PanelInfo
}

// ---------------------------------------------------------------------------
// Errors. Codes from both API surfaces in docs/app-control.md, plus client-side ones.
// ---------------------------------------------------------------------------

export type Esp32ErrorCode =
  // POST /api/provision (4xx body `error`)
  | 'ssid_empty'
  | 'ssid_too_long'
  | 'pass_too_long'
  // Shared by /api/provision and the control writes
  | 'vault_url_invalid'
  | 'too_large'
  | 'read_error'
  // POST /api/* (4xx body `error`)
  | 'bad_json'
  | 'page_range'
  | 'busy'
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
  /** Base URL of the board. Defaults to EXPO_PUBLIC_ESP32_BASE_URL or 192.168.4.1. */
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

/** Mirrors the firmware's PROV_URL_MAX_LEN — the board rejects anything longer. */
export const VAULT_URL_MAX_LEN = 128

/** One fixed landscape artwork; page 0 remains for API compatibility. */
export const PAGE_COUNT = 1

const DEFAULT_BASE_URL = process.env.EXPO_PUBLIC_ESP32_BASE_URL || 'http://192.168.4.1'
const DEFAULT_TIMEOUT_MS = 8000
// The connect test briefly hops the SoftAP to the home AP's channel, dropping the phone for a
// few seconds; poll generously so we ride through the gap and still catch the 'connected' read
// before the board reboots out of AP mode.
const DEFAULT_WAIT_TIMEOUT_MS = 45000
const DEFAULT_POLL_INTERVAL_MS = 1500

// Coercers — the board's JSON is trusted but we defensively normalize so a missing/garbage field
// never crashes a render.
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

function parseVault(raw: Record<string, unknown> | undefined): VaultSummary {
  const v = raw ?? {}
  return {
    valid: asBool(v.valid),
    demo: asBool(v.demo),
    name: asStr(v.name),
    generatedAt: asStr(v.generatedAt),
    notes: asNum(v.notes),
    links: asNum(v.links),
    orphans: asNum(v.orphans),
    tags: asNum(v.tags),
    addedToday: asNum(v.addedToday),
    added7d: asNum(v.added7d),
    agents: asNum(v.agents),
    agentsRunning: asNum(v.agentsRunning),
    recent: asNum(v.recent),
    inbox: asNum(v.inbox),
  }
}

function parseSource(raw: Record<string, unknown> | undefined): VaultSource {
  const s = raw ?? {}
  const result = asStr(s.lastResult)
  return {
    url: asStr(s.url),
    lastResult: (FETCH_RESULTS.includes(result) ? result : 'unknown') as VaultFetchResult,
    pollSeconds: asNum(s.pollSeconds),
    // -1 is "never synced", which is NOT "synced zero seconds ago". Defaulting a missing field to
    // 0 would draw a board that had just polled successfully when it never has.
    ageSeconds: asNum(s.ageSeconds, -1),
    stale: asBool(s.stale),
  }
}

function parseBattery(raw: Record<string, unknown> | undefined): BatteryInfo {
  const b = raw ?? {}
  return {
    present: asBool(b.present),
    percent: asNum(b.percent),
    millivolts: asNum(b.millivolts),
  }
}

function parsePanel(raw: Record<string, unknown> | undefined): PanelInfo {
  const p = raw ?? {}
  return {
    partialChain: asNum(p.partialChain),
    fullRefreshMs: asNum(p.fullRefreshMs),
    partialRefreshMs: asNum(p.partialRefreshMs),
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

  // Read the firmware's {ok:false,error:<code>} off a failed response, falling back to http_error
  // for a non-JSON or fieldless body. Shared by the JSON writes and the form POST.
  async function errorCodeOf(res: Response): Promise<Esp32ErrorCode> {
    try {
      const j = (await res.json()) as { error?: string }
      if (j && typeof j.error === 'string') return j.error as Esp32ErrorCode
    } catch {
      // non-JSON error body
    }
    return 'http_error'
  }

  // Shared POST helper for the JSON control endpoints. Resolves on a 2xx, otherwise throws a
  // typed Esp32Error carrying the firmware's error code.
  async function postJson(path: string, body: unknown, label: string): Promise<void> {
    const res = await request(path, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    })
    if (res.ok) return
    throw new Esp32Error(await errorCodeOf(res), `${label} responded ${res.status}`, res.status)
  }

  // The board's POST handlers take no body for /api/refresh and /api/display/test. Sending one
  // anyway is harmless, but sending none keeps the request identical to the documented curl.
  async function postEmpty(path: string, label: string): Promise<void> {
    const res = await request(path, { method: 'POST' })
    if (res.ok) return
    throw new Esp32Error(await errorCodeOf(res), `${label} responded ${res.status}`, res.status)
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

  // POST the home-Wi-Fi credentials and the vault URL as a url-encoded form, matching the
  // firmware's HTML /save path. Returns once the board has accepted them (202); the caller then
  // polls waitForConnected.
  //
  // `vault_url` is always sent, empty string included. Provisioning REWRITES the whole stored
  // config (the firmware zeroes its struct and fills it from the form), so omitting the field
  // would still clear the URL — sending '' says that on purpose instead of relying on it.
  async function provision(ssid: string, password: string, vaultUrl = ''): Promise<void> {
    const body =
      `ssid=${encodeURIComponent(ssid)}` +
      `&password=${encodeURIComponent(password)}` +
      `&vault_url=${encodeURIComponent(vaultUrl)}`
    const res = await request('/api/provision', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body,
    })
    if (res.ok) return
    throw new Esp32Error(await errorCodeOf(res), `provision responded ${res.status}`, res.status)
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

  // Poll /api/status until the board reports connected/failed, or the overall budget elapses.
  // Transient fetch failures are tolerated (the SoftAP drops momentarily during the connect
  // test's channel hop, and disappears entirely once the board reboots into station mode after
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

  // ----- Control (STA) -----

  // The live snapshot. Defensively coerced so a malformed/partial payload renders as empty
  // counters rather than crashing the dashboard.
  async function getState(): Promise<DeviceState> {
    const j = await getJson('/api/state', 'state')
    return {
      deviceId: asStr(j.deviceId),
      model: asStr(j.model),
      fw: asStr(j.fw),
      ip: asStr(j.ip),
      page: 0,
      pageTitle: 'Artwork',
      vault: parseVault(j.vault as Record<string, unknown> | undefined),
      source: parseSource(j.source as Record<string, unknown> | undefined),
      battery: parseBattery(j.battery as Record<string, unknown> | undefined),
      panel: parsePanel(j.panel as Record<string, unknown> | undefined),
    }
  }

  // Compatibility endpoint for the single artwork composition. Only page 0 is valid.
  async function setPage(page: number): Promise<void> {
    return postJson('/api/page', { page }, 'page')
  }

  // Poll the vault source now instead of waiting out the interval. The board only refreshes the
  // panel when what comes back differs from what is already on the glass, so this is safe to call
  // repeatedly — a no-change refresh costs nothing and flashes nothing.
  async function refresh(): Promise<void> {
    return postEmpty('/api/refresh', 'refresh')
  }

  // Point the board at a different snapshot URL (NVS-persisted, applied live, no reboot). An empty
  // string is valid and meaningful: it switches the board to its built-in demo snapshot.
  async function setVaultUrl(url: string): Promise<void> {
    return postJson('/api/vault', { url }, 'vault')
  }

  // Run the e-Paper self-test sweep. Tens of seconds of full refreshes on the board; the request
  // returns as soon as it is queued, not when the sweep finishes.
  async function displayTest(): Promise<void> {
    return postEmpty('/api/display/test', 'display test')
  }

  return {
    baseUrl,
    // provisioning
    getInfo,
    scanNetworks,
    provision,
    getStatus,
    waitForConnected,
    // control
    getState,
    setPage,
    refresh,
    setVaultUrl,
    displayTest,
  }
}

export type Esp32Client = ReturnType<typeof createEsp32Client>

/** Default client bound to EXPO_PUBLIC_ESP32_BASE_URL (or 192.168.4.1). */
export const esp32: Esp32Client = createEsp32Client()
