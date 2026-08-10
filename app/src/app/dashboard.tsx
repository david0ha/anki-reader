import { useCallback, useRef, useState } from 'react'
import {
  KeyboardAvoidingView,
  Platform,
  Pressable,
  RefreshControl,
  ScrollView,
  StyleSheet,
  Text,
  TextInput,
  View,
} from 'react-native'
import { Ionicons } from '@expo/vector-icons'
import { useFocusEffect, useRouter } from 'expo-router'
import { Screen } from '../components/Screen'
import { Card } from '../components/Card'
import { Chip } from '../components/Chip'
import { Button } from '../components/Button'
import { InfoRow } from '../components/InfoRow'
import { SegmentedControl } from '../components/SegmentedControl'
import { StatTile } from '../components/StatTile'
import { ScreenMessage } from '../components/ScreenMessage'
import { useDevice } from '../lib/device'
import { Esp32Error, type DeviceState } from '../lib/esp32'
import { captureErrorMessage, captureMemo, captureUrlFor } from '../lib/capture'
import { DEFAULT_HOST, discoverDevice } from '../lib/discovery'
import { getDeviceBaseUrl } from '../lib/store'
import {
  PAGE_LABELS,
  fetchResultLabel,
  fetchResultMessage,
  fetchResultTone,
  formatAge,
  formatCount,
  formatDelta,
  formatDensity,
  formatInterval,
  formatMs,
  formatRatio,
} from '../lib/format'
import { colors, layout, radius, space } from '../theme'

// The board polls its source every few minutes and only redraws when something changed, so there
// is nothing to gain from polling it fast. This is "keep the phone screen roughly current", not a
// live feed.
const POLL_MS = 5000

export default function Dashboard() {
  const router = useRouter()
  const { client, baseUrl, setBaseUrl } = useDevice()

  const [state, setState] = useState<DeviceState | null>(null)
  const [error, setError] = useState<string | null>(null)
  const [refreshing, setRefreshing] = useState(false)
  // Disable controls briefly while a write command is in flight so taps can't race.
  const [busy, setBusy] = useState(false)
  // A page the user asked for that the board has not confirmed yet. A page change is a full
  // refresh of a 5.83" panel — seconds, not milliseconds — so without this the segmented control
  // snaps back to the old page and looks like the tap was lost.
  const [pendingPage, setPendingPage] = useState<number | null>(null)
  const focused = useRef(true)

  const load = useCallback(
    async (opts: { silent?: boolean } = {}) => {
      if (!client) return
      if (!opts.silent) setError(null)
      try {
        const s = await client.getState()
        setState(s)
        setPendingPage((p) => (p === null || p === s.page ? null : p))
        setError(null)
      } catch (e) {
        // Keep the last good snapshot on a transient poll failure; only surface an error when we
        // have nothing to show yet.
        if (!opts.silent) {
          setError(e instanceof Esp32Error ? humanError(e) : 'Couldn’t reach the board.')
        }
      }
    },
    [client],
  )

  // Poll while the screen is focused. useFocusEffect pauses polling when the user navigates away
  // and resumes on return, so we never poll a backgrounded screen.
  useFocusEffect(
    useCallback(() => {
      focused.current = true
      load()
      const id = setInterval(() => {
        if (focused.current) load({ silent: true })
      }, POLL_MS)
      return () => {
        focused.current = false
        clearInterval(id)
      }
    }, [load]),
  )

  const onPullRefresh = useCallback(async () => {
    setRefreshing(true)
    await load()
    setRefreshing(false)
  }, [load])

  // "Couldn't reach the board" retry: the saved address may be stale after the user rejoined their
  // home Wi-Fi or the board took a new DHCP lease. Re-probe the LAN (saved address + the mDNS
  // name), persist whichever answers, then reload.
  const retry = useCallback(async () => {
    setError(null)
    const saved = await getDeviceBaseUrl()
    const found = await discoverDevice([saved, baseUrl, `http://${DEFAULT_HOST}`])
    if (found && found !== baseUrl) {
      await setBaseUrl(found)
      // The client is recreated from the new baseUrl on the next render; the focus-effect poll and
      // this explicit load will then hit the rediscovered board.
    }
    await load()
  }, [baseUrl, setBaseUrl, load])

  // Wrap a control command: re-poll afterwards so the UI reflects the board quickly.
  const command = useCallback(
    async (fn: () => Promise<void>) => {
      if (!client || busy) return
      setBusy(true)
      try {
        await fn()
        await load({ silent: true })
      } catch (e) {
        setError(e instanceof Esp32Error ? humanError(e) : 'That command failed. Please try again.')
      } finally {
        setBusy(false)
      }
    },
    [client, busy, load],
  )

  if (!client) {
    return (
      <Screen>
        <ScreenMessage loading message="Connecting…" />
      </Screen>
    )
  }

  if (!state) {
    return (
      <Screen>
        <Header baseUrl={baseUrl} onSettings={() => router.push('/settings')} />
        <ScreenMessage loading={!error} error={error} message="Loading…" onRetry={retry} />
      </Screen>
    )
  }

  const { vault, source, battery, panel } = state
  const shownPage = pendingPage ?? state.page

  return (
    <Screen edges={['top']}>
      <Header baseUrl={baseUrl} onSettings={() => router.push('/settings')} />

      <ScrollView
        contentContainerStyle={styles.scroll}
        refreshControl={
          <RefreshControl refreshing={refreshing} onRefresh={onPullRefresh} tintColor={colors.accent} />
        }
      >
        {/* Status chips: how the last poll went, whether what's on the glass is demo/stale, and
            the battery when there is one. */}
        <View style={styles.chipRow}>
          <Chip
            label={fetchResultLabel(source.lastResult)}
            icon="cloud-download"
            tone={fetchResultTone(source.lastResult)}
          />
          {vault.demo ? <Chip label="demo data" icon="flask" tone="warn" /> : null}
          {source.stale ? <Chip label="stale" icon="time" tone="warn" /> : null}
          {battery.present ? (
            <Chip
              label={`${battery.percent}%`}
              icon="battery-half"
              tone={battery.percent < 20 ? 'down' : 'neutral'}
            />
          ) : null}
        </View>

        {/* The vault, as the board understands it. */}
        <Card style={styles.hero}>
          <Text style={styles.heroName} numberOfLines={1}>
            {vault.name || 'No vault'}
          </Text>
          <Text style={styles.heroMeta}>
            {vault.valid
              ? `snapshot ${vault.generatedAt || '—'} · ${formatAge(source.ageSeconds)}`
              : 'no snapshot yet'}
          </Text>
        </Card>

        <View style={styles.tiles}>
          <StatTile
            label="Notes"
            value={formatCount(vault.notes)}
            footnote={`${formatDelta(vault.addedToday)} today · ${formatDelta(vault.added7d)} this week`}
          />
          <StatTile
            label="Links"
            value={formatCount(vault.links)}
            footnote={`${formatDensity(vault.links, vault.notes)} per note`}
          />
          <StatTile
            label="Orphans"
            value={formatCount(vault.orphans)}
            footnote={`${formatRatio(vault.orphans, vault.notes)} of the vault`}
            tone={vault.orphans > 0 ? 'warn' : 'neutral'}
          />
          <StatTile label="Tags" value={formatCount(vault.tags)} />
        </View>

        {/* Capture. Only offered when the board has a snapshot URL, because that URL is the
            address this writes to — a board on demo data has nowhere to put a memo. */}
        {captureUrlFor(source.url) ? (
          <Section title="Quick memo">
            <MemoBox
              sourceUrl={source.url}
              // The board polls every few minutes; asking it to poll now is what makes a memo
              // typed on the sofa appear on the panel while you are still looking at it.
              onSaved={() => command(() => client.refresh())}
            />
          </Section>
        ) : null}

        <Section title="Agents & queue">
          <Card style={styles.rows}>
            <InfoRow
              label="Agents running"
              value={`${vault.agentsRunning} of ${vault.agents}`}
              tone={vault.agentsRunning > 0 ? 'up' : 'dim'}
            />
            <InfoRow label="Recent notes" value={formatCount(vault.recent)} />
            <InfoRow label="Inbox" value={formatCount(vault.inbox)} last />
          </Card>
        </Section>

        {/* Page control. The board's own title for the page it is showing sits underneath, in its
            UI language — that is the ground truth for what is on the glass. */}
        <Section title="On the panel">
          <SegmentedControl
            segments={[...PAGE_LABELS]}
            selectedIndex={shownPage}
            disabled={busy}
            onChange={(page) => {
              setPendingPage(page)
              command(() => client.setPage(page))
            }}
          />
          <Text style={styles.pageNote}>
            {pendingPage !== null && pendingPage !== state.page
              ? 'Switching… a page change is a full refresh, which takes a few seconds.'
              : `Showing “${state.pageTitle || PAGE_LABELS[state.page] || '—'}”.`}
          </Text>
        </Section>

        {/* Where the data comes from, and how the last poll went. */}
        <Section title="Source">
          <Card style={styles.rows}>
            <InfoRow label="URL" value={source.url || 'not set (demo)'} tone={source.url ? 'neutral' : 'dim'} />
            <InfoRow
              label="Last poll"
              value={fetchResultLabel(source.lastResult)}
              tone={fetchResultTone(source.lastResult) === 'down' ? 'down' : 'neutral'}
            />
            <InfoRow label="Last success" value={formatAge(source.ageSeconds)} />
            <InfoRow label="Polls" value={formatInterval(source.pollSeconds)} last />
          </Card>
          {source.lastResult !== 'ok' ? (
            <Text style={styles.sourceNote}>{fetchResultMessage(source.lastResult)}</Text>
          ) : null}
        </Section>

        {/* Measured panel timings — the numbers the refresh policy is meant to be chosen from. */}
        <Section title="Panel">
          <Card style={styles.rows}>
            <InfoRow label="Full refresh" value={formatMs(panel.fullRefreshMs)} />
            <InfoRow label="Partial refresh" value={formatMs(panel.partialRefreshMs)} />
            <InfoRow label="Partials since full" value={String(panel.partialChain)} last />
          </Card>
        </Section>

        <View style={styles.actions}>
          <Button
            label="Poll now"
            variant="secondary"
            disabled={busy}
            onPress={() => command(() => client.refresh())}
            style={styles.actionBtn}
          />
          <Button
            label="Self-test"
            variant="secondary"
            disabled={busy}
            onPress={() => command(() => client.displayTest())}
            style={styles.actionBtn}
          />
        </View>
        <Text style={styles.actionsNote}>
          Polling only redraws the panel if the snapshot changed. The self-test sweeps the panel for
          about a minute.
        </Text>

        {error ? <Text style={styles.errorLine}>{error}</Text> : null}
      </ScrollView>
    </Screen>
  )
}

function humanError(e: Esp32Error): string {
  switch (e.code) {
    case 'network_error':
      return 'Couldn’t reach the board. Check it’s powered on and on the same Wi-Fi.'
    case 'page_range':
      return 'That page doesn’t exist on the board.'
    case 'vault_url_invalid':
      return 'The board wouldn’t accept that address.'
    case 'busy':
      return 'The board is busy redrawing. Try again in a moment.'
    default:
      return 'That command failed. Please try again.'
  }
}

function Header({ baseUrl, onSettings }: { baseUrl: string | null; onSettings: () => void }) {
  return (
    <View style={styles.header}>
      <View style={styles.headerText}>
        <Text style={styles.headerTitle}>Obsidian Board</Text>
        <Text style={styles.headerSub} numberOfLines={1}>
          {baseUrl ?? ''}
        </Text>
      </View>
      <Pressable accessibilityLabel="Settings" onPress={onSettings} hitSlop={8} style={styles.settingsBtn}>
        <Ionicons name="settings-outline" size={22} color={colors.text} />
      </Pressable>
    </View>
  )
}

/**
 * Type a memo, write it into the vault.
 *
 * The write goes to the machine serving the snapshot, not to the board — see src/lib/capture.ts.
 * Most producers will not accept it, so "this server doesn't do capture" is an ordinary answer
 * and gets its own sentence rather than a generic failure.
 */
function MemoBox({ sourceUrl, onSaved }: { sourceUrl: string; onSaved: () => void }) {
  const [text, setText] = useState('')
  const [saving, setSaving] = useState(false)
  const [saved, setSaved] = useState<string | null>(null)
  const [error, setError] = useState<string | null>(null)

  const save = async () => {
    if (saving || !text.trim()) return
    setSaving(true)
    setSaved(null)
    setError(null)
    try {
      const { path } = await captureMemo(sourceUrl, text)
      setText('')
      setSaved(path || 'Saved to your inbox.')
      onSaved()
    } catch (e) {
      setError(captureErrorMessage(e))
    } finally {
      setSaving(false)
    }
  }

  return (
    <KeyboardAvoidingView behavior={Platform.OS === 'ios' ? 'padding' : undefined}>
      <TextInput
        value={text}
        onChangeText={(t) => {
          setText(t)
          setSaved(null)
          setError(null)
        }}
        placeholder="Something to deal with later…"
        placeholderTextColor={colors.textFaint}
        multiline
        style={styles.memoInput}
        editable={!saving}
      />
      {error ? <Text style={styles.memoError}>{error}</Text> : null}
      {saved ? <Text style={styles.memoSaved}>Saved · {saved}</Text> : null}
      <Button
        label="Add to inbox"
        variant="secondary"
        disabled={!text.trim()}
        loading={saving}
        onPress={save}
      />
    </KeyboardAvoidingView>
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

const styles = StyleSheet.create({
  header: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    paddingHorizontal: layout.gutter,
    paddingVertical: 12,
  },
  headerText: {
    flexShrink: 1,
  },
  headerTitle: {
    fontSize: 20,
    fontWeight: '700',
    color: colors.text,
  },
  headerSub: {
    fontSize: 12,
    color: colors.textFaint,
    marginTop: 2,
  },
  settingsBtn: {
    width: 40,
    height: 40,
    alignItems: 'center',
    justifyContent: 'center',
  },
  scroll: {
    paddingHorizontal: layout.gutter,
    paddingBottom: 32,
    gap: space.lg,
  },
  chipRow: {
    flexDirection: 'row',
    flexWrap: 'wrap',
    gap: 8,
  },
  hero: {
    alignItems: 'center',
    gap: 4,
    paddingVertical: 20,
  },
  heroName: {
    fontSize: 28,
    fontWeight: '700',
    color: colors.text,
  },
  heroMeta: {
    fontSize: 12,
    color: colors.textFaint,
  },
  tiles: {
    flexDirection: 'row',
    flexWrap: 'wrap',
    gap: 12,
    justifyContent: 'space-between',
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
  rows: {
    padding: 0,
  },
  pageNote: {
    fontSize: 12,
    color: colors.textFaint,
    lineHeight: 16,
  },
  memoInput: {
    minHeight: 84,
    borderRadius: radius.md,
    borderWidth: StyleSheet.hairlineWidth,
    borderColor: colors.border,
    backgroundColor: colors.surface,
    paddingHorizontal: 14,
    paddingTop: 12,
    paddingBottom: 12,
    color: colors.text,
    fontSize: 16,
    textAlignVertical: 'top',
    marginBottom: 10,
  },
  memoError: {
    fontSize: 12,
    color: colors.down,
    lineHeight: 16,
    marginBottom: 8,
  },
  memoSaved: {
    fontSize: 12,
    color: colors.up,
    lineHeight: 16,
    marginBottom: 8,
  },
  sourceNote: {
    fontSize: 12,
    color: colors.textDim,
    lineHeight: 16,
  },
  actions: {
    flexDirection: 'row',
    gap: 12,
  },
  actionBtn: {
    flex: 1,
  },
  actionsNote: {
    fontSize: 12,
    color: colors.textFaint,
    lineHeight: 16,
    marginTop: -8,
  },
  errorLine: {
    fontSize: 13,
    color: colors.down,
    textAlign: 'center',
  },
})
