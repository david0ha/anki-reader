import { Stack } from 'expo-router'
import { OnboardingProvider } from '../../onboarding/OnboardingContext'

export default function OnboardingLayout() {
  // No auth gate — the app is local-only. The provider carries the wizard's collected state
  // (network, password, watchlist, device identity) across the steps.
  return (
    <OnboardingProvider>
      <Stack screenOptions={{ headerShown: false }} />
    </OnboardingProvider>
  )
}
