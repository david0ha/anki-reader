# Companion app ↔ firmware contract

The React Native app in `app/` talks to the device over plain HTTP/JSON on the local network. There
are two phases with two different servers.

**Local-network only.** No auth, no TLS, no cloud account, no outbound connection to anything but
Open-Meteo. That is a deliberate scope choice: the device holds no credentials worth stealing after
onboarding, and the only actions are "show a different fortune" and "change the city". Anyone on
your LAN can do those. If that is not acceptable for your network, do not expose port 80.

---

## Phase 1 — onboarding (SoftAP, `components/provisioning`)

The device broadcasts `Ticker Board-XXXX` (XXXX = MAC suffix) at `192.168.4.1`. The phone joins it,
and the app drives:

| Route | Body / response |
|---|---|
| `GET /api/info` | `{ deviceId, model, apSsid }` |
| `GET /api/scan` | `{ networks: [{ ssid, rssi, secure }] }` |
| `POST /api/provision` | form-urlencoded `ssid`, `ssid_manual`, `password`, `location` → `202` |
| `GET /api/status` | `{ state: idle\|connecting\|connected\|failed, ssid?, reason? }` |

`POST /api/provision` replies **202 before** the connect test starts, because the test hops channels
and the phone would otherwise lose the response mid-flight. The app polls `/api/status` for the
outcome.

> **The shipped app still POSTs `tickers`, `finnhub_key`, `fmp_key` and `econ_url`.** Unknown form
> fields are simply not read, so they are discarded and onboarding works unchanged against an
> un-updated app. Do not "clean this up" by rejecting unknown fields.

### The single-radio caveat

The ESP32-S3 has one radio. While the connect test runs, the SoftAP hops to the target network's
channel and the phone's association may drop. The firmware therefore treats a lost poll after a
successful association as **"connected (presumed)"** rather than a failure. The captive-portal
auto-popup was removed for the same reason — it raced the connect test.

---

## Phase 2 — control (STA, `components/device_api`)

Once on the home network the device serves port 80 and advertises `_http._tcp` over mDNS as
**`tickerboard.local`**.

| Route | Body | Effect |
|---|---|---|
| `GET /api/info` | — | `{ deviceId, model, fw, ip }` |
| `GET /api/state` | — | full snapshot, below |
| `POST /api/fortune/draw` | — | draw a new omikuji (full refresh, ~2s) |
| `POST /api/page` | `{ page: 0\|1 }` | 0 = 오미쿠지, 1 = 홈 |
| `POST /api/location` | `{ location: "Seoul" }` | persisted to NVS, re-geocoded live; `""` hides weather |
| `POST /api/display/test` | — | e-Paper self-test sweep (~10s) |

Errors are `400` with `{"ok":false,"error":"<code>"}` — `bad_json`, `too_large`, `read_error`,
`page_range`, `location_too_long`, `busy`. Success is `{"ok":true}`.

Writes **queue** a command for the UI task and return immediately; they do not wait for the panel.
`busy` means the queue was full, not that the device refused.

### `GET /api/state`

```json
{
  "deviceId": "1A2B", "model": "Ticker Board", "fw": "0.2.0", "ip": "192.168.0.42",
  "page": 1,
  "partialChain": 3,
  "fortune": { "valid": true, "rank": 6, "hanja": "大吉", "hangul": "대길",
               "message": "바라던 일이\n이루어집니다" },
  "iljin":   { "index": 50, "hanja": "甲寅", "hangul": "갑인" },
  "weather": { "valid": true, "kind": 1, "tempC": 28, "city": "Seoul, KR", "location": "Seoul",
               "forecast": [ { "dow": "FRI", "kind": 1, "lo": 15, "hi": 22 } ] },
  "battery": { "valid": true, "percent": 84, "millivolts": 4012 }
}
```

Notes:

- **Every number is an integer.** Temperatures are whole degrees, the battery is millivolts. The
  previous revision of this API carried doubles and had to defend against NaN and against `"%.2f"`
  of a huge magnitude truncating on the decimal point into JSON that strict parsers reject. That
  class of bug is designed out here rather than guarded against.
- `kind` is `wx_kind_t`: 0 sun, 1 partly, 2 cloud, 3 rain.
- `rank` is 0 (大凶) … 6 (大吉), so it sorts as a luck score.
- `forecast` carries up to 7 days; the panel draws the first 5 (24 px columns — seven would be 17 px
  and illegible).
- `partialChain` is how many partial refreshes have run since the last full one, out of 10. It makes
  the refresh policy observable without a serial cable.
- Korean passes through as UTF-8, not `\u` escapes, and survives the round trip byte for byte —
  including the `\n` inside `message`, which is escaped as `\\n`.

The serializer is pure and host-tested (`test_api_json.c`): every field name, the UTF-8 handling,
clamping of an out-of-range `forecast_count`, and **every** truncation point — a buffer one byte
short at any stage must return −1 and leave an empty string, never a half-written document.

---

## What the shipped app expects that firmware must not change

Three things are hardcoded on the app side. Changing any of them needs an app release, not just a
firmware one:

| Thing | Where |
|---|---|
| mDNS hostname `tickerboard.local` | `app/src/lib/discovery.ts:12` |
| AP SSID prefix `Ticker Board-XXXX` | `app/src/app/onboarding/turn-on.tsx:70` |
| `GET /api/info` field names, incl. `ip` | `app/src/lib/discovery.ts` (probes candidates, picks by `ip`) |

## Known breakage

`app/` was **not** updated in the saju/omikuji conversion. Onboarding and device discovery work
unchanged. The app's **dashboard and watchlist screens will 404** — they call `/api/stock/*`, which
no longer exists. Fixing that is an app-side task.
