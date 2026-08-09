import { useCallback, useEffect, useState, type ReactNode } from 'react'
import { Text } from 'react-native'
import { useRouter } from 'expo-router'
import { StepScaffold } from '../../components/StepScaffold'
import { StepHero, heroBold } from '../../components/StepHero'
import { IconBadge } from '../../components/IconBadge'
import { useOnboarding } from '../../onboarding/OnboardingContext'
import { ONBOARDING_ROUTES, progressFor } from '../../onboarding/flow'
import { esp32 } from '../../lib/esp32'
import { colors } from '../../theme'

// Step 1: power the device on and join its setup AP, then probe http://192.168.4.1 over the
// SoftAP. The default `esp32` client is bound to 192.168.4.1, which is correct here because the
// phone is on the device's AP (not yet the home LAN).
type Reach = 'checking' | 'found' | 'not-found'

export default function TurnOn() {
  const router = useRouter()
  const { setDeviceInfo } = useOnboarding()
  const [reach, setReach] = useState<Reach>('checking')
  const [apSsid, setApSsid] = useState<string | null>(null)

  const check = useCallback(async () => {
    setReach('checking')
    try {
      const info = await esp32.getInfo()
      setDeviceInfo(info)
      setApSsid(info.apSsid || null)
      setReach('found')
    } catch (e) {
      console.warn('[onboarding] device probe failed (likely not on the setup Wi-Fi yet)', e)
      setReach('not-found')
    }
  }, [setDeviceInfo])

  useEffect(() => {
    check()
  }, [check])

  const onNext = () => {
    if (reach === 'found') router.push(ONBOARDING_ROUTES['wifi-list'])
    else check()
  }

  let ctaLabel: string
  let title: string
  let body: ReactNode
  switch (reach) {
    case 'checking':
      ctaLabel = 'CHECKING…'
      title = 'Looking for your board'
      body = <>Looking for Ticker Board on its setup Wi-Fi…</>
      break
    case 'found':
      ctaLabel = 'NEXT'
      title = 'Board found'
      body = (
        <>
          Connected to <Text style={heroBold}>‘{apSsid ?? 'the device'}’</Text>. You’re ready — tap
          NEXT to choose your home Wi-Fi.
        </>
      )
      break
    case 'not-found':
      ctaLabel = 'CHECK AGAIN'
      title = 'Turn on your board'
      body = (
        <>
          Power the board with USB-C, then in your phone’s Wi-Fi settings join the network named{' '}
          <Text style={heroBold}>‘Ticker Board-XXXX’</Text>. Come back and tap CHECK AGAIN.
        </>
      )
      break
  }

  return (
    <StepScaffold
      progress={progressFor('turn-on')}
      // First step: no Back. No SKIP — onboarding is required to discover the device.
      ctaLabel={ctaLabel}
      canProceed={reach !== 'checking'}
      onNext={onNext}
    >
      <StepHero
        illustration={<IconBadge name={reach === 'found' ? 'checkmark-circle' : 'hardware-chip'} size={52} />}
        title={title}
        body={body}
      />
      {reach === 'not-found' ? (
        <Text style={{ color: colors.textFaint, textAlign: 'center', fontSize: 12, paddingBottom: 8 }}>
          The board reaches http://192.168.4.1 over its own Wi-Fi.
        </Text>
      ) : null}
    </StepScaffold>
  )
}
