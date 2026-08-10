# CLAUDE.md

This repository is an **Obsidian vault dashboard**: an ESP32-S3 driving a 5.83" monochrome e-Paper
panel that shows vault statistics, the link graph, agent status and the note queue. It polls one
JSON URL on the local network and is set up over Wi-Fi from a captive portal.

## Quick start (do this first)

Activate the ESP-IDF environment — **once per new shell session**:

```bash
. ~/esp/v5.4.3/esp-idf/export.sh      # v5.4.3 is what is installed here
```

Standard workflow after activation, run from the **repository root** (the root *is* the IDF project):

```bash
idf.py set-target esp32s3      # once per checkout
idf.py build
idf.py -p <PORT> flash monitor # exit with Ctrl+]
```

- `<PORT>`: usually `/dev/cu.usbmodem*` (USB Serial/JTAG). `ls /dev/cu.*`.
- If it won't enter flash mode, **hold BOOT while pressing RESET**, release, and retry.

## Verify before claiming anything works

Four layers, three of them runnable without hardware. Run them in this order — each is faster than
the next and catches a different class of mistake.

```bash
# 1) pure logic — the wire format, the fetch layer, the graph layout,
#    the demo snapshot, the API JSON
cmake -S components/vault_core/test/host -B /tmp/vt && cmake --build /tmp/vt
/tmp/vt/test_vault_parse && /tmp/vt/test_vault_service && /tmp/vt/test_graph_layout \
  && /tmp/vt/test_vault_mock && /tmp/vt/test_api_json

# 2) provisioning pure logic
sh components/provisioning/test/run.sh

# 3) the real UI at the real resolution -> PNG, plus layout/glyph assertions
cd sim && ./sim.sh          # VAULT_URL=http://localhost:8123/vault.json ./sim.sh

# 4) firmware
idf.py build
```

The simulator is not a preview, it is a **test**: it fails the build on a missing glyph, on a list
row that rendered nothing, or on a graph node placed outside its canvas. Look at `sim/shots/*.png`
after any UI change.

## Target hardware

| Item | Specification |
|------|------|
| Board | **Seeed XIAO ePaper Display Board EE04** + **XIAO ESP32-S3 Plus** |
| SoC | ESP32-S3 (Xtensa LX7 dual-core), 16MB Flash / 8MB Octal PSRAM |
| Display | 5.83" monochrome e-Paper, **648 × 480**, **UC8179**, 4-wire SPI + BUSY, 24-pin FPC |
| RTC | **none** — the clock is SNTP only |
| Wireless | WiFi 802.11 b/g/n |
| Buttons | KEY0 (GPIO2), KEY1 (GPIO3), KEY2 (GPIO5), BOOT (GPIO0 on the XIAO) |
| Power | 5V USB-C, optional battery (JST 2.0 + slide switch) |

Wiring is the EE04's fixed routing, kept in `main/user_config.h`: `SCLK=7, MOSI=9, CS=44, DC=10,
RST=38, BUSY=4, POWER=43`; battery ADC=1 with its load switch on 6. Nothing else hardcodes a GPIO —
even the buttons are passed to `user_app` as data. Three traps:

- **BUSY is active LOW on the UC8179.** The panel is idle when the pin is HIGH. This is the inverse
  of the SSD1680 this driver started as, and it fails *silently*: every wait returns instantly and
  every refresh comes out torn, with nothing in the log to say why.
- **GPIO43 gates the panel's power** (load switch, must be HIGH — `epd_init()` does it).
- **GPIO43/44 are the default UART0 pins**, so the console must stay on USB Serial/JTAG
  (`sdkconfig.defaults`).

There is **no I2C bus**: on the EE04, GPIO5 and GPIO6 are KEY2 and the battery divider's enable.
See [docs/pinout.md](docs/pinout.md).

> The panel is 648 px wide, which is a multiple of 8, so a framebuffer row is exactly 81 bytes with
> no padding. The framebuffer is 81 × 480 = 38,880 bytes.

## The two things that make this board different

**1. A refresh is not free.** A full refresh takes seconds and flashes the whole panel. So drawing
and presenting are separate everywhere:

```c
...update widgets...      /* ui_vault_set_*(), cheap, no panel traffic */
Lvgl_RenderNow();         /* synchronous render -> flush_cb -> framebuffer */
epd_refresh_full();       /* or epd_refresh_partial_area(...) */
```

The LVGL flush callback **never** refreshes the panel. Exactly one task (`UiTask` in
`components/user_app/user_app.cpp`) touches LVGL or starts a refresh; everything else posts a
command. Full refresh for new data or a page change; a windowed partial only for the header clock,
throttled to one every five minutes and promoted to a full refresh every sixth time.

**2. A poll that changes nothing must not touch the panel.** `vault_hash()` fingerprints everything
that reaches the glass and `VaultTask` compares before it notifies `UiTask`. On a device that polls
every five minutes forever, this is the difference between a silent dashboard and one that flashes
at nobody all day. Details in [docs/epaper-5in83.md](docs/epaper-5in83.md).

## Project structure

```
main/                     app_main: panel + LVGL bring-up, provisioning, task launch
components/
  port_bsp/               UC8179 driver (epd_panel.c) — the only file that talks to the panel
  app_bsp/                LVGL port (RGB565 draw buffers, binarized in the flush callback)
  vault_core/             the portable core — compiles identically on device, sim and host tests
    vault_model.c         the snapshot struct + UTF-8-safe copy + content fingerprint
    vault_parse.c         wire JSON -> model, clamping every field
    vault_mock.c          the built-in demo snapshot (shown when no URL is set)
    vault_service.c       one fetch: http_get + parse
    ui_vault.c            header, footer, overlay, page routing
    ui_page_{stats,graph,agents,notes}.c    one file per page
    ui_graph.c            deterministic concentric-ring layout, integer trigonometry
    ui_common.c           the shared shapes; ui_internal.h holds the layout grid
    ui_icons.c            vector glyphs
    device_api_json.c     the JSON the companion app receives
    fonts/                full 완성형 Noto Sans KR faces (OFL) — generated, do not hand-edit
    test/host/            unit tests for all of the above
  provisioning/           SoftAP + captive portal + NVS + SNTP + /api/* onboarding
  device_api/             STA-mode HTTP/JSON control server + mDNS (obsidianboard.local)
  board_io/               battery ADC
  buttons/                KEY0/1/2 + BOOT edge events
app/                      React Native companion app — setup + control over the LAN
sim/                      desktop simulator — renders the real UI to 648x480 and asserts on it
third_party/cJSON/        vendored (ESP-IDF v6 dropped cJSON from core)
tools/
  mock_vault_server.py    the vault contract, as a runnable reference server
  gen_fonts.py            regenerates components/vault_core/fonts/
```

## Working rules

- **Never hand-edit `components/vault_core/fonts/*.c`.** Run `python3 tools/gen_fonts.py --download`.
  Both faces carry the whole 완성형 set (2350 syllables + ASCII + `S_DATA_PUNCT`) because half the
  strings on this board arrive over the network and cannot be subset. The 2350 are derived from
  Python's EUC-KR codec, not tabulated. **All fixed user-visible strings belong in `ui_strings.h`** —
  that is where the generator reads the punctuation from, and where the simulator's coverage check
  reads them from.
- **`vault_mock.c` and `tools/mock_vault_server.py` must stay identical.** `test_vault_mock.c`
  asserts it by parsing the server's committed fixture and comparing fingerprints. Change one and
  the test tells you which field diverged; then run
  `python3 tools/mock_vault_server.py --write-fixture`.
- **A rejected payload must leave the previous snapshot alone.** `vault_parse()` writes `*out` only
  on success. Blanking the panel is the one failure a user actually notices, and a stale dashboard
  badged 오래됨 beats an empty one.
- **The graph layout must stay deterministic and libm-free.** `sin()` agrees between x86 and Xtensa
  only to within an ulp, which is enough to move a node one pixel and fail a screenshot test for a
  reason unrelated to the layout. Hence the integer sine table in `ui_graph.c`.
- **Labels get a fixed height, not just a width.** `ui_lab_w()` does this; bypassing it makes LVGL
  auto-size the height and *wrap* instead of ellipsizing, and the second line lands on the row below.
- **`sdkconfig` holds per-developer values and is gitignored — never commit it.** Wi-Fi passwords
  live in NVS via the portal, never in Kconfig.
- The mDNS hostname is `obsidianboard` and the AP prefix `"Obsidian Board"` — deliberately **not**
  the `tickerboard` / `"Ticker Board"` of the project this forked from, whose shipped app resolves
  those names.
- If anything about the hardware is uncertain, don't guess — check
  [docs/references.md](docs/references.md).

## Documentation

- [docs/vault-contract.md](docs/vault-contract.md) — the JSON the device polls, and how it fails
- [docs/pages.md](docs/pages.md) — the four pages, the layout grid, the font decision
- [docs/epaper-5in83.md](docs/epaper-5in83.md) — the UC8179 driver, the refresh policy, the self-test
- [docs/pinout.md](docs/pinout.md) — GPIO assignments
- [docs/board-hardware.md](docs/board-hardware.md) — hardware notes
- [docs/app-control.md](docs/app-control.md) — the companion-app HTTP/JSON contract
- [docs/simulator.md](docs/simulator.md) — the desktop simulator
- [docs/graphics.md](docs/graphics.md) — 1-bit rendering notes
- [docs/esp-idf-development.md](docs/esp-idf-development.md) — install / build / flash / menuconfig
- [docs/references.md](docs/references.md) — datasheets and upstream sources
- [docs/specs/](docs/specs/) — the design this was built from, including what was deliberately
  deferred
