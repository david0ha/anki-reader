// Client-side validation of the vault snapshot URL, mirroring the firmware's
// `prov_validate_vault_url()` (components/provisioning/prov_config.c).
//
// Why mirror it at all, when the board validates anyway: during onboarding the board's answer
// arrives ~45 seconds later, on the far side of a Wi-Fi join the user cannot undo. A typo caught
// here costs nothing; the same typo caught there costs the whole join. And in settings, a rejected
// write comes back as the single code `vault_url_invalid`, which cannot say *what* was wrong.
//
// The firmware's rule, exactly:
//   - empty is valid — it means "run on the built-in demo snapshot"
//   - at most PROV_URL_MAX_LEN (128) bytes
//   - must start with http:// or https://
//   - must have at least one character of host before the path
//
// This file adds nothing to that rule. A URL this accepts and the board rejects (or vice versa) is
// a bug in one of the two, which is what `vaulturl.test.ts` is for.

import { VAULT_URL_MAX_LEN } from './esp32'

export type VaultUrlError = 'too_long' | 'bad_scheme' | 'no_host'

export interface VaultUrlResult {
  ok: boolean
  /** The trimmed URL when ok — this is what should be sent to the board. */
  value?: string
  /** True when the (trimmed) input was empty, i.e. "use the demo snapshot". */
  empty?: boolean
  error?: VaultUrlError
}

/**
 * Validate free text the user typed as a snapshot URL.
 *
 * Leading/trailing whitespace is trimmed first — a URL pasted from a terminal usually carries a
 * trailing newline, and the board would count those bytes against the length limit and then fail
 * to resolve the host.
 *
 * The length is measured in **UTF-8 bytes**, not characters, because the firmware's limit is a C
 * buffer. A URL with non-ASCII in it (an IDN host, a Korean path) is longer on the wire than it
 * looks in the field.
 */
export function validateVaultUrl(input: string): VaultUrlResult {
  const url = (input ?? '').trim()
  if (url.length === 0) return { ok: true, value: '', empty: true }

  if (utf8Length(url) > VAULT_URL_MAX_LEN) return { ok: false, error: 'too_long' }

  let rest: string
  if (url.startsWith('http://')) {
    rest = url.slice(7)
  } else if (url.startsWith('https://')) {
    rest = url.slice(8)
  } else {
    return { ok: false, error: 'bad_scheme' }
  }

  // "http://" and "http:///vault.json" both parse as URLs and both fail at connect time with an
  // error the user cannot act on, so they are rejected here as the firmware rejects them.
  if (rest.length === 0 || rest.startsWith('/')) return { ok: false, error: 'no_host' }

  return { ok: true, value: url }
}

/** A sentence for each rejection, for the field's error line. */
export function vaultUrlErrorMessage(result: VaultUrlResult): string {
  switch (result.error) {
    case 'too_long':
      return `That address is too long — the board stores at most ${VAULT_URL_MAX_LEN} characters.`
    case 'bad_scheme':
      return 'The address must start with http:// or https://.'
    case 'no_host':
      return 'The address is missing a host, e.g. http://mymac.local:8123/vault.json.'
    default:
      return 'That doesn’t look like a valid address.'
  }
}

/** Byte length of a string as UTF-8, without depending on TextEncoder (absent on old Hermes). */
function utf8Length(s: string): number {
  let n = 0
  for (const ch of s) {
    const cp = ch.codePointAt(0) as number
    if (cp < 0x80) n += 1
    else if (cp < 0x800) n += 2
    else if (cp < 0x10000) n += 3
    else n += 4
  }
  return n
}
