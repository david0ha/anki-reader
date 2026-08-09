// Pure logic for the device-onboarding wizard. The networking lives in src/lib/esp32.ts (the
// device's SoftAP JSON API); this module models only step order, progress, and per-step
// "can proceed" gating, so the screens stay declarative and the gating is unit-testable.

export const ONBOARDING_STEPS = ['turn-on', 'wifi-list', 'keys', 'password', 'complete'] as const

export type OnboardingStep = (typeof ONBOARDING_STEPS)[number]

/** Route path for a step, as registered under `src/app/onboarding/`. */
export const ONBOARDING_ROUTES: Record<OnboardingStep, string> = {
  'turn-on': '/onboarding/turn-on',
  'wifi-list': '/onboarding/wifi-list',
  keys: '/onboarding/keys',
  password: '/onboarding/password',
  complete: '/onboarding/complete',
}

export function stepIndex(step: OnboardingStep): number {
  return ONBOARDING_STEPS.indexOf(step)
}

/** Progress-bar fill fraction (0..1) for the bar shown at the top. */
export function progressFor(step: OnboardingStep): number {
  return (stepIndex(step) + 1) / ONBOARDING_STEPS.length
}

/** State the wizard collects and sends to the device (see src/lib/esp32.ts). */
export interface OnboardingState {
  selectedNetwork: string | null
  password: string
  /** Whether the chosen network is password-protected. Undefined until one is picked. */
  selectedSecured?: boolean
}

/** Whether the bottom CTA is enabled for the given step. */
export function canProceed(step: OnboardingStep, state: OnboardingState): boolean {
  switch (step) {
    case 'turn-on':
    case 'complete':
    // Keys are all optional — the user can skip the step entirely (device uses its defaults).
    case 'keys':
      return true
    case 'wifi-list':
      return state.selectedNetwork !== null
    case 'password':
      // An open (passwordless) network needs no password; secured ones require a non-empty one.
      return state.selectedSecured === false || state.password.trim().length > 0
  }
}
