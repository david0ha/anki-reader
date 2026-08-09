import { describe, it, expect } from '@jest/globals'
import { DEFAULT_BASE_URL, discoverDevice, normalizeBaseUrl, resolveBaseUrl } from './discovery'

describe('normalizeBaseUrl', () => {
  it('defaults the scheme to http:// for a bare host', () => {
    expect(normalizeBaseUrl('tickerboard.local')).toEqual({ ok: true, value: 'http://tickerboard.local' })
  })

  it('defaults the scheme for a bare IPv4', () => {
    expect(normalizeBaseUrl('192.168.0.42')).toEqual({ ok: true, value: 'http://192.168.0.42' })
  })

  it('keeps an explicit port', () => {
    expect(normalizeBaseUrl('192.168.0.42:8080')).toEqual({ ok: true, value: 'http://192.168.0.42:8080' })
  })

  it('lower-cases the host and strips a trailing slash + path', () => {
    expect(normalizeBaseUrl('http://TickerBoard.local/api/')).toEqual({
      ok: true,
      value: 'http://tickerboard.local',
    })
  })

  it('preserves an https scheme', () => {
    expect(normalizeBaseUrl('https://device.local')).toEqual({ ok: true, value: 'https://device.local' })
  })

  it('trims surrounding whitespace', () => {
    expect(normalizeBaseUrl('  192.168.1.5  ')).toEqual({ ok: true, value: 'http://192.168.1.5' })
  })

  it('rejects empty input', () => {
    expect(normalizeBaseUrl('   ')).toMatchObject({ ok: false, error: 'empty' })
  })

  it('rejects a non-http(s) scheme', () => {
    expect(normalizeBaseUrl('ftp://192.168.0.1')).toMatchObject({ ok: false, error: 'bad_scheme' })
  })

  it('rejects an out-of-range IPv4 octet', () => {
    expect(normalizeBaseUrl('999.1.1.1')).toMatchObject({ ok: false, error: 'bad_host' })
  })

  it('rejects a host with spaces', () => {
    expect(normalizeBaseUrl('my host')).toMatchObject({ ok: false })
  })

  it('rejects an out-of-range port', () => {
    expect(normalizeBaseUrl('192.168.0.1:99999')).toMatchObject({ ok: false, error: 'bad_port' })
  })
})

describe('resolveBaseUrl', () => {
  it('prefers a valid manual override over the saved value', () => {
    expect(resolveBaseUrl({ saved: 'http://1.2.3.4', manual: '10.0.0.9' })).toBe('http://10.0.0.9')
  })

  it('falls back to the saved value when no manual override', () => {
    expect(resolveBaseUrl({ saved: 'http://1.2.3.4' })).toBe('http://1.2.3.4')
  })

  it('ignores an invalid manual override and uses the saved value', () => {
    expect(resolveBaseUrl({ saved: 'http://1.2.3.4', manual: 'not a url!!' })).toBe('http://1.2.3.4')
  })

  it('falls back to the mDNS default when nothing valid is provided', () => {
    expect(resolveBaseUrl({})).toBe(DEFAULT_BASE_URL)
    expect(resolveBaseUrl({ saved: null, manual: '   ' })).toBe(DEFAULT_BASE_URL)
  })
})

describe('discoverDevice', () => {
  // A fake fetch keyed by URL: an entry can be `true` (200 ok), `false` (non-ok), or an Error to
  // throw (simulating an unreachable host / DNS failure for an .local name).
  function fakeFetch(map: Record<string, boolean | Error>) {
    const calls: string[] = []
    const fetchImpl = (async (url: string) => {
      calls.push(String(url))
      const r = map[String(url)]
      if (r instanceof Error) throw r
      return { ok: r ?? false } as Response
    }) as unknown as typeof fetch
    return { fetchImpl, calls }
  }

  it('resolves the first candidate when the saved IP is reachable', async () => {
    const { fetchImpl, calls } = fakeFetch({ 'http://192.168.0.42/api/info': true })
    const found = await discoverDevice(['192.168.0.42', 'http://tickerboard.local'], { fetchImpl })
    expect(found).toBe('http://192.168.0.42')
    expect(calls).toContain('http://192.168.0.42/api/info')
  })

  it('falls through to the second candidate when the first fails', async () => {
    const { fetchImpl } = fakeFetch({
      'http://192.168.0.42/api/info': new TypeError('Network request failed'),
      'http://tickerboard.local/api/info': true,
    })
    const found = await discoverDevice(['192.168.0.42', 'http://tickerboard.local'], { fetchImpl })
    expect(found).toBe('http://tickerboard.local')
  })

  it('prefers the earlier candidate even if a later one also succeeds', async () => {
    const { fetchImpl } = fakeFetch({
      'http://192.168.0.42/api/info': true,
      'http://tickerboard.local/api/info': true,
    })
    const found = await discoverDevice(['192.168.0.42', 'http://tickerboard.local'], { fetchImpl })
    expect(found).toBe('http://192.168.0.42')
  })

  it('returns null when every candidate fails', async () => {
    const { fetchImpl } = fakeFetch({
      'http://192.168.0.42/api/info': new TypeError('boom'),
      'http://tickerboard.local/api/info': false,
    })
    const found = await discoverDevice(['192.168.0.42', 'http://tickerboard.local'], { fetchImpl })
    expect(found).toBeNull()
  })

  it('returns null when given no valid candidates', async () => {
    const { fetchImpl, calls } = fakeFetch({})
    const found = await discoverDevice([null, undefined, '   ', 'not a url!!'], { fetchImpl })
    expect(found).toBeNull()
    expect(calls).toEqual([]) // nothing probed
  })

  it('de-dupes equivalent candidates so a host is probed once', async () => {
    const { fetchImpl, calls } = fakeFetch({ 'http://tickerboard.local/api/info': true })
    const found = await discoverDevice(
      ['tickerboard.local', 'http://tickerboard.local/'],
      { fetchImpl },
    )
    expect(found).toBe('http://tickerboard.local')
    expect(calls.length).toBe(1)
  })
})
