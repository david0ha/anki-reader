import { ActivityIndicator, Pressable, StyleSheet, Text, View } from 'react-native'
import { colors } from '../theme'

/**
 * Centered loading / error / empty state shared across screens. Pass `loading` for a spinner,
 * `error` for an error message (with optional retry), or `message` for a neutral empty state.
 */
export function ScreenMessage({
  loading,
  error,
  message,
  onRetry,
}: {
  loading?: boolean
  error?: string | null
  message?: string
  onRetry?: () => void
}) {
  return (
    <View style={styles.center}>
      {loading ? (
        <ActivityIndicator color={colors.accent} />
      ) : error ? (
        <>
          <Text style={styles.error}>{error}</Text>
          {onRetry ? (
            <Pressable accessibilityRole="button" onPress={onRetry} style={styles.retry} hitSlop={8}>
              <Text style={styles.retryText}>Try again</Text>
            </Pressable>
          ) : null}
        </>
      ) : (
        <Text style={styles.message}>{message}</Text>
      )}
    </View>
  )
}

const styles = StyleSheet.create({
  center: {
    flex: 1,
    alignItems: 'center',
    justifyContent: 'center',
    gap: 12,
    paddingHorizontal: 24,
  },
  error: {
    fontSize: 14,
    color: colors.down,
    textAlign: 'center',
    lineHeight: 20,
  },
  retry: {
    paddingHorizontal: 16,
    paddingVertical: 8,
  },
  retryText: {
    fontSize: 14,
    fontWeight: '600',
    color: colors.accent,
  },
  message: {
    fontSize: 14,
    color: colors.textDim,
    textAlign: 'center',
  },
})
