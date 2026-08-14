import { createContext, useContext, useMemo, useState, type ReactNode } from 'react'
import type { OnboardingState } from './flow'
import type { DeviceInfo } from '../lib/esp32'

interface OnboardingContextValue extends OnboardingState {
  setSelectedNetwork: (ssid: string | null) => void
  setSelectedSecured: (secured: boolean | undefined) => void
  setPassword: (password: string) => void
  /** The study URL entered on the study step; sent with /api/provision. */
  setStudyUrl: (url: string) => void
  /** Identity read from the board's GET /api/info, used to seed the dashboard on completion. */
  deviceInfo: DeviceInfo | null
  setDeviceInfo: (info: DeviceInfo | null) => void
  reset: () => void
}

const OnboardingContext = createContext<OnboardingContextValue | null>(null)

export function OnboardingProvider({ children }: { children: ReactNode }) {
  const [selectedNetwork, setSelectedNetwork] = useState<string | null>(null)
  const [selectedSecured, setSelectedSecured] = useState<boolean | undefined>(undefined)
  const [password, setPassword] = useState('')
  const [studyUrl, setStudyUrl] = useState('')
  const [deviceInfo, setDeviceInfo] = useState<DeviceInfo | null>(null)

  const value = useMemo<OnboardingContextValue>(
    () => ({
      selectedNetwork,
      selectedSecured,
      password,
      studyUrl,
      deviceInfo,
      setSelectedNetwork,
      setSelectedSecured,
      setPassword,
      setStudyUrl,
      setDeviceInfo,
      reset: () => {
        setSelectedNetwork(null)
        setSelectedSecured(undefined)
        setPassword('')
        setStudyUrl('')
        setDeviceInfo(null)
      },
    }),
    [selectedNetwork, selectedSecured, password, studyUrl, deviceInfo],
  )

  return <OnboardingContext.Provider value={value}>{children}</OnboardingContext.Provider>
}

export function useOnboarding(): OnboardingContextValue {
  const ctx = useContext(OnboardingContext)
  if (!ctx) throw new Error('useOnboarding must be used inside OnboardingProvider')
  return ctx
}
