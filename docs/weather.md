# Weather (Open-Meteo — free, no API key)

The home page's current conditions (glyph + °C + city) and its forecast strip come from
[Open-Meteo](https://open-meteo.com). **No key is needed** — it is free for non-commercial use, so
there is no secret to provision, store or leak. It is the only host this firmware contacts.

## How it works

1. In the **captive portal** (or the app), you type a **place name**, not coordinates — `Seoul`,
   `Paris`, `Austin, US`. The portal's SoftAP has no internet, so it cannot geocode there; the text
   is stored as-is in NVS (`prov` namespace, key `loc`).
2. Once Wi-Fi is up, `WeatherTask` resolves it through Open-Meteo's **geocoding API** (first, nearest
   match). The resolved `"City, CC"` is shown on screen so you can **confirm it matched what you
   meant** — `Paris` is ambiguous, and this is the only feedback that tells you which one you got.
3. From then on it refreshes current + 7-day every 30 minutes using the coordinates.

Leaving the location blank hides the whole weather block. Changing it via `POST /api/location`
re-geocodes live — no reboot.

## Code

| File | Role |
|------|------|
| `components/fortune_core/include/weather_model.h` | `geo_loc_t` / `weather_t` / 4-state `wx_kind_t` |
| `components/fortune_core/weather_parse.c` | geocoding + forecast JSON, WMO→glyph mapping, day-of-week |
| `components/fortune_core/weather_service.c` | keyless URL assembly + `http_get` |
| `components/fortune_core/test/host/test_weather.c` | parser tests (`om_geo.json`, `om_forecast.json`) |

`wx_kind_t` has four states — clear, partly cloudy, overcast, rain — because that is what a 1-bit
panel can draw distinguishably at 20 px. Snow, fog and storms collapse onto the nearest of them
(`wx_from_wmo()`, host-tested).

## On the panel

The service fetches 7 days; **the strip draws 5**. Seven columns across 122 px is 17 px each, at
which the weather glyph is an illegible blob and a one-letter weekday is ambiguous (S/S, T/T). Five
gives 24 px and reads at arm's length. All seven days are still returned by `GET /api/state`.

Weather updates arrive on the 60-second UI tick and use a **partial** panel refresh, throttled to at
most one per 55 seconds — see [epaper-2in13.md](epaper-2in13.md).

## Rendering live data without hardware

```bash
cd sim && LOCATION="Seoul" ./sim.sh
```

This runs the device's exact geocode + fetch + render path against the live API and writes
`sim/shots/home.bmp`. Without `LOCATION` it uses sample data.

## TLS note

`sdkconfig.defaults` still enables a custom certificate bundle left over from a previous data source.
Open-Meteo should validate against IDF's default bundle; that needs confirming on hardware before
the setting is removed. See the "still to verify" section of [epaper-2in13.md](epaper-2in13.md).
