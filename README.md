# 사주·오미쿠지 보드 — a fortune slip on e-Paper

An ESP32-S3 and a 2.13" e-Paper panel that draws you an **오미쿠지** (大吉 … 大凶), shows the day's
**일진** (60갑자 day pillar), and the local weather. Set it up from your phone; after that it sits
there and uses almost no power, because e-Paper only draws current while it changes.

```
╔══════════════╗   ┌──────────────┐
║  今 日 運 勢  ║   │    16:18     │
║ 2026. 8. 9(일)║   │ 2026.08.09 Sun│
║┌병┐      ┌을┐ ║   ├──────────────┤
║│오│ 中吉 │묘│ ║   │  ☀   28°     │
║└년┘      └일┘ ║   │  Seoul, KR   │
║ ─────◆─────  ║   ├──────────────┤
║  즐과 소기 뻗나║   │ FR SA SU MO TU│
║  기정 식다 어무║   │ ☁  ☀  ☁  ☂  ☀ │
║ …세로쓰기(6열) ║   │ 22 24 20 17 21│
║ ⊕吉           ║   ├──────────────┤
║┌財運┬事業┬──┐║   │  오늘의 일진  │
║└안정┴순항┴──┘║   │  乙卯 을묘    │
╚══════════════╝   ├──────────────┤
                   │ 16:18   84%  │
   USER = 새로 뽑기 └──────────────┘
   BOOT = 페이지
```

## What it does

- **오미쿠지, laid out as a 만세력 slip** — seven ranks with a weighted, configurable distribution;
  a new one at boot, at local midnight, on a button press, or from the app. The slip carries the
  grade in large Hanja, the year and day pillars in vertical side boxes, a vertical-writing verse
  ([흐름] from the day's element, [해석]/[조언] from the draw), a tilted 吉/凶 seal, and a
  財運/事業/對人/健康 table — all inside a double frame, all in a serif face.
  [docs/omikuji.md](docs/omikuji.md)
- **오늘의 일진** — the day's 60갑자 pillar, computed from the date. The anchor is pinned against two
  independent sources and locked down by tests, because a wrong anchor is silently wrong every day.
  [docs/saju.md](docs/saju.md)
- **날씨** — current conditions and a forecast strip from Open-Meteo. No API key, no account, no
  cloud service. You type a city name; the device geocodes it and shows you what it matched.
  [docs/weather.md](docs/weather.md)
- **Wi-Fi setup** — a captive portal on the device's own access point, or the companion app.
  Credentials live in NVS, never in the source.
- **배터리** — voltage and percentage in the footer.

## Hardware

ESP32-S3 (16 MB flash, 8 MB PSRAM) + a 2.13" **SSD1680** e-Paper module, 122 × 250, mono. A
PCF85063A RTC keeps the clock — and therefore the day pillar — correct across power loss and before
Wi-Fi comes up. Optional Li-ion cell.

Wiring is yours; the defaults are in `main/user_config.h` and documented in
[docs/pinout.md](docs/pinout.md). Nothing else in the firmware hardcodes a GPIO.

## Build

```bash
. ~/esp/v5.4.3/esp-idf/export.sh   # once per shell
idf.py set-target esp32s3          # once per checkout
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```

## Develop without the hardware

Everything above the panel driver runs on your desktop, and the interesting parts are tested there:

```bash
# pure logic — fortune distribution, the 일진 anchor, weather parsing, the app's JSON
cmake -S components/fortune_core/test/host -B /tmp/ft && cmake --build /tmp/ft
/tmp/ft/test_omikuji && /tmp/ft/test_saju && /tmp/ft/test_weather && /tmp/ft/test_api_json

sh components/provisioning/test/run.sh

# the real UI, at the real 122x250, to BMP
cd sim && ./sim.sh                 # LOCATION="Seoul" ./sim.sh for live weather
```

The simulator compiles the same `ui_fortune.c` and the same fonts the firmware does, and it is a
**test**, not a preview: it fails on a missing glyph or on content running off the panel. Look at
`sim/shots/` after any UI change. [docs/simulator.md](docs/simulator.md)

## The design constraint worth knowing about

A full e-Paper refresh takes about two seconds and flashes the panel; a partial one is quick and
silent but leaves ghosting behind. So drawing and presenting are separate everywhere in this
codebase — the LVGL flush callback fills a framebuffer and never touches the panel, and exactly one
task decides when a change is worth two seconds. [docs/epaper-2in13.md](docs/epaper-2in13.md)

## Companion app

`app/` is a local-network-only React Native app: it finds the device over mDNS at
`tickerboard.local`, walks you through Wi-Fi setup, and can draw a fortune or change the city. No
auth, no TLS, no cloud — see [docs/app-control.md](docs/app-control.md) for the contract and the
scope decision behind it.

**The app has not yet been updated for this firmware.** Onboarding and discovery work; its dashboard
screens still call the removed stock endpoints and will 404.

## Repository

```
main/                app_main — panel + LVGL bring-up, provisioning, task launch
components/
  port_bsp/          SSD1680 driver — the only file that talks to the panel
  app_bsp/           LVGL port
  fortune_core/      omikuji, saju, UI, icons, weather, API JSON, fonts, host tests
  provisioning/      SoftAP + captive portal + NVS + SNTP
  device_api/        HTTP/JSON control server + mDNS
  board_io/          RTC + battery
  buttons/           USER/BOOT
app/                 React Native companion app
sim/                 desktop simulator
tools/gen_fonts.py   regenerates the subset CJK fonts from the source strings
```

## Licence & credits

Code: see [LICENSE](LICENSE). Fonts: **Noto Serif KR** under the SIL Open Font License 1.1
(`components/fortune_core/fonts/OFL.txt`). The e-Paper command sequence is transcribed from
[Waveshare's reference driver](https://github.com/waveshareteam/e-Paper). Weather from
[Open-Meteo](https://open-meteo.com).
