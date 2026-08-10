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
| `0x10` | Write RAM (previous image) | primed with `0x00` for a full refresh |
| `0x12` | Display refresh | the actual update trigger |
| `0x13` | Write RAM (new image) | where our framebuffer goes |
| `0x15` | Dual SPI | `0x00`, single-SPI source data |
| `0x50` | VCOM / data interval | `0x10 0x07` full, `0xA9 0x07` partial, `0xF7` before sleep |
| `0x60` | TCON setting | `0x22` |
| `0x61` | Resolution | `0x0288 0x01E0` = 648 × 480 |
| `0x71` | Get status | issued before every BUSY sample |
| `0x90`/`0x91`/`0x92` | Partial window / in / out | the windowed refresh |
| `0xE0`/`0xE5` | Cascade / force temperature | partial-mode waveform |

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
...update widgets...      /* ui_vault_set_*(), cheap, no panel traffic */
Lvgl_RenderNow();         /* synchronous render -> flush_cb -> framebuffer */
epd_refresh_full();       /* or epd_refresh_partial_area(...) */
```

The LVGL flush callback **never** refreshes the panel. Exactly one task (`UiTask` in
`components/user_app/user_app.cpp`) touches LVGL or starts a refresh; everything else — buttons, the
HTTP API, the vault poller — posts a command and returns.

### Full refresh

Writes `0x00` into the previous-image plane and the framebuffer into the new-image plane, then
triggers `0x12`. Priming the old plane with black rather than with the outgoing frame is what forces
every pixel through a complete black→target transition, and is therefore what makes a full refresh
*clear* ghosting rather than merely repaint.

Used for: any content change, any page change, and the boot screen.

### Partial refresh

`epd_refresh_partial_area(x1, y1, x2, y2)` refreshes one rectangle. X is snapped outward to a byte
boundary — the controller addresses source lines in groups of eight, and a window starting mid-byte
comes out shifted rather than clipped — so the refreshed area may be up to 7 px wider on each side
than asked for. Harmless, since the framebuffer there is already correct.

Only one thing uses it: the header strip, via `ui_vault_header_area()`. Everything else on this
panel changes the whole content area.

The window is the **whole** header, not just the clock's slot, and that is a correctness point
rather than a convenience. Three things up there move without new vault data — the clock, the
battery level, and the badge — and the badge is the important one: a board whose source has gone
away goes stale purely by the passage of time, so if the tick only refreshed the clock's rectangle
the 오래됨 badge would never appear at all. Nothing else in that state triggers a refresh. The cost
is the same either way; a UC8179 partial refresh is dominated by the waveform frames, not by how
many bytes were sent.

Every `EPD_PARTIAL_CHAIN_MAX` (6) partials is promoted to a full refresh so residue still gets
cleared. Six, not the ten this code used at 122 × 250: this panel is ten times the area, and residue
on a large white dashboard is far more visible than on a dense slip.

## The refresh policy, and how it was chosen

The 2.13" board this forked from refreshed its clock every minute, because a partial refresh there
was ~0.3 s and silent. That number does not transfer, and assuming it did would be a guess about a
panel ten times the size.

So the driver measures. `epd_last_full_ms()` and `epd_last_partial_ms()` report what the last
refresh of each kind actually took on this board, they are logged at every refresh, and they are
served over the network at `GET /api/state` under `panel` — so the measurement can be read off a
phone rather than by holding a serial cable to a board on a shelf.

Fixed regardless of what the numbers say:

- a page change is always a **full** refresh;
- new data is a **full** refresh;
- the clock tick is the only partial-refresh candidate, and it is throttled to one refresh every
  `CLOCK_REFRESH_EVERY` (5) minutes rather than every minute.

And the rule that matters more than any of it: **a poll that returns unchanged content does not
touch the panel at all.** `vault_hash()` fingerprints everything that reaches the glass, and
`VaultTask` compares before it notifies. On a device that mostly sits still, this is the difference
between a silent dashboard and one that flashes at nobody every five minutes forever.

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
