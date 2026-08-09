import { Pressable, StyleSheet, Text, View, type ViewStyle } from 'react-native'
import { Ionicons } from '@expo/vector-icons'
import { colors, radius } from '../theme'

/**
 * A small rounded label/toggle. With `onPress` it acts as a selectable pill (the dashboard's
 * page/econ controls); without one it's a static status chip (sensor/battery readouts).
 */
export function Chip({
  label,
  icon,
  active = false,
  tone = 'neutral',
  onPress,
  disabled = false,
  style,
}: {
  label: string
  icon?: React.ComponentProps<typeof Ionicons>['name']
  active?: boolean
  tone?: 'neutral' | 'up' | 'down' | 'accent' | 'warn'
  onPress?: () => void
  disabled?: boolean
  style?: ViewStyle
}) {
  const toneColor =
    tone === 'up'
      ? colors.up
      : tone === 'down'
        ? colors.down
        : tone === 'warn'
          ? colors.warn
          : tone === 'accent'
            ? colors.accent
            : colors.text
  const content = (
    <View
      style={[
        styles.chip,
        active && styles.active,
        disabled && styles.disabled,
        style,
      ]}
    >
      {icon ? <Ionicons name={icon} size={14} color={active ? colors.ink : toneColor} /> : null}
      <Text style={[styles.label, { color: active ? colors.ink : toneColor }]}>{label}</Text>
    </View>
  )

  if (!onPress) return content
  return (
    <Pressable
      accessibilityRole="button"
      accessibilityState={{ selected: active, disabled }}
      onPress={onPress}
      disabled={disabled}
      hitSlop={6}
      style={({ pressed }) => (pressed && !disabled ? styles.pressed : undefined)}
    >
      {content}
    </Pressable>
  )
}

const styles = StyleSheet.create({
  chip: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 6,
    paddingHorizontal: 12,
    paddingVertical: 7,
    borderRadius: radius.pill,
    backgroundColor: colors.surfaceAlt,
    borderWidth: StyleSheet.hairlineWidth,
    borderColor: colors.border,
  },
  active: {
    backgroundColor: colors.accent,
    borderColor: colors.accent,
  },
  disabled: {
    opacity: 0.45,
  },
  pressed: {
    opacity: 0.7,
  },
  label: {
    fontSize: 13,
    fontWeight: '600',
    letterSpacing: 0.2,
  },
})
