// City autocomplete via Open-Meteo's keyless geocoding API — the SAME endpoint the firmware uses
// to resolve a weather location (components/stock_core/weather_service.c: GET
// https://geocoding-api.open-meteo.com/v1/search?name=<text>&count=1&language=en&format=json,
// taking the FIRST result). We query it with count=6 so the user can pick from a list, but the
// value we save/send is always the bare result `name` (e.g. "Seoul") — see GeoCity.name below.
//
// This is keyless + HTTPS + CORS-enabled, so the autocomplete talks to Open-Meteo directly with a
// plain fetch — it does NOT go through the esp32 client / the board.

/** One geocoding suggestion. */
export interface GeoCity {
  /** Stable key for React lists (Open-Meteo's numeric result id, stringified). */
  id: string
  /**
   * The bare city name to SAVE/SEND to the device (e.g. "Seoul"), NOT the rich label. The firmware
   * re-geocodes the saved text with count=1 and takes the top hit; Open-Meteo's `name` search
   * matches on the bare city name, so "Seoul" resolves back to this exact city. Sending the rich
   * "Seoul, South Korea" would NOT match the name search and would break geocoding on the board.
   */
  name: string
  /** Primary display line in the dropdown — the city name (= result.name). */
  label: string
  /** Secondary display line for disambiguation, e.g. "Gyeonggi, South Korea". May be empty. */
  sublabel: string
  latitude: number
  longitude: number
}

/** Shape of a single Open-Meteo geocoding result (only the fields we use). */
interface OpenMeteoResult {
  id?: number | string
  name?: string
  admin1?: string
  country?: string
  latitude?: number
  longitude?: number
}

interface OpenMeteoResponse {
  results?: OpenMeteoResult[]
}

const GEO_SEARCH_URL = 'https://geocoding-api.open-meteo.com/v1/search'

/**
 * Map a raw Open-Meteo geocoding response to our GeoCity[] (pure — no network). Skips entries
 * without a usable `name`. `label` is the bare city name; `sublabel` joins [admin1, country].
 */
export function mapGeoResults(json: unknown): GeoCity[] {
  const results = (json as OpenMeteoResponse | null | undefined)?.results
  if (!Array.isArray(results)) return []
  const cities: GeoCity[] = []
  for (const r of results) {
    const name = typeof r?.name === 'string' ? r.name.trim() : ''
    if (!name) continue
    const sublabel = [r.admin1, r.country]
      .filter((p): p is string => typeof p === 'string' && p.trim().length > 0)
      .join(', ')
    cities.push({
      id: r.id != null ? String(r.id) : `${name}:${r.latitude ?? ''},${r.longitude ?? ''}`,
      name, // bare name — what we save/send (see GeoCity.name doc above)
      label: name,
      sublabel,
      latitude: Number(r.latitude) || 0,
      longitude: Number(r.longitude) || 0,
    })
  }
  return cities
}

export interface SearchCitiesOptions {
  /** Injectable fetch (defaults to the global) so this is unit-testable without a network. */
  fetchImpl?: typeof fetch
  /** Optional AbortSignal so callers can cancel a stale in-flight request. */
  signal?: AbortSignal
}

/**
 * Query Open-Meteo for cities matching `query`. Returns up to 6 suggestions, or [] on a blank
 * query, a non-200 response, a missing `results` array, or any network/abort error — it NEVER
 * throws to the UI, so the caller can always fall back to free typing.
 */
export async function searchCities(
  query: string,
  options: SearchCitiesOptions = {},
): Promise<GeoCity[]> {
  const trimmed = query.trim()
  if (!trimmed) return []
  const doFetch = options.fetchImpl ?? fetch
  const url = `${GEO_SEARCH_URL}?name=${encodeURIComponent(trimmed)}&count=6&language=en&format=json`
  try {
    const res = await doFetch(url, { signal: options.signal })
    if (!res.ok) return []
    const json = await res.json()
    return mapGeoResults(json)
  } catch {
    // Network failure, abort, or malformed JSON — surface as "no suggestions", never throw.
    return []
  }
}
