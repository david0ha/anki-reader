# Lexicographic Instrument UI redesign

**Date:** 2026-08-14
**Status:** approved direction, document review required before implementation
**Surface:** 5.83-inch 648×480 monochrome e-Paper firmware UI
**Design contract:** [`../../../DESIGN.md`](../../../DESIGN.md)

## 1. Objective

Replace the current Shorts-derived visual language with a paper-dominant,
dictionary-proof composition that looks intentional as an always-on desk
object. Preserve the existing study model, network contract, action enum,
screen topology, draw/present separation, and simulator evidence.
The sole navigation-behavior expansion is description pagination: its `1` key
uses the existing next-page action when another semantic page exists.

The default question screen remains both the learning prompt and the ambient
screen. This project does not add an idle mode, clock, artwork playlist, or new
background task.

## 2. Scope

### Included

- Shared firmware chrome, visual primitives, geometry, and copy.
- Question, answer, description, comments, FSRS, session-complete, offline, and
  Wi-Fi setup states.
- A Japanese display-serif experiment at the existing 56 px hero size.
- The four-cell partial-refresh grade dock.
- Host layout tests, native simulator screenshots, pixel assertions, font
  coverage checks, and the firmware build.
- Documentation that describes the new visual system and verification rules.

### Excluded

- React Native companion app styling.
- Provisioning portal styling.
- JSON, HTTP, discovery, FSRS, or kanjis.ai behavior changes.
- Touch, animation, scroll, transition effects, grayscale, or a new panel
  driver.
- Migrating the proven RGB565-to-1-bit render path to LVGL I1.

## 3. Constraints retained from the repository

- The screen is exactly 648×480 and physically monochrome.
- LVGL and the panel refresh remain separate operations.
- UI objects are touched only by `UiTask`.
- New card, reveal, screen change, and sheet change are full refreshes.
- Grade cursor movement is the sole windowed partial refresh.
- The dock's X bounds are multiples of eight. Widget and driver coordinates use
  the same half-open rectangle contract, `[x1, x2) × [y1, y2)`.
- UI geometry remains pure integer data in `ui_kanji_layout.c` and is host
  tested without LVGL.
- Dynamic Japanese and Korean strings retain full glyph coverage and UTF-8-safe
  truncation.
- The desktop simulator and firmware use the same UI code, generated fonts,
  and RGB565 threshold.

## 4. Visual architecture

### 4.1 Fixed frame

The screen uses a 16 px outer edge, an 80 px left index rail, a 16 px gutter,
and a 520 px main column:

```text
0       16       96      112                              632   648
┌───────┬────────┬───────┬─────────────────────────────────┬─────┐
│ edge  │ rail   │gutter │ main                            │edge │
└───────┴────────┴───────┴─────────────────────────────────┴─────┘
```

The rail is separated by a 1 px vertical rule and holds only real session or
state information. Its two fixed blocks are identity and progress: queue
position on question/answer or page position on a paged reading sheet. A
demo, stale, or offline stamp replaces identity rather than creating a third
block. The rail replaces the full-width inverted header and the Shorts-style
action rail.

The main-column masthead retains the existing session measures as
`연속 N · 오늘 N`. Healthy battery state is silent; a low-battery state may use
the far end of that line. This preserves useful product information without
turning the index rail into a dashboard.

The bottom 40 px are reserved for a quiet, context-sensitive physical-control
legend. The screen presents keys as `1`, `2`, `3`, and `i`, never as `KEY0` or
`BOOT`.

### 4.2 Question

The question is a white typographic field. A short headword uses the 56 px
Japanese display face in the main column. A long or unsupported headword uses
the existing 28 px multilingual fallback. The reading and meanings remain
hidden. The reveal prompt is text, not an outlined or filled button.

The rail shows level or exceptional state in its identity block and queue
position in its progress block. Compact new/review/retry counts remain in a
single main-column line below the primary action. They do not become a third
rail block.

### 4.3 Answer

The answer preserves the rail and headword's visual origin so the reveal feels
like opening the same specimen, not navigating to another application.
Reading, meanings, and examples occupy fixed main-column rows. Empty example
rows disappear without moving subsequent geometry.

The rating prompt begins above a dock at `x=112`, `y=344`, `w=520`, `h=80`.
The dock has four equal 130 px cells. Its outer X bounds are byte aligned. The
partial refresh always covers the entire dock, so individual cell boundaries
do not need byte alignment.

### 4.4 Reading sheets

Description, comments, and FSRS use the same rail and main column. They are
close-reading screens, so bounded long prose may retain the existing 16 px
multilingual face. Titles and actionable labels use 20 or 28 px. Paragraphs are
left aligned and no title sits in a full-width black band.

The existing model limits remain authoritative. This redesign does not silently
drop a third example, comment, shape part, or FSRS page to make the composition
easier. Description changes from one fixed page to up to three semantic pages:
shape, memory hook, and components. Empty sections do not create a page. Each
prose page reserves at least 320 px of body height below its heading in the
520 px main column and collapses embedded whitespace to single spaces before
drawing. The current 16 px face has a 20 px line height and a measured maximum
16 px glyph advance, so even `KANJI_BODY_MAX - 1` single-byte glyphs occupy at
most fifteen wrapped lines, or 300 px. The simulator must prove that bound from
the generated font rather than trusting the comment. Clipping or truncating
below the model limit is not an allowed fallback.

### 4.5 Exceptional states

- Session complete reuses the paper-dominant question composition.
- Offline leaves the last valid card visible and marks the rail.
- No-data uses the same stable frame and a direct recovery action.
- Wi-Fi setup remains opaque and uses one top rule, one title, and direct body
  copy without decorative chrome.

## 5. Typography and font pipeline

The implementation adds Noto Serif CJK JP Semibold as a fifth source in
`tools/gen_fonts.py`, fetched from the official notofonts/noto-cjk repository
under SIL OFL 1.1. `ui_font_jp_56` keeps its public symbol name but is generated
from the serif source at 56 px, 1 bpp, for the existing Japanese hero symbol
set. The body faces and their source partition do not change.

The existing 28 px multilingual face remains the coverage and long-headword
fallback. `ui_font_can_draw()` continues to decide coverage before the large
face is selected. `CONFIG_LV_FONT_FMT_TXT_LARGE` remains enabled.

Montserrat 18 is enabled in both firmware and simulator for compact Latin
figures. Montserrat 14 is removed if no remaining object uses it. Korean text
and mixed-script metrics never switch to Montserrat.

The first simulator pass compares the new serif shot with the existing sans
baseline. The serif ships only if counters remain open, thin strokes survive
binarization, all requested glyphs are present, and the application retains at
least 512 KiB free in the 8 MiB partition. Otherwise `ui_font_jp_56` returns to
Noto Sans JP Regular while the approved geometry remains unchanged.

## 6. Component and file boundaries

### Existing boundaries retained

- `ui_kanji.c`: router, shared rail/footer, and setup overlay.
- `ui_card_question.c`: question specimen only.
- `ui_card_answer.c`: answer and grade dock only.
- `ui_sheet_*.c`: one reading surface per file.
- `ui_kanji_layout.c`: all physical rectangles and content-dependent size
  choices as pure integers.
- `ui_common.c` / `ui_internal.h`: un-themed monochrome primitives and shared
  typography aliases.

### Shared primitives added or revised

- A 1 px rule helper that does not create a bordered parent.
- A short selected stamp with black surface and white label.
- A shared index-rail builder/updater owned by `ui_kanji.c`.
- User-facing keycap/action pairs for the footer.
- Font aliases for display, title, action, body, and utility roles.

No default LVGL theme, opacity, animation, flex layout, scroll container,
shadow, pill, or card abstraction is introduced. `lv_label`, stripped
`lv_obj`, solid rules/fills, and existing custom draw callbacks are sufficient.

## 7. Navigation and data flow

The action enum and screen topology do not change. Description pagination
expands from one fixed page to the non-empty subset of shape, memory hook, and
components. The UI continues to receive one immutable `kanji_t` snapshot plus
`kanji_nav_t` and status. A navigation change selects one prebuilt screen and
updates its labels. No screen starts a panel refresh or stores a second copy of
application state.

Footer copy changes by state but uses the existing hint functions:

| State | `1` | `2` | `3` | `i` |
|---|---|---|---|---|
| Question | 정답 보기 | 힌트 | 새로고침 | 학습 정보 |
| Answer | 등급 바꾸기 | 확정 | 새로고침 | 설명 |
| Sheet | 다음 쪽 | 닫기 | 새로고침 | 다음 탭 |

The key identifiers remain internal to C enums and GPIO configuration.
`kanji_nav` exposes one pure availability query so the footer can omit controls
that would produce `KANJI_ACT_NONE`: reveal on a complete session, hint on a
card with no description, and next-page on a one-page sheet. The query reuses
the state machine's existing availability and page-count rules rather than
duplicating them in the UI. Adding this query does not change any button action
or screen type; description's reachable page indexes expand as specified.

## 8. Refresh behavior

The render and physical-refresh pipeline is unchanged. The implementation
changes the dock rectangle from the old full-width location to the new main
column location and updates the layout test and driver call together.

The driver already interprets its rectangle as half-open: it loops while
`x < x2` and `y < y2` and programs the controller through `x2 - 1`, `y2 - 1`.
The current `ui_kanji_dock_area()` instead returns inclusive maxima. Add a pure
`kanji_rect_to_half_open()` helper to `ui_kanji_layout.c`, make the UI accessor
delegate to it, and document the contract in both public headers. The host test
links and verifies the pure helper; the LVGL simulator verifies the public UI
accessor. For the approved dock, both must return `(112, 344, 632, 424)` and
cover all 520 × 80 pixels.

For every grade move the simulator captures the frame before and after, XORs
them, and verifies that every changed pixel lies inside the declared dock.
The existing partial-chain promotion to a full refresh remains unchanged.

## 9. Failure and overflow behavior

- Missing dynamic fields leave their fixed row blank.
- A short headword that the serif face cannot draw uses the multilingual
  fallback; it never renders a tofu box.
- A long headword uses the fallback and is never ellipsized.
- Fixed one-line metadata ellipsizes inside its rectangle.
- Only explicitly sized prose areas wrap.
- Offline and stale status never cover the headword or the primary action.
- `streak` and `reviewed_today` remain visible in the quiet masthead; queue
  position remains in the rail; new/review/retry counts remain in the question
  main column.
- A font generation or coverage mismatch fails before firmware compilation.

## 10. Verification

### Host logic

- Update `test_kanji_layout` for the 16/80/16/520 frame, footer, and dock.
- Add pure-layout tests that `kanji_rect_to_half_open()` converts
  `{x, y, w, h}` to `(x, y, x + w, y + h)` and that the dock produces the exact
  approved boundary; a one-pixel-short maximum fails.
- Preserve navigation behavior coverage while updating footer-label
  expectations, description page-count cases, and adding an availability
  matrix for every screen/page. Retain parsing, model, service, mock, and API
  behavior tests unchanged.
- Assert every layout rectangle stays within 648×480 and every partial-refresh
  X bound is byte aligned.
- Update the `KANJI_BODY_MAX` comment to describe the new 520 × 320 px prose
  page bound, while retaining its value and protocol behavior.

### Native simulator

- Maintain twenty canonical native 648×480 gallery shots: the existing sixteen,
  two additional description pages (`05b-description-hook` and
  `05c-description-parts`), plus `15-no-data` and `16-stale`. Rename the
  original `05-description` to `05-description-shape`.
- Replace assertions that require a filled header/player with assertions for
  the rail divider, quiet masthead, visible hero, footer actions, and
  paper-dominant question/complete states.
- Retain exact-one-selected-grade-cell and full glyph coverage checks.
- Inspect the RGB565 draw buffer before thresholding and require every pixel to
  lie on the neutral paper-to-ink coverage ramp; reject chromatic pixels. Walk
  the visible LVGL object tree separately and assert that configured text,
  background, border, and line colors use only the paper/ink tokens and that no
  opacity is used for hierarchy. This allows font edge coverage from LVGL's
  built-in Montserrat while still catching an authored gray or color. Retain
  the thresholded bitmap as the device-equivalence artifact; do not use that
  already-binary output to prove its own source palette.
- Add an ink-coverage ceiling of 30% for question and session-complete shots.
- Add a dock-diff containment assertion for at least three cursor positions.
- Assert that the public `ui_kanji_dock_area()` returns the same exact half-open
  bounds as the pure layout helper.
- Add three auxiliary captures, outside the canonical gallery count, for
  maximum-content shape, hook, and components pages. The two prose fields each
  hold `KANJI_BODY_MAX - 1` bytes, including a worst-width single-byte fixture,
  and all three part rows use their field limits. Verify the generated body's
  maximum glyph advance, normalize whitespace, measure each prose page with
  LVGL, assert its natural height is at most 320 px, and assert every component
  row stays on its page.

### Firmware

- Build for ESP32-S3 with the 8 MiB factory partition.
- Require at least 512 KiB application-partition headroom.
- Preserve the existing PSRAM and DMA framebuffer allocations.

### Physical panel

- Compare serif and sans hero output at 50, 70, and 100 cm.
- Confirm the primary action is readable at 70 cm.
- Walk all four grade states through repeated partial refreshes and inspect
  ghosting before and after automatic full-refresh promotion.
- Confirm the physical key order matches the on-screen `1 / 2 / 3 / i` legend
  in the intended enclosure.

## 11. Completion criteria

The redesign is complete when all host and simulator tests pass, firmware
builds with required partition headroom, all twenty canonical shots and the
three auxiliary description captures follow `DESIGN.md`, no implementation
identifier is visible to the learner, and
physical review confirms a legible hero and stable grade dock. Physical review
may choose sans over serif without reopening the approved layout or interaction
design.
