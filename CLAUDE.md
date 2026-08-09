# CLAUDE.md

This repository is a **사주·오미쿠지 (fortune) board**: an ESP32-S3 driving a 2.13" monochrome
e-Paper panel that shows a drawn omikuji (大吉 … 大凶), the day's 일진 (60갑자 day pillar), and the
local weather. It is set up over Wi-Fi from a captive portal or the companion phone app.

## Quick start (do this first)

Activate the ESP-IDF environment — **once per new shell session**:

```bash
. ~/esp/v5.4.3/esp-idf/export.sh      # v5.4.3 is what is installed here
```

Standard workflow after activation, run from the **repository root** (the root *is* the IDF project):

```bash
idf.py set-target esp32s3      # once per project
idf.py build
idf.py -p <PORT> flash monitor # exit with Ctrl+]
```

- `<PORT>`: usually `/dev/cu.usbmodem*` (USB Serial/JTAG) or `/dev/cu.usbserial-*`. `ls /dev/cu.*`.
- If it won't enter flash mode, **hold BOOT while pressing RESET**, release, and retry.

## Verify before claiming anything works

Three layers, all runnable without hardware. Run them in this order — each is faster than the next
and catches a different class of mistake.

```bash
# 1) pure logic — omikuji distribution, 일진 anchor, weather parse, API JSON
cmake -S components/fortune_core/test/host -B /tmp/ft && cmake --build /tmp/ft
/tmp/ft/test_omikuji && /tmp/ft/test_saju && /tmp/ft/test_weather && /tmp/ft/test_api_json

# 2) provisioning pure logic
sh components/provisioning/test/run.sh

# 3) the real UI at the real resolution -> BMP, plus layout/glyph assertions
cd sim && ./sim.sh          # LOCATION="Seoul" ./sim.sh for live weather

# 4) firmware
idf.py build
```

The simulator is not a preview, it is a **test**: it fails the build on a missing glyph or content
running off the 122×250 panel. Look at `sim/shots/*.png` after any UI change.

## Target hardware

| Item | Specification |
|------|------|
| Board | **Seeed XIAO ePaper Display Board EE05** + **XIAO ESP32-S3 Plus** |
| SoC | ESP32-S3 (Xtensa LX7 dual-core), 16MB Flash / 8MB Octal PSRAM |
| Display | 2.13" monochrome e-Paper, **122 × 250**, **SSD1680**, 4-wire SPI + BUSY, 24-pin FPC |
| RTC | none on the EE05 — `board_io` probes 0x51 and falls back to SNTP-only |
| Wireless | WiFi 802.11 b/g/n |
| Buttons | BOOT (GPIO0, on the XIAO), USER (GPIO2, EE05 side button 1) |
| Power | 5V USB-C, optional battery (JST 2.0 + slide switch) |

Wiring is the EE05's fixed routing, kept in `main/user_config.h`: `SCLK=7, MOSI=9, CS=44, DC=10,
RST=38, BUSY=4, POWER=43`; I2C `SDA=5, SCL=6`. Nothing else hardcodes an e-Paper GPIO. Two traps:
**GPIO43 gates the panel's power** (load switch, must be HIGH — `epd_init()` does it), and
**GPIO43/44 are the default UART0 pins**, so the console must stay on USB Serial/JTAG
(`sdkconfig.defaults`). See [docs/pinout.md](docs/pinout.md).

> The panel is 122 px wide but the controller's RAM row is 128 px (16 bytes), so the top 6 bits of
> every row are off-panel padding. The framebuffer is 16 × 250 = 4000 bytes.

## The one thing that makes this board different

**A refresh is not free.** A full e-Paper refresh takes ~2s and flashes; a partial one is ~0.3s and
silent but accumulates ghosting. So drawing and presenting are separate everywhere:

```c
...update widgets...      /* ui_fortune_set_*(), cheap, no panel traffic */
Lvgl_RenderNow();         /* synchronous render -> flush_cb -> framebuffer */
epd_refresh_full();       /* or epd_refresh_partial() */
```

The LVGL flush callback **never** refreshes the panel. Exactly one task (`UiTask` in
`components/user_app/user_app.cpp`) touches LVGL or starts a refresh; everything else posts a
command. Full refresh for a new fortune or a page change, throttled partial for the clock/battery
tick; `epd_refresh_partial()` promotes itself to a full refresh every 10 calls so ghosting still
gets cleared. Details in [docs/epaper-2in13.md](docs/epaper-2in13.md).

## Project structure

```
main/                     app_main: panel + LVGL bring-up, provisioning, task launch
components/
  port_bsp/               SSD1680 driver (epd_panel.c) — the only file that talks to the panel
  app_bsp/                LVGL port (RGB565 draw buffers, binarized in the flush callback)
  fortune_core/           the portable core — compiles identically on device, sim and host tests
    omikuji.c             7-rank weighted draw, injected RNG; verse/table content accessors
    saju.c                60갑자 day pillar + year pillar + day-stem element
    ui_fortune.c          the whole 122x250 UI (page 0 = the 만세력 slip)
    ui_vtext.c            vertical-writing (세로쓰기) text renderer for the verse
    ui_icons.c            vector weather/battery glyphs
    weather_*.c           Open-Meteo geocoding + forecast
    device_api_json.c     the JSON the phone app receives
    fonts/                subset Noto Serif KR faces (OFL) — generated, do not hand-edit
    test/host/            unit tests for all of the above
  provisioning/           SoftAP + captive portal + NVS + SNTP + /api/* onboarding
  device_api/             STA-mode HTTP/JSON control server + mDNS (tickerboard.local)
  board_io/               PCF85063A RTC + battery ADC
  buttons/                USER/BOOT edge events
app/                      React Native companion app (local-network only)
sim/                      desktop simulator — renders the real UI to 122x250 BMPs
third_party/cJSON/        vendored (ESP-IDF v6 dropped cJSON from core)
tools/gen_fonts.py        regenerates components/fortune_core/fonts/
```

## Working rules

- **Never hand-edit `components/fortune_core/fonts/*.c` or hand-maintain a glyph list.** Run
  `python3 tools/gen_fonts.py --download`. It derives the symbol set from `omikuji_messages.h` and
  `saju.c`, so changing a message and forgetting the font — which shows up as a tofu box (□) only
  once the firmware is on the glass — is not possible. All user-visible strings belong in
  `omikuji_messages.h`.
- **Do not change the 일진 anchor constant in `saju.c`.** It is pinned against two independent
  sources and baked into `test_saju.c`. A wrong anchor produces a plausible-looking wrong answer
  every single day.
- **`sdkconfig` holds per-developer values and is gitignored — never commit it.** Wi-Fi passwords
  live in NVS via the portal, never in Kconfig.
- The mDNS hostname `tickerboard`, the AP SSID prefix `"Ticker Board"` and the `/api/info` response
  shape are **hardcoded in the shipped app** (`app/src/lib/discovery.ts`,
  `app/src/app/onboarding/turn-on.tsx`). Renaming them needs an app release, not just a firmware one.
- If anything about the hardware is uncertain, don't guess — check
  [docs/references.md](docs/references.md).

## Documentation

- [docs/epaper-2in13.md](docs/epaper-2in13.md) — the SSD1680 driver, the refresh policy, self-test
- [docs/omikuji.md](docs/omikuji.md) — the seven ranks, weights, and how randomness is injected
- [docs/saju.md](docs/saju.md) — the 일진 calculation, its anchor, and the day-boundary choice
- [docs/pinout.md](docs/pinout.md) — GPIO assignments
- [docs/board-hardware.md](docs/board-hardware.md) — hardware notes
- [docs/weather.md](docs/weather.md) — Open-Meteo, location entry, geocoding flow
- [docs/app-control.md](docs/app-control.md) — the companion-app HTTP/JSON contract
- [docs/simulator.md](docs/simulator.md) — the desktop simulator
- [docs/graphics.md](docs/graphics.md) — 1-bit rendering notes
- [docs/esp-idf-development.md](docs/esp-idf-development.md) — install / build / flash / menuconfig
- [docs/references.md](docs/references.md) — datasheets and upstream sources
