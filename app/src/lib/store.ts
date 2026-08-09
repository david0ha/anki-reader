// Thin AsyncStorage wrapper for the two bits of state the app persists between launches:
//   - the device's last-known control base URL (so the dashboard reconnects without rediscovery)
//   - an onboarding-complete flag (so the entry screen routes to the dashboard, not the wizard)
//
// Everything is best-effort: a storage failure never throws into the UI, it just behaves as if
// nothing was saved (worst case: the user re-runs onboarding). Reads are cached in-memory for
// the session so the entry screen doesn't re-hit disk on every render.

import AsyncStorage from '@react-native-async-storage/async-storage'
import { normalizeBaseUrl } from './discovery'

const KEY_BASE_URL = 'tickerboard.deviceBaseUrl'
const KEY_ONBOARDED = 'tickerboard.onboardingComplete'

let onboardedCache: boolean | null = null
let baseUrlCache: string | null | undefined // undefined = not yet read

export async function getDeviceBaseUrl(): Promise<string | null> {
  if (baseUrlCache !== undefined) return baseUrlCache ?? null
  try {
    baseUrlCache = await AsyncStorage.getItem(KEY_BASE_URL)
  } catch {
    baseUrlCache = null
  }
  return baseUrlCache ?? null
}

/** Persist a base URL, normalizing it first. Invalid input is ignored (returns false). */
export async function setDeviceBaseUrl(url: string): Promise<boolean> {
  const norm = normalizeBaseUrl(url)
  if (!norm.ok || !norm.value) return false
  baseUrlCache = norm.value
  try {
    await AsyncStorage.setItem(KEY_BASE_URL, norm.value)
  } catch {
    // best-effort
  }
  return true
}

export async function clearDeviceBaseUrl(): Promise<void> {
  baseUrlCache = null
  try {
    await AsyncStorage.removeItem(KEY_BASE_URL)
  } catch {
    // best-effort
  }
}

export async function isOnboardingComplete(): Promise<boolean> {
  if (onboardedCache !== null) return onboardedCache
  try {
    onboardedCache = (await AsyncStorage.getItem(KEY_ONBOARDED)) === 'true'
  } catch {
    onboardedCache = false
  }
  return onboardedCache
}

export async function markOnboardingComplete(): Promise<void> {
  onboardedCache = true
  try {
    await AsyncStorage.setItem(KEY_ONBOARDED, 'true')
  } catch {
    // best-effort: cost of failure is showing the wizard again
  }
}

/** Reset both flags — used by "re-run onboarding" in settings. Mainly for completeness/tests. */
export async function resetOnboarding(): Promise<void> {
  onboardedCache = false
  try {
    await AsyncStorage.removeItem(KEY_ONBOARDED)
  } catch {
    // best-effort
  }
}

/** Test hook: drop the in-memory caches so a fresh read hits the (mocked) store. */
export function __resetStoreCacheForTests(): void {
  onboardedCache = null
  baseUrlCache = undefined
}
