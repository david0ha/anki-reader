# CLAUDE.md

This repository is a **Japanese vocabulary study device**: an ESP32-S3 driving a 5.83" monochrome
e-Paper panel that shows one card at a time from a kanjis.ai study session — headword, reading,
Korean senses, examples — and takes the learner's FSRS rating on three buttons. It polls one JSON
URL on the local network and is set up over Wi-Fi from a captive portal.

The board holds **no credentials**. `tools/kanji_server.py` runs on a machine you own, holds the
kanjis.ai session, and serves the board a flat card over the LAN. See
[docs/kanji-contract.md](docs/kanji-contract.md).

## Quick start (do this first)

Activate the ESP-IDF environment — **once per new shell session**:

```bash
. ~/esp/v5.4.3/esp-idf/export.sh      # v5.4.3 is what is installed here
```

Standard workflow after activation, run from the **repository root** (the root *is* the IDF project):

```bash
idf.py set-target esp32s3      # once per checkout
idf.py build
./tools/flash.sh               # finds the port, flashes, monitors (Ctrl+] to exit)
```

`tools/flash.sh` is `idf.py -p <PORT> flash monitor` with the two things that actually go wrong
handled: which of `/dev/cu.usbmodem*` / `cu.usbserial-*` / `cu.wchusbserial*` this board
enumerates as, and the second between the device node appearing and the CDC endpoint accepting a
connection. It activates the IDF environment if you have not.

- If it won't enter flash mode, **hold BOOT while pressing RESET**, release, and retry.
- If it finds no port at all, check the USB-C cable carries data. A charge-only cable powers the
  board — the panel will even light up — and enumerates nothing.
- If the build stops on `Too large font or glyphs in UI_FONT_JP_56`, your `sdkconfig` predates
  `CONFIG_LV_FONT_FMT_TXT_LARGE=y` in `sdkconfig.defaults`. `sdkconfig` is generated once and never
  re-derived, so `rm sdkconfig && idf.py build` is the fix — not menuconfig, and not editing the
  generated face.

## Verify before claiming anything works

Five layers, none of which needs a board. Run them in this order — each is faster than the next and
catches a different class of mistake.

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

# 4) the real UI at the real resolution -> PNG, plus layout/glyph assertions
cd sim && ./kanji_sim.sh    # KANJI_URL=http://localhost:8123/kanji.json ./kanji_sim.sh

# 5) firmware
idf.py build
```

The simulator is not a preview, it is a **test**: it fails the build on a glyph no shipped face can
draw, on a rectangle the layout says holds something and that rendered nothing, and on a grade dock
with anything other than exactly one filled cell — which is the defect a screenshot cannot reveal,
because a dock with two cursors and a dock with one look equally plausible until the black pixels
are counted. Look at `sim/shots/*.png` after any UI change.

`test_kanji_server.py` needs no kanjis.ai account and no network. Where it finds a local copy of the
catalog it also sweeps a sample of it — 400 of the 9,956 cards by default, `KANJIS_SAMPLE=` to widen
— through the glyph check, which is how a card citing a component form no shipped face covers gets
caught on a laptop rather than as a tofu box on the glass.

## Target hardware

| Item | Specification |
|------|------|
| Board | **Seeed XIAO ePaper Display Board EE04** + **XIAO ESP32-S3 Plus** |
| SoC | ESP32-S3 (Xtensa LX7 dual-core), 16MB Flash / 8MB Octal PSRAM |
| Display | 5.83" monochrome e-Paper, **648 × 480**, **UC8179**, 4-wire SPI + BUSY, 24-pin FPC |
| RTC | **none** — and the UI never needs one; see below |
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

## Five screens, three buttons

Three side buttons and no touch have to drive five screens and a four-way rating. The mapping is a
pure state machine in `components/vault_core/kanji_nav.c` — no LVGL, no panel, no card content
beyond "is there a card" and "how long is the open sheet" — and `test_kanji_nav.c` drives every
button from every reachable state. **Read that file rather than guessing; nothing else in the
firmware knows the mapping.**

| Screen | What it holds |
|---|---|
| 문제 | the headword alone on a filled player, and the reveal prompt |
| 정답 | the headword, its reading, the Korean senses, examples, and the four-rating dock |
| 설명 | the shape story, the memory hook, the headword's components |
| 댓글 | what people said under this card, two to a page |
| FSRS | what the scheduler is, in three fixed pages, plus this card's own numbers |

| Button | 문제 | 정답 | inside a sheet |
|---|---|---|---|
| KEY0 | reveal the answer | walk the grade cursor | next page |
| KEY1 | open 설명 | commit the rating | close the sheet |
| KEY2 | refresh — from anywhere. **Hold 5 s → reboot into Wi-Fi setup** | | |
| BOOT | open FSRS | open 설명 | next sheet, then back to the card |

The cursor cycles 보통 → 쉬움 → 다시 → 어려움 → 보통, so every rating is at most three presses away
and the default is where FSRS and Anki both start. BOOT skips a sheet that has nothing to say about
this card; FSRS is always available, because a learner staring at an empty session is exactly who
wants to read what the scheduler does.

The footer legend is derived from the same nav state (`kanji_nav_hint_*`). A fixed legend on a board
whose KEY0 means 정답 on one screen and 등급 on the next is a lie printed in 16 px.

## The two things that make this board different

**1. A refresh is not free.** A full refresh takes seconds and flashes the whole panel. So drawing
and presenting are separate everywhere:

```c
...update widgets...      /* ui_kanji_set_*(), cheap, no panel traffic */
Lvgl_RenderNow();         /* synchronous render -> flush_cb -> framebuffer */
epd_refresh_full();       /* or epd_refresh_partial_area(...) */
```

The LVGL flush callback **never** refreshes the panel. Exactly one task (`UiTask` in
`components/user_app/user_app.cpp`) touches LVGL or starts a refresh; everything else posts a
command. Full refresh for a new card or a screen change; a windowed partial for exactly one thing —
the grade dock. Choosing among four ratings takes up to three presses, and three full refreshes is
nine seconds of the panel strobing before the learner has told the board anything.

**2. A poll that changes nothing must not touch the panel.** `kanji_hash()` fingerprints everything
that reaches the glass and `KanjiTask` compares before it notifies `UiTask`. On a device that polls
every five minutes forever, this is the difference between a silent board and one that flashes at
nobody all day. Details in [docs/epaper-5in83.md](docs/epaper-5in83.md).

A third rule falls out of the first two: **grading never runs on `UiTask`.** It is an HTTP round
trip to a laptop that may be asleep, and `UiTask` owns the panel — a stalled request would freeze
every button on the board. KEY1 hands the rating to `KanjiTask` and returns, and the panel keeps
showing the answer that was graded until the next card actually arrives. Drawing anything sooner
would be drawing a guess.

## Project structure

```
main/                     app_main: panel + LVGL bring-up, provisioning, task launch
components/
  port_bsp/               UC8179 driver (epd_panel.c) — the only file that talks to the panel
  app_bsp/                LVGL port (RGB565 draw buffers, binarized in the flush callback)
  vault_core/             the portable core — compiles identically on device, sim and host tests
    kanji_model.c         the snapshot struct + UTF-8-safe copy + content fingerprint
    kanji_parse.c         wire JSON -> model, clamping every field
    kanji_mock.c          the built-in demo card (shown when no URL is set)
    kanji_service.c       one fetch and one grade: http_get + parse
    kanji_nav.c           the button state machine — the only interaction state on the board
    ui_kanji.c            header, footer, overlay, screen routing
    ui_card_{question,answer}.c   the player side and the answer side
    ui_sheet_{desc,comments,fsrs}.c   one file per rising sheet
    ui_kanji_layout.c     every rectangle, as pure integers; host-tested before a widget exists
    ui_common.c           the shared shapes; ui_internal.h holds the drawing shorthand
    ui_icons.c            vector glyphs
    device_api_json.c     the JSON the companion app receives
    fonts/                Noto Sans KR + JP faces (OFL) — generated, do not hand-edit
    test/host/            unit tests for all of the above
  provisioning/           SoftAP + captive portal + NVS + SNTP + /api/* onboarding
  device_api/             STA-mode HTTP/JSON control server + mDNS (obsidianboard.local)
  board_io/               battery ADC
  buttons/                KEY0/1/2 + BOOT edge events
  user_app/               the two tasks, the command queue, the source guard
app/                      React Native companion app — setup + control over the LAN
sim/                      desktop simulator — renders the real UI to 648x480 and asserts on it
third_party/cJSON/        vendored (ESP-IDF v6 dropped cJSON from core)
tools/
  kanji_server.py         holds a REAL kanjis.ai session and serves the contract from it
  mock_kanji_server.py    the same contract from a fixed payload — the reference producer
  test_kanji_server.py    the proxy's tests, including the catalog-wide glyph check
  test_mock_kanji_server.py   the reference producer's tests
  gen_fonts.py            regenerates components/vault_core/fonts/
  flash.sh                find the board and flash it
```

## Working rules

- **Never hand-edit `components/vault_core/fonts/*.c`.** Run `python3 tools/gen_fonts.py --download`.
  The three body faces are converted from **two** families — Noto Sans KR and Noto Sans JP — because
  neither can draw this board alone: KR is missing 289 of the 2965 JIS level 1 kanji (the 新字体 with
  no hanja counterpart) and JP has no Hangul at all. Each body face carries **9,242** glyphs — ask
  the generator rather than trusting this line: `python3 -c "import sys; sys.path.insert(0,'tools');
  import gen_fonts; print(len(gen_fonts.symbol_set()))"`. That is 완성형 Hangul, ASCII, every kana,
  both JIS X 0208 kanji levels, the JIS punctuation row, and the 158 curated component forms in
  `S_DATA_RADICALS` that the 설명 sheet's shape stories cite. The hero face carries 6,713 — Japanese
  only, because 56 px of Hangul is flash for glyphs a Japanese headword cannot contain. None of
  those tables is in this repo: they are derived from Python's own EUC-KR and EUC-JP codecs, and the
  row counts are asserted on every run. **All fixed user-visible strings belong in `ui_strings.h`** — that is where
  the generator reads its punctuation from, and where the simulator's coverage check reads them from.
  Subsetting is not an option here: a headword, a かな reading and a comment body all arrive over the
  network, and the failure mode of guessing is a tofu box in the middle of somebody's card.
- **The grade dock's rectangle is the only partial refresh on the board, and it must stay
  byte-aligned.** `kanji_answer_layout()->dock` goes verbatim to `epd_refresh_partial_area()`.
  `test_kanji_layout.c` asserts that its x and w are multiples of 8 and that every cell, label and
  span it draws is inside it. Drift by one pixel and the panel refreshes a strip that does not
  contain the thing that changed — the board then silently shows a stale rating, and nothing logs it.
- **`kanji_mock.c` and `tools/mock_kanji_server.py` must stay identical.** `test_kanji_mock.c`
  asserts it by parsing the server's committed fixture and comparing fingerprints. Change one and
  the test tells you which field diverged; then run
  `python3 tools/mock_kanji_server.py --write-fixture`.
- **A rejected payload must leave the previous card alone.** `kanji_parse()` writes `*out` only on
  success. Blanking the panel is the one failure a learner actually notices, and a stale card badged
  오래됨 beats an empty one. The same rule covers a rejected grade: a 409 leaves the learner on the
  answer they already read.
- **The board has no RTC, and no code here should want one.** Every span the panel prints — `9일 뒤`,
  `10분 뒤`, `복습` — is worded by the proxy against the *server's* clock and arrives as a string.
  Nothing on the glass changes with time, which is why an idle wake never refreshes the panel.
- **Labels get a fixed height, not just a width.** `ui_lab_w()` does this; bypassing it makes LVGL
  auto-size the height and *wrap* instead of ellipsizing, and the second line lands on the row below.
- **`sdkconfig` holds per-developer values and is gitignored — never commit it.** Wi-Fi passwords
  live in NVS via the portal, never in Kconfig; kanjis.ai credentials live in
  `tools/kanji_server.py`'s environment and never touch the board at all.
- The setup AP prefix and the model string are both `"Kanjis Board"`
  (`components/provisioning/provisioning.c`), and the app prints the same words at every step of
  onboarding. The mDNS hostname stays `obsidianboard` — it is what already-flashed boards answer to
  and what `app/src/lib/discovery.ts` probes, and renaming it would strand them. Neither is the
  `tickerboard` / `"Ticker Board"` of the project this forked from, whose shipped app resolves
  those names. **A doc that names a different AP than the firmware raises sends the learner hunting
  for a network that does not exist**, so these three strings and the docs quoting them move
  together.
- If anything about the hardware is uncertain, don't guess — check
  [docs/references.md](docs/references.md).

## Documentation

- [docs/bring-up.md](docs/bring-up.md) — first power-on: the boot log line by line, and the numbers to record
- [docs/kanji-contract.md](docs/kanji-contract.md) — the JSON the device polls, and how it fails
- [docs/epaper-5in83.md](docs/epaper-5in83.md) — the UC8179 driver, the refresh policy, the self-test
- [docs/pinout.md](docs/pinout.md) — GPIO assignments
- [docs/board-hardware.md](docs/board-hardware.md) — hardware notes
- [docs/app-control.md](docs/app-control.md) — the companion-app HTTP/JSON contract
- [docs/simulator.md](docs/simulator.md) — the desktop simulator, and what it asserts
- [docs/graphics.md](docs/graphics.md) — 1-bit rendering notes, and the font decision
- [docs/esp-idf-development.md](docs/esp-idf-development.md) — install / build / flash / menuconfig
- [docs/references.md](docs/references.md) — datasheets and upstream sources
- [docs/specs/](docs/specs/) — the design record this was built from, superseded in part
