# Rendering onto a 1-bit panel

The display is **648 × 480, black and white, no greyscale**. Every rendering decision in this
project follows from that plus the e-Paper refresh cost. This is what was chosen and why.

## The pipeline

```
LVGL widgets (RGB565)
   ↓  lv_refr_now()  — synchronous, on demand
flush callback (main.cpp): px < 0x7FFF ? black : white
   ↓  epd_set_pixel()
1-bit framebuffer (81 × 480 = 38,880 B)
   ↓  epd_refresh_full() / epd_refresh_partial_area()   — explicit, never automatic
panel
```

The two arrows that matter: **the flush callback does not refresh the panel**, and the render is
triggered on demand rather than whenever LVGL feels like it. See
[epaper-5in83.md](epaper-5in83.md).

## Why LVGL with an RGB565 buffer, and not 1 bpp

LVGL v9 has a native `I1` colour format, which would cut the two draw buffers from 622 KB each to
39 KB each. It was not used.

The reason is the simulator. `sim/` compiles the real UI and the real fonts against desktop LVGL and
binarizes with the *same* `px < 0x7FFF` rule. Every layout constant in this project was measured off
those bitmaps, and the simulator asserts on them on every run. Keeping one colour format across
device and host means a screenshot is evidence about the device, not an approximation of it. 1.2 MB
of an 8 MB PSRAM is a cheap price for that; a silent rendering difference between the two is not.

The cost is real but bounded: rendering 311,040 RGB565 pixels in PSRAM and thresholding them takes a
fraction of a second, against a panel refresh measured in seconds.

## Why hand-positioned pixels, not flex/grid

Every page positions everything with absolute constants against the grid in `ui_internal.h`. That is
unusual for LVGL and deliberate, for two reasons that are not the usual one:

- **A dashboard that reflows is unreadable.** If a row moves when a number gains a digit, the eye
  has to re-find everything. The four headline counters and the agent rows are meant to be read from
  across a room, in the same place every time.
- **A reflow means a full refresh.** On e-Paper, "the layout shifted slightly" and "every pixel
  changed" are the same event, and the second one costs seconds and a flash.

The constants are not guesses. They were read off the simulator's bitmaps, and the simulator asserts
the load-bearing ones on every run — every list row inked, every graph node and label inside the
canvas, the rules intact, the legend inside its slot.

Two LVGL traps worth naming:

- **Children are positioned relative to the parent's *content* area**, which a `border_width`
  insets. A border style on a page object silently shifts every absolute Y in that page's grid, so
  frames are drawn as child boxes, never as a style on a container that has children.
- **A label with only a width set will wrap, not ellipsize.** `LV_LABEL_LONG_MODE_DOTS` needs the
  height pinned to one line, or LVGL auto-sizes the height downwards and the second line lands on
  whatever is below it. `ui_lab_w()` sets both; this cost two real bugs before it did.

## Text

**Anti-aliasing is the enemy.** A 1-bit threshold turns a grey edge pixel into a hard black or white
one, so hairline strokes shimmer and thin fonts break up. Hence:

- **A sans face** (Noto Sans KR), unlike the serif the fortune board this forked from used. That
  panel was printing a 만세력 slip; this one is a dashboard, and at 16 px after binarization a
  serif's thin strokes drop out entirely while a uniform stroke survives.
- **1-bpp font generation** (`--bpp 1` in `tools/gen_fonts.py`). Generating at higher bpp and
  thresholding at runtime looks worse than letting the font converter decide — and costs four times
  the flash to do it.
- **Full 완성형 faces, not subsets.** The readings arrive over the network, so there is nothing
  safe to subset from. `tools/gen_fonts.py --dry-run` is the canonical coverage check.
- Latin numerals at display sizes come from LVGL's built-in Montserrat; everything else, including
  mixed Korean-and-digit strings, is drawn from the Korean faces so a line stays in one voice.

## Icons

`ui_icons.c` draws its glyphs as **vectors** in a `LV_EVENT_DRAW_MAIN` callback — no image assets,
no canvas buffers, and they composite identically in the simulator and on the device. Eight of them:
battery, plug, wifi, wifi-off, filled dot, hollow dot, cross, check.

Two techniques worth reusing:

- **Punch white to separate two blacks.** The wifi-off slash draws a thick white line first and a
  thin black one on top, so the bar stays visible where it crosses an arc. Without it the two blacks
  merge and the "off" reading is lost. The graph page uses the same trick at a larger scale: a white
  disc under every node, so six edges converging on a hub do not turn it into a black star.
- **Inset the fill from the shell.** The battery's fill is inset by a clear pixel, so at low
  percentages it reads as a fill rather than a slightly thicker border.

## Rules of thumb

- Rules and dividers are **solid black**, never grey — a "subtle" grey lands on one side of the
  threshold or the other, arbitrarily.
- Prefer filled silhouettes to outlines below about 20 px.
- Ellipsize, never wrap, outside the provisioning overlay. An ellipsis is an honest "there was
  more"; a wrap is a collision.
- Give text an opaque background wherever it sits on top of something already drawn.
- Measure in the simulator before committing a constant, and add an assertion if the constant is
  load-bearing.
