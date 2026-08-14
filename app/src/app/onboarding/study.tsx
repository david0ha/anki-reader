import { ScrollView, StyleSheet, Text, TextInput, View } from 'react-native'
import { useRouter } from 'expo-router'
import { StepScaffold } from '../../components/StepScaffold'
import { IconBadge } from '../../components/IconBadge'
import { useOnboarding } from '../../onboarding/OnboardingContext'
import { ONBOARDING_ROUTES, canProceed, progressFor } from '../../onboarding/flow'
import { validateStudyUrl, studyUrlErrorMessage } from '../../lib/studyurl'
import { colors, radius } from '../../theme'

// Where the board fetches its study card from. Collected BEFORE the Wi-Fi handover so it is
// written to NVS at provisioning time and the board's very first poll after joining already has
// somewhere to go.
//
// Optional on purpose: a board with no URL runs its built-in demo card, which is a complete
// product on a desk with no PC on. So this step can be skipped, and the URL added later from
// Settings — but a URL that IS typed is validated here, because the board's own rejection would
// otherwise arrive on the far side of a ~45s join.
export default function Study() {
  const router = useRouter()
  const { studyUrl, setStudyUrl } = useOnboarding()

  const result = validateStudyUrl(studyUrl)
  const showError = !result.ok && studyUrl.trim().length > 0

  const next = () => router.push(ONBOARDING_ROUTES.password)
  const skip = () => {
    setStudyUrl('')
    next()
  }

  return (
    <StepScaffold
      progress={progressFor('study')}
      onBack={() => router.back()}
      onSkip={skip}
      ctaLabel="NEXT"
      canProceed={canProceed('study', { selectedNetwork: null, password: '', studyUrl })}
      onNext={next}
    >
      <ScrollView contentContainerStyle={styles.body} keyboardShouldPersistTaps="handled">
        <View style={styles.header}>
          <IconBadge name="book" size={44} />
          <Text style={styles.caption}>
            Point the board at the study card your kanjis.ai proxy serves on this network. Skip
            this and the board runs on its built-in demo card — you can add the address later from
            Settings.
          </Text>
        </View>

        <View style={styles.field}>
          <Text style={styles.label}>Card URL (optional)</Text>
          <View style={styles.inputRow}>
            <TextInput
              value={studyUrl}
              onChangeText={setStudyUrl}
              placeholder="http://mymac.local:8123/kanji.json"
              placeholderTextColor={colors.textFaint}
              autoCapitalize="none"
              autoCorrect={false}
              keyboardType="url"
              style={styles.input}
              onSubmitEditing={() => {
                if (result.ok) next()
              }}
            />
          </View>
          {showError ? (
            <Text style={styles.error}>{studyUrlErrorMessage(result)}</Text>
          ) : (
            <Text style={styles.hint}>
              Plain http on your own LAN is fine — the board never leaves it, and the proxy is what
              holds your kanjis.ai session. Run `python3 tools/mock_kanji_server.py` on that machine
              to try it out.
            </Text>
          )}
        </View>
      </ScrollView>
    </StepScaffold>
  )
}

const styles = StyleSheet.create({
  body: {
    paddingTop: 16,
    paddingBottom: 24,
    gap: 24,
  },
  header: {
    alignItems: 'center',
    gap: 14,
  },
  caption: {
    fontSize: 14,
    color: colors.textDim,
    textAlign: 'center',
    lineHeight: 20,
  },
  field: {
    gap: 8,
  },
  label: {
    fontSize: 14,
    fontWeight: '600',
    color: colors.text,
  },
  inputRow: {
    minHeight: 48,
    borderRadius: radius.md,
    borderWidth: StyleSheet.hairlineWidth,
    borderColor: colors.border,
    backgroundColor: colors.surface,
    paddingHorizontal: 14,
    flexDirection: 'row',
    alignItems: 'center',
  },
  input: {
    flex: 1,
    color: colors.text,
    fontSize: 16,
    paddingVertical: 12,
  },
  hint: {
    fontSize: 12,
    color: colors.textFaint,
    lineHeight: 16,
  },
  error: {
    fontSize: 12,
    color: colors.down,
    lineHeight: 16,
  },
})
