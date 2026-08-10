import { ScrollView, StyleSheet, Text, TextInput, View } from 'react-native'
import { useRouter } from 'expo-router'
import { StepScaffold } from '../../components/StepScaffold'
import { IconBadge } from '../../components/IconBadge'
import { useOnboarding } from '../../onboarding/OnboardingContext'
import { ONBOARDING_ROUTES, canProceed, progressFor } from '../../onboarding/flow'
import { validateVaultUrl, vaultUrlErrorMessage } from '../../lib/vaulturl'
import { colors, radius } from '../../theme'

// Where the board fetches its vault snapshot from. Collected BEFORE the Wi-Fi handover so it is
// written to NVS at provisioning time and the board's very first poll after joining already has
// somewhere to go.
//
// Optional on purpose: a board with no URL runs its built-in demo snapshot, which is a complete
// product on a desk with no PC on. So this step can be skipped, and the URL added later from
// Settings — but a URL that IS typed is validated here, because the board's own rejection would
// otherwise arrive on the far side of a ~45s join.
export default function Vault() {
  const router = useRouter()
  const { vaultUrl, setVaultUrl } = useOnboarding()

  const result = validateVaultUrl(vaultUrl)
  const showError = !result.ok && vaultUrl.trim().length > 0

  const next = () => router.push(ONBOARDING_ROUTES.password)
  const skip = () => {
    setVaultUrl('')
    next()
  }

  return (
    <StepScaffold
      progress={progressFor('vault')}
      onBack={() => router.back()}
      onSkip={skip}
      ctaLabel="NEXT"
      canProceed={canProceed('vault', { selectedNetwork: null, password: '', vaultUrl })}
      onNext={next}
    >
      <ScrollView contentContainerStyle={styles.body} keyboardShouldPersistTaps="handled">
        <View style={styles.header}>
          <IconBadge name="link" size={44} />
          <Text style={styles.caption}>
            Point the board at the JSON your vault publishes on this network. Skip this and the
            board runs on its built-in demo data — you can add the address later from Settings.
          </Text>
        </View>

        <View style={styles.field}>
          <Text style={styles.label}>Snapshot URL (optional)</Text>
          <View style={styles.inputRow}>
            <TextInput
              value={vaultUrl}
              onChangeText={setVaultUrl}
              placeholder="http://mymac.local:8123/vault.json"
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
            <Text style={styles.error}>{vaultUrlErrorMessage(result)}</Text>
          ) : (
            <Text style={styles.hint}>
              Plain http on your own LAN is fine — the board and the machine serving this never
              leave it. Run `python3 tools/mock_vault_server.py` on that machine to try it out.
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
