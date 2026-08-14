// Client for the Kanjis Board's two HTTP/JSON APIs (firmware:
// components/provisioning/prov_portal.c + components/device_api). See docs/app-control.md and
// components/provisioning/README.md for the contract — this file is the TypeScript mirror of it,
// and the only place in the app that knows a field name.
//
// [1] Provisioning (SoftAP, http://192.168.4.1): join the board's setup AP first.
//   GET  /api/info       -> { deviceId, model, apSsid }
//   GET  /api/scan       -> { networks: [{ ssid, rssi, secure }] }
//   POST /api/provision  (x-www-form-urlencoded: ssid, password, study_url?) -> 202 | 4xx
//   GET  /api/status     -> { state: idle|connecting|connected|failed, ssid?, reason? }
//
// [2] Control (STA, http://obsidianboard.local or the board's IP): same home Wi-Fi.
//   GET  /api/info          -> { deviceId, model, fw, ip }
//   GET  /api/state         -> DeviceState snapshot (polled by the dashboard)
//   POST /api/refresh       -> poll the study source now
//   POST /api/screen        { screen: 0..4 }
//   POST /api/study         { url }      // '' switches the board to its built-in demo card
//   POST /api/display/test  -> run the e-Paper self-test sweep
//
// The mDNS hostname and the setup-AP name are deliberately unchanged from the board this study
// firmware replaced — same hardware, same discovery probe, same network identity.
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
 * How the board's last poll of the study URL went (`source.lastResult`). These are the firmware's
 * own strings — the three failures are separate codes because they send the user to three
 * different places: `transport` is DNS/connect/timeout (is the PC awake?), `http_status` means the
 * server answered but not with a 2xx (is the path right?), and `bad_payload` means it answered 2xx
 * with something that is not a study card (is that a captive portal?).
 */
export type StudyFetchResult =
  | 'ok'
  | 'no_url'
  | 'transport'
  | 'http_status'
  | 'bad_payload'
  /** Anything the firmware might add later — rendered as-is rather than crashing the row. */
  | 'unknown'

const FETCH_RESULTS: readonly string[] = ['ok', 'no_url', 'transport', 'http_status', 'bad_payload']

/**
 * The scheduler state of the card on the glass (`card.fsrsState`). The wire words of FSRS itself,
 * not the Korean the panel prints — the board sends `fsrs.state`, and the panel's own label is
 * derived from it on the far side.
 */
export type FsrsState = 'new' | 'learning' | 'review' | 'relearning' | 'unknown'

const FSRS_STATES: readonly string[] = ['new', 'learning', 'review', 'relearning']

/** Today's study session, as the board understands it (GET /api/state, `session`). */
export interface SessionSummary {
  /** The deck the proxy is serving, e.g. "JLPT N5 Vocabulary". */
  deck: string
  /** Consecutive days studied. */
  streak: number
  reviewedToday: number
  /** Unseen cards still queued for today. */
  leftNew: number
  /** Scheduled reviews still queued for today. */
  leftReview: number
  /** 1-based position in today's queue, and its length. */
  track: number
  trackTotal: number
  /** The queue is empty; the board is showing 오늘 학습 완료 and has stopped asking. */
  complete: boolean
}

/** The card on the glass (GET /api/state, `card`). */
export interface CardSummary {
  /** A card has been parsed. False when the session served none — see `session.complete`. */
  valid: boolean
  /** The board is showing its built-in demo card (no study URL configured). */
  demo: boolean
  /** The headword, e.g. "会う". */
  front: string
  /** The kana reading. */
  reading: string
  /** The first Korean gloss. The board holds three; the phone needs the one that names the card. */
  meaning: string
  fsrsState: FsrsState
  /** When this card is next due, as the proxy already worded it ("9일 뒤"). '' = never scheduled. */
  due: string
  reps: number
  lapses: number
  /** Rounded days of stability. **-1 means "not scheduled yet"**, which is not zero. */
  stabilityDays: number
  /** 0..100. -1 means not scheduled yet. */
  difficultyPct: number
}

/** Where the board is fetching from and how that is going (GET /api/state, `source`). */
export interface StudySource {
  /** Configured card URL. Empty string = unconfigured, running on the demo card. */
  url: string
  lastResult: StudyFetchResult
  pollSeconds: number
  /** Seconds since the last SUCCESSFUL poll; -1 when none has ever succeeded. */
  ageSeconds: number
  /** The board reports that the last good card is old; the companion may surface it. */
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
  /** Which of the five screens is up: 0 문제, 1 정답, 2 설명, 3 댓글, 4 FSRS. */
  screen: number
  /** The board's own name for that screen, in the Korean the footer prints. */
  screenTitle: string
  /** The answer side has been shown for the current card. */
  revealed: boolean
  /** Where the grade dock's cursor is parked, as `kanji_grade_t`: 1 again … 4 easy. */
  grade: number
  session: SessionSummary
  card: CardSummary
  source: StudySource
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
  | 'study_url_invalid'
  | 'too_large'
  | 'read_error'
  // POST /api/* (4xx body `error`)
  | 'bad_json'
  | 'screen_range'
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
export const STUDY_URL_MAX_LEN = 128

/** `KANJI_SCREEN_COUNT`: 문제, 정답, 설명, 댓글, FSRS. */
export const SCREEN_COUNT = 5

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

function parseSession(raw: Record<string, unknown> | undefined): SessionSummary {
  const s = raw ?? {}
  return {
    deck: asStr(s.deck),
    streak: asNum(s.streak),
    reviewedToday: asNum(s.reviewedToday),
    leftNew: asNum(s.leftNew),
    leftReview: asNum(s.leftReview),
    track: asNum(s.track),
    trackTotal: asNum(s.trackTotal),
    complete: asBool(s.complete),
  }
}

function parseCard(raw: Record<string, unknown> | undefined): CardSummary {
  const c = raw ?? {}
  const state = asStr(c.fsrsState)
  return {
    valid: asBool(c.valid),
    demo: asBool(c.demo),
    front: asStr(c.front),
    reading: asStr(c.reading),
    meaning: asStr(c.meaning),
    fsrsState: (FSRS_STATES.includes(state) ? state : 'unknown') as FsrsState,
    due: asStr(c.due),
    reps: asNum(c.reps),
    lapses: asNum(c.lapses),
    // -1 is "the scheduler has no value for this card yet", which is NOT "zero days of
    // stability". Defaulting a missing field to 0 would print a number the board never claimed.
    stabilityDays: asNum(c.stabilityDays, -1),
    difficultyPct: asNum(c.difficultyPct, -1),
  }
}

function parseSource(raw: Record<string, unknown> | undefined): StudySource {
  const s = raw ?? {}
  const result = asStr(s.lastResult)
  return {
    url: asStr(s.url),
    lastResult: (FETCH_RESULTS.includes(result) ? result : 'unknown') as StudyFetchResult,
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

  // POST the home-Wi-Fi credentials and the study URL as a url-encoded form, matching the
  // firmware's HTML /save path. Returns once the board has accepted them (202); the caller then
  // polls waitForConnected.
  //
  // `study_url` is always sent, empty string included. Provisioning REWRITES the whole stored
  // config (the firmware zeroes its struct and fills it from the form), so omitting the field
  // would still clear the URL — sending '' says that on purpose instead of relying on it.
  async function provision(ssid: string, password: string, studyUrl = ''): Promise<void> {
    const body =
      `ssid=${encodeURIComponent(ssid)}` +
      `&password=${encodeURIComponent(password)}` +
      `&study_url=${encodeURIComponent(studyUrl)}`
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

  // The live snapshot. Defensively coerced so a malformed/partial payload renders as an empty
  // session rather than crashing the dashboard.
  async function getState(): Promise<DeviceState> {
    const j = await getJson('/api/state', 'state')
    return {
      deviceId: asStr(j.deviceId),
      model: asStr(j.model),
      fw: asStr(j.fw),
      ip: asStr(j.ip),
      screen: asNum(j.screen),
      screenTitle: asStr(j.screenTitle),
      revealed: asBool(j.revealed),
      grade: asNum(j.grade),
      session: parseSession(j.session as Record<string, unknown> | undefined),
      card: parseCard(j.card as Record<string, unknown> | undefined),
      source: parseSource(j.source as Record<string, unknown> | undefined),
      battery: parseBattery(j.battery as Record<string, unknown> | undefined),
      panel: parsePanel(j.panel as Record<string, unknown> | undefined),
    }
  }

  // Put the board on one of the five screens. It is the same nav state a button press drives, so
  // the phone can never leave the board somewhere the buttons cannot get out of.
  async function setScreen(screen: number): Promise<void> {
    return postJson('/api/screen', { screen }, 'screen')
  }

  // Poll the study source now instead of waiting out the interval. The board only refreshes the
  // panel when what comes back differs from what is already on the glass, so this is safe to call
  // repeatedly — a no-change refresh costs nothing and flashes nothing.
  async function refresh(): Promise<void> {
    return postEmpty('/api/refresh', 'refresh')
  }

  // Point the board at a different study URL (NVS-persisted, applied live, no reboot). An empty
  // string is valid and meaningful: it switches the board to its built-in demo card.
  async function setStudyUrl(url: string): Promise<void> {
    return postJson('/api/study', { url }, 'study')
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
    setScreen,
    refresh,
    setStudyUrl,
    displayTest,
  }
}

export type Esp32Client = ReturnType<typeof createEsp32Client>

/** Default client bound to EXPO_PUBLIC_ESP32_BASE_URL (or 192.168.4.1). */
export const esp32: Esp32Client = createEsp32Client()
