// Writing a memo into the vault.
//
// This is deliberately NOT in esp32.ts. `POST /capture` is not part of the device contract — the
// firmware has never heard of it. It is served by whatever is producing the snapshot, and only if
// that producer chose to offer it (`tools/vault_server.py --allow-capture`). Keeping it in its own
// file keeps that boundary visible: a change here cannot be mistaken for a change to the board's
// API, and a producer that does not implement it is a supported, ordinary case rather than a bug.
//
// The address is derived from the snapshot URL the BOARD reports, so the phone never has to be
// told separately where the vault lives: whatever the board is polling is what gets written to.

import { normalizeBaseUrl } from './discovery'

export type CaptureErrorCode =
  /** The board has no snapshot URL — it is running on its demo data. */
  | 'no_source'
  /** The producer answered, but capture is switched off (`--allow-capture`). */
  | 'disabled'
  /** The producer has no such endpoint. Ordinary: most producers will not. */
  | 'unsupported'
  | 'empty'
  | 'too_large'
  | 'bad_json'
  | 'write_failed'
  | 'http_error'
  | 'network_error'

export class CaptureError extends Error {
  code: CaptureErrorCode
  status?: number
  constructor(code: CaptureErrorCode, message?: string, status?: number) {
    super(message ?? code)
    this.name = 'CaptureError'
    this.code = code
    this.status = status
  }
}

/** Mirrors the server's CAPTURE_MAX_BYTES, so an over-long memo fails before the round trip. */
export const CAPTURE_MAX_BYTES = 8192

const DEFAULT_TIMEOUT_MS = 8000

/**
 * The capture endpoint for a given snapshot URL: same host, `/capture`.
 *
 * Returns null when there is no usable source — an unconfigured board, or a URL the app cannot
 * make sense of. The path of the snapshot URL is dropped deliberately: `/vault.json` is one
 * resource on that server and `/capture` is another.
 */
export function captureUrlFor(sourceUrl: string | null | undefined): string | null {
  if (!sourceUrl) return null
  const norm = normalizeBaseUrl(sourceUrl)
  if (!norm.ok || !norm.value) return null
  return `${norm.value}/capture`
}

/** UTF-8 byte length, without TextEncoder (absent on older Hermes). */
export function memoByteLength(text: string): number {
  let n = 0
  for (const ch of text) {
    const cp = ch.codePointAt(0) as number
    if (cp < 0x80) n += 1
    else if (cp < 0x800) n += 2
    else if (cp < 0x10000) n += 3
    else n += 4
  }
  return n
}

export interface CaptureOptions {
  fetchImpl?: typeof fetch
  timeoutMs?: number
}

/**
 * Write one memo. Resolves with the vault-relative path the producer created.
 *
 * Every failure is a typed code because they need four different sentences: "your board has no
 * vault URL", "the server is running but capture is off", "this server does not do capture at
 * all", and "the network". Collapsing them into one "couldn't save" would leave the user with
 * nothing to act on.
 */
export async function captureMemo(
  sourceUrl: string | null | undefined,
  text: string,
  opts: CaptureOptions = {},
): Promise<{ path: string }> {
  const url = captureUrlFor(sourceUrl)
  if (!url) throw new CaptureError('no_source')

  const body = text.trim()
  if (!body) throw new CaptureError('empty')
  if (memoByteLength(body) > CAPTURE_MAX_BYTES) throw new CaptureError('too_large')

  const doFetch = opts.fetchImpl ?? fetch
  const controller = new AbortController()
  const timer = setTimeout(() => controller.abort(), opts.timeoutMs ?? DEFAULT_TIMEOUT_MS)

  let res: Response
  try {
    res = await doFetch(url, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ text: body }),
      signal: controller.signal,
    })
  } catch (e) {
    throw new CaptureError('network_error', e instanceof Error ? e.message : 'network error')
  } finally {
    clearTimeout(timer)
  }

  if (res.ok) {
    let path = ''
    try {
      const j = (await res.json()) as { path?: string }
      if (j && typeof j.path === 'string') path = j.path
    } catch {
      // A producer that writes the file and answers with an empty body still wrote the file.
    }
    return { path }
  }

  // 404/405 is "this producer has no capture endpoint", which is the normal state of affairs for
  // anything other than tools/vault_server.py and must not read as a failure of the app.
  if (res.status === 404 || res.status === 405) throw new CaptureError('unsupported', undefined, res.status)

  let code: CaptureErrorCode = 'http_error'
  try {
    const j = (await res.json()) as { error?: string }
    if (j && typeof j.error === 'string') {
      code = j.error === 'capture_disabled' ? 'disabled' : (j.error as CaptureErrorCode)
    }
  } catch {
    // non-JSON error body — keep http_error
  }
  throw new CaptureError(code, `capture responded ${res.status}`, res.status)
}

/** A sentence per failure, saying what to go and do. */
export function captureErrorMessage(e: unknown): string {
  const code = e instanceof CaptureError ? e.code : 'network_error'
  switch (code) {
    case 'no_source':
      return 'The board has no vault URL yet — it’s showing demo data. Set one in Settings.'
    case 'disabled':
      return 'Your vault server has capture switched off. Restart it with --allow-capture.'
    case 'unsupported':
      return 'Whatever is serving your vault doesn’t accept memos. tools/vault_server.py does.'
    case 'empty':
      return 'Nothing to save.'
    case 'too_large':
      return 'That memo is too long to save.'
    case 'write_failed':
      return 'The vault server couldn’t write the file. Check the disk it’s on.'
    case 'network_error':
      return 'Couldn’t reach the machine serving your vault. Is it awake and on this Wi-Fi?'
    default:
      return 'Couldn’t save that memo. Please try again.'
  }
}
