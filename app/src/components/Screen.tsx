import { type ReactNode } from 'react'
import { StyleSheet, View, type ViewStyle } from 'react-native'
import { SafeAreaView, type Edge } from 'react-native-safe-area-context'
import { StatusBar } from 'expo-status-bar'
import { colors } from '../theme'

/** Full-bleed dark screen base used by every route. */
export function Screen({
  children,
  edges = ['top', 'bottom'],
  style,
}: {
  children: ReactNode
  edges?: Edge[]
  style?: ViewStyle
}) {
  return (
    <View style={styles.root}>
      <StatusBar style="light" />
      <SafeAreaView style={[styles.safe, style]} edges={edges}>
        {children}
      </SafeAreaView>
    </View>
  )
}

const styles = StyleSheet.create({
  root: {
    flex: 1,
    backgroundColor: colors.bg,
  },
  safe: {
    flex: 1,
  },
})
