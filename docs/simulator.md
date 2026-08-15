# The desktop simulator

```bash
cd sim && ./kanji_sim.sh
```

This builds the real UI against desktop LVGL, renders every screen at the panel's native
`648 × 480`, applies the same RGB565-to-monochrome threshold the firmware does, writes
`sim/shots/*.bmp` (plus PNG copies where `sips` exists), and **exits non-zero when any assertion
fails.** It is a test that happens to leave a gallery behind.

The substitution list is one item long: instead of transferring the one-bit result to the UC8179,
the flush callback writes a bitmap. Everything else — `ui_kanji.c`, `ui_kanji_layout.c`,
`kanji_nav.c`, `kanji_parse.c`, the four bitmap faces — is the code that ships.

```bash
KANJI_URL=http://localhost:8123/kanji.json ./kanji_sim.sh
```

With `KANJI_URL` set, the simulator uses the device's own `kanji_service_fetch()` against a real
proxy rather than the built-in demo card, so a contract change is caught on a laptop instead of on
the glass. Without it, it renders `kanji_mock.c`.

## What it renders

Eighteen shots, one per state the board can be in:

| | |
|---|---|
| `01-front`, `02-back` | the demo card, both faces |
| `03-front-kanji`, `04-back-kanji` | a real kanji card (語), where 성립 and 구성 do their work — the demo card is a VOCAB card and honestly has no on-yomi, no principle and one component |
| `05-back-again`, `06-back-easy` | a grade committed, reached through a real `kanji_nav_press()` |
| `07-front-new-card`, `08-back-new-card` | a card with no history: the plate collapses to 새 카드 |
| `09-front-no-examples` | the pull-quote and its ornament go together, or not at all |
| `10-back-worst-case`, `11-front-worst-case` | every field at its model maximum in the widest glyph the body face has |
| `12-front-offline`, `13-back-stale` | the two failure badges |
| `14-session-complete` | 오늘 학습 완료 — no card, session counters intact |
| `15-front-long-headword`, `16-back-long-headword` | too long for the 56 px hero, dropped to 28 px |
| `17-setup` | the Wi-Fi setup overlay |
| `18-no-data` | no snapshot at all |

The worst case and the failure states are the ones worth looking at after a layout change: a failure
state is exactly the screen nobody renders by hand before shipping.

## What it asserts

**The chrome, on every single screen.** The header band must be *filled* and must have white text
punched out of it — both halves, because a header that stopped filling would still show its text and
a header whose text stopped rendering would still look like a band. The footer must have ink, and so
must all four key-hint slots individually, so a legend that lost one is caught.

**Per screen, that each rectangle the layout claims holds something actually does.** The player is
inverted, the hero has ink, the reveal prompt and the deck caption and the queue counters are
present, the action rail is drawn. The scrubber is checked at both ends: the demo card is 35 of 60,
so the left third must be filled and the right end must not be.

**The page uses the panel.** The content area is divided into 16 px cells — one line of body type —
and the cells carrying any ink at all are counted. This is deliberately NOT a measure of how black
the page is, and the difference is the whole point: one-bit CJK type is thin, and the design this
replaced was simultaneously *blacker* and *emptier* than the one that replaced it (7.9% ink at 39%
occupancy, against 5.0% at 51%), because its dock filled a whole cell solid to show a cursor. Ink
mass cannot tell a full page from a sparse one. The floors are calibrated to what they must catch:
45% on the answer face fails if any one of its six blocks stops rendering.

**No two visible labels share paper.** Pixels cannot catch this — black text over black text is
still black, so two overlapping labels read as one slightly bold label. On a fourteen-block
two-column page that is what happens the first time a block grows a line.

**Nothing crosses a column.** Asserted against the widgets' own coordinates rather than the layout
constants, because the fault being hunted is a renderer that ignored the rectangle it was given.

**The question face does not leak the answer.** Every visible label on 문제 is searched for the
card's `senses[]`, `parts[].meaning` and `examples[].gloss`. The last is the one that ships by
accident: it reads as harmless context right up until it prints 우연히 만나다 under 会う.

**The dock has exactly as many filled cells as there are grades in flight — one, or none.** A dock
with two black cells and a dock with one look equally plausible until the pixels are counted. The
filled cell's labels must survive the inversion (white on black); the others must not. The
acknowledgement refresh is then XORed against the previous frame: pixels must change inside the dock
rectangle and *zero* may change outside it.

**Every string is drawable.** Every field of the snapshot — headword, reading, senses, examples,
description, hook, parts, FSRS labels, rating previews — is walked codepoint by codepoint against
all three body faces. The bilingual eyebrows from `ui_strings.h` go through the same check, because
they are the only fixed copy left on the answer face and the likeliest strings on the board to carry
a character no other literal does: 成り立ち's 成 and 立 reach the face from nowhere else.

The hero face gets its own pass. `kanji_hero_is_large()` picks it by *length*, so a headword the
Japanese-only 56 px face cannot draw would otherwise be silently chosen for it — the coverage check,
not the length rule, is what catches that.

## Why the pixels are evidence and not an approximation

The simulator binarizes with the identical `px < 0x7FFF` rule as `Lvgl_FlushCallback` in
`main/main.cpp`, from the identical RGB565 draw buffer, through the identical fonts. That is the
whole reason LVGL is not run in its native `I1` format on the device, where it would save 1.2 MB of
PSRAM — see [graphics.md](graphics.md). One colour format across host and device means a screenshot
is a statement about the board rather than a picture that resembles it.

Every layout constant in `ui_kanji_layout.c` was measured off these bitmaps, and the load-bearing
ones are asserted on every run.
