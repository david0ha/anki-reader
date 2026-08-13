# Obsidian Board

An always-on daily tarot reading generated from an Obsidian vault. A 5.83" monochrome panel on a
Seeed EE04 carrier and a XIAO ESP32-S3 Plus polls one URL on your LAN. A native-pixel Rider–Waite–
Smith card fills the left 96% of the glass height; a cut-corner reading plate on the right carries
the day's headline, flow, caution and action. There is no application header or footer chrome.

It is set up over Wi-Fi from a captive portal. With no URL, the board renders an explicitly labeled
built-in design preview, so display bring-up still works offline. A current dated reading comes from
the reference producer or another schema-3 source.

The preview from `sim/artwork_sim.sh` is not a mockup. It uses the exact LVGL
composition and binarization used by the device — see [the simulator](docs/simulator.md).

## Quick start

```bash
. ~/esp/v5.4.3/esp-idf/export.sh    # once per shell

idf.py set-target esp32s3           # once per checkout
idf.py build
./tools/flash.sh                    # finds the port, flashes, monitors
```

Then join the `Obsidian Board-XXXX` Wi-Fi network the board raises, and give it your Wi-Fi
credentials and — optionally — a snapshot URL.

Doing this for the **first** time on a given board, follow [docs/bring-up.md](docs/bring-up.md)
instead: the three things most likely to be wrong on a first power-on all look like a blank screen,
and the boot log is the only place they are told apart.

To feed it your actual vault, run the scanner on the machine that holds it:

```bash
python3 tools/vault_server.py ~/Documents/MyVault   # http://<you>:8123/vault.json
```

It is read-only: it opens `.md` files and writes nothing. To try the plumbing without a vault,
`python3 tools/mock_vault_server.py` serves the same contract from a fixed payload.

Then point the board at it, from the portal or over the network:

```bash
curl -X POST http://obsidianboard.local/api/vault \
     -d '{"url":"http://mymac.local:8123/vault.json"}'
```

Anything that serves that JSON works — a plugin inside Obsidian, a cron job, a shell script. The
device cannot tell the difference. The format is [documented and tested](docs/vault-contract.md).

### Making it two-way

Two optional pieces turn the dashboard from something you watch into something you use:

```bash
# a memo goes into the vault's inbox (the tarot composition stays focused)
python3 tools/vault_server.py ~/Documents/MyVault --allow-capture
curl -X POST http://localhost:8123/capture -d 'ring the dentist'

# a script reports what it is doing, enriching the source snapshot
python3 tools/agent_status.py --file ~/agents.json set indexer running --progress 40
python3 tools/vault_server.py ~/Documents/MyVault --agents ~/agents.json
```

Capture is off unless asked for: it is an unauthenticated LAN service that creates files in your
notes. Neither piece is part of the device contract — the firmware has never heard of either. See
[docs/vault-contract.md](docs/vault-contract.md#optional-capture).

## Verify before claiming anything works

Four layers, three of which need no hardware. Each is faster than the next and catches a different
class of mistake.

```bash
# 1) pure logic — the wire format, fetch layer, tarot layout,
#    demo snapshot and API JSON
cmake -S components/vault_core/test/host -B /tmp/vt && cmake --build /tmp/vt
/tmp/vt/test_vault_parse && /tmp/vt/test_vault_service && /tmp/vt/test_graph_layout \
  && /tmp/vt/test_artwork_layout && /tmp/vt/test_vault_mock && /tmp/vt/test_api_json

# 2) provisioning pure logic, and the vault scanner
sh components/provisioning/test/run.sh
sh components/user_app/test/run.sh
python3 tools/test_vault_server.py
python3 tools/test_tarot_assets.py

# 3) the real native 648x480 artwork -> PNG, plus layout and glyph assertions
cd sim && ./artwork_sim.sh

# 4) firmware
idf.py build
```

The simulator is not a preview, it is a **test**: it fails on a missing glyph or on a list row that
rendered nothing. Look at `sim/shots/*.png` after any UI change.

## Hardware

| Item | Specification |
|------|------|
| Board | **Seeed XIAO ePaper Display Board EE04** + **XIAO ESP32-S3 Plus** |
| SoC | ESP32-S3 (Xtensa LX7 dual-core), 16 MB Flash / 8 MB Octal PSRAM |
| Display | 5.83" monochrome e-Paper, **648 × 480**, **UC8179**, 4-wire SPI + BUSY, 24-pin FPC |
| RTC | none — the clock is SNTP only |
| Buttons | KEY0/1/2 (GPIO2/3/5) on the carrier, BOOT (GPIO0) on the XIAO |
| Power | 5 V USB-C, optional Li-ion (JST 2.0 + slide switch) |

Wiring is the EE04's fixed routing, kept in `main/user_config.h`. Three traps: **BUSY is active
LOW** on the UC8179 (the inverse of most SSD-family panels, and it fails silently), **GPIO43 gates
the panel's power**, and **GPIO43/44 are the default UART0 pins** so the console must stay on USB
Serial/JTAG. See [docs/pinout.md](docs/pinout.md).

## Controls

| | |
|---|---|
| KEY0 | keep current artwork |
| KEY1 | poll the vault source now |
| KEY2 | tap → keep artwork · **hold 5 s → reboot into Wi-Fi setup** |
| BOOT | keep current artwork |

## The thing that makes this board different

**A refresh is not free.** A full refresh of this panel takes seconds and flashes the whole screen.
So drawing and presenting are separate everywhere:

```c
...update widgets...      /* ui_artwork_set_data(), cheap, no panel traffic */
Lvgl_RenderNow();         /* synchronous render -> flush_cb -> framebuffer */
epd_refresh_full();       /* or epd_refresh_partial_area(...) */
```

Exactly one task (`UiTask`) touches LVGL or starts a refresh; everything else posts a command.

And the rule that matters most for a device that mostly sits still: **a poll that returns unchanged
content does not touch the panel at all.** Every snapshot is fingerprinted, and the poller compares
before it notifies. Details in [docs/epaper-5in83.md](docs/epaper-5in83.md).

## Project structure

```
main/                   app_main: panel + LVGL bring-up, provisioning, task launch
components/
  port_bsp/             UC8179 driver (epd_panel.c) — the only file that talks to the panel
  app_bsp/              LVGL port (RGB565 draw buffers, binarized in the flush callback)
  vault_core/           the portable core — compiles identically on device, sim and host tests
    vault_model.c       the snapshot struct, a UTF-8-safe copy, a content fingerprint
    vault_parse.c       the wire contract, clamping every field
    vault_mock.c        the built-in demo snapshot
    vault_service.c     one fetch: http_get + parse
    ui_artwork.c        native tarot card + framed daily-reading composition
    ui_artwork_layout.c exact card, deck-spine and reading-frame geometry
    assets/tarot/       78 byte-aligned 272x464 LVGL I1 card descriptors
    fonts/              full 완성형 Noto Sans KR faces (OFL) — generated, do not hand-edit
    test/host/          unit tests for all of the above
  provisioning/         SoftAP + captive portal + NVS + SNTP onboarding
  device_api/           STA-mode HTTP/JSON control server + mDNS (obsidianboard.local)
  board_io/             battery ADC
  buttons/              KEY0/1/2 + BOOT edge events
sim/                    desktop simulator — renders the native 648x480 artwork and asserts pixels
tools/
  vault_server.py       scans a REAL Obsidian vault and serves the contract from it
  mock_vault_server.py  the same contract from a fixed payload — the reference producer
  tarot_assets/         reproducible source manifest and 1-bit card generator
  tarot_readings_ko.json bounded Korean readings for all 78 cards
  gen_fonts.py          regenerates components/vault_core/fonts/
  agent_status.py       one line for a script to report an agent to the board
  flash.sh              find the board and flash it
app/                    React Native companion app — setup + control over the LAN
third_party/cJSON/      vendored (ESP-IDF v6 dropped cJSON from core)
```

## Documentation

- [docs/bring-up.md](docs/bring-up.md) — first power-on: reading the boot log, and the numbers to record
- [docs/vault-contract.md](docs/vault-contract.md) — the JSON the device polls, and how it fails
- [docs/epaper-5in83.md](docs/epaper-5in83.md) — the UC8179 driver, the refresh policy, the self-test
- [docs/pinout.md](docs/pinout.md) — GPIO assignments and the three traps
- [docs/board-hardware.md](docs/board-hardware.md) — hardware notes
- [docs/simulator.md](docs/simulator.md) — the desktop simulator, and what it asserts
- [docs/app-control.md](docs/app-control.md) — the HTTP/JSON contract
- [docs/graphics.md](docs/graphics.md) — 1-bit rendering notes
- [docs/esp-idf-development.md](docs/esp-idf-development.md) — install / build / flash / menuconfig
- [docs/references.md](docs/references.md) — datasheets and upstream sources
- [docs/specs/](docs/specs/) — the design this was built from

## Lineage

Forked from `saju_omi_esp32`, a 2.13" fortune-slip board on an EE05. This project kept its
structure — the draw-and-present split, the captive-portal provisioning, the device API, the
simulator, and the habit of writing a host test before believing anything — and replaced the entire
content axis. It shares no content code, and deliberately not its mDNS name or AP prefix: two
devices answering one discovery probe on the same LAN is a fault nobody can diagnose.

## License

MIT — see [LICENSE](LICENSE). The bundled Noto Sans KR faces are SIL OFL 1.1
(`components/vault_core/fonts/OFL.txt`).
