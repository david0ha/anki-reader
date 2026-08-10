# Obsidian Board — companion app

A **local-only** React Native (Expo) app that sets up and controls the **ESP32-S3 Obsidian Board**
over your home Wi-Fi. No cloud, no accounts, no API keys — the app talks **directly** to the board
over plain HTTP on the LAN.

It does two things:

1. **Onboarding** over the board's setup Wi-Fi (SoftAP): pick your home Wi-Fi, enter the password
   and the vault snapshot URL, and the board reboots onto your network.
2. **Live control** over the LAN: a dashboard that polls the board, shows what it is displaying and
   how its last poll went, switches the page on the panel, changes the snapshot URL, and runs the
   panel self-test.

The HTTP/JSON contract it implements is documented in [`../docs/app-control.md`](../docs/app-control.md).
`src/lib/esp32.ts` is the TypeScript mirror of that document and the only file in the app that
knows a field name.

## What the dashboard shows

Not the vault — the *board*. The full snapshot is on the URL the board polls, which the phone can
open too; what a companion app is for is the half-metre of air between the user and a device with
no keyboard:

- **Status** — how the last poll went, whether what's on the glass is demo data or stale, battery.
- **Counters** — notes, links, orphans, tags, with link density and orphan rate derived.
- **Agents & queue** — how many agents are running, and how deep the note/inbox queues are.
- **On the panel** — which page is showing (with the board's own Korean title for it), and a
  switcher. A page change is a full refresh of a 5.83" panel, so the control stays on the page you
  asked for until the board confirms it, rather than snapping back and looking like a lost tap.
- **Source** — the URL, the last result, when it last succeeded, how often it polls. The three
  failure codes (`transport` / `http_status` / `bad_payload`) each get their own sentence, because
  they send you to three different places.
- **Panel** — the measured full/partial refresh times. The refresh policy for this panel is meant
  to be chosen from measurement, and this is how you read the measurements off a board on a shelf
  instead of holding a serial cable to it.
- **Quick memo** — type something and it lands in the vault's inbox, so the queue on the panel is
  somewhere you can add to from the sofa. Saving also asks the board to poll, which is what makes
  the memo appear on the glass while you are still looking at it.

The memo box is the one thing here that does **not** talk to the board. `POST /capture` is served
by whatever is producing the snapshot — `tools/vault_server.py --allow-capture` does, most
producers will not — so the app derives its address from the snapshot URL the board reports, and
treats "this server doesn't do capture" as an ordinary answer with its own sentence. The boundary
is why it lives in `src/lib/capture.ts` and not in `src/lib/esp32.ts`.

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

The mock is not a stub. Give it a real snapshot URL and it fetches it and summarises it exactly as
the firmware would, including the three distinct failure codes — so the whole chain the board walks
is exercised, with only the panel missing:

```bash
python3 ../tools/mock_vault_server.py --port 8123     # the vault contract, as a server
curl -X POST http://localhost:8080/api/vault -d '{"url":"http://localhost:8123/vault.json"}'
```

To exercise the memo box too, serve a real (or throwaway) vault with capture enabled instead:

```bash
python3 ../tools/vault_server.py ~/some/vault --port 8123 --allow-capture
curl -X POST http://localhost:8080/api/vault -d '{"url":"http://localhost:8123/vault.json"}'
```

Provisioning test knobs in the mock: enter password **`wrong`** to exercise the auth-failure path;
set `CONNECT_MS=8000` to slow the connect test.

### 2. Run on a real device against real hardware

```bash
npx expo run:ios      # or: npx expo run:android
```

Then follow the in-app onboarding:

1. **Turn on** the board (USB-C). In your phone's Wi-Fi settings, join the network named
   `Obsidian Board-XXXX`. The app probes `http://192.168.4.1` to confirm it's reachable.
2. **Pick your Wi-Fi** from the scanned list (or "Other…" for a hidden SSID).
3. **Enter the snapshot URL** — or skip it, and the board runs on its built-in demo data. A URL you
   do type is validated against the firmware's own rule before anything is sent, because the
   board's rejection would otherwise arrive on the far side of a ~45s join.
4. **Enter the Wi-Fi password.** The app `POST`s to `/api/provision` and polls `/api/status` until
   the board confirms it joined.
5. **Setup complete** — reconnect your phone to the same home Wi-Fi, then open the dashboard. The
   board is reached at `http://obsidianboard.local` (mDNS) or its IP; you can override the address
   in **Settings** if mDNS isn't available on your network.

## Onboarding → control flow

```
[AP setup]                                    [home LAN control]
turn-on  ─ join "Obsidian Board-XXXX"         dashboard ─ GET /api/state (poll)
wifi-list ─ GET /api/scan                       │           POST /api/{page,refresh,display/test}
vault    ─ (validate locally)                   └─ settings ─ GET /api/info + /api/state
password ─ POST /api/provision (ssid, pass,                   POST /api/vault, change host,
           vault_url) → poll GET /api/status                  re-onboard
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

## Project layout

```
app/
├─ app.json            Expo config (local-networking + cleartext + Bonjour, dark UI)
├─ babel.config.js
├─ jest.setup.js       mocks @react-native-async-storage for tests
├─ scripts/
│  └─ mock-esp32.js    Node mock for BOTH board APIs — really fetches the snapshot URL
└─ src/
   ├─ theme.ts         dark design tokens
   ├─ app/             expo-router file-based routes
   │  ├─ _layout.tsx       providers (DeviceProvider)
   │  ├─ index.tsx         entry → onboarding or dashboard
   │  ├─ dashboard.tsx     live dashboard (polls getState)
   │  ├─ settings.tsx      board info, snapshot URL, host override, re-onboard
   │  └─ onboarding/       turn-on → wifi-list → vault → password → complete
   ├─ components/      Screen, Button, Card, Chip, SegmentedControl, StatTile, InfoRow, …
   ├─ lib/
   │  ├─ esp32.ts          the board client (both API surfaces) + types  ← core
   │  ├─ esp32.test.ts     thorough unit tests with a fake fetch
   │  ├─ discovery.ts      base-URL normalize/validate/resolve (pure)
   │  ├─ store.ts          AsyncStorage: board base URL + onboarding flag
   │  ├─ device.tsx        app-wide board connection context
   │  ├─ vaulturl.ts       snapshot-URL validation mirroring the firmware
   │  ├─ capture.ts        writing a memo — to the vault server, NOT the board
   │  └─ format.ts         count / age / ms / fetch-result display helpers
   └─ onboarding/      flow.ts (step logic) + OnboardingContext
```

## Local-only by design

There is **no** Supabase / AWS / MQTT / cloud auth anywhere in this app. The only network calls it
makes are direct HTTP requests to the board's IP / `obsidianboard.local`. Wi-Fi credentials and the
snapshot URL live on the board (NVS); the app persists only the board's base URL and an
onboarding-complete flag in `AsyncStorage`.

Those two AsyncStorage keys are namespaced `obsidianboard.*`. A phone that once ran the fortune
board's app keeps its `tickerboard.*` entries untouched — they point at different hardware on the
same LAN.
