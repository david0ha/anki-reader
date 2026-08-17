# AnkiReader

> One name, everywhere: the repository, the firmware image, the setup Wi-Fi it raises
> (`AnkiReader-XXXX`), the model it reports over `/api/info`, its mDNS host `ankireader.local`, and
> the companion app all say **AnkiReader**. **A board flashed before this rename answers to
> `obsidianboard.local` and raises `Kanjis Board-XXXX`** — reflash it, because nothing here looks
> for either name any more.

A desk-sized Japanese vocabulary trainer. A 5.83" monochrome e-Paper panel on a Seeed EE04 carrier
and a XIAO ESP32-S3 Plus boots directly into a 9,956-card offline JLPT catalog — headword, かな
reading, Korean senses, and shape notes — and accepts four ratings on three buttons. A configured
[kanjis.ai](https://kanjis.ai) proxy can take over when Wi-Fi is available. Reveal, rate, next card.
No screen to unlock, no app to open, no notification.

Two faces, and nothing else. **문제** is an art print — the headword and the learner's own record
with this card, and deliberately nothing else: an example printed here turns out to be a spoiler
even when it is pure Japanese, because the catalog's examples are the words hanging off the
kanji's readings rather than sentences using the headword, so 破れる was captioned 破る / やぶる —
a reading one kana off the answer. **정답** is a dictionary spread that puts
everything on one page at once: the reading, the Korean senses, where the character comes from, what
it is built out of, its examples, its FSRS figures, and the four-rating dock. Nothing is behind a
button, because a learner does not have time to press four of them to read two lines.

The board holds **no credentials**. `tools/kanji_server.py` runs on a machine you own, holds the
kanjis.ai session, and serves the board one flat card over the LAN. Nothing on the device can leak
an account.

To see it before you have a board, run `cd sim && ./kanji_sim.sh`. It renders all sixteen states to
`sim/shots/` at the panel's native 648 × 480, through the same UI code, the same fonts and the same
binarization the device runs. Those are not mockups, and they are not committed — they are generated
from the code that ships, and the run fails the build if any of them is wrong. See
[the simulator](docs/simulator.md).

## Quick start

```bash
. ~/esp/v5.4.3/esp-idf/export.sh    # once per shell

idf.py set-target esp32s3           # once per checkout
idf.py -DKANJI_CATALOG_DB=/absolute/path/to/kanjis-backend.sqlite3 catalog_image
idf.py build
./tools/flash.sh                    # normal flash: firmware + offline catalog, then monitor
```

The first boot displays the restored offline card immediately and does not force Wi-Fi setup. Hold
KEY2 for five seconds when you want the `AnkiReader-XXXX` setup network, then give it Wi-Fi
credentials and, optionally, a remote study URL. A missing or corrupt catalog uses the built-in
card badged `DEMO` as the final fallback rather than leaving a blank board.

**If `idf.py build` fails on a font symbol, the problem is your `sdkconfig`, not the code.** It is
generated once and never re-derived, so anything added to `sdkconfig.defaults` after your checkout
is silently absent — and the symptom is always a compile error that reads like a bug. `rm sdkconfig
&& idf.py build` is the fix; `sdkconfig` is gitignored and per-developer, so back it up first if you
keep local settings there. Both instances so far were fonts: `Too large font or glyphs in
UI_FONT_JP_56` wants `CONFIG_LV_FONT_FMT_TXT_LARGE=y`, and `'lv_font_montserrat_18' undeclared`
wants `CONFIG_LV_FONT_MONTSERRAT_18=y` for the grade dock's button glyphs. Do not take the
compiler's suggestion of `lv_font_montserrat_14` — an old `sdkconfig` really does have it, and it is
the wrong size, so the dock would shrink instead of failing.

Doing this for the **first** time on a given board, follow [docs/bring-up.md](docs/bring-up.md)
instead: the three things most likely to be wrong on a first power-on all look like a blank screen,
and the boot log is the only place they are told apart.

### Feeding it your own session

Run the proxy on the machine that will hold your kanjis.ai login:

```bash
export KANJIS_EMAIL=you@example.com KANJIS_PASSWORD=...   # never on the command line
python3 tools/kanji_server.py --check     # verify the setup without a board
python3 tools/kanji_server.py             # http://<you>:8123/kanji.json
```

Give the board that URL in the setup portal, or over the network once it is online — see
[docs/app-control.md](docs/app-control.md). To try the whole loop without an account,
`python3 tools/mock_kanji_server.py` serves the same contract from a fixed payload.

Anything that serves that JSON works; the device cannot tell the difference. The format is
[documented and tested](docs/kanji-contract.md).

### Offline catalog and local ratings

`KANJI_CATALOG_DB` selects the read-only backend SQLite file. `KANJI_CATALOG_USER_ID` optionally
selects one source user; without it, the exporter deterministically chooses the user with the most
active card/deck coverage. `KANJI_CATALOG_SEED` defaults to `0` and deterministically fixes the
per-deck SHA-256 order and balanced round-robin traversal. Generate without flashing via
`idf.py catalog_image`.

A normal `idf.py flash` (and `tools/flash.sh`) writes the application and generated `catalog`
partition. `idf.py catalog-flash` updates only the catalog. `idf.py app-flash` updates only the
application, leaving both the existing catalog and its usable local progress unchanged. Normal and
catalog-only flashing also leave the physical `study_state` bytes untouched because there is no
generated state image. Replay is catalog-specific, however: state is used only when its catalog ID
matches the active catalog. Changing the database content, selected user, seed, or projected card
content can produce a new catalog ID and intentionally starts fresh progress even though the old
state bytes remain in flash. `idf.py erase-flash` physically erases them.

Offline ratings persist the current position, grade, repetitions, lapses, and now the scheduler's
own state: stability, difficulty and a due epoch, as fixed point in the two-bank flash journal.
`components/vault_core/kanji_fsrs.c` is FSRS-6 transcribed from the backend's own py-fsrs 6.3.1,
pinned by host tests against golden vectors printed by that interpreter, and it runs **unfuzzed** —
the interval a learner is shown has to be the interval they get.

The board still has no battery-backed wall clock, and `kanji_clock.c` does not pretend otherwise.
It answers in three tiers: `TRUSTED` after an SNTP sync this boot, `APPROXIMATE` from a persisted
anchor plus uptime, and `UNKNOWN` when it has never synced — which is a real state with its own
wording, not a zero standing in for a date. A day-granular scheduler tolerates hours of drift,
which is why the middle tier is useful rather than dangerous.

Generated catalog images are local build artifacts and must not be committed or redistributed
without a separate rights review.

## Controls

| Button | 문제 | 정답 |
|---|---|---|
| KEY0 | 뜻 보기 — reveal | **다시** (again) |
| KEY1 | 뜻 보기 — reveal | **어려움** (hard) |
| KEY2 | 새로고침 · **hold 5 s → reboot into Wi-Fi setup** | **보통** (good) · same hold |
| BOOT | 뜻 보기 — reveal | **쉬움** (easy) |

## Verify before claiming anything works

Five layers, none of which needs a board. Each is faster than the next and catches a different class
of mistake.

```bash
# 1) pure logic — the model, the button state machine, the wire format,
#    the layout rectangles, the fetch layer, the demo card, the API JSON
cmake -S components/vault_core/test/host -B /tmp/vt && cmake --build /tmp/vt
/tmp/vt/test_kanji_model && /tmp/vt/test_kanji_nav && /tmp/vt/test_kanji_parse \
  && /tmp/vt/test_kanji_layout && /tmp/vt/test_kanji_service && /tmp/vt/test_kanji_mock \
  && /tmp/vt/test_api_json

# 2) provisioning pure logic, and the app's source guard
sh components/provisioning/test/run.sh
sh components/user_app/test/run.sh

# 3) the reference producer, and the real kanjis.ai proxy
python3 tools/test_mock_kanji_server.py
python3 tools/test_kanji_server.py
python3 tools/test_offline_catalog.py
KANJIS_DB=/absolute/path/to/kanjis-backend.sqlite3 python3 tools/test_offline_catalog.py

# 4) the real UI at 648x480 -> PNG, plus layout and glyph assertions
cd sim && ./kanji_sim.sh

# 5) firmware
idf.py build
```

The simulator is not a preview, it is a **test**: it fails on a glyph no shipped face can draw, on a
rectangle that rendered nothing, and on a grade dock with anything other than exactly one filled
cell. Look at `sim/shots/*.png` after any UI change.

## Hardware

| Item | Specification |
|------|------|
| Board | **Seeed XIAO ePaper Display Board EE04** + **XIAO ESP32-S3 Plus** |
| SoC | ESP32-S3 (Xtensa LX7 dual-core), 16 MB Flash / 8 MB Octal PSRAM |
| Display | 5.83" monochrome e-Paper, **648 × 480**, **UC8179**, 4-wire SPI + BUSY, 24-pin FPC |
| RTC | none — and the UI never needs one; every date arrives already worded |
| Buttons | KEY0/1/2 (GPIO2/3/5) on the carrier, BOOT (GPIO0) on the XIAO |
| Power | 5 V USB-C, optional Li-ion (JST 2.0 + slide switch) |

Wiring is the EE04's fixed routing, kept in `main/user_config.h`. Three traps: **BUSY is active
LOW** on the UC8179 (the inverse of most SSD-family panels, and it fails silently), **GPIO43 gates
the panel's power**, and **GPIO43/44 are the default UART0 pins** so the console must stay on USB
Serial/JTAG. See [docs/pinout.md](docs/pinout.md).

## The two things that make this board different

**A refresh is not free.** A full refresh of this panel takes seconds and flashes the whole screen.
So drawing and presenting are separate everywhere:

```c
...update widgets...      /* ui_kanji_set_*(), cheap, no panel traffic */
Lvgl_RenderNow();         /* synchronous render -> flush_cb -> framebuffer */
epd_refresh_full();       /* or epd_refresh_partial_area(...) */
```

Exactly one task (`UiTask`) touches LVGL or starts a refresh; everything else posts a command. There
is exactly one windowed partial refresh in the whole firmware — the grade dock — because choosing
among four ratings takes up to three presses, and three full refreshes is nine seconds of strobing
before you have told the board anything.

**A poll that returns unchanged content does not touch the panel at all.** Every card is
fingerprinted and the poller compares before it notifies. On a device that polls every five minutes
forever, that is the difference between a silent board and one that flashes at nobody all day.
Details in [docs/epaper-5in83.md](docs/epaper-5in83.md).

## Project structure

```
main/                   app_main: panel + LVGL bring-up, provisioning, task launch
components/
  port_bsp/             UC8179 driver (epd_panel.c) — the only file that talks to the panel
  app_bsp/              LVGL port (RGB565 draw buffers, binarized in the flush callback)
  vault_core/           the portable core — compiles identically on device, sim and host tests
    kanji_model.c       the snapshot struct, a UTF-8-safe copy, a content fingerprint
    kanji_parse.c       the wire contract, clamping every field
    kanji_mock.c        the built-in demo card
    kanji_service.c     one fetch and one grade: http_get + parse
    kanji_nav.c         the button state machine — the only interaction state on the board
    kanji_fsrs.c        FSRS-6 on the board — the backend's py-fsrs 6.3.1, transcribed
    kanji_clock.c       SNTP-anchored clock in three tiers + the Korean span wording
    ui_kanji.c          overlay and the two-face router
    ui_card_front.c     문제 — the art print
    ui_card_back.c      정답 — the dictionary spread
    ui_kanji_layout.c   every rectangle, as pure integers, host-tested
    fonts/              Noto Sans KR + JP faces (OFL) — generated, do not hand-edit
    test/host/          unit tests for all of the above
  provisioning/         SoftAP + captive portal + NVS + SNTP onboarding
  device_api/           STA-mode HTTP/JSON control server + mDNS (ankireader.local)
  board_io/             battery ADC
  buttons/              KEY0/1/2 + BOOT edge events
  user_app/             the two tasks, the command queue, the source guard
sim/                    desktop simulator — renders the real UI at 648x480 and asserts on the pixels
tools/
  kanji_server.py       holds a REAL kanjis.ai session and serves the contract from it
  mock_kanji_server.py  the same contract from a fixed payload — the reference producer
  gen_fonts.py          regenerates components/vault_core/fonts/
  flash.sh              find the board and flash it
app/                    React Native companion app — setup + control over the LAN
third_party/cJSON/      vendored (ESP-IDF v6 dropped cJSON from core)
```

## Documentation

- [docs/bring-up.md](docs/bring-up.md) — first power-on: reading the boot log, and the numbers to record
- [docs/kanji-contract.md](docs/kanji-contract.md) — the JSON the device polls, and how it fails
- [docs/epaper-5in83.md](docs/epaper-5in83.md) — the UC8179 driver, the refresh policy, the self-test
- [docs/pinout.md](docs/pinout.md) — GPIO assignments and the three traps
- [docs/board-hardware.md](docs/board-hardware.md) — hardware notes
- [docs/simulator.md](docs/simulator.md) — the desktop simulator, and what it asserts
- [docs/app-control.md](docs/app-control.md) — the HTTP/JSON contract
- [docs/graphics.md](docs/graphics.md) — 1-bit rendering notes, and the font decision
- [docs/esp-idf-development.md](docs/esp-idf-development.md) — install / build / flash / menuconfig
- [docs/references.md](docs/references.md) — datasheets and upstream sources
- [docs/specs/](docs/specs/) — the design record this was built from

## Lineage

Forked from `saju_omi_esp32`, a 2.13" fortune-slip board on an EE05, by way of an Obsidian vault
dashboard and a daily-tarot display on this same panel. Each of those replaced the content axis
whole and kept the skeleton — the draw-and-present split, the captive-portal provisioning, the
device API, the simulator, and the habit of writing a host test before believing anything.
**AnkiReader** is this board's own name — the repository, the firmware, the AP, the mDNS host and
the app all use it, and nothing of the vault dashboard's naming survives. Two devices answering one
discovery probe on the same LAN is a fault nobody can diagnose, so this board deliberately does not
reuse the *fortune* board's names either.

## License

MIT — see [LICENSE](LICENSE). The bundled Noto Sans KR and Noto Sans JP faces are SIL OFL 1.1
(`components/vault_core/fonts/OFL.txt`).
