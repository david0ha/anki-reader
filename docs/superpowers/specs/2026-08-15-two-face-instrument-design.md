# Two-face instrument — design record

Supersedes the five-screen routing of
[2026-08-14-lexicographic-instrument-ui-design.md](2026-08-14-lexicographic-instrument-ui-design.md).
The offline catalog and study-state work of
[2026-08-15-offline-study-catalog-design.md](2026-08-15-offline-study-catalog-design.md) is kept
whole; this record extends it with an on-device scheduler and replaces everything above the model.

## Why

The board is a **picture frame that happens to teach**. It stands on a desk all day and is looked
at far more often than it is pressed. Two things follow, and the shipped UI honours neither:

1. **A frame that is 40% blank is furniture that failed.** The five-screen UI reserves an 80 px
   left rail (`RAIL_W 80`) to carry a badge and the string `35/60`, then draws its content into
   `MAIN_W 520` of the 648 available and fills perhaps two thirds of that. Measured on the shipped
   simulator output, the question face inks **2.55%** of the panel.
2. **Study time is not browsing time.** 유래, 구성요소 and the FSRS numbers each live behind their
   own button-paged sheet (`ui_sheet_desc`, `ui_sheet_comments`, `ui_sheet_fsrs`), so seeing what a
   character is made of costs three presses and three full refreshes — nine seconds of a panel
   strobing to read two lines that would have fit beside the senses all along.

The remedy is not more screens. Every field those sheets page through is **already in `kanji_t`**
— `description`, `hook_body`, `composition`, `parts[]`, `examples[]`, `fsrs{}`, `preview{}`. This
is a layout problem wearing a navigation problem's clothes.

## The shape

Two faces, and nothing else.

| Face | What it is | What it holds |
|---|---|---|
| **문제** | an art print | the headword alone, a Japanese example set as a pull-quote, and the learner's own history with this card |
| **정답** | a dictionary spread | everything — reading, senses, 성립, 구성, 예문, FSRS, and the four ratings |

### The front is spoiler-bound

The front may print **only Japanese and the learner's own history**. Never `senses`, never
`parts[].meaning`, never `examples[].gloss` — each of those is Korean, and each is the answer.
This is a rule a test enforces, not a habit: `examples[].gloss` in particular reads as innocuous
context right up until it prints 우연히 만나다 under 会う.

What is left is richer than it sounds. `fsrs.state_label`, `reps`, `lapses`, `stability_days`
are the learner's own record with this exact character, they spoil nothing, and they are the
reason the frame is worth looking at when nobody is studying.

### Buttons: the four buttons *are* the four ratings

The board has four buttons and FSRS has four grades. The shipped UI spends three presses walking
a cursor around a dock; the mapping below spends none.

| Button | 문제 | 정답 |
|---|---|---|
| KEY0 | 뜻 보기 | **다시** |
| KEY1 | 뜻 보기 | **어려움** |
| KEY2 | 새로고침 · hold 5 s → Wi-Fi 설정 | **보통** · hold 5 s → Wi-Fi 설정 |
| BOOT | 뜻 보기 | **쉬움** |

Left-to-right across the physical row this is Anki's canonical again → hard → good → easy, and
each dock cell prints its own button glyph, so the legend documents itself. The 5 s hold that
opens the portal is untouched: already-flashed boards answer to it and the docs quote it.

**Consequence: there is no partial refresh left on the board.** The dock cursor was the only one.
`test_kanji_layout.c` keeps asserting the dock rect is byte-aligned anyway — the assertion is free
and it keeps the invariant true if a partial is ever reintroduced.

Comments lose their sheet. They are the least valuable thing a frame can show and there is no room
for them beside the senses. `comments[]` stays in the model and on the wire; only the screen goes.

## The grid

648 × 480. Margin 20 on all four sides — up from 16, because the rail is gone and the measure no
longer needs stealing from the edge. Content 608 × 440.

Two columns, and the arithmetic that makes them a rule rather than a taste:

```
   20  +  392  +  16 │ 1 │ 15  +  184  +  20   =  648
  edge    left    gutter rule      right   edge
```

A CJK glyph is full-width, so a column's measure in characters is `w / font_px`:

| column | px | KR 16 | KR 20 | KR 28 |
|---|---|---|---|---|
| left | 392 | 24 자 | 19 자 | 14 자 |
| right | 184 | 11 자 | 9 자 | 6 자 |

Korean and Japanese prose read comfortably at 25–35 characters. **The left column at 16 px is the
only measure on this panel that sets prose**; the right column is for figures and two-word rows,
which is what makes the right column a *rail* rather than a second column of text. That constraint
is the design: 성립 and 예문 go left, 읽기 · 구성 · 기억 go right.

## The scale

No new face. The four that exist are a 1 : 1.25 : 1.75 : 3.5 scale and that is enough.

| Role | Face | Used for |
|---|---|---|
| eyebrow | KR 16 + 2 px tracking | `뜻 · MEANING`, `성립 · 成り立ち`, `예문 · れいぶん` |
| body | KR 16 | prose, examples, figures |
| row | KR 20 | reading rows, part rows, dock names, the plate |
| sense | KR 28 | the Korean senses — the one thing on the back read from across the room |
| hero | JP 56 | the headword |

The flash arithmetic behind "no new face": compiled bitmap data ≈ source bytes / 5.6, giving
jp56 ≈ 2.6 MB, kr28 ≈ 1.0, kr20 ≈ 0.65, kr16 ≈ 0.48 — 4.7 MB against a measured app image of
`0x56FA60` (5.70 MB), the balance being code. The app partition is 8 MB. A fifth face is 0.7–1.3 MB
of the 2.3 MB left, spent on a size the scale does not have a job for.

## The devices, and how loud each one is

There is no grey on this panel. Hierarchy is built from four things, listed loudest first, and
the discipline is in how rarely the top one is spent.

1. **One inverted block per screen.** On 정답 it is nothing at all until a grade is committed; on
   문제 it is the level badge. Two inverted blocks on one sheet and neither is read first.
2. **Scale** — 56 against 16 is a 3.5× step and does all the work the shipped UI asks a rail to do.
3. **Rule weight — exactly three: 1 px hairline, 2 px band, 3 px masthead.** A fourth weight is how
   a page grows a hierarchy the eye reads as a mistake.
4. **Tracking on the eyebrows only.** `ui_track()` adds 2 px of letter-spacing; applied to a caps
   label it separates the words, applied to Korean prose it takes it apart. Eyebrows only.

There are no cards, no borders, no radii and no fills beyond the badge. A section is
**eyebrow → hairline → content**, which is the whole vocabulary.

### 문제 — the plate

```
 0   ┌────────────────────────────────────────────────────────┐
     │ ▌N5▐  KANJIS · JLPT N5 어휘        연속 12 · 오늘 34    │  16, badge inverted
 40  ═════════════════════════════════════════════════════════   1 px
     │                                                        │
100  │                      会 う                             │  JP 56, centred
     │                                                        │
180  │              ────────────  ◆  ────────────             │  hairline + ornament
     │                                                        │
215  │                    「出会う」                           │  KR 28, centred, ≤2 lines
     │                                                        │
300  │                  단계  │  복습                          │  KR 20, label right-aligned,
     │                  반복  │  5회                           │  value left, 1 px vrule
     │                  안정  │  9일                           │
     │                  실패  │  1회                           │
418  ═════════════════════════════════════════════════════════   1 px
     │ 새 7 · 복습 18 · 다시 2                     뜻 보기 →   │  16
480  └────────────────────────────────────────────────────────┘
```

The plate is the serendipity print's `ORIGIN | USAGE | NOTE` block: right-aligned labels, a
vertical hairline, left-aligned values. It is what fills the lower third without saying anything
the learner is meant to be recalling.

Degradation, in order: no example → the hero rises to the optical centre and the ornament rule
moves with it; no FSRS history (a new card) → the plate prints `새 카드` as a single centred row
rather than four rows of `—`.

### 정답 — the spread

```
 0   ┌────────────────────────────────────────────────────────┐
     │ 会う                    あう          N5 · 복습 · 9일 뒤 │  JP 56 / KR 20 / KR 16
 96  ═════════════════════════════════════════════════════════   2 px band rule
     │ LEFT  x=20  w=392            │  RIGHT  x=444  w=184     │
     │ 뜻 · MEANING          eyebrow │  읽기 · よみ      eyebrow │
     │ 만나다, 대면하다,        KR 28 │  음독   カイ        KR 20 │
     │ 우연히 만나다                 │  훈독   あう              │
     │ ───────────────────── 1 px   │  ─────────────────  1 px │
     │ 성립 · 成り立ち       eyebrow │  구성 · つくり     eyebrow │
     │ 会는 사람들이 모여      KR 16 │  人  모이다         KR 20 │
     │ 서로 말하고 교류하는…         │  云  말하다               │
     │ 人 + 云 = 会           KR 20 │  ─────────────────  1 px │
     │ ───────────────────── 1 px   │  기억 · FSRS      eyebrow │
     │ 예문 · れいぶん       eyebrow │  반복   5회         KR 16 │
     │ 01 出会う  であう      KR 16 │  실패   1회               │
     │    우연히 만나다              │  안정   9일               │
     │ 02 出会い  であい             │  난이도  47%              │
412  ═════════════════════════════════════════════════════════   1 px
     │ ① 다시  │ ② 어려움 │ ③ 보통  │ ⓑ 쉬움  │                  KR 20 name
     │ 10분 뒤 │  4일 뒤  │  9일 뒤 │ 21일 뒤 │                  KR 16 span
480  └────────────────────────────────────────────────────────┘
```

Budget: left column y = 100…412 is 312 px against ≈286 px of content; right column ≈298 px. Both
fit with a line to spare, and both are *checked*, not asserted by eye — see below.

### The 831-byte problem

`description` is capped at 831 bytes and the measured catalog's longest is 819. The 성립 block is
392 px wide at 16 px — 24 characters a line, four lines, ≈96 characters. **The full explanation
does not fit and never will.** The shipped simulator already fails on exactly this
(`FAIL shape max: natural height 500 exceeds the 320 px prose page`) on a sheet that had 320 px to
give it; this design gives it 80.

So the block **ellipsizes deterministically at a character boundary**, and that is a design
decision rather than a defect: a learner reading a frame wants the shape of the story, and the
sentence that carries it is the first one. Squeezing 819 bytes into 80 px by any other means
produces either a clipped descender or a wall of 12 px type nobody reads. The rule follows
wp_news's field-dropping ladder — **drop whole units rather than squeezing all of them** — and the
simulator asserts the block never overflows its rectangle for the catalog's worst case.

The same ladder governs the right rail: `parts[]` may carry six rows into a slot that holds three.
Three print; the rest are dropped, not shrunk.

## On-device FSRS

Today `fsrs{}` and `preview{}` arrive as strings the proxy already worded. Offline they must be
computed on the board, and the offline-catalog record's ruling — *"does not claim local FSRS due
scheduling — the board lacks a trusted power-off clock"* — is what this section revisits.

The ruling was right about the clock and wrong to conclude the scheduler was therefore impossible.
Scheduling needs to know **what day it is**, not what second, and there are three tiers of answer:

| Tier | Source | What the board may claim |
|---|---|---|
| `TRUSTED` | SNTP this boot | real due dates, real spans |
| `APPROXIMATE` | last SNTP epoch persisted to the state partition + `esp_timer` uptime since boot | spans, badged as approximate |
| `UNKNOWN` | never synced | intervals only (`9일 간격`), never a due date |

`UNKNOWN` is a real state with its own wording, not a lie with a fallback number. The board has no
RTC and this design does not pretend otherwise; what it refuses to do is discard the scheduler
because the clock is imperfect, when a day-granular scheduler tolerates hours of drift.

Five pure, host-testable pieces:

1. **`fsrs.c`** — FSRS-6 stability/difficulty update and interval from stability and desired
   retention. The parameter vector is transcribed from the backend **by reading the backend**, and
   a host test asserts parity against golden vectors produced by running the backend's own Python.
   Relaying eighteen weights through a summary is how a scheduler ends up subtly wrong forever.
2. **`kanji_clock`** — the three tiers above, with the anchor persisted in the state partition.
3. **`kanji_relative_due()`** — the contract's rounding table (곧 / N분 / N시간 / N일 / N개월 /
   N년) in C, matching `kanjis-front`'s `relativeDue()` exactly, because the wire and the local
   path must word the same span identically or a card changes its mind when Wi-Fi returns.
4. **State record extension** — stability, difficulty and due-epoch alongside the existing
   outcomes, reps, lapses and position, in the two-bank journal already shipped.
5. **Arbitration** — wire-supplied `fsrs`/`preview` win when the payload carries them; locally
   computed values fill in when it does not. One rule, one place.

## What this deletes

`ui_sheet_desc.c`, `ui_sheet_comments.c`, `ui_sheet_fsrs.c`, their layout struct
(`kanji_sheet_layout_t`), their nav states, `ui_sheet_band_*`, `ui_pager_set`, and the grade-cursor
walk with its partial refresh. `kanji_nav.c` goes from five screens and a cursor to two faces and
four direct grades.

## Verification

Beyond the five existing layers, this design adds assertions the shipped UI has no equivalent of:

- **`_Static_assert` on the grid.** The columns and gutters must sum to the panel width; the
  masthead, columns and dock must stack without overlapping; the dock must stay byte-aligned. wp_news
  proves these catch drift at compile time, which is where a layout fault is cheapest.
- **Ink coverage floor.** The question face currently inks 2.55%. The simulator asserts **≥ 8%** on
  both faces — the single number that would have caught this redesign's absence.
- **No-overlap walk.** No two non-blank labels may share paper. Pixels cannot catch black-on-black.
- **Column containment.** Nothing drawn in the left column may cross into the gutter rule, and
  nothing in the right column may cross the margin, for the catalog's worst-case card.
- **Spoiler assertion.** No Korean from `senses`, `parts[].meaning` or `examples[].gloss` may
  appear anywhere on the 문제 face.
