import { describe, it, expect } from '@jest/globals'
import { VAULT_URL_MAX_LEN } from './esp32'
import { validateVaultUrl, vaultUrlErrorMessage } from './vaulturl'

// These cases are the firmware's own, from components/provisioning/test/test_prov_config.c and
// prov_validate_vault_url(). If this file and that one ever disagree, one of the two is wrong —
// which is the point of writing them out twice.

describe('validateVaultUrl — what the board accepts', () => {
  it('accepts empty as "use the demo snapshot"', () => {
    expect(validateVaultUrl('')).toEqual({ ok: true, value: '', empty: true })
    expect(validateVaultUrl('   ')).toEqual({ ok: true, value: '', empty: true })
  })

  it('accepts http and https with a host', () => {
    expect(validateVaultUrl('http://mac.local:8123/vault.json')).toEqual({
      ok: true,
      value: 'http://mac.local:8123/vault.json',
    })
    expect(validateVaultUrl('https://example.com/v.json').ok).toBe(true)
    expect(validateVaultUrl('http://192.168.0.9:8000/vault.json').ok).toBe(true)
  })

  it('accepts a bare host with no path — the board will fetch "/"', () => {
    expect(validateVaultUrl('http://mac.local').ok).toBe(true)
  })

  it('trims surrounding whitespace before measuring or sending', () => {
    // A URL pasted from a terminal usually carries a trailing newline. Those bytes count against
    // the board's 128-byte buffer and then break host resolution.
    expect(validateVaultUrl('  http://mac.local/v.json\n')).toEqual({
      ok: true,
      value: 'http://mac.local/v.json',
    })
  })
})

describe('validateVaultUrl — what the board rejects', () => {
  it('rejects a missing or wrong scheme', () => {
    expect(validateVaultUrl('mac.local/vault.json')).toMatchObject({ ok: false, error: 'bad_scheme' })
    expect(validateVaultUrl('ftp://mac.local/v.json')).toMatchObject({ ok: false, error: 'bad_scheme' })
    expect(validateVaultUrl('HTTP://mac.local')).toMatchObject({ ok: false, error: 'bad_scheme' })
  })

  it('rejects a scheme with no host', () => {
    // Both parse as URLs and both fail at connect time with an error the user cannot act on.
    expect(validateVaultUrl('http://')).toMatchObject({ ok: false, error: 'no_host' })
    expect(validateVaultUrl('http:///vault.json')).toMatchObject({ ok: false, error: 'no_host' })
    expect(validateVaultUrl('https://')).toMatchObject({ ok: false, error: 'no_host' })
  })

  it('rejects anything longer than the board’s buffer', () => {
    const fits = 'http://h/' + 'a'.repeat(VAULT_URL_MAX_LEN - 'http://h/'.length)
    expect(fits.length).toBe(VAULT_URL_MAX_LEN)
    expect(validateVaultUrl(fits).ok).toBe(true)
    expect(validateVaultUrl(fits + 'a')).toMatchObject({ ok: false, error: 'too_long' })
  })

  it('measures the length in UTF-8 bytes, as the board’s buffer does', () => {
    // 40 Korean characters are 120 UTF-8 bytes; with the scheme and host they overflow 128 even
    // though the string is only 49 characters long.
    const url = 'http://h/' + '가'.repeat(40)
    expect(url.length).toBeLessThan(VAULT_URL_MAX_LEN)
    expect(validateVaultUrl(url)).toMatchObject({ ok: false, error: 'too_long' })
  })
})

describe('vaultUrlErrorMessage', () => {
  it('says something different for each rejection', () => {
    const messages = new Set(
      ['too_long', 'bad_scheme', 'no_host'].map((e) =>
        vaultUrlErrorMessage({ ok: false, error: e as never }),
      ),
    )
    expect(messages.size).toBe(3)
  })

  it('has a fallback for a result with no error code', () => {
    expect(vaultUrlErrorMessage({ ok: false })).toMatch(/valid address/)
  })
})
