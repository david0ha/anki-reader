# Kanjis Board — companion app

A **local-only** React Native (Expo) app that sets up and controls the **ESP32-S3 Kanjis Board**
over your home Wi-Fi. No cloud, no accounts, no API keys — the app talks **directly** to the board
over plain HTTP on the LAN, and the board never holds a kanjis.ai credential either: it polls one
URL, served by the proxy on your own machine.

It does two things:

1. **Onboarding** over the board's setup Wi-Fi (SoftAP): pick your home Wi-Fi, enter the password
   and the study card URL, and the board reboots onto your network.
2. **Live control** over the LAN: a dashboard that polls the board, shows the card on the glass and
   the state of today's session, moves the board between its five screens, changes the card URL,
   and runs the panel self-test.

The HTTP/JSON contract it implements is documented in [`../docs/app-control.md`](../docs/app-control.md).
`src/lib/esp32.ts` is the TypeScript mirror of that document and the only file in the app that
knows a field name.

> Two names, on purpose. The setup AP and the model string `/api/info` reports are both
> **`Kanjis Board`** — that is what the learner reads, and both come from the same literal in
> `components/provisioning/provisioning.c`. The mDNS host stayed **`obsidianboard.local`**, because
> boards flashed before the rename answer to it and this app resolves it; renaming it would strand
> them for nothing.

## What the dashboard shows

Not the catalog — the *board*. What a companion app is for is the half-metre of air between the
user and a device with three buttons and no keyboard:

- **Status** — how the last poll went, whether what's on the glass is the demo card or stale,
  battery.
- **The card** — the headword, its reading and its first Korean gloss. The phone shows the answer
  whatever screen the board is on: hiding it from the person holding the phone would be a quiz
  nobody asked for.
- **Today's queue** — reviewed today, streak, new left, reviews left, and the position in the queue.
- **On the panel** — which of the five screens is up (문제 / 정답 / 설명 / 댓글 / FSRS), whether the
  answer is revealed, and where the grade dock's cursor is parked. Tapping a screen name drives the
  same nav state a button press does, so the phone and the board cannot disagree about what is up.
- **FSRS** — the scheduler's view of this card: state, next due, reviews, lapses, stability,
  difficulty. `—` where the scheduler has no value yet, which is not the same as zero.
- **Source** — the URL, the last result, when it last succeeded, how often it polls. The three
  failure codes (`transport` / `http_status` / `bad_payload`) each get their own sentence, because
  they send you to three different places.
- **Panel** — the measured full/partial refresh times. The refresh policy for this panel is meant
  to be chosen from measurement, and this is how you read the measurements off a board on a shelf
  instead of holding a serial cable to it.

Grading is deliberately **not** here. The four ratings are a KEY1 press on the board, in front of
the card, and a rating sent from a phone that is not looking at the panel would be graded against
whatever card the proxy happens to be serving.

## Why not Expo Go?

This app **cannot** run in Expo Go. It needs a **native build** (Expo **Dev Client**) for two
reasons:

- It talks to the board over **plain HTTP** on the local network. iOS requires
  `NSAllowsLocalNetworking` + `NSLocalNetworkUsageDescription` and Android requires
  `usesCleartextTraffic` — these are baked into a native build, not available in Expo Go.
- mDNS discovery of `obsidianboard.local` needs the iOS `NSBonjourServices` entitlement.

So you run it with `npx expo run:ios` / `npx expo run:android` (a real device or simulator with a
dev build), not by scanning a QR code into Expo Go.

## Quick start

```bash
cd app
npm install
```

### 1. Develop against the mock (no hardware needed)

A Node mock implements **both** board APIs (provisioning + control):

```bash
npm run mock                      # http://localhost:8080  (PORT=9000 to change)
# in another terminal:
EXPO_PUBLIC_ESP32_BASE_URL=http://localhost:8080 npx expo start
```

`EXPO_PUBLIC_ESP32_BASE_URL` points the app's client at the mock and **skips onboarding** (it
routes straight to the dashboard). Open it in the iOS Simulator (which can reach the host's
`localhost`) or an Android emulator (use `http://10.0.2.2:8080` instead of `localhost`).

The mock is not a stub. Give it a real card URL and it fetches it and normalizes it exactly as
`components/vault_core/kanji_parse.c` would — same UTF-8 byte caps, same clamps, same rejections —
then summarises it as `device_api_json.c` does, including the three distinct failure codes. So the
whole chain the board walks is exercised, with only the panel missing:

```bash
python3 ../tools/mock_kanji_server.py --port 8123   # the card contract, from a fixed payload
curl -X POST http://localhost:8080/api/study -d '{"url":"http://localhost:8123/kanji.json"}'
```

Point it at the real proxy instead to study against your own kanjis.ai account:

```bash
python3 ../tools/kanji_server.py --port 8123
curl -X POST http://localhost:8080/api/study -d '{"url":"http://localhost:8123/kanji.json"}'
```

Provisioning test knobs in the mock: enter password **`wrong`** to exercise the auth-failure path;
set `CONNECT_MS=8000` to slow the connect test.

### 2. Run on a real device against real hardware

```bash
npx expo run:ios      # or: npx expo run:android
```

Then follow the in-app onboarding:

1. **Turn on** the board (USB-C). In your phone's Wi-Fi settings, join the network named
   `Kanjis Board-XXXX`. The app probes `http://192.168.4.1` to confirm it's reachable.
2. **Pick your Wi-Fi** from the scanned list (or "Other…" for a hidden SSID).
3. **Enter the card URL** — or skip it, and the board runs on its built-in demo card. A URL you do
   type is validated against the firmware's own rule before anything is sent, because the board's
   rejection would otherwise arrive on the far side of a ~45s join.
4. **Enter the Wi-Fi password.** The app `POST`s to `/api/provision` and polls `/api/status` until
   the board confirms it joined.
5. **Setup complete** — reconnect your phone to the same home Wi-Fi, then open the dashboard. The
   board is reached at `http://obsidianboard.local` (mDNS) or its IP; you can override the address
   in **Settings** if mDNS isn't available on your network.

## Onboarding → control flow

```
[AP setup]                                    [home LAN control]
turn-on  ─ join "Kanjis Board-XXXX"         dashboard ─ GET /api/state (poll)
wifi-list ─ GET /api/scan                       │           POST /api/{screen,refresh,display/test}
study    ─ (validate locally)                   └─ settings ─ GET /api/info + /api/state
password ─ POST /api/provision (ssid, pass,                   POST /api/study, change host,
           study_url) → poll GET /api/status                  re-onboard
complete ─ save board base URL
```

## Scripts

| command            | what it does                                        |
| ------------------ | --------------------------------------------------- |
| `npm run mock`     | start the dual-API mock board on port 8080          |
| `npm start`        | start the Metro/Expo dev server                     |
| `npm run ios`      | native dev build + run on iOS simulator/device      |
| `npm run android`  | native dev build + run on Android emulator/device   |
| `npm test`         | Jest unit tests (no network — pure logic + client)  |
| `npm run typecheck`| `tsc --noEmit`                                       |
| `npx expo export --platform web` | bundle everything — catches what `tsc` cannot |

The web export is also the cheapest way to actually *look* at the app without a native build: serve
the bundle and the mock board behind one origin (a small proxy forwarding `/api/*` to the board and
`/kanji.json` to the proxy) and the whole dashboard runs in a browser. Doing it that way rather
than adding CORS headers to the mock keeps a browser-only problem out of the repo — React Native
has no CORS.

## Project layout

```
app/
├─ app.json            Expo config (local-networking + cleartext + Bonjour, dark UI)
├─ babel.config.js
├─ jest.setup.js       mocks @react-native-async-storage for tests
├─ scripts/
│  └─ mock-esp32.js    Node mock for BOTH board APIs — really fetches the card URL
└─ src/
   ├─ theme.ts         dark design tokens
   ├─ app/             expo-router file-based routes
   │  ├─ _layout.tsx       providers (DeviceProvider)
   │  ├─ index.tsx         entry → onboarding or dashboard
   │  ├─ dashboard.tsx     live dashboard (polls getState)
   │  ├─ settings.tsx      board info, card URL, host override, re-onboard
   │  └─ onboarding/       turn-on → wifi-list → study → password → complete
   ├─ components/      Screen, Button, Card, Chip, StatTile, InfoRow, …
   ├─ lib/
   │  ├─ esp32.ts          the board client (both API surfaces) + types  ← core
   │  ├─ esp32.test.ts     unit tests with a fake fetch, plus the mock as a running board
   │  ├─ discovery.ts      base-URL normalize/validate/resolve (pure)
   │  ├─ store.ts          AsyncStorage: board base URL + onboarding flag
   │  ├─ device.tsx        app-wide board connection context
   │  ├─ studyurl.ts       card-URL validation mirroring the firmware
   │  └─ format.ts         screen / FSRS / count / age / ms display helpers
   └─ onboarding/      flow.ts (step logic) + OnboardingContext
```

## Local-only by design

There is **no** Supabase / AWS / MQTT / cloud auth anywhere in this app. The only network calls it
makes are direct HTTP requests to the board's IP / `obsidianboard.local`. Wi-Fi credentials and the
card URL live on the board (NVS); the kanjis.ai session lives in the proxy on your own machine; the
app persists only the board's base URL and an onboarding-complete flag in `AsyncStorage`.

Those two AsyncStorage keys are namespaced `obsidianboard.*`, matching the board's own LAN identity.
A phone that once ran the fortune board's app keeps its `tickerboard.*` entries untouched — they
point at different hardware on the same LAN.
