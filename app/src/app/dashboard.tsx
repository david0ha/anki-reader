import { useCallback, useRef, useState } from 'react'
import { Pressable, RefreshControl, ScrollView, StyleSheet, Text, View } from 'react-native'
import { Ionicons } from '@expo/vector-icons'
import { useFocusEffect, useRouter } from 'expo-router'
import { Screen } from '../components/Screen'
import { Card } from '../components/Card'
import { Chip } from '../components/Chip'
import { Button } from '../components/Button'
import { InfoRow } from '../components/InfoRow'
import { StatTile } from '../components/StatTile'
import { ScreenMessage } from '../components/ScreenMessage'
import { useDevice } from '../lib/device'
import { Esp32Error, SCREEN_COUNT, type DeviceState } from '../lib/esp32'
import { DEFAULT_HOST, discoverDevice } from '../lib/discovery'
import { getDeviceBaseUrl } from '../lib/store'
import {
  SCREEN_LABELS,
  fetchResultLabel,
  fetchResultMessage,
  fetchResultTone,
  formatAge,
  formatCount,
  formatDifficulty,
  formatDue,
  formatInterval,
  formatMs,
  formatStability,
  formatStreak,
  formatTrack,
  fsrsStateLabel,
  fsrsStateTone,
  gradeLabel,
  screenLabel,
  trackFraction,
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
  const focused = useRef(true)

  const load = useCallback(
    async (opts: { silent?: boolean } = {}) => {
      if (!client) return
      if (!opts.silent) setError(null)
      try {
        const s = await client.getState()
        setState(s)
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
        <Header model={null} baseUrl={baseUrl} onSettings={() => router.push('/settings')} />
        <ScreenMessage loading={!error} error={error} message="Loading…" onRetry={retry} />
      </Screen>
    )
  }

  const { session, card, source, battery, panel } = state
  return (
    <Screen edges={['top']}>
      <Header model={state.model} baseUrl={baseUrl} onSettings={() => router.push('/settings')} />

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
          {card.demo ? <Chip label="demo card" icon="flask" tone="warn" /> : null}
          {source.stale ? <Chip label="stale" icon="time" tone="warn" /> : null}
          {battery.present ? (
            <Chip
              label={`${battery.percent}%`}
              icon="battery-half"
              tone={battery.percent < 20 ? 'down' : 'neutral'}
            />
          ) : null}
        </View>

        {/* The card on the glass. The reading and the meaning are shown here whatever screen the
            board is on: this is the phone, and hiding the answer from the person holding it would
            be a quiz nobody asked for. */}
        <Card style={styles.hero}>
          {card.valid ? (
            <>
              <Text style={styles.heroFront} numberOfLines={1}>
                {card.front}
              </Text>
              {card.reading ? <Text style={styles.heroReading}>{card.reading}</Text> : null}
              {card.meaning ? <Text style={styles.heroMeaning}>{card.meaning}</Text> : null}
            </>
          ) : (
            <>
              <Text style={styles.heroFront}>{session.complete ? '오늘 학습 완료' : '—'}</Text>
              <Text style={styles.heroMeaning}>
                {session.complete
                  ? 'Today’s queue is empty. The board has stopped asking.'
                  : 'No card yet.'}
              </Text>
            </>
          )}
          <Text style={styles.heroDeck} numberOfLines={1}>
            {session.deck || 'no deck'} · {formatAge(source.ageSeconds)}
          </Text>
        </Card>

        {/* Today's queue. */}
        <View style={styles.tiles}>
          <StatTile label="Reviewed today" value={formatCount(session.reviewedToday)} />
          <StatTile label="Streak" value={formatStreak(session.streak)} />
          <StatTile label="New left" value={formatCount(session.leftNew)} />
          <StatTile
            label="Reviews left"
            value={formatCount(session.leftReview)}
            tone={session.leftReview > 0 ? 'warn' : 'neutral'}
          />
        </View>

        <Section title="Today’s queue">
          <Card style={styles.progressCard}>
            <View style={styles.progressHead}>
              <Text style={styles.progressLabel}>Position</Text>
              <Text style={styles.progressValue}>
                {formatTrack(session.track, session.trackTotal)}
              </Text>
            </View>
            <View style={styles.progressTrack}>
              <View
                style={[
                  styles.progressFill,
                  { width: `${trackFraction(session.track, session.trackTotal) * 100}%` },
                ]}
              />
            </View>
          </Card>
        </Section>

        {/* The five screens the three side buttons cycle. The phone drives the same nav state a
            press does, so a tap here and a press there cannot disagree about which screen is up. */}
        <Section title="On the panel">
          <View style={styles.chipRow}>
            {SCREEN_LABELS.map((label, i) => (
              <Chip
                key={label}
                label={label}
                active={state.screen === i}
                disabled={busy}
                onPress={() => command(() => client.setScreen(i))}
              />
            ))}
          </View>
          <Card style={styles.rows}>
            <InfoRow label="Screen" value={state.screenTitle || screenLabel(state.screen)} />
            <InfoRow
              label="Answer"
              value={state.revealed ? 'shown' : 'hidden'}
              tone={state.revealed ? 'up' : 'dim'}
            />
            <InfoRow label="Grade cursor" value={gradeLabel(state.grade)} last />
          </Card>
          <Text style={styles.panelNote}>
            KEY0 and KEY1 do what the footer says on the screen that is up; KEY2 re-polls, and BOOT
            cycles the three sheets. A full refresh of this panel takes seconds, so the board only
            redraws when the card actually changed.
          </Text>
        </Section>

        {/* What the scheduler thinks of the card on the glass. */}
        <Section title="FSRS">
          <Card style={styles.rows}>
            <InfoRow label="State" value={fsrsStateLabel(card.fsrsState)} tone={fsrsStateTone(card.fsrsState)} />
            <InfoRow label="Next due" value={formatDue(card.due)} />
            <InfoRow label="Reviews" value={formatCount(card.reps)} />
            <InfoRow label="Lapses" value={formatCount(card.lapses)} tone={card.lapses > 0 ? 'warn' : 'neutral'} />
            <InfoRow label="Stability" value={formatStability(card.stabilityDays)} />
            <InfoRow label="Difficulty" value={formatDifficulty(card.difficultyPct)} last />
          </Card>
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
          Polling only redraws the panel if the card changed. The self-test sweeps the panel for
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
    case 'screen_range':
      // The client only ever offers SCREEN_COUNT screens, so this means the board has fewer of
      // them than this app was built against.
      return `That screen doesn’t exist on the board (this app knows ${SCREEN_COUNT}).`
    case 'study_url_invalid':
      return 'The board wouldn’t accept that address.'
    case 'busy':
      return 'The board is busy redrawing. Try again in a moment.'
    default:
      return 'That command failed. Please try again.'
  }
}

function Header({
  model,
  baseUrl,
  onSettings,
}: {
  model: string | null
  baseUrl: string | null
  onSettings: () => void
}) {
  return (
    <View style={styles.header}>
      <View style={styles.headerText}>
        {/* The board names itself (DEVICE_MODEL). Reading it back beats hardcoding a second copy
            of the name here that a firmware rename would silently make wrong. */}
        <Text style={styles.headerTitle}>{model || 'AnkiReader'}</Text>
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
    gap: 6,
    paddingVertical: 20,
  },
  heroFront: {
    fontSize: 40,
    fontWeight: '700',
    color: colors.text,
  },
  heroReading: {
    fontSize: 16,
    color: colors.textDim,
  },
  heroMeaning: {
    fontSize: 15,
    color: colors.text,
    textAlign: 'center',
  },
  heroDeck: {
    fontSize: 12,
    color: colors.textFaint,
    marginTop: 4,
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
  progressCard: {
    gap: 10,
  },
  progressHead: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    alignItems: 'center',
  },
  progressLabel: {
    fontSize: 14,
    color: colors.textDim,
  },
  progressValue: {
    fontSize: 14,
    color: colors.text,
  },
  progressTrack: {
    height: 6,
    borderRadius: radius.pill,
    backgroundColor: colors.surfaceAlt,
    overflow: 'hidden',
  },
  progressFill: {
    height: '100%',
    borderRadius: radius.pill,
    backgroundColor: colors.accent,
  },
  panelNote: {
    fontSize: 12,
    color: colors.textFaint,
    lineHeight: 16,
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
