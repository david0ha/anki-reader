import { useState } from 'react'
import { StyleSheet, Text, View } from 'react-native'
import { useRouter } from 'expo-router'
import { Screen } from '../../components/Screen'
import { Button } from '../../components/Button'
import { IconBadge } from '../../components/IconBadge'
import { useOnboarding } from '../../onboarding/OnboardingContext'
import { useDevice } from '../../lib/device'
import { markOnboardingComplete } from '../../lib/store'
import { DEFAULT_BASE_URL } from '../../lib/discovery'
import { colors, layout } from '../../theme'

export default function Complete() {
  const router = useRouter()
  const { deviceInfo, selectedNetwork, reset } = useOnboarding()
  const { setBaseUrl } = useDevice()
  const [busy, setBusy] = useState(false)

  const getStarted = async () => {
    if (busy) return
    setBusy(true)

    // The board has rebooted into station mode. Persist a control base URL for the dashboard:
    // prefer the station IP it reported (most reliable, no mDNS needed), else fall back to the
    // mDNS hostname. The dashboard will refine this once it can reach the device on the LAN.
    const ip = deviceInfo?.ip?.trim()
    const baseUrl = ip ? `http://${ip}` : DEFAULT_BASE_URL
    await setBaseUrl(baseUrl)
    await markOnboardingComplete()
    reset()
    router.replace('/dashboard')
  }

  return (
    <Screen style={styles.screen}>
      <View style={styles.center}>
        <IconBadge name="checkmark-circle" size={56} />
        <Text style={styles.title}>Setup complete</Text>
        <Text style={styles.subtitle}>
          {selectedNetwork
            ? `Your board is connected to ‘${selectedNetwork}’.`
            : 'Your board is connected.'}
        </Text>
        <Text style={styles.guidance}>
          Reconnect your phone to that same Wi-Fi network, then tap Open Dashboard to control your
          board over the local network.
        </Text>
      </View>

      <Button label={busy ? 'OPENING…' : 'OPEN DASHBOARD'} onPress={getStarted} loading={busy} />
    </Screen>
  )
}

const styles = StyleSheet.create({
  screen: {
    paddingHorizontal: layout.gutter,
    paddingBottom: 8,
  },
  center: {
    flex: 1,
    alignItems: 'center',
    justifyContent: 'center',
    gap: 18,
  },
  title: {
    fontSize: 30,
    fontWeight: '700',
    color: colors.text,
    letterSpacing: 0.3,
  },
  subtitle: {
    fontSize: 17,
    color: colors.textDim,
    textAlign: 'center',
  },
  guidance: {
    fontSize: 14,
    color: colors.textFaint,
    textAlign: 'center',
    lineHeight: 20,
    maxWidth: 300,
  },
})
