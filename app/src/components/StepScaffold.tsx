import { type ReactNode } from 'react'
import { Pressable, StyleSheet, Text, View } from 'react-native'
import { Screen } from './Screen'
import { Button } from './Button'
import { BackButton } from './BackButton'
import { colors, layout } from '../theme'

/**
 * Shared chrome for every onboarding step: an optional back-circle + SKIP bar, the progress bar,
 * a content slot, and the bottom CTA.
 */
export function StepScaffold({
  progress,
  onBack,
  onSkip,
  ctaLabel,
  onNext,
  canProceed = true,
  ctaVariant = 'primary',
  loading = false,
  children,
}: {
  progress: number
  onBack?: () => void
  onSkip?: () => void
  ctaLabel: string
  onNext: () => void
  canProceed?: boolean
  ctaVariant?: 'primary' | 'secondary'
  loading?: boolean
  children: ReactNode
}) {
  return (
    <Screen>
      <View style={styles.topBar}>
        {onBack ? <BackButton onPress={onBack} /> : <View style={styles.backSpacer} />}
        {onSkip ? (
          <Pressable accessibilityRole="button" accessibilityLabel="Skip" onPress={onSkip} style={styles.skipHit}>
            <Text style={styles.skip}>SKIP</Text>
          </Pressable>
        ) : null}
      </View>

      <View style={styles.progressTrack}>
        <View style={[styles.progressFill, { width: `${Math.max(0, Math.min(1, progress)) * 100}%` }]} />
      </View>

      <View style={styles.content}>{children}</View>

      <View style={styles.ctaWrap}>
        <Button label={ctaLabel} onPress={onNext} disabled={!canProceed} loading={loading} variant={ctaVariant} />
      </View>
    </Screen>
  )
}

const styles = StyleSheet.create({
  topBar: {
    height: 64,
    paddingHorizontal: layout.gutter,
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
  },
  backSpacer: {
    width: 42,
    height: 42,
  },
  skipHit: {
    minHeight: 44,
    minWidth: 44,
    alignItems: 'flex-end',
    justifyContent: 'center',
  },
  skip: {
    fontSize: 14,
    fontWeight: '600',
    color: colors.textDim,
    letterSpacing: 0.5,
  },
  progressTrack: {
    height: 4,
    marginHorizontal: layout.gutter,
    borderRadius: 2,
    backgroundColor: colors.surfaceAlt,
    overflow: 'hidden',
  },
  progressFill: {
    height: '100%',
    borderRadius: 2,
    backgroundColor: colors.accent,
  },
  content: {
    flex: 1,
    paddingHorizontal: layout.gutter,
  },
  ctaWrap: {
    paddingHorizontal: layout.gutter,
    paddingBottom: 8,
  },
})
