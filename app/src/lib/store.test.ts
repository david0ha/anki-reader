import { describe, it, expect, beforeEach } from '@jest/globals'
import AsyncStorage from '@react-native-async-storage/async-storage'
import {
  __resetStoreCacheForTests,
  clearDeviceBaseUrl,
  getDeviceBaseUrl,
  isOnboardingComplete,
  markOnboardingComplete,
  resetOnboarding,
  setDeviceBaseUrl,
} from './store'

beforeEach(async () => {
  await AsyncStorage.clear()
  __resetStoreCacheForTests()
})

describe('device base URL', () => {
  it('returns null when nothing is stored', async () => {
    expect(await getDeviceBaseUrl()).toBeNull()
  })

  it('normalizes before persisting and reads it back', async () => {
    expect(await setDeviceBaseUrl('192.168.0.42')).toBe(true)
    __resetStoreCacheForTests()
    expect(await getDeviceBaseUrl()).toBe('http://192.168.0.42')
  })

  it('rejects an invalid URL and stores nothing', async () => {
    expect(await setDeviceBaseUrl('not a url')).toBe(false)
    __resetStoreCacheForTests()
    expect(await getDeviceBaseUrl()).toBeNull()
  })

  it('clears a stored URL', async () => {
    await setDeviceBaseUrl('http://1.2.3.4')
    await clearDeviceBaseUrl()
    __resetStoreCacheForTests()
    expect(await getDeviceBaseUrl()).toBeNull()
  })

  it('serves the in-memory cache without re-reading disk', async () => {
    await setDeviceBaseUrl('http://5.6.7.8')
    // No cache reset: the cached value should come straight back.
    expect(await getDeviceBaseUrl()).toBe('http://5.6.7.8')
  })
})

describe('onboarding flag', () => {
  it('is false until marked complete', async () => {
    expect(await isOnboardingComplete()).toBe(false)
  })

  it('persists complete and reads it back across a cache reset', async () => {
    await markOnboardingComplete()
    __resetStoreCacheForTests()
    expect(await isOnboardingComplete()).toBe(true)
  })

  it('resetOnboarding clears the flag', async () => {
    await markOnboardingComplete()
    await resetOnboarding()
    __resetStoreCacheForTests()
    expect(await isOnboardingComplete()).toBe(false)
  })
})
