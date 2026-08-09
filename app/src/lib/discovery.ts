// Resolving the device's base URL for the stock-control (STA) API.
//
// Three sources, in priority order:
//   1. A persisted last-known base URL (src/lib/store.ts), saved on successful onboarding/connect.
//   2. The mDNS default `http://tickerboard.local` (NSBonjourservices _http._tcp / Android mDNS).
//   3. A manual IP/host the user typed in settings.
//
// This module is intentionally pure (no AsyncStorage, no fetch) so the URL hygiene — scheme
// defaulting, trimming, host/IP validation, trailing-slash stripping — is unit-testable.

/** mDNS hostname advertised by the firmware (components/stock_api). */
export const DEFAULT_HOST = 'tickerboard.local'
export const DEFAULT_BASE_URL = `http://${DEFAULT_HOST}`

// host:        a DNS label, a dotted hostname (e.g. tickerboard.local), or an IPv4 address.
// We keep this permissive (any label.label…) but reject whitespace and obvious garbage so a
// fat-fingered settings entry surfaces immediately instead of producing a dead base URL.
const HOSTNAME_RE = /^[a-zA-Z0-9]([a-zA-Z0-9-]*[a-zA-Z0-9])?(\.[a-zA-Z0-9]([a-zA-Z0-9-]*[a-zA-Z0-9])?)*$/
const IPV4_RE = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/

function isValidIpv4(host: string): boolean {
  const m = IPV4_RE.exec(host)
  if (!m) return false
  return m.slice(1).every((oct) => {
    const n = Number(oct)
    return n >= 0 && n <= 255 && String(n) === oct.replace(/^0+(?=\d)/, '')
  })
}

// A host shaped like four dotted numeric labels (e.g. 999.1.1.1) is meant to be an IPv4 — if it
// isn't a *valid* one, reject it rather than letting it slip through as a hostname.
const DOTTED_NUMERIC_RE = /^\d{1,3}(\.\d{1,3}){3}$/

export interface NormalizeResult {
  ok: boolean
  /** Cleaned `http://host[:port]` (no trailing slash) when ok; otherwise the reason. */
  value?: string
  error?: string
}

/**
 * Normalize free-text the user typed (an IP, a host, or a full URL) into a canonical base URL:
 *   - defaults the scheme to http:// when omitted (the device speaks plain HTTP only)
 *   - lower-cases the host, strips a trailing slash and any path/query
 *   - validates the host is an IPv4 address or a plausible hostname, and the port (if any) is 1..65535
 */
export function normalizeBaseUrl(input: string): NormalizeResult {
  const raw = (input ?? '').trim()
  if (raw.length === 0) return { ok: false, error: 'empty' }

  // Reject an explicitly non-http(s) scheme; default to http:// when none is given. We parse the
  // scheme/host/port by hand (rather than via `new URL`) so the error codes are exact and
  // independent of how strict the host JS engine's URL parser is (Hermes vs. Node differ — e.g.
  // an out-of-range IPv4 or port should report bad_host/bad_port, not a generic parse failure).
  let scheme = 'http'
  let rest = raw
  const schemeMatch = /^([a-zA-Z][a-zA-Z0-9+.-]*):\/\/(.*)$/s.exec(raw)
  if (schemeMatch) {
    scheme = schemeMatch[1].toLowerCase()
    if (scheme !== 'http' && scheme !== 'https') return { ok: false, error: 'bad_scheme' }
    rest = schemeMatch[2]
  }

  // Strip any path / query / fragment — we only keep the authority (host[:port]).
  const authority = rest.split(/[/?#]/)[0]
  if (!authority) return { ok: false, error: 'bad_host' }

  // Split a single trailing :port off the host.
  let host = authority
  let port: string | undefined
  const colon = authority.lastIndexOf(':')
  if (colon !== -1) {
    host = authority.slice(0, colon)
    port = authority.slice(colon + 1)
  }

  host = host.toLowerCase()
  if (!host) return { ok: false, error: 'bad_host' }
  // A dotted-numeric host must be a valid IPv4; otherwise require a plausible hostname.
  if (DOTTED_NUMERIC_RE.test(host)) {
    if (!isValidIpv4(host)) return { ok: false, error: 'bad_host' }
  } else if (!HOSTNAME_RE.test(host)) {
    return { ok: false, error: 'bad_host' }
  }

  if (port !== undefined) {
    if (!/^\d+$/.test(port)) return { ok: false, error: 'bad_port' }
    const p = Number(port)
    if (p < 1 || p > 65535) return { ok: false, error: 'bad_port' }
  }

  const portPart = port !== undefined ? `:${port}` : ''
  return { ok: true, value: `${scheme}://${host}${portPart}` }
}

/**
 * Decide the base URL to use for the control API, given a saved value and an optional manual
 * override. Priority: a valid manual override > a valid saved value > the mDNS default.
 * Always returns a usable URL (never throws) so the dashboard can always attempt a connection.
 */
export function resolveBaseUrl(opts: { saved?: string | null; manual?: string | null } = {}): string {
  if (opts.manual) {
    const m = normalizeBaseUrl(opts.manual)
    if (m.ok && m.value) return m.value
  }
  if (opts.saved) {
    const s = normalizeBaseUrl(opts.saved)
    if (s.ok && s.value) return s.value
  }
  return DEFAULT_BASE_URL
}

export interface DiscoverOptions {
  /** Injectable fetch (RN global by default) so this is unit-testable without a device. */
  fetchImpl?: typeof fetch
  /** Per-candidate probe timeout in ms. Kept short so an unreachable host fails fast. */
  timeoutMs?: number
}

const DISCOVER_TIMEOUT_MS = 2000

/**
 * Find a reachable device base URL by probing each candidate's `GET /api/info` in parallel and
 * resolving to the FIRST one that answers ok (or null if none do). The device leaves AP mode after
 * onboarding and lives on the home LAN, reachable via its DHCP IP or mDNS `tickerboard.local`.
 *
 * Dependency-light by design: just `fetch` (no native zeroconf library). Note that Android (esp.
 * older versions) often can't resolve `.local` mDNS names, which is why the persisted device IP
 * (from `/api/info`'s `ip` field) is the preferred candidate and `tickerboard.local` is the
 * fallback — pass them in that order.
 *
 * Each candidate is normalized (scheme defaulting, trailing-slash stripping); invalid candidates
 * are skipped. The resolved value is the normalized base URL, ready to persist via setBaseUrl.
 */
export async function discoverDevice(
  candidates: Array<string | null | undefined>,
  opts: DiscoverOptions = {},
): Promise<string | null> {
  const doFetch = opts.fetchImpl ?? fetch
  const timeoutMs = opts.timeoutMs ?? DISCOVER_TIMEOUT_MS

  // Normalize + de-dupe up front so we don't probe the same host twice or a malformed one at all.
  const bases: string[] = []
  for (const c of candidates) {
    if (!c) continue
    const norm = normalizeBaseUrl(c)
    if (norm.ok && norm.value && !bases.includes(norm.value)) bases.push(norm.value)
  }
  if (bases.length === 0) return null

  // Probe every candidate in parallel; a probe resolves with its base URL on success, or null on
  // any failure/timeout. We then pick the first candidate (by input order) that succeeded, so the
  // preferred candidate wins even if a later one answers first.
  const probe = async (base: string): Promise<string | null> => {
    const controller = new AbortController()
    const timer = setTimeout(() => controller.abort(), timeoutMs)
    try {
      const res = await doFetch(`${base}/api/info`, { signal: controller.signal })
      return res.ok ? base : null
    } catch {
      return null
    } finally {
      clearTimeout(timer)
    }
  }

  const results = await Promise.all(bases.map(probe))
  return results.find((r) => r !== null) ?? null
}
