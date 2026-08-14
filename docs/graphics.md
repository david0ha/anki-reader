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

Every screen positions everything with absolute constants, and the constants are not in the screen
files. They are in `ui_kanji_layout.c`, which is pure integers — no LVGL, no ESP-IDF, no libm — and
which `test_kanji_layout.c` interrogates before a single widget exists. `ui_internal.h` holds only
the drawing shorthand.

That separation is unusual for LVGL, and there are three reasons for it here:

- **A rectangle is testable on a laptop in a millisecond; a rendered panel is not.** "Does this
  overlap", "is this on screen", "is the dock byte-aligned" are all arithmetic, and answering them in
  arithmetic is what makes the answers cheap enough to assert on every build.
- **A reflow means a full refresh.** On e-Paper, "the layout shifted slightly" and "every pixel
  changed" are the same event, and the second one costs seconds and a flash. A card whose senses
  gain a character must not move its examples.
- **One rectangle leaves the layout and reaches the driver.** `kanji_answer_layout()->dock` is
  handed verbatim to `epd_refresh_partial_area()`. If the geometry the widgets were drawn from and
  the geometry the panel refreshes ever diverge by a pixel, the board silently shows a stale rating
  with nothing in the log. Deriving both from the same integers is the only way that stays true.

The constants were read off the simulator's bitmaps, and the load-bearing ones are asserted on every
run — every claimed rectangle inked, the header band both filled and legible, exactly one grade cell
selected.

Two LVGL traps worth naming:

- **Children are positioned relative to the parent's *content* area**, which a `border_width`
  insets. A border style on a screen object silently shifts every absolute Y inside it, so frames
  are drawn as child boxes, never as a style on a container that has children.
- **A label with only a width set will wrap, not ellipsize.** `LV_LABEL_LONG_MODE_DOTS` needs the
  height pinned to one line, or LVGL auto-sizes the height downwards and the second line lands on
  whatever is below it. `ui_lab_w()` sets both; this cost two real bugs before it did.

## Text

**Anti-aliasing is the enemy.** A 1-bit threshold turns a grey edge pixel into a hard black or white
one, so hairline strokes shimmer and thin fonts break up. Hence:

- **Sans, not serif**, unlike the fortune board this codebase forked from. That panel was printing a
  만세력 slip; this one has to survive a 16 px Korean gloss after binarization, where a serif's thin
  strokes drop out entirely and a uniform stroke does not.
- **1-bpp generation** (`--bpp 1` in `tools/gen_fonts.py`). Generating at higher bpp and
  thresholding at runtime looks worse than letting the font converter decide — and costs four times
  the flash to do it.
- **Whole scripts, not subsets.** A headword, its かな reading, an example sentence and a comment
  body all arrive from kanjis.ai at runtime. There is no symbol list derivable ahead of time, and the
  failure mode of guessing is a tofu box in the middle of somebody's card, on the glass, after a
  two-second refresh, where nobody is watching.
- **Latin numerals** at display sizes come from LVGL's built-in Montserrat; everything else,
  including mixed Korean-and-digit strings, is drawn from the body faces so a line stays in one voice.

### Two families, one baseline

Each body face is converted from **both** Noto Sans KR and Noto Sans JP, because neither can draw
this board alone: KR is missing 289 of the 2965 JIS level 1 kanji (the 新字体 with no hanja
counterpart) and JP has no Hangul at all. They are cuts of the same Source Han Sans design, so a
Japanese headword and the Korean gloss under it share a baseline, a stroke weight and a colour.

The split is a **partition**, and that is load-bearing rather than tidy. `lv_font_conv` lets the
*last* `--font` group win a contested codepoint, and if that winner turns out not to have the glyph,
the character is dropped from the output silently with exit code 0 — so overlap is a way to lose
glyphs without being told. Nothing overlaps, and `verify_face()` re-reads every generated `.c` and
fails if a requested character did not make it.

The tables are not in this repository. The 2350 완성형 syllables are exactly what Python's `euc-kr`
codec reaches; the kana and both kanji levels are exactly what `euc-jp` reaches. The counts are
asserted on every run, so a codec that stops agreeing is a loud failure rather than a face that
quietly ships 289 tofu boxes. `tools/gen_fonts.py --dry-run` reports the symbol set without touching
anything.

`tools/kanji_server.py` imports the same `symbol_set()` and checks every string it is about to send,
substituting anything outside the shipped faces. The catalog does contain characters no face here
covers — simplified-Chinese component forms and astral-plane radicals inside `hint.shapes[]` — and
that check is what keeps them off the glass.

### The hero is a special case

`ui_font_jp_56` is Japanese-only: kana, both kanji levels, the JIS punctuation row and the ten
digits. 완성형 Hangul at 56 px would be roughly another megabyte of flash for glyphs a Japanese
headword cannot contain.

`kanji_hero_is_large()` chooses that face by the headword's **length**, not its content — a word too
long for 56 px drops to the 28 px face rather than being ellipsized, because a truncated headword is
not a headword. Which means a headword outside the hero's coverage would be silently routed to it,
and that, not the length rule, is what the simulator's coverage check exists to catch.

It is also the reason the build needs `CONFIG_LV_FONT_FMT_TXT_LARGE=y`: LVGL packs a glyph's bitmap
offset into 20 bits otherwise, capping a face at 1 MB of bitmap, and this one has 2.02 MB. It fails
loudly — `lv_font_conv` writes an `#error` into the file — so it is a requirement, not a trap.

## Icons

`ui_icons.c` draws its glyphs as **vectors** in a `LV_EVENT_DRAW_MAIN` callback — no image assets,
no canvas buffers, and they composite identically in the simulator and on the device. Eleven of
them: battery, plug, wifi, wifi-off, filled dot, hollow dot, cross, check, and the three that label
the action rail — book (설명), comment (댓글), clock (FSRS).

Two techniques worth reusing:

- **Punch white to separate two blacks.** The wifi-off slash draws a thick white line first and a
  thin black one on top, so the bar stays visible where it crosses an arc. Without it the two blacks
  merge and the "off" reading is lost.
- **Inset the fill from the shell.** The battery's fill is inset by a clear pixel, so at low
  percentages it reads as a fill rather than a slightly thicker border.
- **On an inverted area, give the icon a white chip rather than a white stroke.** The question
  screen's action rail sits on the filled player; at 26 px a white glyph loses its thin parts to
  binarization, so each one gets a white square and is drawn black inside it. The chip is also what
  makes the rail read as three buttons rather than three decorations.

## Rules of thumb

- Rules and dividers are **solid black**, never grey — a "subtle" grey lands on one side of the
  threshold or the other, arbitrarily.
- Prefer filled silhouettes to outlines below about 20 px.
- **Wrap only where the box was sized for prose**, which on this board is the sheets: the shape
  explanation and the memory hook get `ui_lab_wrap()` with an explicit height, and the model's byte
  caps were chosen to fit those heights (`KANJI_BODY_MAX` is 480 bytes because that is four wrapped
  16 px lines across 620 px, not because it is a round number). Everywhere else, ellipsize. An
  ellipsis is an honest "there was more"; an unplanned wrap is a collision with the row below.
- White-on-black is for two or three characters seen from across a room, never for prose. At 16 px
  after binarization, white-on-black Hangul loses its thin strokes.
- Give text an opaque background wherever it sits on top of something already drawn.
- Measure in the simulator before committing a constant, and add an assertion if the constant is
  load-bearing.
