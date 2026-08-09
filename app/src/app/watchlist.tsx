import { useCallback, useEffect, useState } from 'react'
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
import { Screen } from '../components/Screen'
import { BackButton } from '../components/BackButton'
import { Button } from '../components/Button'
import { Card } from '../components/Card'
import { ScreenMessage } from '../components/ScreenMessage'
import { useDevice } from '../lib/device'
import { Esp32Error } from '../lib/esp32'
import {
  WATCHLIST_MAX,
  isValidSymbol,
  normalizeSymbol,
  validateWatchlist,
  watchlistErrorMessage,
} from '../lib/watchlist'
import { colors, fonts, layout, radius, space } from '../theme'

const mono = Platform.OS === 'ios' ? fonts.monoIos : fonts.mono

export default function WatchlistEditor() {
  const router = useRouter()
  const { client } = useDevice()

  const [symbols, setSymbols] = useState<string[]>([])
  const [draft, setDraft] = useState('')
  const [loading, setLoading] = useState(true)
  const [loadError, setLoadError] = useState<string | null>(null)
  const [error, setError] = useState<string | null>(null)
  const [saving, setSaving] = useState(false)

  const load = useCallback(async () => {
    if (!client) return
    setLoading(true)
    setLoadError(null)
    try {
      const st = await client.getState()
      setSymbols(st.watchlist.map((t) => t.symbol))
    } catch {
      setLoadError('Couldn’t load the current watchlist.')
    } finally {
      setLoading(false)
    }
  }, [client])

  useEffect(() => {
    load()
  }, [load])

  const add = () => {
    setError(null)
    const sym = normalizeSymbol(draft)
    if (sym.length === 0) return
    if (!isValidSymbol(sym)) {
      setError(watchlistErrorMessage({ ok: false, error: 'invalid_symbol', offending: sym }))
      return
    }
    if (symbols.includes(sym)) {
      setError(`${sym} is already on the list.`)
      return
    }
    if (symbols.length >= WATCHLIST_MAX) {
      setError(watchlistErrorMessage({ ok: false, error: 'too_many' }))
      return
    }
    setSymbols((prev) => [...prev, sym])
    setDraft('')
  }

  const remove = (index: number) => {
    setError(null)
    setSymbols((prev) => prev.filter((_, i) => i !== index))
  }

  const move = (index: number, delta: number) => {
    setError(null)
    setSymbols((prev) => {
      const next = [...prev]
      const target = index + delta
      if (target < 0 || target >= next.length) return prev
      ;[next[index], next[target]] = [next[target], next[index]]
      return next
    })
  }

  const save = async () => {
    if (!client || saving) return
    const validated = validateWatchlist(symbols)
    if (!validated.ok) {
      setError(watchlistErrorMessage(validated))
      return
    }
    setSaving(true)
    setError(null)
    try {
      await client.setWatchlist(validated.symbols ?? [])
      router.back()
    } catch (e) {
      if (e instanceof Esp32Error) {
        setError(
          e.code === 'empty_watchlist'
            ? 'Add at least one symbol.'
            : e.code === 'too_many_tickers'
              ? `Too many symbols (max ${WATCHLIST_MAX}).`
              : e.code === 'network_error'
                ? 'Couldn’t reach the board.'
                : 'Couldn’t save the watchlist.',
        )
      } else {
        setError('Couldn’t save the watchlist.')
      }
      setSaving(false)
    }
  }

  return (
    <Screen>
      <KeyboardAvoidingView style={styles.flex} behavior={Platform.OS === 'ios' ? 'padding' : undefined}>
        <View style={styles.titleRow}>
          <BackButton onPress={() => router.back()} />
          <Text style={styles.title}>Watchlist</Text>
          <View style={styles.backSpacer} />
        </View>

        {loading ? (
          <ScreenMessage loading />
        ) : loadError ? (
          <ScreenMessage error={loadError} onRetry={load} />
        ) : (
          <>
            <ScrollView contentContainerStyle={styles.body} keyboardShouldPersistTaps="handled">
              {/* Add field */}
              <View style={styles.addRow}>
                <TextInput
                  value={draft}
                  onChangeText={setDraft}
                  placeholder="Add symbol (e.g. AAPL)"
                  placeholderTextColor={colors.textFaint}
                  autoCapitalize="characters"
                  autoCorrect={false}
                  returnKeyType="done"
                  onSubmitEditing={add}
                  style={[styles.addInput, { fontFamily: mono }]}
                />
                <Pressable
                  accessibilityLabel="Add symbol"
                  onPress={add}
                  style={styles.addBtn}
                  hitSlop={6}
                >
                  <Ionicons name="add" size={24} color={colors.ink} />
                </Pressable>
              </View>

              <Text style={styles.counter}>
                {symbols.length} / {WATCHLIST_MAX} symbols
              </Text>

              <Card style={styles.listCard}>
                {symbols.length === 0 ? (
                  <Text style={styles.empty}>No symbols. Add one above.</Text>
                ) : (
                  symbols.map((sym, i) => (
                    <View key={`${sym}-${i}`} style={[styles.row, i > 0 && styles.rowBordered]}>
                      <Text style={[styles.symbol, { fontFamily: mono }]} numberOfLines={1}>
                        {sym}
                      </Text>
                      <View style={styles.rowActions}>
                        <Pressable
                          accessibilityLabel={`Move ${sym} up`}
                          disabled={i === 0}
                          onPress={() => move(i, -1)}
                          hitSlop={6}
                          style={styles.iconBtn}
                        >
                          <Ionicons name="chevron-up" size={20} color={i === 0 ? colors.textFaint : colors.text} />
                        </Pressable>
                        <Pressable
                          accessibilityLabel={`Move ${sym} down`}
                          disabled={i === symbols.length - 1}
                          onPress={() => move(i, 1)}
                          hitSlop={6}
                          style={styles.iconBtn}
                        >
                          <Ionicons
                            name="chevron-down"
                            size={20}
                            color={i === symbols.length - 1 ? colors.textFaint : colors.text}
                          />
                        </Pressable>
                        <Pressable
                          accessibilityLabel={`Remove ${sym}`}
                          onPress={() => remove(i)}
                          hitSlop={6}
                          style={styles.iconBtn}
                        >
                          <Ionicons name="trash-outline" size={20} color={colors.down} />
                        </Pressable>
                      </View>
                    </View>
                  ))
                )}
              </Card>

              {error ? <Text style={styles.error}>{error}</Text> : null}
            </ScrollView>

            <View style={styles.ctaWrap}>
              <Button
                label={saving ? 'SAVING…' : 'SAVE WATCHLIST'}
                onPress={save}
                loading={saving}
                disabled={symbols.length === 0}
              />
            </View>
          </>
        )}
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
    paddingTop: 12,
    gap: space.md,
  },
  addRow: {
    flexDirection: 'row',
    gap: 10,
  },
  addInput: {
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
  addBtn: {
    width: 48,
    height: 48,
    borderRadius: radius.md,
    backgroundColor: colors.accent,
    alignItems: 'center',
    justifyContent: 'center',
  },
  counter: {
    fontSize: 12,
    color: colors.textFaint,
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
  row: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    paddingVertical: 12,
    paddingHorizontal: 10,
  },
  rowBordered: {
    borderTopWidth: StyleSheet.hairlineWidth,
    borderTopColor: colors.border,
  },
  symbol: {
    fontSize: 16,
    fontWeight: '600',
    color: colors.text,
    flexShrink: 1,
  },
  rowActions: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 4,
  },
  iconBtn: {
    width: 34,
    height: 34,
    alignItems: 'center',
    justifyContent: 'center',
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
})
