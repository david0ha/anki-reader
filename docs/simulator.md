# Desktop simulator

`sim/` builds the **real** `ui_fortune.c`, the real fonts and the real logic against LVGL on the
host, renders at 122 × 250, binarizes with the device's exact rule (`px < 0x7FFF`), and writes one
BMP per screen.

```bash
cd sim && ./sim.sh                    # sample weather
LOCATION="Seoul" ./sim.sh             # live Open-Meteo, the device's own code path
```

Output lands in `sim/shots/` — the seven omikuji ranks, the home page, and the setup overlay (plus
PNG copies on macOS).

## It is a test, not a preview

`./build/sim` exits non-zero on a layout or glyph problem, so it belongs in the verification loop
alongside the unit tests. It checks three things a screenshot would not:

**Glyph coverage, asked directly.** For every string the UI will draw — the grades, seals, verses
and table values of all seven ranks, the five 흐름 texts, every fixed label, all 60 일진 pillars in
both the composed `"<hanja> <hangul>"` form and the side-pillar syllables, and the runtime-composed
date line (digits, parentheses, every weekday syllable) — it calls `lv_font_get_glyph_dsc()` against
*the font that will actually render it*. A missing glyph is reported as `U+XXXX missing from the
font -> tofu` rather than left to be spotted as a small empty rectangle in a thumbnail. This is what
caught the space character: `' '` appears in no source literal, only at runtime, so the first
generated fonts had every Korean message rendering as `바라던□일이`. The date line's digits are the
same class of bug, which is why the composed strings are checked explicitly.

**Content staying inside the frame.** The 만세력 page is framed to the panel edge on purpose, so its
checks are structural: the double frame present at the corners with a clean white gap between the
borders, the title band actually black, the grade inked between the pillar boxes at a plausible
width (a clipped grade fails), and the grid's promised blank rows (above the grade row, between
verse and table) staying blank — a label that outgrows its slot lands there first. The home page and
overlay have no frame, so they keep the old rule: ink must not reach the panel edge.

It also prints an ink percentage per screen. A screen that renders nothing comes out at 0%, one that
has gone solid black near 100% — both are bugs that a filename listing would hide.

## Why it exists

122 × 250 leaves no room for "it'll probably fit", and the feedback loop on hardware is a flash cycle
plus a 2-second refresh. Every layout constant in `ui_fortune.c` was measured here first — the P0_*
grid, the vertical-verse pitch, the seal's overlap — and re-checked by the assertions above on every
run.

## Requirements

- CMake, a C compiler, libcurl (for the live weather path)
- LVGL sources at `managed_components/lvgl__lvgl` — fetched by `idf.py build`, or clone
  `lvgl/lvgl` at `v9.4.0` there directly

The simulator compiles the portable sources straight out of `components/fortune_core/`; only the
HTTP port differs (`http_port_curl.c` instead of `http_port_esp.c`). There is no second copy of the
UI to drift.
