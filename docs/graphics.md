# Rendering onto a 1-bit panel

The display is **122 × 250, black and white, no greyscale**. Every rendering decision in this project
follows from that plus the e-Paper refresh cost. This is what was chosen and why.

## The pipeline

```
LVGL widgets (RGB565)
   ↓  lv_refr_now()  — synchronous, on demand
flush callback (main.cpp): px < 0x7FFF ? black : white
   ↓  epd_set_pixel()
1-bit framebuffer (16 × 250 = 4000 B)
   ↓  epd_refresh_full() / epd_refresh_partial()   — explicit, never automatic
panel
```

The two arrows that matter: **the flush callback does not refresh the panel**, and the render is
triggered on demand rather than whenever LVGL feels like it. See
[epaper-2in13.md](epaper-2in13.md).

## Why LVGL with an RGB565 buffer, and not 1bpp

LVGL v9 has a native `I1` colour format, which would save the 122 KB of PSRAM the two RGB565 draw
buffers cost. It was not used.

The reason is the simulator. `sim/` compiles the real `ui_fortune.c` and the real fonts against
desktop LVGL and binarizes with the *same* `px < 0x7FFF` rule. Every layout constant in this project
was measured off those bitmaps. Keeping one colour format across device and host means a screenshot
is evidence about the device, not an approximation of it. 122 KB of an 8 MB PSRAM is a cheap price
for that; a silent rendering difference between the two is not.

The buffers fall back to internal RAM when there is no PSRAM (`lvgl_bsp.cpp`), so this does not
strand a PSRAM-less board.

## Why hand-positioned pixels, not flex/grid

`ui_fortune.c` positions everything with absolute Y constants that add up to exactly 250. That is
unusual for LVGL and deliberate: at this size, layout-engine rounding costs more pixels than it
saves, and a 1-bit panel gives no visual slack — a label two pixels too tall clips instead of
overlapping softly.

The constants are not guesses. They were read off the simulator's bitmaps, and the simulator asserts
the load-bearing ones on every run (the 만세력 frame present with a clean gap between its borders,
the grade unclipped between the pillar boxes, the grid's blank rows staying blank, and — on the
unframed pages — nothing reaching the panel edge).

One LVGL trap worth naming, because it cost a whole misrendered frame: children are positioned
relative to the parent's *content* area, which a `border_width` insets. A border style on the page
object silently shifts every absolute Y in the grid by the border width — so the 만세력 frame is
drawn as child boxes, never as a style on the page itself.

## Text

**Anti-aliasing is the enemy.** A 1-bit threshold turns a grey edge pixel into a hard black or white
one, so hairline strokes shimmer and thin fonts break up. Hence:

- **A serif face** (Noto Serif KR). Its thick/thin contrast survives binarization better than a
  uniform-stroke sans at these sizes — and it is what a real fortune slip is printed in. The Bold
  weight is used exactly where the mockup asks for weight 700: the grade, the seal, the table
  headers.
- **1-bpp font generation** (`--bpp 1` in `tools/gen_fonts.py`). Generating at higher bpp and
  thresholding at runtime looks worse than letting the font converter decide.
- **Subset fonts.** A few hundred glyphs instead of ~11,000, derived automatically from the source
  strings — including the characters that only exist in runtime-composed text (the date line's
  digits, the space in the 일진 line). See [omikuji.md](omikuji.md).
- ASCII on the home page comes from LVGL's built-in Montserrat faces; the 만세력 page's date line
  uses the Korean face's own digits so the whole slip stays in one voice.

## Vertical writing

LVGL has no vertical text, so the 만세력 verse is a custom widget (`ui_vtext.c`) drawn glyph-by-
glyph in a `LV_EVENT_DRAW_MAIN` callback, same architecture as the icons: columns right→left, glyphs
top→bottom on a fixed pitch, spaces as half-gaps, inverted section tags as filled rectangles with
white glyphs. Two metric details matter on a 1-bit panel: CJK ink sits high in its ~1.4×em line box,
so each glyph's draw area is the full line height pulled up by half the excess (otherwise the
descent allowance visibly pushes every glyph down its column); and the pillar side boxes get the
same effect for free from a plain multi-line label with *negative* line spacing, computed from the
measured line height rather than hardcoded.

## Icons

`ui_icons.c` draws the weather and battery glyphs as **vectors** in a `LV_EVENT_DRAW_MAIN` callback —
no image assets, no canvas buffers, and they composite identically in the simulator and on the
device.

Two geometries, switched at `size < 26`:

| | ≥ 26 px | < 26 px (the forecast strip) |
|---|---|---|
| Sun | outline disc + 8 rays | **filled** disc + 4 rays |
| Cloud | outline (silhouette with a white one punched out) | **filled** silhouette, bumps pushed to the edges |
| Rain | outline cloud + 3 drops | filled cloud + 2 drops |
| Partly | sun with rays behind an outline cloud | offset sun + cloud, separated by a 1 px white halo |

The small variants exist because outlines stop working below ~26 px: a 2 px ring around a 4 px disc,
or a punched-out cloud whose wall is one pixel, binarizes into a smudge. The partly-cloudy halo is
the subtlest of these — without it the sun and cloud merge into one head-shaped blob, which reads as
neither.

All of this was found by dumping the rendered 20 × 20 cells as ASCII art from the simulator's BMP
output, not by looking at scaled-up screenshots.

## Rules of thumb

- Rules and dividers are **solid black**, never grey — a "subtle" grey lands on one side of the
  threshold or the other, arbitrarily.
- Prefer filled silhouettes to outlines at small sizes.
- Explicit `\n` in Korean text, never automatic wrap: the break point should not depend on font
  metrics. `test_omikuji.c` enforces the per-line budget.
- Measure in the simulator before committing a constant, and add an assertion if the constant is
  load-bearing.
