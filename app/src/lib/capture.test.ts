import { describe, it, expect } from '@jest/globals'
import {
  CAPTURE_MAX_BYTES,
  CaptureError,
  captureErrorMessage,
  captureMemo,
  captureUrlFor,
  memoByteLength,
} from './capture'

type Reply = { ok?: boolean; status?: number; body?: unknown; jsonThrows?: boolean } | Error

function fakeFetch(reply: Reply) {
  const calls: Array<{ url: string; init?: RequestInit }> = []
  const fetchImpl = (async (url: string, init?: RequestInit) => {
    calls.push({ url: String(url), init })
    if (reply instanceof Error) throw reply
    return {
      ok: reply.ok ?? true,
      status: reply.status ?? 200,
      json: async () => {
        if (reply.jsonThrows) throw new SyntaxError('not json')
        return reply.body
      },
    } as unknown as Response
  }) as unknown as typeof fetch
  return { fetchImpl, calls }
}

describe('captureUrlFor', () => {
  it('keeps the host and replaces the path', () => {
    // /vault.json is one resource on that server and /capture is another.
    expect(captureUrlFor('http://mac.local:8123/vault.json')).toBe('http://mac.local:8123/capture')
    expect(captureUrlFor('http://192.168.0.9:8000/a/b/c.json')).toBe('http://192.168.0.9:8000/capture')
    expect(captureUrlFor('https://vault.example.com/snapshot')).toBe('https://vault.example.com/capture')
  })

  it('returns null when there is no usable source', () => {
    // An unconfigured board reports an empty URL — it is running on demo data.
    expect(captureUrlFor('')).toBeNull()
    expect(captureUrlFor(null)).toBeNull()
    expect(captureUrlFor(undefined)).toBeNull()
    expect(captureUrlFor('not a url at all')).toBeNull()
  })
})

describe('memoByteLength', () => {
  it('measures UTF-8 bytes, not characters', () => {
    expect(memoByteLength('abc')).toBe(3)
    expect(memoByteLength('가')).toBe(3)
    expect(memoByteLength('가나다')).toBe(9)
    expect(memoByteLength('😀')).toBe(4)
  })
})

describe('captureMemo', () => {
  const SRC = 'http://mac.local:8123/vault.json'

  it('POSTs the memo as JSON and returns the path the server created', async () => {
    const { fetchImpl, calls } = fakeFetch({ status: 201, body: { ok: true, path: 'Inbox/x.md' } })
    const res = await captureMemo(SRC, '  ring the dentist  ', { fetchImpl })
    expect(res.path).toBe('Inbox/x.md')
    expect(calls[0].url).toBe('http://mac.local:8123/capture')
    expect(calls[0].init?.method).toBe('POST')
    // Trimmed: leading whitespace would become the first character of the filename.
    expect(calls[0].init?.body).toBe('{"text":"ring the dentist"}')
  })

  it('accepts a written file whose response body is unreadable', async () => {
    // The file exists either way; failing here would tell the user to write it again.
    const { fetchImpl } = fakeFetch({ status: 201, jsonThrows: true })
    expect((await captureMemo(SRC, 'x', { fetchImpl })).path).toBe('')
  })

  it('refuses an empty memo without a round trip', async () => {
    const { fetchImpl, calls } = fakeFetch({ status: 201, body: {} })
    await expect(captureMemo(SRC, '   ', { fetchImpl })).rejects.toMatchObject({ code: 'empty' })
    expect(calls.length).toBe(0)
  })

  it('refuses an over-long memo by BYTES, before the round trip', async () => {
    const { fetchImpl, calls } = fakeFetch({ status: 201, body: {} })
    // 3000 Korean characters are 9000 UTF-8 bytes — over the cap while well under
    // it counted as characters, which is how the server measures and so must this.
    await expect(captureMemo(SRC, '가'.repeat(3000), { fetchImpl })).rejects.toMatchObject({
      code: 'too_large',
    })
    expect(calls.length).toBe(0)
    expect(memoByteLength('가'.repeat(3000))).toBeGreaterThan(CAPTURE_MAX_BYTES)
  })

  it('reports a board with no vault URL separately', async () => {
    const { fetchImpl, calls } = fakeFetch({ status: 201, body: {} })
    await expect(captureMemo('', 'x', { fetchImpl })).rejects.toMatchObject({ code: 'no_source' })
    expect(calls.length).toBe(0)
  })

  it('distinguishes "switched off" from "not supported"', async () => {
    // These need different sentences: one is a flag on a server that is running,
    // the other is a producer that was never going to accept memos.
    const off = fakeFetch({ ok: false, status: 403, body: { ok: false, error: 'capture_disabled' } })
    await expect(captureMemo(SRC, 'x', { fetchImpl: off.fetchImpl })).rejects.toMatchObject({
      code: 'disabled',
    })

    for (const status of [404, 405]) {
      const none = fakeFetch({ ok: false, status })
      await expect(captureMemo(SRC, 'x', { fetchImpl: none.fetchImpl })).rejects.toMatchObject({
        code: 'unsupported',
        status,
      })
    }
  })

  it('passes the server’s own error codes through', async () => {
    for (const error of ['empty', 'too_large', 'bad_json', 'write_failed']) {
      const { fetchImpl } = fakeFetch({ ok: false, status: 400, body: { ok: false, error } })
      await expect(captureMemo(SRC, 'x', { fetchImpl })).rejects.toMatchObject({ code: error })
    }
  })

  it('falls back to http_error on a fieldless or non-JSON failure', async () => {
    const { fetchImpl } = fakeFetch({ ok: false, status: 500, jsonThrows: true })
    await expect(captureMemo(SRC, 'x', { fetchImpl })).rejects.toMatchObject({
      code: 'http_error',
      status: 500,
    })
  })

  it('maps a dropped connection to network_error', async () => {
    const { fetchImpl } = fakeFetch(new TypeError('Network request failed'))
    await expect(captureMemo(SRC, 'x', { fetchImpl })).rejects.toMatchObject({
      code: 'network_error',
    })
  })
})

describe('captureErrorMessage', () => {
  it('says something different for each failure', () => {
    const codes = [
      'no_source', 'disabled', 'unsupported', 'empty',
      'too_large', 'write_failed', 'network_error',
    ] as const
    const messages = codes.map((c) => captureErrorMessage(new CaptureError(c)))
    expect(new Set(messages).size).toBe(codes.length)
  })

  it('names the flag that fixes the "switched off" case', () => {
    // The fix is one word on a command line; not naming it leaves the user stuck.
    expect(captureErrorMessage(new CaptureError('disabled'))).toContain('--allow-capture')
  })

  it('falls back for a non-CaptureError', () => {
    expect(captureErrorMessage(new Error('boom'))).toMatch(/Couldn’t reach/)
  })
})
