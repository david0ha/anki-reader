import { describe, it, expect } from '@jest/globals'
import { ONBOARDING_STEPS, canProceed, progressFor, stepIndex } from './flow'

describe('onboarding flow ordering', () => {
  it('lists the steps in order', () => {
    // The optional data-source "keys" step sits between Wi-Fi selection and the password/join step,
    // so its values are collected before provisioning hands them over with the credentials.
    expect(ONBOARDING_STEPS).toEqual(['turn-on', 'wifi-list', 'keys', 'password', 'complete'])
  })

  it('indexes steps', () => {
    expect(stepIndex('turn-on')).toBe(0)
    expect(stepIndex('complete')).toBe(4)
  })

  it('progresses from 1/5 to 5/5', () => {
    expect(progressFor('turn-on')).toBeCloseTo(0.2)
    expect(progressFor('complete')).toBeCloseTo(1)
  })
})

describe('canProceed', () => {
  const base = { selectedNetwork: null as string | null, password: '' }

  it('always allows the info + completion steps', () => {
    expect(canProceed('turn-on', base)).toBe(true)
    expect(canProceed('complete', base)).toBe(true)
  })

  it('always allows the keys step (all keys are optional)', () => {
    expect(canProceed('keys', base)).toBe(true)
  })

  it('requires a selected network on wifi-list', () => {
    expect(canProceed('wifi-list', base)).toBe(false)
    expect(canProceed('wifi-list', { ...base, selectedNetwork: 'Home' })).toBe(true)
  })

  it('requires a password only for secured networks', () => {
    expect(canProceed('password', { selectedNetwork: 'Home', selectedSecured: true, password: '' })).toBe(false)
    expect(canProceed('password', { selectedNetwork: 'Home', selectedSecured: true, password: 'pw' })).toBe(true)
    // open network needs no password
    expect(canProceed('password', { selectedNetwork: 'Cafe', selectedSecured: false, password: '' })).toBe(true)
  })
})
