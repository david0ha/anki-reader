import { useCallback, useEffect, useMemo, useState } from 'react'
import { ActivityIndicator, Pressable, ScrollView, StyleSheet, Text, View } from 'react-native'
import { Ionicons } from '@expo/vector-icons'
import { useRouter } from 'expo-router'
import { StepScaffold } from '../../components/StepScaffold'
import { IconBadge } from '../../components/IconBadge'
import { useOnboarding } from '../../onboarding/OnboardingContext'
import { ONBOARDING_ROUTES, canProceed, progressFor } from '../../onboarding/flow'
import { esp32, type ScanNetwork } from '../../lib/esp32'
import { colors, radius } from '../../theme'

// "Other…" sentinel — lets the user provision a hidden/unlisted SSID typed on the password step.
const OTHER = '__other__'

export default function WifiList() {
  const router = useRouter()
  const { selectedNetwork, setSelectedNetwork, setSelectedSecured, password } = useOnboarding()
  const [networks, setNetworks] = useState<ScanNetwork[]>([])
  const [loading, setLoading] = useState(true)
  const [error, setError] = useState(false)
  const [other, setOther] = useState(false)

  const scan = useCallback(async () => {
    setLoading(true)
    setError(false)
    try {
      setNetworks(await esp32.scanNetworks())
    } catch (e) {
      console.warn('[onboarding] Wi-Fi scan failed', e)
      setError(true)
    } finally {
      setLoading(false)
    }
  }, [])

  useEffect(() => {
    scan()
  }, [scan])

  // The device scan often returns the same SSID more than once (dual-band radios, mesh nodes,
  // repeated scan hits). Collapse to one row per SSID — keeping the strongest signal.
  const uniqueNetworks = useMemo(() => {
    const strongest = new Map<string, ScanNetwork>()
    for (const net of networks) {
      const prev = strongest.get(net.ssid)
      if (!prev || net.rssi > prev.rssi) strongest.set(net.ssid, net)
    }
    return Array.from(strongest.values()).sort((a, b) => b.rssi - a.rssi)
  }, [networks])

  // When "Other…" is chosen the SSID is entered on the password step, so we don't yet have one.
  // Treat it as secured so the password field is required.
  const proceed = other || canProceed('wifi-list', { selectedNetwork, password })

  return (
    <StepScaffold
      progress={progressFor('wifi-list')}
      onBack={() => router.back()}
      ctaLabel="NEXT"
      ctaVariant="secondary"
      canProceed={proceed}
      onNext={() => router.push(ONBOARDING_ROUTES.keys)}
    >
      <View style={styles.header}>
        <IconBadge name="wifi" size={44} />
        <Text style={styles.caption}>Choose the Wi-Fi the board should join.</Text>
      </View>

      <View style={styles.sectionRow}>
        <Text style={styles.sectionLabel}>NETWORKS</Text>
        <Pressable accessibilityLabel="Rescan networks" onPress={scan} hitSlop={8} disabled={loading}>
          <Ionicons name="refresh" size={18} color={loading ? colors.textFaint : colors.text} />
        </Pressable>
      </View>

      <ScrollView style={styles.card} contentContainerStyle={styles.cardContent} showsVerticalScrollIndicator={false}>
        {loading ? (
          <View style={styles.state}>
            <ActivityIndicator color={colors.accent} />
            <Text style={styles.stateText}>Scanning…</Text>
          </View>
        ) : error ? (
          <Pressable style={styles.state} onPress={scan} accessibilityRole="button">
            <Text style={styles.stateText}>Couldn’t reach the board. Make sure you’re on its setup Wi-Fi.</Text>
            <Text style={styles.retry}>TAP TO RETRY</Text>
          </Pressable>
        ) : (
          <>
            {uniqueNetworks.map((net, i) => {
              const selected = !other && net.ssid === selectedNetwork
              return (
                <Pressable
                  key={net.ssid}
                  accessibilityRole="button"
                  accessibilityState={{ selected }}
                  style={[styles.row, i > 0 && styles.rowBordered]}
                  onPress={() => {
                    setOther(false)
                    setSelectedNetwork(net.ssid)
                    setSelectedSecured(net.secured)
                  }}
                >
                  <Text style={[styles.ssid, selected && styles.ssidSelected]} numberOfLines={1}>
                    {net.ssid}
                  </Text>
                  <View style={styles.icons}>
                    {selected ? <Ionicons name="checkmark" size={20} color={colors.accent} /> : null}
                    {net.secured ? <Ionicons name="lock-closed" size={16} color={colors.textDim} /> : null}
                    <Ionicons name="wifi" size={18} color={colors.text} />
                  </View>
                </Pressable>
              )
            })}

            {/* Manual / hidden SSID entry — the name itself is typed on the next step. The top
                border always shows: the "Other…" row sits below the network list (or the empty
                placeholder), so there's always a row above it to divide from. */}
            <Pressable
              accessibilityRole="button"
              accessibilityState={{ selected: other }}
              style={[styles.row, styles.rowBordered]}
              onPress={() => {
                setOther(true)
                setSelectedNetwork(null)
                setSelectedSecured(true)
              }}
            >
              <Text style={[styles.ssid, other && styles.ssidSelected]}>Other…</Text>
              {other ? <Ionicons name="checkmark" size={20} color={colors.accent} /> : null}
            </Pressable>
          </>
        )}
      </ScrollView>
    </StepScaffold>
  )
}

const styles = StyleSheet.create({
  header: {
    alignItems: 'center',
    gap: 14,
    paddingTop: 16,
  },
  caption: {
    fontSize: 14,
    color: colors.textDim,
    textAlign: 'center',
    lineHeight: 20,
  },
  sectionRow: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    marginTop: 24,
    marginBottom: 12,
  },
  sectionLabel: {
    fontSize: 12,
    fontWeight: '600',
    color: colors.textDim,
    letterSpacing: 1,
  },
  card: {
    backgroundColor: colors.surface,
    borderRadius: radius.lg,
    borderWidth: StyleSheet.hairlineWidth,
    borderColor: colors.border,
  },
  cardContent: {
    paddingHorizontal: 16,
  },
  state: {
    paddingVertical: 32,
    alignItems: 'center',
    gap: 12,
  },
  stateText: {
    fontSize: 14,
    color: colors.textDim,
    textAlign: 'center',
    lineHeight: 20,
  },
  retry: {
    fontSize: 12,
    fontWeight: '600',
    color: colors.accent,
    letterSpacing: 0.5,
  },
  row: {
    height: 58,
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
  },
  rowBordered: {
    borderTopWidth: StyleSheet.hairlineWidth,
    borderTopColor: colors.border,
  },
  ssid: {
    fontSize: 16,
    color: colors.text,
    flexShrink: 1,
  },
  ssidSelected: {
    color: colors.accent,
    fontWeight: '600',
  },
  icons: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 8,
  },
})
