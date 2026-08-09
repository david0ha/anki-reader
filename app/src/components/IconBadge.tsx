import { StyleSheet, View } from 'react-native'
import { Ionicons } from '@expo/vector-icons'
import { colors, radius } from '../theme'

/** A rounded badge holding a single accent glyph, used by the onboarding hero steps. */
export function IconBadge({
  name,
  size = 48,
}: {
  name: React.ComponentProps<typeof Ionicons>['name']
  size?: number
}) {
  return (
    <View style={styles.badge}>
      <Ionicons name={name} size={size} color={colors.accent} />
    </View>
  )
}

const styles = StyleSheet.create({
  badge: {
    width: 96,
    height: 96,
    borderRadius: radius.lg,
    backgroundColor: colors.accentDim,
    alignItems: 'center',
    justifyContent: 'center',
  },
})
