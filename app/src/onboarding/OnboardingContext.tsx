import { createContext, useContext, useMemo, useState, type ReactNode } from 'react'
import type { OnboardingState } from './flow'
import type { DeviceInfo } from '../lib/esp32'

interface OnboardingContextValue extends OnboardingState {
  setSelectedNetwork: (ssid: string | null) => void
  setSelectedSecured: (secured: boolean | undefined) => void
  setPassword: (password: string) => void
  /** Free-text watchlist the user enters on the password step; sent with /api/provision. */
  tickers: string
  setTickers: (tickers: string) => void
  // Optional data-source keys/URL entered on the keys step, sent with /api/provision so they land
  // in NVS at setup time (empty = the device keeps its compiled default). Never displayed back.
  finnhubKey: string
  setFinnhubKey: (key: string) => void
  fmpKey: string
  setFmpKey: (key: string) => void
  econUrl: string
  setEconUrl: (url: string) => void
  /** Optional weather location (free-text place) sent with /api/provision; empty = weather off. */
  location: string
  setLocation: (location: string) => void
  /** Identity read from the device's GET /api/info, used to seed the dashboard on completion. */
  deviceInfo: DeviceInfo | null
  setDeviceInfo: (info: DeviceInfo | null) => void
  reset: () => void
}

const OnboardingContext = createContext<OnboardingContextValue | null>(null)

export function OnboardingProvider({ children }: { children: ReactNode }) {
  const [selectedNetwork, setSelectedNetwork] = useState<string | null>(null)
  const [selectedSecured, setSelectedSecured] = useState<boolean | undefined>(undefined)
  const [password, setPassword] = useState('')
  const [tickers, setTickers] = useState('')
  const [finnhubKey, setFinnhubKey] = useState('')
  const [fmpKey, setFmpKey] = useState('')
  const [econUrl, setEconUrl] = useState('')
  const [location, setLocation] = useState('')
  const [deviceInfo, setDeviceInfo] = useState<DeviceInfo | null>(null)

  const value = useMemo<OnboardingContextValue>(
    () => ({
      selectedNetwork,
      selectedSecured,
      password,
      tickers,
      finnhubKey,
      fmpKey,
      econUrl,
      location,
      deviceInfo,
      setSelectedNetwork,
      setSelectedSecured,
      setPassword,
      setTickers,
      setFinnhubKey,
      setFmpKey,
      setEconUrl,
      setLocation,
      setDeviceInfo,
      reset: () => {
        setSelectedNetwork(null)
        setSelectedSecured(undefined)
        setPassword('')
        setTickers('')
        setFinnhubKey('')
        setFmpKey('')
        setEconUrl('')
        setLocation('')
        setDeviceInfo(null)
      },
    }),
    [selectedNetwork, selectedSecured, password, tickers, finnhubKey, fmpKey, econUrl, location, deviceInfo],
  )

  return <OnboardingContext.Provider value={value}>{children}</OnboardingContext.Provider>
}

export function useOnboarding(): OnboardingContextValue {
  const ctx = useContext(OnboardingContext)
  if (!ctx) throw new Error('useOnboarding must be used inside OnboardingProvider')
  return ctx
}
