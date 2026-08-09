import { useCallback, useEffect, useState } from 'react'
import {
  KeyboardAvoidingView,
  Platform,
  Pressable,
  ScrollView,
  StyleSheet,
  Text,
  TextInput,
  View,
} from 'react-native'
import { useRouter } from 'expo-router'
import { Screen } from '../components/Screen'
import { BackButton } from '../components/BackButton'
import { Button } from '../components/Button'
import { Card } from '../components/Card'
import { LocationAutocomplete } from '../components/LocationAutocomplete'
import { useDevice } from '../lib/device'
import { type DeviceInfo, type StockKeys, type StockWeather } from '../lib/esp32'
import { DEFAULT_HOST, discoverDevice, normalizeBaseUrl } from '../lib/discovery'
import { clearDeviceBaseUrl, getDeviceBaseUrl, resetOnboarding } from '../lib/store'
import { colors, layout, radius, space } from '../theme'

export default function Settings() {
  const router = useRouter()
  const { client, baseUrl, setBaseUrl } = useDevice()

  const [info, setInfo] = useState<DeviceInfo | null>(null)
  const [infoError, setInfoError] = useState(false)
  const [host, setHost] = useState('')
  const [hostError, setHostError] = useState<string | null>(null)
  const [saved, setSaved] = useState(false)

  // Which data-source keys are currently configured on the device (presence only — the firmware
  // never exposes the stored values). Loaded from GET /api/stock/state alongside device info.
  const [keys, setKeysState] = useState<StockKeys | null>(null)

  // Configured weather location + the device's resolved current weather (GET /api/stock/state).
  // Unlike keys, the location is plain text the device echoes back, so we can prefill the editor.
  const [location, setLocationState] = useState<string | null>(null)
  const [weather, setWeatherState] = useState<StockWeather | null>(null)

  // Reconnect ("find device") UI state.
  const [reconnecting, setReconnecting] = useState(false)
  const [reconnectMsg, setReconnectMsg] = useState<string | null>(null)

  // Pre-fill the host field with the current base URL (sans scheme, for friendlier editing).
  useEffect(() => {
    if (baseUrl) setHost(baseUrl.replace(/^https?:\/\//, ''))
  }, [baseUrl])

  const loadInfo = useCallback(async () => {
    if (!client) return
    setInfoError(false)
    try {
      setInfo(await client.getInfo())
    } catch {
      setInfoError(true)
    }
    // Best-effort: also pull which keys are set (data-sources) plus the weather location/resolved
    // weather so those sections reflect the device.
    try {
      const st = await client.getState()
      setKeysState(st.keys)
      setLocationState(st.location)
      setWeatherState(st.weather)
    } catch {
      // leave the last-known values; the sections just show "unknown" until a state read succeeds
    }
  }, [client])

  useEffect(() => {
    loadInfo()
  }, [loadInfo])

  // Re-probe the LAN for the board (saved IP + tickerboard.local) and persist whichever answers.
  // Used after the user rejoins their home Wi-Fi and the saved address has gone stale.
  const reconnect = useCallback(async () => {
    setReconnecting(true)
    setReconnectMsg(null)
    try {
      const saved = await getDeviceBaseUrl()
      const found = await discoverDevice([info?.ip, saved, `http://${DEFAULT_HOST}`])
      if (found) {
        await setBaseUrl(found)
        setReconnectMsg(`Found your board at ${found.replace(/^https?:\/\//, '')}.`)
        loadInfo()
      } else {
        setReconnectMsg('Couldn’t find the board. Make sure it’s powered on and on this Wi-Fi.')
      }
    } finally {
      setReconnecting(false)
    }
  }, [info?.ip, setBaseUrl, loadInfo])

  const updateKey = useCallback(
    async (patch: Parameters<NonNullable<typeof client>['setKeys']>[0]): Promise<boolean> => {
      if (!client) return false
      try {
        await client.setKeys(patch)
        // Re-read presence so the "set / not set" labels reflect the change.
        try {
          setKeysState((await client.getState()).keys)
        } catch {
          // ignore — the write succeeded; presence will refresh on the next load
        }
        return true
      } catch {
        return false
      }
    },
    [client],
  )

  // Set the weather location live (empty string clears it → weather widget off). Re-reads state so
  // the resolved-weather line refreshes; the device re-geocodes asynchronously so it may stay
  // invalid for a poll or two.
  const updateLocation = useCallback(
    async (next: string): Promise<boolean> => {
      if (!client) return false
      try {
        await client.setLocation(next)
        try {
          const st = await client.getState()
          setLocationState(st.location)
          setWeatherState(st.weather)
        } catch {
          // write succeeded; the value will refresh on the next load
          setLocationState(next)
        }
        return true
      } catch {
        return false
      }
    },
    [client],
  )

  const applyHost = async () => {
    setSaved(false)
    const norm = normalizeBaseUrl(host)
    if (!norm.ok) {
      setHostError('That doesn’t look like a valid IP address or hostname.')
      return
    }
    setHostError(null)
    const ok = await setBaseUrl(host)
    if (ok) {
      setSaved(true)
      loadInfo()
    }
  }

  const reonboard = async () => {
    // Drop the saved device + onboarding flag, then restart the wizard.
    await clearDeviceBaseUrl()
    await resetOnboarding()
    router.replace('/onboarding/turn-on')
  }

  return (
    <Screen>
      <KeyboardAvoidingView style={styles.flex} behavior={Platform.OS === 'ios' ? 'padding' : undefined}>
        <View style={styles.titleRow}>
          <BackButton onPress={() => router.back()} />
          <Text style={styles.title}>Settings</Text>
          <View style={styles.backSpacer} />
        </View>

        <ScrollView contentContainerStyle={styles.body} keyboardShouldPersistTaps="handled">
          {/* Device identity */}
          <Section title="Device">
            <Card style={styles.infoCard}>
              {infoError ? (
                <Pressable onPress={loadInfo} accessibilityRole="button" style={styles.infoRetry}>
                  <Text style={styles.infoRetryText}>Couldn’t reach the board. Tap to retry.</Text>
                </Pressable>
              ) : (
                <>
                  <InfoRow label="Model" value={info?.model || '—'} />
                  <InfoRow label="Firmware" value={info?.fw || '—'} />
                  <InfoRow label="Device ID" value={info?.deviceId || '—'} />
                  <InfoRow label="IP" value={info?.ip || baseUrl?.replace(/^https?:\/\//, '') || '—'} last />
                </>
              )}
            </Card>
          </Section>

          {/* Manual host / IP override */}
          <Section title="Connection">
            <Text style={styles.help}>
              The app finds your board at tickerboard.local. If that doesn’t work on your network,
              enter its IP address or hostname here.
            </Text>
            <View style={styles.hostRow}>
              <TextInput
                value={host}
                onChangeText={(t) => {
                  setHost(t)
                  setHostError(null)
                  setSaved(false)
                }}
                placeholder="192.168.0.42 or tickerboard.local"
                placeholderTextColor={colors.textFaint}
                autoCapitalize="none"
                autoCorrect={false}
                keyboardType="url"
                style={styles.hostInput}
                onSubmitEditing={applyHost}
              />
            </View>
            {hostError ? <Text style={styles.error}>{hostError}</Text> : null}
            {saved ? <Text style={styles.saved}>Saved.</Text> : null}
            <Button label="Use this address" variant="secondary" onPress={applyHost} />

            <Text style={styles.help}>
              Rejoined your home Wi-Fi? Find the board automatically on this network.
            </Text>
            {reconnectMsg ? <Text style={styles.saved}>{reconnectMsg}</Text> : null}
            <Button
              label="Find device"
              variant="secondary"
              loading={reconnecting}
              onPress={reconnect}
            />
          </Section>

          {/* Data-source keys — write-only (the device never returns stored values). */}
          <Section title="Data sources">
            <Text style={styles.help}>
              Keys are stored on the board, never shown back. Enter a new value to replace one; leave
              blank to keep the current setting.
            </Text>
            <KeyRow
              label="Finnhub key"
              isSet={keys?.finnhub}
              placeholder="enter new Finnhub key"
              onSave={(v) => updateKey({ finnhubKey: v })}
            />
            <KeyRow
              label="FMP key"
              isSet={keys?.fmp}
              placeholder="enter new FMP key"
              onSave={(v) => updateKey({ fmpKey: v })}
            />
            <KeyRow
              label="Economic calendar URL"
              isSet={keys?.econUrl}
              placeholder="enter new calendar URL"
              keyboardType="url"
              onSave={(v) => updateKey({ econUrl: v })}
            />
          </Section>

          {/* Weather location — plain text, prefilled with the current value. */}
          <Section title="Weather">
            <Text style={styles.help}>
              Show local weather on the board's home screen. Enter a place like “Seoul” or
              “Paris, FR”. Leave it blank and tap Save to turn the weather widget off.
            </Text>
            {weather?.valid ? (
              <Text style={styles.saved}>
                {weather.city} · {Math.round(weather.tempC)}°C
              </Text>
            ) : location ? (
              <Text style={styles.help}>Resolving weather for “{location}”…</Text>
            ) : null}
            <LocationAutocomplete
              key={location ?? ''}
              initial={location ?? ''}
              onSave={updateLocation}
            />
          </Section>

          {/* Re-run onboarding */}
          <Section title="Setup">
            <Button label="Set up a different board" variant="ghost" onPress={reonboard} />
          </Section>
        </ScrollView>
      </KeyboardAvoidingView>
    </Screen>
  )
}

function InfoRow({ label, value, last = false }: { label: string; value: string; last?: boolean }) {
  return (
    <View style={[styles.infoRow, !last && styles.infoRowBordered]}>
      <Text style={styles.infoLabel}>{label}</Text>
      <Text style={styles.infoValue} numberOfLines={1}>
        {value}
      </Text>
    </View>
  )
}

function Section({ title, children }: { title: string; children: React.ReactNode }) {
  return (
    <View style={styles.section}>
      <Text style={styles.sectionTitle}>{title}</Text>
      {children}
    </View>
  )
}

// One write-only data-source key: shows whether it's currently set, and a field to replace it.
// We never receive the stored value, so the input starts empty and only sends on an explicit Save.
// An empty value is a valid "clear this key" request (the device reverts to its compiled default).
function KeyRow({
  label,
  isSet,
  placeholder,
  keyboardType,
  onSave,
}: {
  label: string
  /** undefined = unknown (device unreachable); true/false = set / not set. */
  isSet?: boolean
  placeholder: string
  keyboardType?: 'url'
  onSave: (value: string) => Promise<boolean>
}) {
  const [draft, setDraft] = useState('')
  const [saving, setSaving] = useState(false)
  const [done, setDone] = useState(false)
  const [failed, setFailed] = useState(false)
  // Keys + the proxy URL (which can embed a token) are masked by default; reveal to check a paste.
  const [reveal, setReveal] = useState(false)

  const status = isSet === undefined ? 'unknown' : isSet ? 'set' : 'not set'

  const save = async () => {
    setSaving(true)
    setDone(false)
    setFailed(false)
    const ok = await onSave(draft.trim())
    setSaving(false)
    if (ok) {
      setDone(true)
      setDraft('')
    } else {
      setFailed(true)
    }
  }

  return (
    <View style={styles.keyRow}>
      <View style={styles.keyHead}>
        <Text style={styles.keyLabel}>{label}</Text>
        <Text style={[styles.keyStatus, isSet ? styles.keyStatusSet : undefined]}>{status}</Text>
      </View>
      <View style={styles.hostRow}>
        <TextInput
          value={draft}
          onChangeText={(t) => {
            setDraft(t)
            setDone(false)
            setFailed(false)
          }}
          placeholder={placeholder}
          placeholderTextColor={colors.textFaint}
          autoCapitalize="none"
          autoCorrect={false}
          keyboardType={reveal ? keyboardType : undefined}
          secureTextEntry={!reveal}
          autoComplete="off"
          textContentType="none"
          style={styles.hostInput}
          onSubmitEditing={save}
        />
        <Pressable onPress={() => setReveal((v) => !v)} hitSlop={8} style={styles.keyReveal}>
          <Text style={styles.keyRevealText}>{reveal ? 'Hide' : 'Show'}</Text>
        </Pressable>
      </View>
      {failed ? <Text style={styles.error}>Couldn’t update. Please try again.</Text> : null}
      {done ? <Text style={styles.saved}>Updated.</Text> : null}
      <Button label="Update" variant="secondary" loading={saving} onPress={save} />
    </View>
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
    paddingTop: 12,
    gap: space.xl,
  },
  section: {
    gap: 10,
  },
  sectionTitle: {
    fontSize: 13,
    fontWeight: '600',
    color: colors.textDim,
    letterSpacing: 0.8,
    textTransform: 'uppercase',
  },
  infoCard: {
    padding: 0,
  },
  infoRow: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    paddingVertical: 14,
    paddingHorizontal: 16,
  },
  infoRowBordered: {
    borderBottomWidth: StyleSheet.hairlineWidth,
    borderBottomColor: colors.border,
  },
  infoLabel: {
    fontSize: 14,
    color: colors.textDim,
  },
  infoValue: {
    fontSize: 14,
    color: colors.text,
    flexShrink: 1,
    textAlign: 'right',
    marginLeft: 16,
  },
  infoRetry: {
    padding: 16,
  },
  infoRetryText: {
    fontSize: 14,
    color: colors.accent,
    textAlign: 'center',
  },
  help: {
    fontSize: 13,
    color: colors.textFaint,
    lineHeight: 18,
  },
  hostRow: {
    flexDirection: 'row',
  },
  hostInput: {
    flex: 1,
    height: 48,
    borderRadius: radius.md,
    borderWidth: StyleSheet.hairlineWidth,
    borderColor: colors.border,
    backgroundColor: colors.surface,
    paddingHorizontal: 14,
    color: colors.text,
    fontSize: 16,
  },
  keyReveal: {
    justifyContent: 'center',
    paddingLeft: 12,
  },
  keyRevealText: {
    fontSize: 13,
    fontWeight: '600',
    color: colors.accent,
  },
  error: {
    fontSize: 13,
    color: colors.down,
  },
  saved: {
    fontSize: 13,
    color: colors.up,
  },
  keyRow: {
    gap: 8,
  },
  keyHead: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
  },
  keyLabel: {
    fontSize: 14,
    fontWeight: '600',
    color: colors.text,
  },
  keyStatus: {
    fontSize: 12,
    color: colors.textFaint,
  },
  keyStatusSet: {
    color: colors.up,
  },
})
