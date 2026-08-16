# The 5.83" e-Paper panel (UC8179, 648 × 480)

Everything about how this board draws, and why the code is shaped the way it is.

## The panel

| | |
|---|---|
| Part | Seeed 5.83" monochrome ePaper, 648 × 480 |
| Controller | **UC8179** |
| Interface | 4-wire SPI + BUSY + RST, 24-pin FPC |
| Framebuffer | 81 bytes × 480 = **38,880 bytes**, 1 = white, 0 = black |
| Carrier | XIAO ePaper Display Board EE04 |

648 is a multiple of 8, so a framebuffer row is exactly 81 bytes with no off-panel padding. That is
a real simplification over the 122 × 250 panel this driver replaced, where the last byte of every
row carried six bits of nothing and every piece of address arithmetic had to know it.

## Where the command sequence comes from

`components/port_bsp/epd_panel.c` is transcribed from Waveshare's reference driver for the panel
that shares this controller and resolution — [`EPD_5in83_V2.c`](https://github.com/waveshareteam/e-Paper/blob/master/RaspberryPi_JetsonNano/c/lib/e-Paper/EPD_5in83_V2.c)
— cross-checked against Seeed's own UC8179 tables in
[`Seeed_GFX/TFT_Drivers/UC8179_Init.h`](https://github.com/Seeed-Studio/Seeed_GFX/blob/master/TFT_Drivers/UC8179_Init.h).
The two agree command for command. Deviations from the vendor sequence are marked `NOTE:` in the
source; there are three, and each is explained where it appears.

Not from the datasheet, deliberately: the UC8179 datasheet documents the registers but not the
power-up ordering or the settle delays, and those are exactly the parts that fail intermittently
rather than obviously.

## The command set in use

| Cmd | Name | Use |
|---|---|---|
| `0x00` | Panel setting | `0x1F` = KW mode, LUT from OTP |
| `0x01` | Power setting | VGH 20 V, VGL −20 V, VDH 15 V, VDL −15 V |
| `0x02` | Power off | before deep sleep |
| `0x04` | Power on | after the reset, before anything else |
| `0x06` | Booster soft start | the "enhanced display drive" values |
| `0x07` | Deep sleep | payload `0xA5` |
| `0x10` | Write RAM (previous image) | gets the **same** frame as `0x13` — see below |
| `0x12` | Display refresh | the actual update trigger |
| `0x13` | Write RAM (new image) | where our framebuffer goes |
| `0x15` | Dual SPI | `0x00`, single-SPI source data |
| `0x50` | VCOM / data interval — **and data polarity** | `0x21 0x07` full, `0xA9 0x07` partial, `0xF7` before sleep |
| `0x60` | TCON setting | `0x22` |
| `0x61` | Resolution | `0x0288 0x01E0` = 648 × 480 |
| `0x71` | Get status | issued before every BUSY sample |
| `0x90`/`0x91`/`0x92` | Partial window / in / out | the windowed refresh |
| `0xE0`/`0xE5` | Cascade / force temperature | partial-mode waveform — `0x02`/`0x6E` entering, **`0xE0` back to `0x00` leaving** |

## `0x50` decides whether the panel prints the frame or its negative

The high bits of `0x50`'s first byte select the border data and the **data polarity** — which way a
RAM bit maps to black or white. The vendor sends `0x21` from `Init()` for a full refresh and `0xA9`
from `Display_Partial()` for a windowed one. Those two agree in the polarity bits and differ only in
border and interval:

```
0x21 -> 0b0010_0001     full     (reference)
0xA9 -> 0b1010_1001     partial  (reference, and what this driver already sent)
0x10 -> 0b0001_0000     full     (what this driver used to send)
```

That agreement between the vendor's two values is the check that says a pair is right, and `0x10`
failed it. Every full refresh printed the negative of the card: crisp, correctly laid out, entirely
inverted — white type on a black field. The windowed refresh used the vendor's own `0xA9` and was
unaffected, so the grade dock was the one region on the glass coming out the right way round.

**Crispness is what separates this from an under-driven panel.** An inverted card is sharp and
simply wrong; a panel running the partial waveform by mistake is streaky and patchy. Read the
refresh time before assuming which one you have.

## A full refresh writes the frame to both planes

`EPD_5IN83_V2_Display()` sends the same `Image` bytes to `0x10` and then to `0x13`; `Clear()`
likewise sends `0xFF` to both and lands on white. **`epd_refresh_full()` does the same, and the
reason is not stylistic.**

In KW mode the OTP LUT is indexed by the `(old, new)` pair. Because the vendor always writes
`old == new`, only the `WW` and `BB` entries are ever exercised by a real full refresh — the mixed
`BW`/`WB` paths are never used and are not something this panel's waveform was validated against.

This driver used to prime `0x10` with `0x00` instead, under a comment claiming that was Waveshare's
own trick for clearing ghosting. It was not — there is no `0x00` fill anywhere in that driver. What
it actually did was drive every background pixel down the mixed `BW` path, which on this panel ends
on the opposite rail. The panel built the correct image early, during the phase driven by the `0x13`
data, then spent the remainder of the waveform carrying it to its inverse. The card came out
**sharp, fully driven, and completely inverted** — white type on a black field.

Sharpness is the tell, and it is what separates this from the partial-waveform bug below:

| symptom | cause |
|---|---|
| crisp image, colours inverted | `0x10` primed with something other than the frame |
| correct colours, streaky and patchy | full refresh running the partial waveform (`0xE0`/`0xE5`) |

Ghosting is cleared by the full OTP waveform itself, which runs either way — the priming was never
what did it.

## Leaving partial mode is two registers, not one

The controller picks its OTP waveform **by temperature**. `0xE0 = 0x02` is TSFIX — "ignore the
on-chip sensor, use whatever `0xE5` says" — and pinning `0xE5` at `0x6E` is what selects the short
waveform that makes a windowed refresh ~600 ms instead of ~4 s.

So `0x92` (partial out) is only half of leaving partial mode. It closes the window; it does not give
the temperature back. Leave TSFIX set and the panel stays on the partial waveform for **every
subsequent full refresh** — and a full refresh does not fail, it just returns early:

```
epd: full refresh 3959ms      <- before the first partial
epd: partial refresh 616ms
epd: full refresh 699ms       <- and every one after it
```

Four hundred milliseconds of drive cannot carry a pixel all the way to black or all the way to
white. The previous frame stays half printed under the new one: black streaks through white type,
and patches of panel lighter or darker than their neighbours. It reads as failing hardware, and
nothing logs an error — the whole signature is that one number in this driver's own INFO line.

Which is why `epd_refresh_full()` now asserts on it: a full refresh under `EPD_FULL_REFRESH_MIN_MS`
(2 s) logs `ESP_LOGE`. The measurement was always there; nobody was reading it.

`0xE5` needs no counterpart write — with TSFIX clear its value is ignored.

## BUSY is active LOW

The single most important difference from the SSD1680 driver this replaced. On the UC8179 the panel
is **idle when BUSY is HIGH**. Getting the polarity backwards does not produce an error — it makes
every wait return instantly, so the driver writes the next command into a controller that is still
mid-refresh, and the panel comes out torn or blank. There is nothing in a log to suggest why.

`wait_busy()` also sends `0x71` (GET STATUS) before each sample, following Waveshare: the controller
refreshes its BUSY output on that command, and polling the pin alone can sit on a stale level.

Unlike the vendor driver, `wait_busy()` gives up after 30 seconds and logs rather than spinning
forever. A stuck BUSY means the panel is not connected or not powered, and hanging the UI task on
that is worse than carrying on — `epd_selftest()` and the boot-time `busy_line_probe()` exist to
surface it loudly instead.

## A refresh is not free

This is the constraint the whole application is arranged around.

```c
...update widgets...      /* ui_kanji_set_*(), cheap, no panel traffic */
Lvgl_RenderNow();         /* synchronous render -> flush_cb -> framebuffer */
epd_refresh_full();       /* or epd_refresh_partial_area(...) */
```

The LVGL flush callback **never** refreshes the panel. Exactly one task (`UiTask` in
`components/user_app/user_app.cpp`) touches LVGL or starts a refresh; everything else — buttons, the
HTTP API, the card poller — posts a command and returns.

### Full refresh

Writes `0x00` into the previous-image plane and the framebuffer into the new-image plane, then
triggers `0x12`. Priming the old plane with black rather than with the outgoing frame is what forces
every pixel through a complete black→target transition, and is therefore what makes a full refresh
*clear* ghosting rather than merely repaint.

Used for: a new card, revealing the answer, opening or paging a sheet, the setup overlay, the boot
screen and the self-test.

### Partial refresh

`epd_refresh_partial_area(x1, y1, x2, y2)` refreshes one rectangle. X is snapped outward to a byte
boundary — the controller addresses source lines in groups of eight, and a window starting mid-byte
comes out shifted rather than clipped — so the refreshed area may be up to 7 px wider on each side
than asked for. Harmless, since the framebuffer there is already correct.

There is exactly one partial refresh in the firmware: **the grade dock.** Walking the rating cursor
takes up to three KEY0 presses, and three full refreshes is nine seconds of the panel strobing
before the learner has told the board anything — so `present_dock()` re-renders and refreshes only
that strip.

Its rectangle is not a constant in the driver's caller. It comes from
`kanji_answer_layout()->dock`, the same integers the widgets were positioned from, because the one
failure this path has is silent: refresh a window that does not contain what changed and the panel
keeps showing the previous rating, with nothing in the log. `test_kanji_layout.c` therefore asserts
that the dock's `x` and `w` are multiples of 8 — so the driver's outward snap is a no-op and the
window refreshed is exactly the window drawn — and that every cell, label and span the dock draws
lies inside it.

## The refresh policy, and how it was chosen

The 2.13" board this forked from refreshed its clock every minute, because a partial refresh there
was ~0.3 s and silent. That number does not transfer, and assuming it did would be a guess about a
panel ten times the size.

So the driver measures. `epd_last_full_ms()` and `epd_last_partial_ms()` report what the last
refresh of each kind actually took on this board, they are logged at every refresh, and they are
served over the network at `GET /api/state` under `panel` — so the measurement can be read off a
phone rather than by holding a serial cable to a board on a shelf.

Fixed regardless of what the numbers say: a new card, a reveal and a screen change are **full**
refreshes, and the grade cursor is a **partial**. That split is not a tuning knob — it is what makes
choosing a rating feel like an input rather than a page load.

The partial path promotes itself: after `EPD_PARTIAL_CHAIN_MAX` (6) windowed refreshes in a row the
driver issues a full one instead, so ghosting inside the dock cannot accumulate across a long
session. The caller does not track this and should not; a KEY0 press that occasionally costs a full
refresh is the correct trade against a dock that slowly turns grey.

And the rule that matters more than any of it: **a poll that returns unchanged content does not
touch the panel at all.** `kanji_hash()` fingerprints everything that reaches the glass — and
deliberately not the card id, which does not — and `KanjiTask` compares before it notifies. On a
device that polls every five minutes forever, this is the difference between a silent board and one
that flashes at nobody all day.

Nothing on this panel changes with the clock, either. The board has no RTC and prints no time: every
span it shows (`9일 뒤`, `10분 뒤`) is worded by the proxy and arrives as a string. So the minute
tick that keeps battery telemetry current never redraws anything.

### When the numbers arrive

The constants below are placed to be *decided*, not guessed at twice. Read the measurement:

```bash
curl -s http://obsidianboard.local/api/state | jq .panel
# {"partialChain": 3, "fullRefreshMs": ..., "partialRefreshMs": ...}
```

then run the display self-test and commit a grade on each of the four buttons, watching the
chosen cell invert. That inversion is the board's only partial refresh.

| what you see | change | where |
|---|---|---|
| the dock ghosts after several cards | lower `EPD_PARTIAL_CHAIN_MAX` | `components/port_bsp/epd_panel.h` |
| six partials in a row leave no residue | raise it — every promotion the learner does not need is a full flash they do not see | same |
| a partial is over ~1 s | the dock stops feeling like an input; consider a shorter dock rectangle before anything else | `components/vault_core/ui_kanji_layout.c` |
| full refresh is over ~6 s | nothing to change — it is why API writes return before the panel has caught up | — |
| the card renders but looks streaky, with black lines through white type and uneven light/dark patches | check the refresh time first: if `full refresh` collapsed to well under 2 s after the first partial, the waveform is stuck in partial mode — `panel_leave_partial()` must write `0xE0 = 0x00` | `components/port_bsp/epd_panel.c` |
| the card is crisp but inverted — white type on a black field — and the correct image is visible partway through the refresh before it flips | `0x10` is not getting the outgoing frame; both planes must receive it | same |

## Memory

| | |
|---|---|
| Panel framebuffer | 38,880 B, `MALLOC_CAP_DMA \| MALLOC_CAP_INTERNAL` |
| LVGL draw buffers | 2 × 622,080 B (648 × 480 × RGB565), PSRAM |

LVGL renders RGB565 and `Lvgl_FlushCallback` in `main/main.cpp` binarizes with `px < 0x7FFF`. Keeping
LVGL on RGB565 rather than its I1 format costs about 1.2 MB of the 8 MB PSRAM and buys every widget,
font and anti-aliased shape working exactly as it does in the desktop simulator — which renders
through the identical threshold, which is what makes the simulator's screenshots a test rather than
an approximation.

The panel framebuffer is the one allocation that cannot go to PSRAM: it is the DMA source for a
38,880-byte SPI burst.

## Self-test

`epd_selftest()` (also `POST /api/display/test`) cycles white, black, a 1-px checkerboard, an
ordered-dither density ramp, a border-plus-diagonals frame with a solid block in the top-left
quadrant, and finally a windowed partial refresh — timing the last two.

Each pattern fails differently and on purpose:

- **white / black** — the panel is alive and the power gate is on.
- **checkerboard** — no stuck rows or columns; the SPI clock is not eating bits.
- **dither ramp** — byte order and bit order within each byte.
- **frame + diagonals** — the last row and column are reachable, and the axes are not swapped.
- **the top-left block** — which corner is the origin. The symmetric patterns cannot tell you.
- **partial band** — the windowed path works, and how long it takes.

It blocks for tens of seconds, so it runs on the UI task and the HTTP handler only enqueues it.
