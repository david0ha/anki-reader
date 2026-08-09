// App-wide device connection state. Holds the resolved control-API base URL and a client bound
// to it, plus helpers to (re)point at a new device. The base URL is loaded once from storage on
// mount; changing it (after onboarding, or via settings) re-creates the client and persists it.
//
// EXPO_PUBLIC_ESP32_BASE_URL, when set (e.g. pointing at scripts/mock-esp32.js), overrides the
// stored value so simulator development hits the mock without any onboarding.

import {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useMemo,
  useState,
  type ReactNode,
} from 'react'
import { createEsp32Client, type Esp32Client } from './esp32'
import { resolveBaseUrl } from './discovery'
import { getDeviceBaseUrl, setDeviceBaseUrl as persistBaseUrl } from './store'

const ENV_BASE_URL = process.env.EXPO_PUBLIC_ESP32_BASE_URL

interface DeviceContextValue {
  /** Resolved control-API base URL, or null while it's still loading from storage. */
  baseUrl: string | null
  /** A client bound to the current base URL (null until baseUrl resolves). */
  client: Esp32Client | null
  /** Whether a stored/env base URL was found (vs. falling back to the mDNS default). */
  hasDevice: boolean
  /** Point at a new device: validate+persist, then re-create the client. Returns false if invalid. */
  setBaseUrl: (url: string) => Promise<boolean>
}

const DeviceContext = createContext<DeviceContextValue | null>(null)

export function DeviceProvider({ children }: { children: ReactNode }) {
  const [baseUrl, setBaseUrlState] = useState<string | null>(null)
  const [hasDevice, setHasDevice] = useState(false)

  useEffect(() => {
    let active = true
    ;(async () => {
      if (ENV_BASE_URL) {
        if (active) {
          setBaseUrlState(resolveBaseUrl({ manual: ENV_BASE_URL }))
          setHasDevice(true)
        }
        return
      }
      const saved = await getDeviceBaseUrl()
      if (!active) return
      setBaseUrlState(resolveBaseUrl({ saved }))
      setHasDevice(!!saved)
    })()
    return () => {
      active = false
    }
  }, [])

  const setBaseUrl = useCallback(async (url: string) => {
    const ok = await persistBaseUrl(url)
    if (!ok) return false
    const saved = await getDeviceBaseUrl()
    setBaseUrlState(resolveBaseUrl({ saved }))
    setHasDevice(true)
    return true
  }, [])

  // Recreate the client whenever the base URL changes (so all screens share one client per URL).
  const client = useMemo(() => (baseUrl ? createEsp32Client({ baseUrl }) : null), [baseUrl])

  const value = useMemo<DeviceContextValue>(
    () => ({ baseUrl, client, hasDevice, setBaseUrl }),
    [baseUrl, client, hasDevice, setBaseUrl],
  )

  return <DeviceContext.Provider value={value}>{children}</DeviceContext.Provider>
}

export function useDevice(): DeviceContextValue {
  const ctx = useContext(DeviceContext)
  if (!ctx) throw new Error('useDevice must be used inside DeviceProvider')
  return ctx
}
