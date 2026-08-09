import { useEffect, useState } from 'react'
import { ActivityIndicator, StyleSheet, Text, View } from 'react-native'
import { useRouter } from 'expo-router'
import { Screen } from '../components/Screen'
import { isOnboardingComplete } from '../lib/store'
import { colors } from '../theme'

// Entry splash: route to onboarding the first time, or straight to the dashboard once the device
// has been set up (or when EXPO_PUBLIC_ESP32_BASE_URL points at a mock).
const ENV_BASE_URL = process.env.EXPO_PUBLIC_ESP32_BASE_URL

export default function Index() {
  const router = useRouter()
  const [target, setTarget] = useState<'/dashboard' | '/onboarding/turn-on' | null>(null)

  useEffect(() => {
    let active = true
    ;(async () => {
      // An explicit base URL (mock/dev) skips onboarding entirely.
      const onboarded = ENV_BASE_URL ? true : await isOnboardingComplete()
      if (active) setTarget(onboarded ? '/dashboard' : '/onboarding/turn-on')
    })()
    return () => {
      active = false
    }
  }, [])

  useEffect(() => {
    if (target) router.replace(target)
  }, [target, router])

  return (
    <Screen>
      <View style={styles.center}>
        <Text style={styles.brand}>Ticker Board</Text>
        <ActivityIndicator color={colors.accent} />
      </View>
    </Screen>
  )
}

const styles = StyleSheet.create({
  center: {
    flex: 1,
    alignItems: 'center',
    justifyContent: 'center',
    gap: 20,
  },
  brand: {
    fontSize: 22,
    fontWeight: '700',
    color: colors.text,
    letterSpacing: 0.5,
  },
})
