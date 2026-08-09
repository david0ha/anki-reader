import { describe, it, expect } from '@jest/globals'
import { mapGeoResults, searchCities } from './geocode'

// A trimmed-down sample of the real Open-Meteo geocoding response for ?name=Seoul.
const SAMPLE = {
  results: [
    {
      id: 1835848,
      name: 'Seoul',
      latitude: 37.566,
      longitude: 126.9784,
      country: 'South Korea',
      admin1: 'Seoul',
    },
    {
      id: 1835235,
      name: 'Seongnam-si',
      latitude: 37.43861,
      longitude: 127.13778,
      country: 'South Korea',
      admin1: 'Gyeonggi-do',
    },
    {
      // No admin1 — sublabel should be just the country.
      id: 4174757,
      name: 'Seoul',
      latitude: 27.83392,
      longitude: -97.51805,
      country: 'United States',
    },
  ],
}

describe('mapGeoResults', () => {
  it('maps a sample Open-Meteo response to GeoCity[]', () => {
    const cities = mapGeoResults(SAMPLE)
    expect(cities).toEqual([
      {
        id: '1835848',
        name: 'Seoul',
        label: 'Seoul',
        sublabel: 'Seoul, South Korea',
        latitude: 37.566,
        longitude: 126.9784,
      },
      {
        id: '1835235',
        name: 'Seongnam-si',
        label: 'Seongnam-si',
        sublabel: 'Gyeonggi-do, South Korea',
        latitude: 37.43861,
        longitude: 127.13778,
      },
      {
        id: '4174757',
        name: 'Seoul',
        label: 'Seoul',
        sublabel: 'United States',
        latitude: 27.83392,
        longitude: -97.51805,
      },
    ])
  })

  it('keeps the bare city name as `name` (not the rich label) so the device re-geocodes it', () => {
    const [first] = mapGeoResults(SAMPLE)
    // The dropdown shows "Seoul" + "Seoul, South Korea", but the saved value is the bare name.
    expect(first.name).toBe('Seoul')
    expect(first.label).toBe('Seoul')
    expect(first.sublabel).toBe('Seoul, South Korea')
  })

  it('returns [] when there is no results array', () => {
    expect(mapGeoResults({})).toEqual([])
    expect(mapGeoResults({ results: null })).toEqual([])
    expect(mapGeoResults(null)).toEqual([])
    expect(mapGeoResults(undefined)).toEqual([])
  })

  it('skips entries without a usable name', () => {
    const cities = mapGeoResults({
      results: [{ id: 1, latitude: 1, longitude: 2 }, { id: 2, name: '   ' }, { id: 3, name: 'Paris' }],
    })
    expect(cities.map((c) => c.name)).toEqual(['Paris'])
  })
})

describe('searchCities', () => {
  function fakeFetch(
    impl: (url: string, init?: RequestInit) => Promise<Partial<Response>> | Partial<Response>,
  ) {
    const calls: string[] = []
    const fetchImpl = (async (url: string, init?: RequestInit) => {
      calls.push(String(url))
      return impl(String(url), init)
    }) as unknown as typeof fetch
    return { fetchImpl, calls }
  }

  it('returns mapped cities for a 200 response with results', async () => {
    const { fetchImpl, calls } = fakeFetch(() => ({
      ok: true,
      json: async () => SAMPLE,
    }))
    const cities = await searchCities('Seoul', { fetchImpl })
    expect(cities.map((c) => c.name)).toEqual(['Seoul', 'Seongnam-si', 'Seoul'])
    // Hits the same Open-Meteo endpoint as the firmware, with count=6 for the list.
    expect(calls[0]).toContain('https://geocoding-api.open-meteo.com/v1/search?name=Seoul')
    expect(calls[0]).toContain('count=6')
  })

  it('returns [] on a non-200 response', async () => {
    const { fetchImpl } = fakeFetch(() => ({ ok: false, status: 429, json: async () => ({}) }))
    expect(await searchCities('Seoul', { fetchImpl })).toEqual([])
  })

  it('returns [] when fetch throws (network error)', async () => {
    const { fetchImpl } = fakeFetch(() => {
      throw new TypeError('Network request failed')
    })
    expect(await searchCities('Seoul', { fetchImpl })).toEqual([])
  })

  it('returns [] when the request is aborted', async () => {
    const { fetchImpl } = fakeFetch(() => {
      const e = new Error('Aborted')
      e.name = 'AbortError'
      throw e
    })
    expect(await searchCities('Seoul', { fetchImpl })).toEqual([])
  })

  it('returns [] for a blank query without hitting the network', async () => {
    const { fetchImpl, calls } = fakeFetch(() => ({ ok: true, json: async () => SAMPLE }))
    expect(await searchCities('   ', { fetchImpl })).toEqual([])
    expect(calls).toEqual([])
  })

  it('passes the abort signal through to fetch', async () => {
    const controller = new AbortController()
    let seenSignal: AbortSignal | undefined
    const { fetchImpl } = fakeFetch((_url, init) => {
      seenSignal = init?.signal ?? undefined
      return { ok: true, json: async () => SAMPLE }
    })
    await searchCities('Seoul', { fetchImpl, signal: controller.signal })
    expect(seenSignal).toBe(controller.signal)
  })
})
