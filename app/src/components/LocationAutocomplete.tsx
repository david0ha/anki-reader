import { useCallback, useEffect, useRef, useState } from 'react'
import { ActivityIndicator, Pressable, StyleSheet, Text, TextInput, View } from 'react-native'
import { Button } from './Button'
import { searchCities, type GeoCity } from '../lib/geocode'
import { colors, radius } from '../theme'

// A weather-location editor with a city autocomplete dropdown. As the user types we query
// Open-Meteo's keyless geocoder (the SAME endpoint the firmware uses) and list matches in a popup
// under the input; tapping one fills the field with the city's BARE name (e.g. "Seoul") — see
// src/lib/geocode.ts for why we save the bare name and not the rich "Seoul, South Korea" label.
//
// Free typing is always allowed: if Open-Meteo is unreachable or the user ignores the suggestions,
// they can type any text and Save anyway. This mirrors the LocationRow contract it replaces
// (initial + onSave(value): Promise<boolean>) so it drops straight into Settings.

const DEBOUNCE_MS = 350
const MIN_QUERY_LEN = 2

export function LocationAutocomplete({
  initial,
  onSave,
}: {
  initial: string
  onSave: (value: string) => Promise<boolean>
}) {
  const [draft, setDraft] = useState(initial)
  const [saving, setSaving] = useState(false)
  const [done, setDone] = useState(false)
  const [failed, setFailed] = useState(false)

  const [suggestions, setSuggestions] = useState<GeoCity[]>([])
  const [loading, setLoading] = useState(false)
  const [open, setOpen] = useState(false)

  // Tracks the latest in-flight request so we can abort it when a new keystroke supersedes it, and
  // a flag to suppress the search that a programmatic setDraft (selecting a suggestion) would
  // otherwise trigger.
  const abortRef = useRef<AbortController | null>(null)
  const suppressSearchRef = useRef(false)

  // Debounced geocoding: 350ms after the last keystroke, with ≥2 chars, abort any prior request.
  useEffect(() => {
    if (suppressSearchRef.current) {
      // The draft changed because the user picked a suggestion — don't re-query for it.
      suppressSearchRef.current = false
      return
    }
    const q = draft.trim()
    if (q.length < MIN_QUERY_LEN) {
      abortRef.current?.abort()
      setSuggestions([])
      setLoading(false)
      setOpen(false)
      return
    }

    setLoading(true)
    const handle = setTimeout(async () => {
      abortRef.current?.abort()
      const controller = new AbortController()
      abortRef.current = controller
      const cities = await searchCities(q, { signal: controller.signal })
      // Ignore results from a request that has since been superseded/aborted.
      if (controller.signal.aborted) return
      setSuggestions(cities)
      setOpen(cities.length > 0)
      setLoading(false)
    }, DEBOUNCE_MS)

    return () => clearTimeout(handle)
  }, [draft])

  // Cancel any pending request on unmount.
  useEffect(() => () => abortRef.current?.abort(), [])

  const onChange = useCallback((t: string) => {
    setDraft(t)
    setDone(false)
    setFailed(false)
  }, [])

  // Fill the field with the city's BARE name (what the device re-geocodes), close the dropdown, and
  // leave Save to the user (explicit affordance below).
  const pick = useCallback((city: GeoCity) => {
    suppressSearchRef.current = true
    abortRef.current?.abort()
    setDraft(city.name)
    setSuggestions([])
    setOpen(false)
    setLoading(false)
    setDone(false)
    setFailed(false)
  }, [])

  const save = useCallback(async () => {
    setOpen(false)
    setSaving(true)
    setDone(false)
    setFailed(false)
    const ok = await onSave(draft.trim())
    setSaving(false)
    if (ok) setDone(true)
    else setFailed(true)
  }, [draft, onSave])

  return (
    <View style={styles.wrap}>
      <View style={styles.inputWrap}>
        <View style={styles.hostRow}>
          <TextInput
            value={draft}
            onChangeText={onChange}
            onFocus={() => {
              if (suggestions.length > 0) setOpen(true)
            }}
            onBlur={() => setOpen(false)}
            placeholder='e.g. "Seoul" or "Paris, FR"'
            placeholderTextColor={colors.textFaint}
            autoCapitalize="words"
            autoCorrect={false}
            style={styles.hostInput}
            onSubmitEditing={save}
          />
          {loading ? (
            <ActivityIndicator
              size="small"
              color={colors.textDim}
              style={styles.spinner}
            />
          ) : null}
        </View>

        {open && suggestions.length > 0 ? (
          <View style={styles.dropdown}>
            {suggestions.map((city, i) => (
              <Pressable
                key={city.id}
                // onPress fires after onBlur on RN; use onPressIn so the tap registers before the
                // input's blur closes the dropdown.
                onPressIn={() => pick(city)}
                accessibilityRole="button"
                style={({ pressed }) => [
                  styles.option,
                  i < suggestions.length - 1 && styles.optionBordered,
                  pressed && styles.optionPressed,
                ]}
              >
                <Text style={styles.optionLabel} numberOfLines={1}>
                  {city.label}
                </Text>
                {city.sublabel ? (
                  <Text style={styles.optionSublabel} numberOfLines={1}>
                    {city.sublabel}
                  </Text>
                ) : null}
              </Pressable>
            ))}
          </View>
        ) : null}
      </View>

      {failed ? <Text style={styles.error}>Couldn’t update. Please try again.</Text> : null}
      {done ? <Text style={styles.saved}>Updated.</Text> : null}
      <Button label="Save" variant="secondary" loading={saving} onPress={save} />
    </View>
  )
}

const styles = StyleSheet.create({
  wrap: {
    gap: 8,
  },
  // The input + dropdown share a relative wrapper so the popup can absolutely-position under the
  // field (zIndex/elevation keep it above the following rows).
  inputWrap: {
    position: 'relative',
    zIndex: 10,
  },
  hostRow: {
    flexDirection: 'row',
    alignItems: 'center',
  },
  hostInput: {
    flex: 1,
    height: 48,
    borderRadius: radius.md,
    borderWidth: StyleSheet.hairlineWidth,
    borderColor: colors.border,
    backgroundColor: colors.surface,
    paddingHorizontal: 14,
    color: colors.text,
    fontSize: 16,
  },
  spinner: {
    position: 'absolute',
    right: 14,
  },
  dropdown: {
    position: 'absolute',
    top: 52,
    left: 0,
    right: 0,
    backgroundColor: colors.surfaceAlt,
    borderRadius: radius.md,
    borderWidth: StyleSheet.hairlineWidth,
    borderColor: colors.borderStrong,
    overflow: 'hidden',
    zIndex: 20,
    elevation: 6,
  },
  option: {
    paddingVertical: 10,
    paddingHorizontal: 14,
  },
  optionBordered: {
    borderBottomWidth: StyleSheet.hairlineWidth,
    borderBottomColor: colors.border,
  },
  optionPressed: {
    backgroundColor: colors.surface,
  },
  optionLabel: {
    fontSize: 15,
    color: colors.text,
  },
  optionSublabel: {
    fontSize: 12,
    color: colors.textDim,
    marginTop: 2,
  },
  error: {
    fontSize: 13,
    color: colors.down,
  },
  saved: {
    fontSize: 13,
    color: colors.up,
  },
})
