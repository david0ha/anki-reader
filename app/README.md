# Ticker Board — companion app

A **local-only** React Native (Expo) app that sets up and controls the **ESP32-S3 Ticker Board**
over your home Wi-Fi. No cloud, no accounts, no API keys — the app talks **directly** to the
device over plain HTTP on the LAN.

It does two things:

1. **Onboarding** over the board's setup Wi-Fi (SoftAP): pick your home Wi-Fi, enter the
   password + an optional stock watchlist, and the board reboots onto your network.
2. **Live control** over the LAN: a dashboard that polls the device, shows the current
   ticker / quotes / sensors / battery, and lets you switch the on-screen ticker and page,
   toggle the economic-calendar overlay, edit the watchlist, and force a refresh.

The HTTP/JSON contract it implements is documented in [`../docs/app-control.md`](../docs/app-control.md).

## Why not Expo Go?

This app **cannot** run in Expo Go. It needs a **native build** (Expo **Dev Client**) for two
reasons:

- It talks to the device over **plain HTTP** on the local network. iOS requires
  `NSAllowsLocalNetworking` + `NSLocalNetworkUsageDescription` and Android requires
  `usesCleartextTraffic` — these are baked into a native build, not available in Expo Go.
- mDNS discovery of `tickerboard.local` needs the iOS `NSBonjourServices` entitlement.

So you run it with `npx expo run:ios` / `npx expo run:android` (a real device or simulator with a
dev build), not by scanning a QR code into Expo Go.

## Quick start

```bash
cd app
npm install
```

### 1. Develop against the mock (no hardware needed)

A Node mock implements **both** device APIs (provisioning + stock control) with drifting fake
quotes, so the whole dashboard works end-to-end:

```bash
npm run mock                      # http://localhost:8080  (PORT=9000 to change)
# in another terminal:
EXPO_PUBLIC_ESP32_BASE_URL=http://localhost:8080 npx expo start
```

`EXPO_PUBLIC_ESP32_BASE_URL` points the app's device client at the mock and **skips onboarding**
(it routes straight to the dashboard). Open it in the iOS Simulator (which can reach the host's
`localhost`) or an Android emulator (use `http://10.0.2.2:8080` instead of `localhost`).

Provisioning test knobs in the mock: enter password **`wrong`** to exercise the auth-failure path;
set `CONNECT_MS=8000` to slow the connect test.

### 2. Run on a real device against real hardware

```bash
npx expo run:ios      # or: npx expo run:android
```

Then follow the in-app onboarding:

1. **Turn on** the board (USB-C). In your phone's Wi-Fi settings, join the network named
   `Ticker Board-XXXX`. The app probes `http://192.168.4.1` to confirm it's reachable.
2. **Pick your Wi-Fi** from the scanned list (or "Other…" for a hidden SSID).
3. **Enter the password** and an optional **watchlist** (e.g. `AAPL, TSLA, MSFT`). The app
   `POST`s to `/api/provision` and polls `/api/status` until the board confirms it joined.
4. **Setup complete** — reconnect your phone to the same home Wi-Fi, then open the dashboard.
   The board is reached at `http://tickerboard.local` (mDNS) or its IP; you can override the
   address in **Settings** if mDNS isn't available on your network.

## Onboarding → control flow

```
[AP setup]                                   [home LAN control]
turn-on  ─ join "Ticker Board-XXXX"          dashboard ─ GET /api/stock/state (poll)
wifi-list ─ GET /api/scan                      │           POST /api/stock/{select,page,econ,refresh}
password ─ POST /api/provision (ssid,pass,     ├─ watchlist ─ POST /api/stock/watchlist
           tickers) → poll GET /api/status     └─ settings  ─ GET /api/info, change host, re-onboard
complete ─ save device base URL
```

## Scripts

| command            | what it does                                        |
| ------------------ | --------------------------------------------------- |
| `npm run mock`     | start the dual-API mock device on port 8080         |
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
│  └─ mock-esp32.js    Node mock for BOTH device APIs (drifting quotes + mutable state)
└─ src/
   ├─ theme.ts         dark design tokens (up/down market colors)
   ├─ app/             expo-router file-based routes
   │  ├─ _layout.tsx       providers (DeviceProvider)
   │  ├─ index.tsx         entry → onboarding or dashboard
   │  ├─ dashboard.tsx     live control dashboard (polls getState)
   │  ├─ watchlist.tsx     add/remove/reorder symbols → setWatchlist
   │  ├─ settings.tsx      device info, host override, re-onboard
   │  └─ onboarding/       turn-on → wifi-list → password → complete
   ├─ components/      Screen, Button, Card, Chip, SegmentedControl, TickerRow, …
   ├─ lib/
   │  ├─ esp32.ts          the device client (both API surfaces) + types  ← core
   │  ├─ esp32.test.ts     thorough unit tests with a fake fetch
   │  ├─ discovery.ts      base-URL normalize/validate/resolve (pure)
   │  ├─ store.ts          AsyncStorage: device base URL + onboarding flag
   │  ├─ device.tsx        app-wide device connection context
   │  ├─ watchlist.ts      symbol validation mirroring the firmware
   │  └─ format.ts         price/percent/age display helpers
   └─ onboarding/      flow.ts (step logic) + OnboardingContext
```

## Local-only by design

There is **no** Supabase / AWS / MQTT / cloud auth anywhere in this app. The only network calls it
makes are direct HTTP requests to the device's IP / `tickerboard.local`. Credentials and the
watchlist live on the device (NVS); the app persists only the device's base URL and an
onboarding-complete flag in `AsyncStorage`.
