import { StyleSheet, Text, View } from 'react-native'
import { colors, radius } from '../theme'

/**
 * One big number with a label, and an optional footnote under it. Four of these make the
 * dashboard's counter grid (notes / links / orphans / tags).
 *
 * `tone` colours the number: the counter grid uses it for orphans, where a large number is the
 * one thing on the tile the user might want to act on.
 */
export function StatTile({
  label,
  value,
  footnote,
  tone = 'neutral',
}: {
  label: string
  value: string
  footnote?: string
  tone?: 'neutral' | 'warn'
}) {
  return (
    <View style={styles.tile}>
      <Text style={styles.label}>{label}</Text>
      <Text style={[styles.value, tone === 'warn' && styles.valueWarn]} numberOfLines={1}>
        {value}
      </Text>
      {footnote ? <Text style={styles.footnote}>{footnote}</Text> : null}
    </View>
  )
}

const styles = StyleSheet.create({
  tile: {
    // Two per row with the parent's 12px gap. Percentages rather than flex so a long number
    // widens the text, not the tile — two tiles of different widths read as a layout bug.
    width: '48%',
    backgroundColor: colors.surface,
    borderRadius: radius.md,
    borderWidth: StyleSheet.hairlineWidth,
    borderColor: colors.border,
    paddingHorizontal: 14,
    paddingVertical: 12,
    gap: 2,
  },
  label: {
    fontSize: 12,
    color: colors.textDim,
    letterSpacing: 0.6,
    textTransform: 'uppercase',
  },
  value: {
    fontSize: 26,
    fontWeight: '700',
    color: colors.text,
  },
  valueWarn: {
    color: colors.warn,
  },
  footnote: {
    fontSize: 12,
    color: colors.textFaint,
  },
})
