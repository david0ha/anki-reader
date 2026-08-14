// Pure logic for the board-onboarding wizard. The networking lives in src/lib/esp32.ts (the
// board's SoftAP JSON API); this module models only step order, progress, and per-step
// "can proceed" gating, so the screens stay declarative and the gating is unit-testable.

import { validateStudyUrl } from '../lib/studyurl'

export const ONBOARDING_STEPS = ['turn-on', 'wifi-list', 'study', 'password', 'complete'] as const

export type OnboardingStep = (typeof ONBOARDING_STEPS)[number]

/** Route path for a step, as registered under `src/app/onboarding/`. */
export const ONBOARDING_ROUTES: Record<OnboardingStep, string> = {
  'turn-on': '/onboarding/turn-on',
  'wifi-list': '/onboarding/wifi-list',
  study: '/onboarding/study',
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

/** State the wizard collects and sends to the board (see src/lib/esp32.ts). */
export interface OnboardingState {
  selectedNetwork: string | null
  password: string
  /** Whether the chosen network is password-protected. Undefined until one is picked. */
  selectedSecured?: boolean
  /** Where the board will fetch its card. Empty = run on the built-in demo card. */
  studyUrl: string
}

/** Whether the bottom CTA is enabled for the given step. */
export function canProceed(step: OnboardingStep, state: OnboardingState): boolean {
  switch (step) {
    case 'turn-on':
    case 'complete':
      return true
    case 'wifi-list':
      return state.selectedNetwork !== null
    case 'study':
      // The URL is optional — blank leaves the board on its demo card, which is a complete
      // product. But a *typed* URL must be well-formed: the board's own rejection would otherwise
      // arrive on the far side of a Wi-Fi join the user cannot undo.
      return validateStudyUrl(state.studyUrl).ok
    case 'password':
      // An open (passwordless) network needs no password; secured ones require a non-empty one.
      return state.selectedSecured === false || state.password.trim().length > 0
  }
}
