---
version: alpha
name: Lexicographic Instrument
surface: firmware-display
resolution: 648x480
physical_size_mm: 119.2x88.3
color_depth: 1-bit
theme: light
---

# Lexicographic Instrument

> A Japanese dictionary proof mounted like a precision desk instrument. The
> headword is read first from 70–80 cm; the next physical action is read second.
> It is a fixed sheet of electronic paper, never a miniature website.

This document is the visual contract for the 5.83-inch LVGL firmware display.
It does not govern the React Native companion app or the provisioning web page.

## Subject, audience, and job

- **Subject:** one Japanese vocabulary card and the learner's progress through it.
- **Audience:** one learner who keeps the board on a desk and usually sees it
  from 50–100 cm away.
- **Single job:** make the current headword beautiful and immediately legible,
  then make the next physical action unambiguous.
- **Default state:** the question screen itself is the ambient composition.
  There is no separate idle, wallpaper, clock, or dashboard screen.

## Tokens — color

The four semantic roles deliberately collapse to two physical colors.

| Role | Value | Use |
|---|---|---|
| Paper | `#FFFFFF` | Canvas and all reading surfaces |
| Ink | `#000000` | Text, rules, icons, and progress marks |
| Selected surface | `#000000` | The current FSRS choice and short status stamps |
| Selected text | `#FFFFFF` | Text punched out of a selected surface |

No gray, opacity hierarchy, shadow, gradient, paper texture, or decorative
dither is allowed. The panel supplies the material character itself.

## Tokens — typography

| Role | Face | Size | Use |
|---|---|---:|---|
| Headword display | Noto Serif CJK JP Semibold, 1 bpp | 56 px | Short Japanese headwords only |
| Headword fallback | Existing Noto Sans KR/JP Medium, 1 bpp | 28 px | Long or unsupported headwords |
| Title | Existing Noto Sans KR/JP Medium, 1 bpp | 28 px | Meanings and sheet titles |
| Action / heading | Existing Noto Sans KR/JP Medium, 1 bpp | 20 px | Reading, primary actions, section headings |
| Reading body | Existing Noto Sans KR/JP Regular, 1 bpp | 16 px | Close-range prose on auxiliary sheets only |
| Utility figures | Montserrat | 18 px | Session position and compact Latin numerals only |

The display serif is the one expressive risk in the system. It is never used
below 48 px. If its counters close or hairlines disappear on the real panel,
the headword returns to Noto Sans without changing the layout.

Important information and button actions never use the 16 px face. Multiline
copy is left-aligned. Headwords are never ellipsized.

## Tokens — geometry

- Base unit: `8 px`
- Outer edge: `16 px`
- Index rail: `80 px`
- Rail-to-content gutter: `16 px`
- Main column: `520 px`
- Rules: `1 px` for separation, `2 px` for active structure
- Corner radius: `0 px`
- Footer control strip: `40 px`
- FSRS dock: `x=112`, `y=344`, `w=520`, `h=80`

All geometry is absolute and derives from the tested layout structs. Content
changes do not reflow surrounding regions. Any rectangle used for a physical
partial refresh begins and ends on an 8-pixel X boundary. Refresh rectangles
use half-open coordinates: `[x1, x2) × [y1, y2)`.

## Signature — the dictionary index rail

Every primary screen has a narrow left rail separated by one vertical rule.
It carries two fixed blocks of real information:

- identity: level or card state; an offline, demo, or stale stamp replaces this
  block when the exceptional state matters more;
- progress: queue position on question/answer, or page position on a paged
  reading sheet.

The rail is not decoration. It never contains fake folio numbers, coordinates,
timestamps, calibration marks, or labels such as `SYSTEM` and `LIVE`.

## Components

### Quiet masthead

The wordmark is small black type on paper, never a full-width inverted band.
The existing `streak` and `reviewed today` values remain visible as one quiet
line: `연속 12 · 오늘 34`. Network state appears only as the exceptional rail
stamp described above. Battery state is omitted while healthy and appears in
the masthead only when it requires attention.

### Word specimen

The headword owns the main column and a generous white field. On the question
screen, meanings and reading remain hidden. The invitation to reveal is plain
text, not a pill or card.

### Answer composition

The headword remains in the same family and position. Reading, meaning, and up
to three examples occupy fixed rows below it. Missing rows stay empty; they do
not cause other regions to move.

### Grade dock

Four stable, equal cells show `다시 / 어려움 / 보통 / 쉬움` and their preview
spans. Exactly one cell is black. Moving the choice updates only the dock's
byte-aligned partial-refresh rectangle.

### Reading sheet

설명, 댓글, and FSRS use the same rail and main column. Titles use 28 or 20 px;
long prose may use 16 px because these screens are intentionally read up close.
There are no inverted title bands, floating cards, shadows, or centered prose.
The description sheet uses up to three semantic pages: shape, memory hook, and
components. Empty sections do not create pages. A prose page reserves at least
320 px of body height in the 520 px main column and normalizes embedded
whitespace to single spaces. Text at the model's byte limit is never clipped to
protect the composition.

### Physical control legend

The footer names user actions, not GPIO or firmware identifiers. The visible
keycaps are `1`, `2`, `3`, and `i`; the UI never prints `KEY0` or `BOOT`.
The same physical action uses the same wording in the in-body prompt and
footer. `힌트 → 설명` and `학습 정보 → FSRS` are deliberate action-to-sheet-title
pairs; neither exposes a GPIO name.
An action that cannot do anything for the current card or page is omitted; the
screen never advertises a dead physical control.

## Screen recipes

### Question

- Paper-dominant composition with no full-screen black field.
- Rail: level or exceptional state, then queue position.
- Main: quiet streak/today line, headword, `정답 보기`, and the compact
  `새 5 · 복습 18 · 다시 2` remaining-count line.
- Footer: `1 정답 보기`, `2 힌트`, `3 새로고침`, `i 학습 정보`.

### Answer

- Rail and headword remain visually continuous with the question.
- Main: reading, meanings, and three fixed example rows.
- Bottom: prompt followed by the stable grade dock.
- Footer: `1 등급 바꾸기`, `2 확정`, `3 새로고침`, `i 설명`.

### Description / comments / FSRS

- Rail identifies the card and the current sheet.
- Main content is left-aligned and page-stable.
- Footer: `1 다음 쪽`, `2 닫기`, `3 새로고침`, `i 다음 탭`.
- A pager is hidden when only one page exists.

### Complete, offline, and setup

- Session complete uses the question composition on paper; it never becomes a
  black end card. Its footer omits reveal and hint actions that have no card to
  operate on.
- Offline preserves the last card and adds a short rail stamp.
- Setup is an opaque paper screen with one strong top rule, a title, and direct
  connection instructions.

## Motion and refresh

There is no animation. Rendering and presentation remain separate. New data,
reveal, screen changes, and sheets use a full panel refresh. Only movement of
the grade cursor uses a partial refresh. Unchanged content never touches the
panel.

## Copy

- Use Korean action verbs the learner recognizes: `정답 보기`, `힌트`, `확정`,
  `닫기`, `새로고침`.
- Do not expose implementation vocabulary such as `KEY0`, `BOOT`, `GPIO`,
  `partial`, or `refresh chain`.
- Keep `FSRS` only where the algorithm itself is being explained; the everyday
  control label is `학습 정보`.
- Errors state what remains visible and what action is available.

## Do

- Reserve expressive typography for the Japanese headword.
- Treat white space as the primary material of the question screen.
- Keep all action and grade geometry spatially stable.
- Let structure encode real card, session, or navigation information.
- Validate every state with real multilingual fixture data at native size.

## Do not

- Do not reproduce a web header, Shorts action rail, dashboard mosaic, or
  mobile card stack.
- Do not use gray, shadow, transparency, rounded cards, pills, or gradients.
- Do not add ornamental numbering, rulers, proof marks, or telemetry.
- Do not use full-screen black for question, complete, offline, or errors.
- Do not shrink critical actions below 20 px or center multiline prose.

## Acceptance

- At 70 cm, a five-second glance identifies the headword, screen state, and
  next primary action.
- At 1 m, a short headword remains identifiable.
- Every rendered pixel is black or white after binarization.
- The shortest and longest fixture strings preserve fixed geometry.
- Maximum-length shape and hook paragraphs remain fully visible on their prose
  pages, and all three component rows remain visible on the components page.
- Grade movement changes no pixel outside the dock rectangle.
- Repeated partial updates retain a clear selected/unselected distinction.
- Firmware and simulator use the same fonts and threshold path.
