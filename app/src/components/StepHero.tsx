import { type ReactNode } from 'react'
import { StyleSheet, Text, View } from 'react-native'
import { colors } from '../theme'

/** Centered illustration + title + body used by the info onboarding steps. */
export function StepHero({
  illustration,
  title,
  body,
  bodyMaxWidth = 280,
}: {
  illustration: ReactNode
  title: string
  body: ReactNode
  bodyMaxWidth?: number
}) {
  return (
    <>
      <View style={styles.illustration}>{illustration}</View>
      <View style={styles.textBlock}>
        <Text style={styles.title}>{title}</Text>
        <Text style={[styles.body, { maxWidth: bodyMaxWidth }]}>{body}</Text>
      </View>
    </>
  )
}

const styles = StyleSheet.create({
  illustration: {
    flex: 1,
    alignItems: 'center',
    justifyContent: 'center',
  },
  textBlock: {
    alignItems: 'center',
    gap: 16,
    paddingBottom: 40,
  },
  title: {
    fontSize: 28,
    fontWeight: '700',
    color: colors.text,
    textAlign: 'center',
    letterSpacing: 0.3,
  },
  body: {
    fontSize: 15,
    lineHeight: 22,
    color: colors.textDim,
    textAlign: 'center',
  },
})

export const heroBold = { fontWeight: '700', color: colors.text } as const
