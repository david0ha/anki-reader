import { useCallback, useRef, useState } from 'react'
import { Platform, Pressable, RefreshControl, ScrollView, StyleSheet, Text, View } from 'react-native'
import { Ionicons } from '@expo/vector-icons'
import { useFocusEffect, useRouter } from 'expo-router'
import { Screen } from '../components/Screen'
import { Card } from '../components/Card'
import { Chip } from '../components/Chip'
import { Button } from '../components/Button'
import { SegmentedControl } from '../components/SegmentedControl'
import { TickerRow } from '../components/TickerRow'
import { ScreenMessage } from '../components/ScreenMessage'
import { useDevice } from '../lib/device'
import { Esp32Error, type StockState, type StockTicker } from '../lib/esp32'
import { DEFAULT_HOST, discoverDevice } from '../lib/discovery'
import { getDeviceBaseUrl } from '../lib/store'
import { PAGE_LABELS, direction, formatChange, formatPercent, formatPrice } from '../lib/format'
import { colors, fonts, layout, radius, space } from '../theme'

const mono = Platform.OS === 'ios' ? fonts.monoIos : fonts.mono
const POLL_MS = 4000

export default function Dashboard() {
  const router = useRouter()
  const { client, baseUrl, setBaseUrl } = useDevice()

  const [state, setState] = useState<StockState | null>(null)
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
  // (settings/watchlist) and resumes on return, so we never poll a backgrounded screen.
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
  // home Wi-Fi. Re-probe the LAN (saved IP + tickerboard.local), persist whichever answers, then
  // reload. If discovery finds nothing we still fall back to a plain reload against the current URL.
  const retry = useCallback(async () => {
    setError(null)
    const saved = await getDeviceBaseUrl()
    const found = await discoverDevice([saved, baseUrl, `http://${DEFAULT_HOST}`])
    if (found && found !== baseUrl) {
      await setBaseUrl(found)
      // The client is recreated from the new baseUrl on the next render; the focus-effect poll and
      // this explicit load will then hit the rediscovered device.
    }
    await load()
  }, [baseUrl, setBaseUrl, load])

  // Wrap a control command: optimistic re-poll afterwards so the UI reflects the device quickly.
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
        <ScreenMessage
          loading={!error}
          error={error}
          message="Loading…"
          onRetry={retry}
        />
      </Screen>
    )
  }

  const current: StockTicker | undefined = state.watchlist[state.index]
  const dir = current ? direction(current.change) : 'flat'
  const dirColor = dir === 'up' ? colors.up : dir === 'down' ? colors.down : colors.textDim

  return (
    <Screen edges={['top']}>
      <Header baseUrl={baseUrl} onSettings={() => router.push('/settings')} />

      <ScrollView
        contentContainerStyle={styles.scroll}
        refreshControl={
          <RefreshControl refreshing={refreshing} onRefresh={onPullRefresh} tintColor={colors.accent} />
        }
      >
        {/* Connection + sensor chips */}
        <View style={styles.chipRow}>
          <Chip label="Connected" icon="ellipse" tone="up" />
          {state.env.valid ? (
            <Chip label={`${state.env.tempC.toFixed(1)}°C`} icon="thermometer" />
          ) : null}
          {state.env.valid ? <Chip label={`${state.env.humidity.toFixed(0)}%`} icon="water" /> : null}
          {/* Weather: show the resolved chip once valid; a subtle loading hint while the device
              geocodes a freshly-set location; nothing at all when no location is configured. */}
          {state.weather.valid ? (
            <Chip label={`${Math.round(state.weather.tempC)}° ${state.weather.city}`} icon="cloud" />
          ) : state.location ? (
            <Chip label={`weather: ${state.location}…`} icon="cloud" tone="neutral" />
          ) : null}
          {state.env.batteryValid ? (
            <Chip
              label={`${state.env.batteryPct}%`}
              icon="battery-half"
              tone={state.env.batteryPct < 20 ? 'down' : 'neutral'}
            />
          ) : null}
        </View>

        {/* Current ticker hero */}
        <Card style={styles.hero}>
          {current ? (
            <>
              <Text style={[styles.heroSymbol, { fontFamily: mono }]}>{current.symbol}</Text>
              {current.valid ? (
                <>
                  <Text style={[styles.heroPrice, { fontFamily: mono }]}>{formatPrice(current.price)}</Text>
                  <Text style={[styles.heroChange, { color: dirColor, fontFamily: mono }]}>
                    {formatChange(current.change)} ({formatPercent(current.percent)})
                  </Text>
                </>
              ) : (
                <Text style={styles.heroLoading}>waiting for first quote…</Text>
              )}
            </>
          ) : (
            <Text style={styles.heroLoading}>No ticker selected</Text>
          )}
          <Text style={styles.heroMeta}>
            Showing {state.index + 1} of {state.watchlist.length} · refreshes every {state.refreshSeconds}s
          </Text>
        </Card>

        {/* View (page) control */}
        <Section title="On-screen view">
          <SegmentedControl
            segments={[...PAGE_LABELS]}
            selectedIndex={state.page}
            disabled={busy}
            onChange={(page) => command(() => client.setPage(page))}
          />
        </Section>

        {/* Economic calendar overlay */}
        <Section title="Economic calendar">
          <View style={styles.econRow}>
            <Chip
              label={state.econMode ? 'Overlay on' : 'Overlay off'}
              icon="calendar"
              tone={state.econMode ? 'warn' : 'neutral'}
              active={state.econMode}
              disabled={busy}
              onPress={() => command(() => client.setEcon(!state.econMode, state.econWeek))}
            />
            {state.econMode ? (
              <View style={styles.weekNav}>
                <Pressable
                  accessibilityLabel="Previous week"
                  disabled={busy}
                  onPress={() => command(() => client.setEcon(true, state.econWeek - 1))}
                  hitSlop={8}
                  style={styles.weekBtn}
                >
                  <Ionicons name="chevron-back" size={18} color={colors.text} />
                </Pressable>
                <Text style={styles.weekLabel}>
                  {state.econWeek === 0 ? 'This week' : state.econWeek > 0 ? `+${state.econWeek}w` : `${state.econWeek}w`}
                </Text>
                <Pressable
                  accessibilityLabel="Next week"
                  disabled={busy}
                  onPress={() => command(() => client.setEcon(true, state.econWeek + 1))}
                  hitSlop={8}
                  style={styles.weekBtn}
                >
                  <Ionicons name="chevron-forward" size={18} color={colors.text} />
                </Pressable>
              </View>
            ) : null}
          </View>
        </Section>

        {/* Watchlist */}
        <Section
          title="Watchlist"
          action={
            <Pressable accessibilityRole="button" onPress={() => router.push('/watchlist')} hitSlop={8}>
              <Text style={styles.editLink}>Edit</Text>
            </Pressable>
          }
        >
          <Card style={styles.listCard}>
            {state.watchlist.length === 0 ? (
              <Text style={styles.empty}>No symbols yet. Tap Edit to add some.</Text>
            ) : (
              state.watchlist.map((t, i) => (
                <TickerRow
                  key={`${t.symbol}-${i}`}
                  ticker={t}
                  selected={i === state.index}
                  onPress={() => command(() => client.select({ index: i }))}
                />
              ))
            )}
          </Card>
        </Section>

        {/* Actions */}
        <View style={styles.actions}>
          <Button
            label="Refresh current"
            variant="secondary"
            disabled={busy}
            onPress={() => command(() => client.refresh(false))}
            style={styles.actionBtn}
          />
          <Button
            label="Refresh all"
            variant="secondary"
            disabled={busy}
            onPress={() => command(() => client.refresh(true))}
            style={styles.actionBtn}
          />
        </View>

        {error ? <Text style={styles.errorLine}>{error}</Text> : null}
      </ScrollView>
    </Screen>
  )
}

function humanError(e: Esp32Error): string {
  switch (e.code) {
    case 'network_error':
      return 'Couldn’t reach the board. Check it’s powered on and on the same Wi-Fi.'
    case 'index_range':
    case 'page_range':
      return 'That option is out of range.'
    case 'symbol_not_found':
      return 'That symbol isn’t on the watchlist.'
    default:
      return 'That command failed. Please try again.'
  }
}

function Header({ baseUrl, onSettings }: { baseUrl: string | null; onSettings: () => void }) {
  return (
    <View style={styles.header}>
      <View>
        <Text style={styles.headerTitle}>Ticker Board</Text>
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

function Section({
  title,
  action,
  children,
}: {
  title: string
  action?: React.ReactNode
  children: React.ReactNode
}) {
  return (
    <View style={styles.section}>
      <View style={styles.sectionHead}>
        <Text style={styles.sectionTitle}>{title}</Text>
        {action}
      </View>
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
    paddingVertical: 24,
  },
  heroSymbol: {
    fontSize: 18,
    fontWeight: '600',
    color: colors.textDim,
    letterSpacing: 1,
  },
  heroPrice: {
    fontSize: 44,
    fontWeight: '700',
    color: colors.text,
  },
  heroChange: {
    fontSize: 16,
  },
  heroLoading: {
    fontSize: 15,
    color: colors.textFaint,
    fontStyle: 'italic',
    paddingVertical: 12,
  },
  heroMeta: {
    fontSize: 12,
    color: colors.textFaint,
    marginTop: 8,
  },
  section: {
    gap: 10,
  },
  sectionHead: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
  },
  sectionTitle: {
    fontSize: 13,
    fontWeight: '600',
    color: colors.textDim,
    letterSpacing: 0.8,
    textTransform: 'uppercase',
  },
  editLink: {
    fontSize: 14,
    fontWeight: '600',
    color: colors.accent,
  },
  econRow: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
  },
  weekNav: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 12,
  },
  weekBtn: {
    width: 32,
    height: 32,
    borderRadius: 16,
    backgroundColor: colors.surfaceAlt,
    alignItems: 'center',
    justifyContent: 'center',
  },
  weekLabel: {
    fontSize: 13,
    color: colors.text,
    minWidth: 64,
    textAlign: 'center',
  },
  listCard: {
    padding: 4,
  },
  empty: {
    fontSize: 14,
    color: colors.textFaint,
    textAlign: 'center',
    paddingVertical: 24,
  },
  actions: {
    flexDirection: 'row',
    gap: 12,
  },
  actionBtn: {
    flex: 1,
  },
  errorLine: {
    fontSize: 13,
    color: colors.down,
    textAlign: 'center',
  },
})
