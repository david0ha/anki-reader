import { StyleSheet, Text, View } from 'react-native'
import { colors } from '../theme'

/**
 * A label on the left, a value on the right, with a hairline under it unless it is the last row.
 * Used by the dashboard's source/panel cards and by the settings device card.
 *
 * The value is allowed to shrink and ellipsize; the label is not. A long snapshot URL should cut
 * itself off rather than push "URL" out of the card.
 */
export function InfoRow({
  label,
  value,
  tone = 'neutral',
  last = false,
}: {
  label: string
  value: string
  tone?: 'neutral' | 'up' | 'down' | 'warn' | 'dim'
  last?: boolean
}) {
  return (
    <View style={[styles.row, !last && styles.bordered]}>
      <Text style={styles.label}>{label}</Text>
      <Text style={[styles.value, toneStyle(tone)]} numberOfLines={1}>
        {value}
      </Text>
    </View>
  )
}

function toneStyle(tone: 'neutral' | 'up' | 'down' | 'warn' | 'dim') {
  switch (tone) {
    case 'up':
      return { color: colors.up }
    case 'down':
      return { color: colors.down }
    case 'warn':
      return { color: colors.warn }
    case 'dim':
      return { color: colors.textFaint }
    default:
      return undefined
  }
}

const styles = StyleSheet.create({
  row: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    paddingVertical: 12,
    paddingHorizontal: 16,
  },
  bordered: {
    borderBottomWidth: StyleSheet.hairlineWidth,
    borderBottomColor: colors.border,
  },
  label: {
    fontSize: 14,
    color: colors.textDim,
  },
  value: {
    fontSize: 14,
    color: colors.text,
    flexShrink: 1,
    textAlign: 'right',
    marginLeft: 16,
  },
})
