import { Pressable, StyleSheet, Text, View, Platform } from 'react-native'
import type { StockTicker } from '../lib/esp32'
import { direction, formatChange, formatPercent, formatPrice } from '../lib/format'
import { colors, fonts, radius } from '../theme'

const mono = Platform.OS === 'ios' ? fonts.monoIos : fonts.mono

/**
 * One watchlist row on the dashboard. Tapping selects it on the device. `selected` mirrors the
 * device's currently-shown ticker. Invalid/never-fetched slots render as "loading".
 */
export function TickerRow({
  ticker,
  selected = false,
  onPress,
}: {
  ticker: StockTicker
  selected?: boolean
  onPress?: () => void
}) {
  const dir = direction(ticker.change)
  const dirColor = dir === 'up' ? colors.up : dir === 'down' ? colors.down : colors.textDim

  return (
    <Pressable
      accessibilityRole="button"
      accessibilityState={{ selected }}
      onPress={onPress}
      style={({ pressed }) => [styles.row, selected && styles.selected, pressed && styles.pressed]}
    >
      <View style={styles.left}>
        {selected ? <View style={styles.dot} /> : <View style={styles.dotSpacer} />}
        <Text style={[styles.symbol, { fontFamily: mono }]} numberOfLines={1}>
          {ticker.symbol}
        </Text>
      </View>

      {ticker.valid ? (
        <View style={styles.right}>
          <Text style={[styles.price, { fontFamily: mono }]}>{formatPrice(ticker.price)}</Text>
          <Text style={[styles.change, { color: dirColor, fontFamily: mono }]}>
            {formatChange(ticker.change)} ({formatPercent(ticker.percent)})
          </Text>
        </View>
      ) : (
        <Text style={styles.loading}>loading…</Text>
      )}
    </Pressable>
  )
}

const styles = StyleSheet.create({
  row: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    paddingVertical: 12,
    paddingHorizontal: 12,
    borderRadius: radius.md,
  },
  selected: {
    backgroundColor: colors.accentDim,
  },
  pressed: {
    opacity: 0.7,
  },
  left: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 10,
    flexShrink: 1,
  },
  dot: {
    width: 8,
    height: 8,
    borderRadius: 4,
    backgroundColor: colors.accent,
  },
  dotSpacer: {
    width: 8,
    height: 8,
  },
  symbol: {
    fontSize: 16,
    fontWeight: '600',
    color: colors.text,
  },
  right: {
    alignItems: 'flex-end',
  },
  price: {
    fontSize: 16,
    color: colors.text,
  },
  change: {
    fontSize: 12,
    marginTop: 2,
  },
  loading: {
    fontSize: 13,
    color: colors.textFaint,
    fontStyle: 'italic',
  },
})
