import { Pressable, ScrollView, StyleSheet, Text, TextInput, View } from 'react-native'
import { useState } from 'react'
import { useRouter } from 'expo-router'
import { StepScaffold } from '../../components/StepScaffold'
import { IconBadge } from '../../components/IconBadge'
import { useOnboarding } from '../../onboarding/OnboardingContext'
import { ONBOARDING_ROUTES, progressFor } from '../../onboarding/flow'
import { DEFAULT_ECON_URL } from '../../lib/esp32'
import { colors, radius } from '../../theme'

// Optional data-source keys collected BEFORE Wi-Fi is handed over, so they're written to NVS at
// provisioning time (the password step sends them with /api/provision). All three are optional —
// leaving them blank lets the device fall back to its compiled-in defaults, so this step never
// blocks setup. The keys are write-only: the firmware never exposes the stored values back.
export default function Keys() {
  const router = useRouter()
  const { finnhubKey, setFinnhubKey, fmpKey, setFmpKey, econUrl, setEconUrl, location, setLocation } =
    useOnboarding()

  const next = () => router.push(ONBOARDING_ROUTES.password)

  return (
    <StepScaffold
      progress={progressFor('keys')}
      onBack={() => router.back()}
      onSkip={next}
      ctaLabel="NEXT"
      ctaVariant="secondary"
      onNext={next}
    >
      <ScrollView contentContainerStyle={styles.body} keyboardShouldPersistTaps="handled">
        <View style={styles.header}>
          <IconBadge name="key" size={44} />
          <Text style={styles.caption}>
            Add your data-source keys so the board can fetch live quotes and the economic calendar.
            All optional — you can skip and add them later from Settings.
          </Text>
        </View>

        <Field
          label="Finnhub API key"
          value={finnhubKey}
          onChangeText={setFinnhubKey}
          placeholder="paste your key"
          hint="Powers stock quotes. Get a free key at finnhub.io."
          secret
        />

        <Field
          label="FMP key (optional)"
          value={fmpKey}
          onChangeText={setFmpKey}
          placeholder="economic-calendar key / proxy token"
          hint="Only needed for the economic calendar via FMP or a self-hosted proxy."
          secret
        />

        <Field
          label="Economic calendar URL (optional)"
          value={econUrl}
          onChangeText={setEconUrl}
          placeholder={DEFAULT_ECON_URL}
          hint="Override the calendar base URL (FMP direct or your own proxy). Leave blank to use the default."
          keyboardType="url"
          secret
        />

        {/* Plain free-text only — NO city autocomplete here. During onboarding the phone is joined
            to the board's SoftAP, which has no internet, so Open-Meteo's geocoder is unreachable.
            Autocomplete lives in Settings (Weather), where the phone is back on Wi-Fi. */}
        <Field
          label="Weather location (optional)"
          value={location}
          onChangeText={setLocation}
          placeholder='e.g. "Seoul" or "Paris, FR"'
          hint="Shows local weather on the board's home screen. Leave blank to turn the weather widget off — you can fine-tune it later in Settings."
        />
      </ScrollView>
    </StepScaffold>
  )
}

function Field({
  label,
  value,
  onChangeText,
  placeholder,
  hint,
  keyboardType,
  secret,
}: {
  label: string
  value: string
  onChangeText: (t: string) => void
  placeholder: string
  hint: string
  keyboardType?: 'url'
  secret?: boolean
}) {
  // Mask secrets (keys / proxy token in the URL) by default, with a reveal toggle so a long
  // pasted key can still be eyeballed for typos. Non-secret fields render unchanged.
  const [reveal, setReveal] = useState(false)
  const masked = secret && !reveal
  return (
    <View style={styles.field}>
      <Text style={styles.label}>{label}</Text>
      <View style={styles.inputRow}>
        <TextInput
          value={value}
          onChangeText={onChangeText}
          placeholder={placeholder}
          placeholderTextColor={colors.textFaint}
          autoCapitalize="none"
          autoCorrect={false}
          keyboardType={masked ? undefined : keyboardType}
          secureTextEntry={masked}
          autoComplete={secret ? 'off' : undefined}
          textContentType={secret ? 'none' : undefined}
          style={styles.input}
        />
        {secret ? (
          <Pressable onPress={() => setReveal((v) => !v)} hitSlop={8} style={styles.reveal}>
            <Text style={styles.revealText}>{reveal ? 'Hide' : 'Show'}</Text>
          </Pressable>
        ) : null}
      </View>
      <Text style={styles.hint}>{hint}</Text>
    </View>
  )
}

const styles = StyleSheet.create({
  body: {
    paddingTop: 16,
    paddingBottom: 24,
    gap: 20,
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
  reveal: {
    paddingLeft: 12,
    paddingVertical: 8,
  },
  revealText: {
    fontSize: 13,
    fontWeight: '600',
    color: colors.accent,
  },
  hint: {
    fontSize: 12,
    color: colors.textFaint,
    lineHeight: 16,
  },
})
