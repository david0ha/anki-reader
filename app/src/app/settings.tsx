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
import { InfoRow } from '../components/InfoRow'
import { useDevice } from '../lib/device'
import { Esp32Error, type DeviceInfo, type DeviceState } from '../lib/esp32'
import { DEFAULT_HOST, discoverDevice, normalizeBaseUrl } from '../lib/discovery'
import { clearDeviceBaseUrl, getDeviceBaseUrl, resetOnboarding } from '../lib/store'
import { validateStudyUrl, studyUrlErrorMessage } from '../lib/studyurl'
import { fetchResultLabel, fetchResultMessage, formatAge, formatInterval } from '../lib/format'
import { colors, layout, radius, space } from '../theme'

export default function Settings() {
  const router = useRouter()
  const { client, baseUrl, setBaseUrl } = useDevice()

  const [info, setInfo] = useState<DeviceInfo | null>(null)
  const [infoError, setInfoError] = useState(false)
  const [host, setHost] = useState('')
  const [hostError, setHostError] = useState<string | null>(null)
  const [saved, setSaved] = useState(false)

  // The board's own view of its source — the URL it is actually using and how the last poll went.
  // Unlike a write-only secret, the URL is plain text the board echoes back, so the editor below
  // can be prefilled with it and the user can see what they are changing.
  const [source, setSource] = useState<DeviceState['source'] | null>(null)

  // Reconnect ("find board") UI state.
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
    // Best-effort: also pull the configured source so the editor below reflects the board.
    try {
      setSource((await client.getState()).source)
    } catch {
      // leave the last-known value; the section just shows "unknown" until a state read succeeds
    }
  }, [client])

  useEffect(() => {
    loadInfo()
  }, [loadInfo])

  // Re-probe the LAN for the board (its reported IP, the saved address, the mDNS name) and persist
  // whichever answers. Used after the user rejoins their home Wi-Fi or the board's lease changes.
  const reconnect = useCallback(async () => {
    setReconnecting(true)
    setReconnectMsg(null)
    try {
      const savedUrl = await getDeviceBaseUrl()
      const found = await discoverDevice([info?.ip, savedUrl, `http://${DEFAULT_HOST}`])
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
    // Drop the saved board + onboarding flag, then restart the wizard.
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
          {/* Board identity */}
          <Section title="Board">
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

          {/* The study card URL — the one setting that decides what the board shows. */}
          <Section title="Study source">
            <Text style={styles.help}>
              The address the board fetches its card from — your kanjis.ai proxy on this network.
              Clear it and save to put the board back on its built-in demo card.
            </Text>
            {source ? (
              <Card style={styles.infoCard}>
                <InfoRow label="Last poll" value={fetchResultLabel(source.lastResult)} />
                <InfoRow label="Last success" value={formatAge(source.ageSeconds)} />
                <InfoRow label="Polls" value={formatInterval(source.pollSeconds)} last />
              </Card>
            ) : null}
            {source && source.lastResult !== 'ok' ? (
              <Text style={styles.help}>{fetchResultMessage(source.lastResult)}</Text>
            ) : null}
            <StudyUrlEditor
              // Remount when the board reports a different URL, so the field picks up the new
              // value instead of holding a draft the board has already moved past.
              key={source?.url ?? ''}
              initial={source?.url ?? ''}
              onSave={async (next) => {
                if (!client) return 'Not connected to the board.'
                try {
                  await client.setStudyUrl(next)
                } catch (e) {
                  if (e instanceof Esp32Error && e.code === 'study_url_invalid') {
                    return 'The board wouldn’t accept that address.'
                  }
                  return 'Couldn’t update. Please try again.'
                }
                // Re-read so the rows above reflect the change. The board polls the new URL
                // immediately, but the result lands a moment later — the next poll of this screen
                // (or a pull-to-refresh on the dashboard) will show it.
                try {
                  setSource((await client.getState()).source)
                } catch {
                  // the write succeeded; the value refreshes on the next load
                }
                return null
              }}
            />
          </Section>

          {/* Manual host / IP override */}
          <Section title="Connection">
            <Text style={styles.help}>
              The app finds your board at {DEFAULT_HOST}. If that doesn’t work on your network,
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
                placeholder={`192.168.0.42 or ${DEFAULT_HOST}`}
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
            <Button label="Find board" variant="secondary" loading={reconnecting} onPress={reconnect} />
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

function Section({ title, children }: { title: string; children: React.ReactNode }) {
  return (
    <View style={styles.section}>
      <Text style={styles.sectionTitle}>{title}</Text>
      {children}
    </View>
  )
}

/**
 * The card-URL field. Prefilled with what the board reports, validated locally against the
 * firmware's own rule before any request goes out, and explicit about the empty case: clearing the
 * field and saving is a real, supported action (back to the demo card), not a mistake.
 *
 * `onSave` returns null on success or a sentence to show on failure.
 */
function StudyUrlEditor({
  initial,
  onSave,
}: {
  initial: string
  onSave: (value: string) => Promise<string | null>
}) {
  const [draft, setDraft] = useState(initial)
  const [saving, setSaving] = useState(false)
  const [done, setDone] = useState(false)
  const [failure, setFailure] = useState<string | null>(null)

  const result = validateStudyUrl(draft)
  const localError = !result.ok ? studyUrlErrorMessage(result) : null
  const dirty = draft.trim() !== initial.trim()

  const save = async () => {
    if (!result.ok) return
    setSaving(true)
    setDone(false)
    setFailure(null)
    const err = await onSave(result.value ?? '')
    setSaving(false)
    if (err) setFailure(err)
    else setDone(true)
  }

  return (
    <View style={styles.field}>
      <View style={styles.hostRow}>
        <TextInput
          value={draft}
          onChangeText={(t) => {
            setDraft(t)
            setDone(false)
            setFailure(null)
          }}
          placeholder="http://mymac.local:8123/kanji.json"
          placeholderTextColor={colors.textFaint}
          autoCapitalize="none"
          autoCorrect={false}
          keyboardType="url"
          style={styles.hostInput}
          onSubmitEditing={save}
        />
      </View>
      {localError ? <Text style={styles.error}>{localError}</Text> : null}
      {failure ? <Text style={styles.error}>{failure}</Text> : null}
      {done ? (
        <Text style={styles.saved}>
          {draft.trim() ? 'Saved. The board is fetching it now.' : 'Cleared — the board is back on its demo card.'}
        </Text>
      ) : null}
      <Button
        label={dirty && !draft.trim() ? 'Clear and use the demo card' : 'Save address'}
        variant="secondary"
        disabled={!result.ok || !dirty}
        loading={saving}
        onPress={save}
      />
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
    paddingBottom: 32,
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
  field: {
    gap: 8,
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
  error: {
    fontSize: 13,
    color: colors.down,
  },
  saved: {
    fontSize: 13,
    color: colors.up,
  },
})
