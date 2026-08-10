import { useEffect, useRef, useState } from 'react'
import {
  ActivityIndicator,
  KeyboardAvoidingView,
  Platform,
  Pressable,
  ScrollView,
  StyleSheet,
  Text,
  TextInput,
  View,
} from 'react-native'
import { Ionicons } from '@expo/vector-icons'
import { useRouter } from 'expo-router'
import { Screen } from '../../components/Screen'
import { BackButton } from '../../components/BackButton'
import { useOnboarding } from '../../onboarding/OnboardingContext'
import { ONBOARDING_ROUTES } from '../../onboarding/flow'
import { esp32, Esp32Error } from '../../lib/esp32'
import { validateVaultUrl, vaultUrlErrorMessage } from '../../lib/vaulturl'
import { colors, layout, radius } from '../../theme'

// Map a provisioning failure to a short, user-facing reason.
function failureMessage(e: unknown): string {
  if (e instanceof Esp32Error) {
    switch (e.code) {
      case 'network_error':
        return 'Lost connection to the board. Make sure you’re still on its setup Wi-Fi.'
      case 'pass_too_long':
        return 'That password is too long (max 64 characters).'
      case 'ssid_empty':
      case 'ssid_too_long':
        return 'Please check the Wi-Fi name and try again.'
      case 'vault_url_invalid':
        return 'The board rejected the snapshot URL. Go back and check it.'
      case 'too_large':
        return 'That was too much for the board to accept. Shorten the snapshot URL and try again.'
      default:
        return 'Something went wrong sending your settings. Please try again.'
    }
  }
  return 'Something went wrong. Please try again.'
}

export default function Password() {
  const router = useRouter()
  const { selectedNetwork, setSelectedNetwork, selectedSecured, password, setPassword, vaultUrl, setDeviceInfo } =
    useOnboarding()
  // "Other…" leaves selectedNetwork null; the user types the SSID here.
  const isManualSsid = selectedNetwork === null
  const [manualSsid, setManualSsid] = useState('')
  const ssid = isManualSsid ? manualSsid.trim() : selectedNetwork

  const [reveal, setReveal] = useState(false)
  const [pending, setPending] = useState(false)
  const [error, setError] = useState<string | null>(null)

  // The provision + poll can run up to ~45s; if the user leaves, don't run setState or fire a
  // router.replace from an unmounted screen.
  const mounted = useRef(true)
  useEffect(() => () => {
    mounted.current = false
  }, [])

  const passwordOk = selectedSecured === false || password.trim().length > 0
  const ssidOk = !!ssid && ssid.length > 0
  const enabled = ssidOk && passwordOk && !pending

  const join = async () => {
    if (!enabled || !ssid) return

    // Re-validate the snapshot URL here, not only on the step that collected it. This is the last
    // point before a ~45s join, and the board's own rejection would land on the far side of it.
    const vu = validateVaultUrl(vaultUrl)
    if (!vu.ok) {
      setError(vaultUrlErrorMessage(vu))
      return
    }

    setError(null)
    setPending(true)
    if (isManualSsid) setSelectedNetwork(ssid) // remember it for the completion screen copy
    try {
      // '' is meaningful and is sent as such: it puts the board on its built-in demo snapshot.
      await esp32.provision(ssid, password, vu.value ?? '')
    } catch (e) {
      if (!mounted.current) return
      setPending(false)
      setError(failureMessage(e))
      return
    }

    // Credentials accepted; the board verifies them with a live join while its SoftAP stays up.
    // Poll until it reports the outcome (tolerating the brief AP drop during the channel hop).
    const result = await esp32.waitForConnected()
    if (!mounted.current) return
    setPending(false)
    if (result.outcome === 'connected') {
      // Refresh identity (now includes the STA ip/fw if the board re-reports it).
      esp32
        .getInfo()
        .then(setDeviceInfo)
        .catch((e) => console.warn('[onboarding] device info refresh failed', e))
      router.replace(ONBOARDING_ROUTES.complete)
    } else if (result.outcome === 'failed') {
      setError(
        result.reason === 'auth_failed'
          ? 'That password didn’t work. Please check it and try again.'
          : 'The board couldn’t join that network. Please try again.',
      )
    } else {
      // 'timeout': on a single-radio board a SUCCESSFUL join hops the SoftAP onto the home AP's
      // channel, so the phone loses the setup AP and never reads 'connected'. A genuine failure
      // (e.g. bad password) is instead reported reliably as 'failed' above (the board restores its
      // own channel first). So a timeout almost always means success — proceed to completion,
      // which has the user rejoin home Wi-Fi and re-confirms the board over the LAN (mDNS / IP).
      router.replace(ONBOARDING_ROUTES.complete)
    }
  }

  return (
    <Screen>
      <KeyboardAvoidingView
        style={styles.flex}
        behavior={Platform.OS === 'ios' ? 'padding' : undefined}
      >
        <View style={styles.titleRow}>
          <BackButton onPress={() => router.back()} />
          <Text style={styles.title}>Connect Wi-Fi</Text>
          <View style={styles.backSpacer} />
        </View>

        <ScrollView contentContainerStyle={styles.body} keyboardShouldPersistTaps="handled">
          {isManualSsid ? (
            <View style={styles.field}>
              <Text style={styles.label}>Network name (SSID)</Text>
              <View style={styles.inputRow}>
                <TextInput
                  value={manualSsid}
                  onChangeText={setManualSsid}
                  placeholder="My Home Wi-Fi"
                  placeholderTextColor={colors.textFaint}
                  autoCapitalize="none"
                  autoCorrect={false}
                  editable={!pending}
                  style={styles.input}
                />
              </View>
            </View>
          ) : (
            <Text style={styles.kicker}>Enter the password for {selectedNetwork}</Text>
          )}

          <View style={styles.field}>
            <Text style={styles.label}>Password</Text>
            <View style={styles.inputRow}>
              <TextInput
                value={password}
                onChangeText={setPassword}
                placeholder={selectedSecured === false ? '(open network — none needed)' : 'password'}
                placeholderTextColor={colors.textFaint}
                secureTextEntry={!reveal}
                autoCapitalize="none"
                autoCorrect={false}
                editable={!pending}
                style={styles.input}
              />
              <Pressable accessibilityLabel="Toggle password visibility" onPress={() => setReveal((r) => !r)} hitSlop={8}>
                <Ionicons name={reveal ? 'eye-outline' : 'eye-off-outline'} size={22} color={colors.textDim} />
              </Pressable>
            </View>
          </View>

          {/* What the board will do once it is on the network — shown here because this screen is
              the last chance to go back and change it. */}
          <Text style={styles.hint}>
            {vaultUrl.trim()
              ? `Once connected, the board will fetch ${vaultUrl.trim()}.`
              : 'No snapshot URL set — the board will show its built-in demo data. You can add an address later from Settings.'}
          </Text>

          {pending ? (
            <Text style={styles.status}>
              <ActivityIndicator color={colors.accent} /> Connecting to {ssid}… this can take up to a minute.
            </Text>
          ) : error ? (
            <Text style={styles.error}>{error}</Text>
          ) : null}
        </ScrollView>

        <View style={styles.ctaWrap}>
          <Pressable
            accessibilityRole="button"
            accessibilityState={{ disabled: !enabled, busy: pending }}
            onPress={join}
            disabled={!enabled}
            style={({ pressed }) => [
              styles.cta,
              !enabled && styles.ctaDisabled,
              pressed && enabled && styles.ctaPressed,
            ]}
          >
            {pending ? (
              <ActivityIndicator color={colors.ink} />
            ) : (
              <Text style={[styles.ctaLabel, !enabled && styles.ctaLabelDisabled]}>JOIN</Text>
            )}
          </Pressable>
        </View>
      </KeyboardAvoidingView>
    </Screen>
  )
}

const styles = StyleSheet.create({
  flex: { flex: 1 },
  titleRow: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    paddingHorizontal: layout.gutter,
    height: 56,
  },
  title: {
    fontSize: 18,
    fontWeight: '700',
    color: colors.text,
  },
  backSpacer: {
    width: 42,
  },
  body: {
    paddingHorizontal: layout.gutter,
    paddingTop: 16,
    gap: 20,
  },
  kicker: {
    fontSize: 14,
    color: colors.textDim,
  },
  field: {
    gap: 8,
  },
  label: {
    fontSize: 14,
    fontWeight: '600',
    color: colors.text,
  },
  inputRow: {
    minHeight: 48,
    borderRadius: radius.md,
    borderWidth: StyleSheet.hairlineWidth,
    borderColor: colors.border,
    backgroundColor: colors.surface,
    paddingHorizontal: 14,
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
  },
  input: {
    flex: 1,
    color: colors.text,
    fontSize: 16,
    paddingVertical: 12,
  },
  hint: {
    fontSize: 12,
    color: colors.textFaint,
    lineHeight: 16,
  },
  status: {
    fontSize: 13,
    color: colors.textDim,
    lineHeight: 18,
  },
  error: {
    fontSize: 13,
    color: colors.down,
    lineHeight: 18,
  },
  ctaWrap: {
    paddingHorizontal: layout.gutter,
    paddingBottom: 8,
    paddingTop: 8,
  },
  cta: {
    height: 52,
    borderRadius: radius.md,
    backgroundColor: colors.accent,
    alignItems: 'center',
    justifyContent: 'center',
  },
  ctaDisabled: {
    opacity: 0.5,
  },
  ctaPressed: {
    opacity: 0.8,
  },
  ctaLabel: {
    fontSize: 16,
    fontWeight: '700',
    color: colors.ink,
    letterSpacing: 0.5,
  },
  ctaLabelDisabled: {
    color: colors.ink,
  },
})
