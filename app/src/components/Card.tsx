import { type ReactNode } from 'react'
import { StyleSheet, View, type ViewStyle } from 'react-native'
import { colors, radius, space } from '../theme'

/** A padded surface panel — the building block for the dashboard sections. */
export function Card({ children, style }: { children: ReactNode; style?: ViewStyle }) {
  return <View style={[styles.card, style]}>{children}</View>
}

const styles = StyleSheet.create({
  card: {
    backgroundColor: colors.surface,
    borderRadius: radius.lg,
    borderWidth: StyleSheet.hairlineWidth,
    borderColor: colors.border,
    padding: space.lg,
  },
})
