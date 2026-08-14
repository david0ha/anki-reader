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

Sixteen shots, one per state the board can be in:

| | |
|---|---|
| `01-question` | the headword alone on the filled player |
| `02-answer`, `03-answer-easy`, `04-answer-again` | the answer side with the grade cursor on three of its four ratings |
| `04b-answer-three-examples` | the same side at full height — three 예문 rows AND the rating prompt, which is where they collided |
| `05-description` | 설명 — the shape story, the memory hook, the components |
| `06-comments`, `07-comments-page2` | 댓글, both pages |
| `08-fsrs-1` … `10-fsrs-3` | the three fixed FSRS pages, each carrying the card's own numbers |
| `11-session-complete` | 오늘 학습 완료 — no card, session counters intact |
| `12-long-headword` | a headword too long for the 56 px hero, dropped to 28 px |
| `12b-ascii-headword` | `~がたい` — a headword the Japanese-only hero face cannot draw, dropped to 28 px rather than rendered as tofu |
| `13-offline` | the last card, badged 오프라인 |
| `14-setup` | the Wi-Fi setup overlay |

The last four are the ones worth looking at after a layout change. Three of them are failure states,
and a failure state is exactly the screen nobody renders by hand before shipping.

## What it asserts

**The chrome, on every single screen.** The header band must be *filled* and must have white text
punched out of it — both halves, because a header that stopped filling would still show its text and
a header whose text stopped rendering would still look like a band. The footer must have ink, and so
must all four key-hint slots individually, so a legend that lost one is caught.

**Per screen, that each rectangle the layout claims holds something actually does.** The player is
inverted, the hero has ink, the reveal prompt and the deck caption and the queue counters are
present, the action rail is drawn. The scrubber is checked at both ends: the demo card is 35 of 60,
so the left third must be filled and the right end must not be.

**The grade dock has exactly one filled cell, and it is the cursor's.** This is the check a
screenshot cannot make. A dock with two black cells and a dock with one look equally plausible until
the pixels are counted, and "the cursor did not move" and "the cursor moved and the old one did not
clear" produce images a human eye reads as the same picture. The selected cell's label and span must
survive the inversion (white on black); the unselected ones must not (black on white).

**Every string is drawable.** Every field of the snapshot — headword, reading, senses, examples,
description, hook, parts, comment authors and bodies, FSRS labels, rating previews — is walked
codepoint by codepoint against all three body faces. The three pages of FSRS copy from
`ui_strings.h` go through the same check, because they are the longest fixed text on the board and
the likeliest to contain a character no other literal does.

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
