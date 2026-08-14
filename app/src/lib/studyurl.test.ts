import { describe, it, expect } from '@jest/globals'
import { STUDY_URL_MAX_LEN } from './esp32'
import { validateStudyUrl, studyUrlErrorMessage } from './studyurl'

// These cases are the firmware's own, from components/provisioning/test/test_prov_config.c and
// prov_validate_study_url(). If this file and that one ever disagree, one of the two is wrong —
// which is the point of writing them out twice.

describe('validateStudyUrl — what the board accepts', () => {
  it('accepts empty as "use the demo card"', () => {
    expect(validateStudyUrl('')).toEqual({ ok: true, value: '', empty: true })
    expect(validateStudyUrl('   ')).toEqual({ ok: true, value: '', empty: true })
  })

  it('accepts http and https with a host', () => {
    expect(validateStudyUrl('http://mac.local:8123/kanji.json')).toEqual({
      ok: true,
      value: 'http://mac.local:8123/kanji.json',
    })
    expect(validateStudyUrl('https://example.com/k.json').ok).toBe(true)
    expect(validateStudyUrl('http://192.168.0.9:8000/kanji.json').ok).toBe(true)
  })

  it('accepts a bare host with no path — the board will fetch "/"', () => {
    expect(validateStudyUrl('http://mac.local').ok).toBe(true)
  })

  it('trims surrounding whitespace before measuring or sending', () => {
    // A URL pasted from a terminal usually carries a trailing newline. Those bytes count against
    // the board's 128-byte buffer and then break host resolution.
    expect(validateStudyUrl('  http://mac.local/k.json\n')).toEqual({
      ok: true,
      value: 'http://mac.local/k.json',
    })
  })
})

describe('validateStudyUrl — what the board rejects', () => {
  it('rejects a missing or wrong scheme', () => {
    expect(validateStudyUrl('mac.local/kanji.json')).toMatchObject({ ok: false, error: 'bad_scheme' })
    expect(validateStudyUrl('ftp://mac.local/k.json')).toMatchObject({ ok: false, error: 'bad_scheme' })
    expect(validateStudyUrl('HTTP://mac.local')).toMatchObject({ ok: false, error: 'bad_scheme' })
  })

  it('rejects a scheme with no host', () => {
    // Both parse as URLs and both fail at connect time with an error the user cannot act on.
    expect(validateStudyUrl('http://')).toMatchObject({ ok: false, error: 'no_host' })
    expect(validateStudyUrl('http:///kanji.json')).toMatchObject({ ok: false, error: 'no_host' })
    expect(validateStudyUrl('https://')).toMatchObject({ ok: false, error: 'no_host' })
  })

  it('rejects anything longer than the board’s buffer', () => {
    const fits = 'http://h/' + 'a'.repeat(STUDY_URL_MAX_LEN - 'http://h/'.length)
    expect(fits.length).toBe(STUDY_URL_MAX_LEN)
    expect(validateStudyUrl(fits).ok).toBe(true)
    expect(validateStudyUrl(fits + 'a')).toMatchObject({ ok: false, error: 'too_long' })
  })

  it('measures the length in UTF-8 bytes, as the board’s buffer does', () => {
    // 40 Korean characters are 120 UTF-8 bytes; with the scheme and host they overflow 128 even
    // though the string is only 49 characters long.
    const url = 'http://h/' + '가'.repeat(40)
    expect(url.length).toBeLessThan(STUDY_URL_MAX_LEN)
    expect(validateStudyUrl(url)).toMatchObject({ ok: false, error: 'too_long' })
  })
})

describe('studyUrlErrorMessage', () => {
  it('says something different for each rejection', () => {
    const messages = new Set(
      ['too_long', 'bad_scheme', 'no_host'].map((e) =>
        studyUrlErrorMessage({ ok: false, error: e as never }),
      ),
    )
    expect(messages.size).toBe(3)
  })

  it('has a fallback for a result with no error code', () => {
    expect(studyUrlErrorMessage({ ok: false })).toMatch(/valid address/)
  })
})
