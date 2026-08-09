import { ActivityIndicator, Pressable, StyleSheet, Text, type ViewStyle } from 'react-native'
import { colors, radius } from '../theme'

type Variant = 'primary' | 'secondary' | 'ghost' | 'danger'

/**
 * The app's main button. `primary` is the filled accent CTA; `secondary` is a translucent
 * surface; `ghost` is borderless text; `danger` is the destructive (red) variant. Shows a
 * spinner and blocks taps while `loading`.
 */
export function Button({
  label,
  onPress,
  disabled = false,
  loading = false,
  variant = 'primary',
  style,
}: {
  label: string
  onPress: () => void
  disabled?: boolean
  loading?: boolean
  variant?: Variant
  style?: ViewStyle
}) {
  const isDisabled = disabled || loading
  const fill =
    variant === 'primary'
      ? styles.primary
      : variant === 'danger'
        ? styles.danger
        : variant === 'ghost'
          ? styles.ghost
          : styles.secondary
  const labelColor =
    variant === 'primary'
      ? colors.ink
      : variant === 'danger'
        ? colors.down
        : isDisabled
          ? colors.textFaint
          : colors.text

  return (
    <Pressable
      accessibilityRole="button"
      accessibilityState={{ disabled: isDisabled, busy: loading }}
      disabled={isDisabled}
      onPress={onPress}
      style={({ pressed }) => [
        styles.base,
        fill,
        isDisabled && styles.disabled,
        pressed && !isDisabled && styles.pressed,
        style,
      ]}
    >
      {loading ? (
        <ActivityIndicator color={variant === 'primary' ? colors.ink : colors.text} />
      ) : (
        <Text style={[styles.label, { color: labelColor }]}>{label}</Text>
      )}
    </Pressable>
  )
}

const styles = StyleSheet.create({
  base: {
    height: 52,
    borderRadius: radius.md,
    paddingHorizontal: 24,
    alignItems: 'center',
    justifyContent: 'center',
  },
  primary: {
    backgroundColor: colors.accent,
  },
  secondary: {
    backgroundColor: colors.surfaceAlt,
  },
  ghost: {
    backgroundColor: 'transparent',
  },
  danger: {
    backgroundColor: colors.downBg,
  },
  disabled: {
    opacity: 0.5,
  },
  pressed: {
    opacity: 0.8,
  },
  label: {
    fontSize: 16,
    fontWeight: '600',
    letterSpacing: 0.3,
  },
})
