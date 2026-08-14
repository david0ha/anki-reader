import { describe, it, expect } from '@jest/globals'
import { ONBOARDING_STEPS, canProceed, progressFor, stepIndex } from './flow'

describe('onboarding flow ordering', () => {
  it('lists the steps in order', () => {
    // The study-URL step sits between Wi-Fi selection and the password/join step, so the address
    // is collected before provisioning hands it over with the credentials — the board's very
    // first poll after joining then already has somewhere to go.
    expect(ONBOARDING_STEPS).toEqual(['turn-on', 'wifi-list', 'study', 'password', 'complete'])
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
  const base = { selectedNetwork: null as string | null, password: '', studyUrl: '' }

  it('always allows the info + completion steps', () => {
    expect(canProceed('turn-on', base)).toBe(true)
    expect(canProceed('complete', base)).toBe(true)
  })

  it('requires a selected network on wifi-list', () => {
    expect(canProceed('wifi-list', base)).toBe(false)
    expect(canProceed('wifi-list', { ...base, selectedNetwork: 'Home' })).toBe(true)
  })

  it('allows a blank study URL — the board runs on its demo card', () => {
    expect(canProceed('study', base)).toBe(true)
  })

  it('allows a well-formed study URL', () => {
    expect(canProceed('study', { ...base, studyUrl: 'http://mac.local:8123/kanji.json' })).toBe(true)
  })

  it('blocks a malformed study URL before the join, not after it', () => {
    // The board would reject this too, but only on the far side of a ~45s Wi-Fi join the user
    // cannot undo.
    expect(canProceed('study', { ...base, studyUrl: 'mac.local/kanji.json' })).toBe(false)
    expect(canProceed('study', { ...base, studyUrl: 'http://' })).toBe(false)
  })

  it('requires a password only for secured networks', () => {
    expect(canProceed('password', { ...base, selectedNetwork: 'Home', selectedSecured: true, password: '' })).toBe(
      false,
    )
    expect(
      canProceed('password', { ...base, selectedNetwork: 'Home', selectedSecured: true, password: 'pw' }),
    ).toBe(true)
    // open network needs no password
    expect(
      canProceed('password', { ...base, selectedNetwork: 'Cafe', selectedSecured: false, password: '' }),
    ).toBe(true)
  })
})
