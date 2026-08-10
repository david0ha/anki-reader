# Obsidian Board — design

**Date:** 2026-08-10
**Status:** implemented — see §9 for where the implementation departed from this, and
§10 for sub-project 2, which this spec deferred and which was then built anyway
**Hardware:** Seeed XIAO ePaper Display Board **EE04** + XIAO ESP32-S3 Plus,
5.83" monochrome ePaper **648×480**, controller **UC8179**

This board is a fork of `saju_omi_esp32` (a 2.13" fortune-slip board). It keeps that
project's skeleton — the LVGL-on-e-Paper discipline, captive-portal provisioning, the
device HTTP API, the desktop simulator and the host-test culture — and replaces the
entire content axis: no omikuji, no 사주, no weather. Instead the board is an
always-on dashboard for an Obsidian vault and the agents working on it.

## Scope

**This spec (sub-project 1):** firmware, end to end, running on hardware.
**Not this spec (sub-project 2):** porting the `app/` React Native companion app.
It is deferred until the HTTP contract below has been exercised on real hardware,
because that contract is the app's entire surface.

## 1. What changes from the base project

| | base (EE05 + 2.13") | this (EE04 + 5.83") |
|---|---|---|
| controller | SSD1680 | **UC8179** |
| BUSY polarity | HIGH = busy | **LOW = busy** |
| resolution | 122×250, 6 bits of row padding | **648×480**, 81 B stride, no padding |
| panel framebuffer | 4,000 B | **38,880 B** |
| LVGL draw buffers | 61 KB × 2 | 622 KB × 2 (RGB565, PSRAM) |
| RAM planes | `0x24` new / `0x26` prev | **`0x13` new / `0x10` prev**, trigger `0x12` |
| buttons | USER(GPIO2) + BOOT(GPIO0) | **KEY0(2), KEY1(3), KEY2(5)** + BOOT(0) |
| I2C / RTC | PCF85063A probe on GPIO5/6 | **removed** — GPIO5/6 are KEY2 and ADC_EN |
| battery | ADC GPIO1, enable not driven | ADC GPIO1, **enable GPIO6 driven HIGH** |

The **SPI wiring is identical** to the EE05 (verified against
`Seeed_GFX/User_Setups/EPaper_Board_Pins_Setups.h`): SCLK=7, MOSI=9, CS=44, DC=10,
RST=38, BUSY=4, PWR_EN=43. The GPIO43/44-are-UART0 trap is unchanged, so the console
stays on USB Serial/JTAG.

## 2. Structure

```
main/                 app_main: panel + LVGL bring-up, provisioning, task launch
                      user_config.h — EE04 pins, 648×480
components/
  port_bsp/           epd_panel.c — NEW UC8179 driver, same public header shape
  app_bsp/            LVGL port (unchanged API)
  vault_core/         the portable core: device + simulator + host tests
    vault_model.h     the data model
    vault_parse.c     JSON -> model, every field clamped
    vault_service.c   HTTP GET polling loop
    vault_mock.c      the built-in demo snapshot
    vault_hash.c      model fingerprint — unchanged data never touches the glass
    ui_vault.c        page router, shared header/footer, overlay
    ui_page_stats.c   page 0
    ui_page_graph.c   page 1
    ui_page_agents.c  page 2
    ui_page_notes.c   page 3
    ui_graph.c        deterministic radial link layout
    ui_icons.c        vector glyphs
    device_api_json.c the companion-app JSON
    fonts/            generated faces (see §6)
    test/host/        unit tests
  provisioning/       + vault_url in the portal form and NVS
  device_api/         + /api/vault
  board_io/           battery only
  buttons/            KEY0/1/2 + BOOT
sim/                  renders all four pages to 648×480 BMP/PNG, with assertions
tools/
  mock_vault_server.py   the contract's reference implementation
  gen_fonts.py           font generation
```

## 3. Data contract

The device performs `GET <vault_url>` every `VAULT_POLL_SECONDS` (default 300).
Any HTTP(S) URL; on a home LAN this is plain HTTP.

```json
{
  "schema": 1,
  "vault": "second-brain",
  "generated_at": "2026-08-10T21:04:00+09:00",
  "stats": {
    "notes": 1428, "links": 3910, "orphans": 37, "tags": 212,
    "added_today": 6, "added_7d": 41,
    "daily": [3, 9, 12, 4, 0, 7, 6]
  },
  "tags":   [ { "name": "project", "count": 186 } ],
  "agents": [ { "name": "indexer", "state": "running", "last_run": "20:55",
                "processed": 1428, "queued": 3, "progress": 78,
                "note": "embedding 6 new notes" } ],
  "graph":  { "nodes": [ { "id": 0, "title": "MOC/Research", "deg": 24 } ],
              "edges": [ [0, 1] ] },
  "recent": [ { "title": "Weekly review", "time": "21:02", "links": 12 } ],
  "inbox":  [ { "title": "todo: 스펙 정리", "age_days": 3 } ]
}
```

`state` is one of `running | idle | error | done`. Every array is capped by the
parser (`VAULT_TAGS_MAX` etc.); extra entries are dropped, missing fields become
zero/empty, and a wrong type is treated as missing. A payload the parser cannot
make sense of leaves the previous snapshot on the glass — a dashboard showing
stale-but-labelled data beats a blank one.

**When `vault_url` is empty**, the device renders `vault_mock.c`'s built-in
snapshot and shows a `DEMO` badge in the header. This is what makes the board a
complete product with no PC running.

**Refresh gating.** After every poll the model is fingerprinted
(`vault_hash()`); if the fingerprint is unchanged, the panel is not refreshed at
all. On e-Paper this is not an optimisation, it is ghosting and panel lifetime.

## 4. Pages

Four pages, `KEY0` cycles. Every page shares a 40 px header (vault name, clock,
battery, DEMO/STALE badges) and a 40 px footer (page indicator + key legend).
Content area is therefore 648×400.

- **0 STATS** — four big counters (notes / links / orphans / tags), a 7-day
  activity bar chart, top tags with proportional bars, and a health block
  (link density, orphan rate, sync freshness).
- **1 GRAPH** — the vault's hubs and their links. Layout is **deterministic**:
  nodes sorted by degree descending are placed on concentric rings (the top node
  at the centre), and edges are straight 1-bit lines. No physics — the simulator
  and the device must produce the same picture, and the host test asserts node
  positions.
- **2 AGENTS** — one row per agent: state bullet, name, state word, last run,
  processed/queued counts, a progress bar, and the current note.
- **3 NOTES** — recent notes on the left, the inbox queue on the right.

## 5. Refresh policy

Both full and partial refresh are implemented. The *policy* is decided from
measurements taken on hardware and recorded in `docs/epaper-5in83.md` — a 5.83"
panel has ten times the area of the 2.13" this code came from, and assuming the
partial refresh is still cheap would be a guess.

Fixed regardless of measurement:
- a page change is always a **full** refresh;
- new data is a **full** refresh (the whole content area changes);
- the clock tick is the only partial-refresh candidate.

## 6. Fonts

Note titles and inbox items are **dynamic Korean** and cannot be subset ahead of
time — that is what makes this different from the base project, where every
string was a source literal.

- `ui_font_kr_16` — **full 완성형 (KS X 1001, 2350 syllables) + ASCII + 구두점**,
  1 bpp. This is the face every dynamic string uses. ~100 KB of flash against an
  8 MB app partition.
- `ui_font_kr_20`, `ui_font_kr_28` — subset faces carrying only the fixed UI
  labels, for headings and the big counters.
- Latin/digits at other sizes come from LVGL's built-in Montserrat.

1 bpp everywhere: the panel binarizes anyway, so anti-aliasing buys nothing and
costs 4×.

`tools/gen_fonts.py` derives the subset symbol list from `ui_strings.h` and emits
the full face for `ui_font_kr_16`. The base project's rule stands: **never
hand-edit `fonts/*.c`, never hand-maintain a glyph list.**

## 7. Verification

```bash
# 1) pure logic
cmake -S components/vault_core/test/host -B /tmp/vt && cmake --build /tmp/vt
/tmp/vt/test_vault_parse && /tmp/vt/test_vault_service && /tmp/vt/test_graph_layout \
  && /tmp/vt/test_vault_mock && /tmp/vt/test_api_json

# 2) provisioning pure logic
sh components/provisioning/test/run.sh

# 3) the real UI at the real resolution -> PNG + layout/glyph assertions
cd sim && ./sim.sh                                   # built-in mock
VAULT_URL=http://localhost:8123/vault.json ./sim.sh  # against the mock server

# 4) firmware
idf.py build && idf.py -p /dev/cu.usbmodem101 flash monitor
```

`test_vault_parse` feeds the parser truncated, oversized, type-confused and
field-missing JSON and asserts it clamps without crashing.
`test_graph_layout` asserts placement is deterministic and that no node escapes
the canvas.
The simulator fails the build on a missing glyph or on ink outside the content
area — it is a test, not a preview.

## 8. Names

The base project's mDNS name `tickerboard` and AP prefix `"Ticker Board"` are
hardcoded in its shipped app, so they are not reusable here. This board is
`obsidianboard.local`, AP prefix `"Obsidian Board"`. Sub-project 2 must match.


## 9. What changed during implementation

This document is the record of what was intended, kept as written. Six things came
out differently, and the differences are worth more than the plan was:

**`vault_hash.c` does not exist.** The fingerprint is thirty lines and belongs
beside the struct it hashes, so it lives in `vault_model.c`. A file per function
would have made the dependency graph look tidier and the code harder to follow.

**There is no `ui_font_kr_28`, and `ui_font_kr_20` is not a subset.** §6 planned
one full face for dynamic text and subsets for the fixed labels. That reintroduces
exactly the bug the full face was chosen to eliminate — "which face can draw this
string" — and it turned out two dynamic strings (the vault name, and agent names)
wanted the larger size. Both faces are now full 완성형: about 450 KB of an 8 MB
partition, against a class of bug that only shows up on the glass. The 28 px face
was dropped entirely; the big counters are digits, and digits come from Montserrat.

**The chrome is 44 px and 34 px, not 40 and 40.** Measured against the rendered
20 px face and the 28 px clock, not guessed. Content is 648×398.

**`ui_vault_clock_area()` became `ui_vault_header_area()`.** §5 said the clock
tick was the only partial-refresh candidate, which is true, and then quietly
implied the clock's rectangle was the right window, which is not. Staleness
arrives by the passage of time; refreshing only the clock would have meant the
오래됨 badge never reached the panel at all. See the commit.

**The badge ranking is offline → stale → demo**, not demo first. A board that has
been given a URL still shows the demo snapshot until its first successful fetch,
so ranking DEMO first made a configured board with an unreachable server badge
itself DEMO — true, and useless.

**A sixth host test appeared**, `test_vault_service`, standing in as the HTTP port
so the fetch layer's failure paths run without a server. §7 listed four; there are
five, plus the simulator.

## 10. Sub-project 2: the companion app

§Scope deferred the `app/` port until the HTTP contract had been exercised on real
hardware, on the reasoning that porting it before then would mean doing it twice.

It was ported anyway, before that condition was met. The board left the USB bus
partway through the session and did not come back, so "wait for hardware" became
"wait indefinitely" — and the contract had by then been exercised three other ways:
by `test_api_json`, by the simulator, and, once `app/scripts/mock-esp32.js` existed,
by a second independent implementation of the whole thing. That is not the same as
hardware, and the risk §Scope named is real: if the board turns out to disagree with
`docs/app-control.md`, the app follows the document and will be wrong with it.

What the port is:

- `src/lib/esp32.ts` rewritten against `docs/app-control.md` — the state snapshot,
  the four writes, and the provisioning form's `vault_url`.
- `src/lib/vaulturl.ts`, mirroring `prov_validate_vault_url()` so a typo is caught
  before a ~45s Wi-Fi join rather than after it.
- The onboarding wizard's third step is the snapshot URL where the stock build asked
  for API keys; the dashboard and settings are rewritten around the vault.
- `scripts/mock-esp32.js` really fetches the configured URL and summarises it the way
  `device_api_json.c` does, including the three failure codes — so the app is
  developed against a second implementation of the contract, not a stub.
- Deleted with the stock build's domain: the watchlist, the geocoder, the location
  autocomplete, the ticker row, the API-key step.

Two decisions inside it are worth keeping when the contract next changes, both
recorded in `docs/app-control.md`: a missing `ageSeconds` parses to `-1`, not `0`,
and an unrecognised `lastResult` maps to `unknown` rather than passing through.

120 unit tests, `tsc --noEmit` clean. Not run on a phone — that needs a native dev
build, which needs Xcode/Android Studio, which is the same class of "not verified
here" as the firmware.

## 11. Two things this spec never mentioned

Both came out of the brief rather than out of this document, and both sit outside
the device contract, which is why they are recorded here rather than folded into
§3.

**A producer that reads a real vault** (`tools/vault_server.py`). §3 defined the
wire format and §7 tested it, and `mock_vault_server.py` served a fixed payload —
so the whole system was verifiable and the product still stopped at "demo data on
a panel". The scanner walks a vault, works the same numbers out of the notes that
are really there, and serves the identical contract. It found four defects that
no fixture could: markdown and HTML tables of contents being read as tags, three
notes called `README` rendering as three identical rows, and two empty-list cases
— one where the simulator's check was wrong and one where the UI was.

Pointing it at real content was the single most productive thing done after the
firmware was complete, which is worth remembering: the fixtures were chosen by
the person who wrote the parser, and they were all *shaped like the parser*.

**Capture** (`--allow-capture`, and the memo box in the app). The brief asked for
a device that helps with memos and manages them; a dashboard showing an inbox
nobody can add to is half of that. The board has three buttons, so capture cannot
live on it — but it can live on the machine already serving the vault, one hop
away, and the app can derive its address from the snapshot URL the board reports.

It is deliberately **not** part of the device contract: no firmware change, no new
endpoint on the board, and a producer that does not implement it is an ordinary
supported case. It is off unless asked for, because it is an unauthenticated LAN
service that creates files in somebody's notes.
