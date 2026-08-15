# Two-face instrument — implementation plan

Spec: [../specs/2026-08-15-two-face-instrument-design.md](../specs/2026-08-15-two-face-instrument-design.md)
Start HEAD: `71116da`

## Baseline, honestly

The tree was **not green** when this started, and both failures are load-bearing for the design:

| Suite | State at start |
|---|---|
| `test_kanji_catalog` | **FAIL** — an uncommitted RED test proves a failed block inflate leaves `_cached_block` naming the old block while `_raw_workspace` already holds half the new one. The next read returns a corrupt card and returns `true`. |
| `sim/kanji_sim.sh` | **FAIL ×2** — a max-length `description` measures 500 px against a 320 px prose page. |
| everything else | pass (8/9 host, provisioning, user_app, both Python suites) |

The simulator failure is not incidental: it is the shipped UI admitting it cannot hold the
catalog's longest explanation in the space it reserved. The new design gives that block 80 px
rather than 320 and therefore has to answer the question properly — see "the 831-byte problem"
in the spec.

## Order of work

Written by hand, before any delegation, because these three are the design and delegating them
would have been delegating the judgment:

1. `include/ui_kanji_layout.h` + `ui_kanji_layout.c` — the whole page as pure integers, with
   `_Static_assert`s for the horizontal sums, the vertical stacking, the plate's optical
   centring, the dock's tiling and its byte alignment. The first compile caught a real error:
   `24 + 384 + 16 + 184 = 608`, not 624 — the gutter is 32 (16 + rule + 15), not 16.
2. `include/kanji_nav.h` + `kanji_nav.c` — five screens, three sheets, page indices and a grade
   cursor collapse to two booleans.
3. `include/ui_strings.h` — the eyebrow vocabulary, verified against `gen_fonts.symbol_set()`
   before use. All new strings are covered by the shipped faces; **no font regeneration**.

Then two workflows, fanned out over disjoint file sets:

**Foundations** (independent of the UI entirely)
- the catalog cache-invalidation fix
- `kanji_fsrs.c` — FSRS-6 transcribed from the backend's own py-fsrs 6.3.1, with golden vectors
  produced by *running* that Python rather than by hand
- `kanji_clock.c` + `kanji_relative_due()` — three honest tiers, and the contract's rounding table
- the study-state record gains stability, difficulty and a due epoch, as fixed point, behind a
  schema bump that rejects an old image cleanly
- `gen_offline_catalog.py` gains `examples` — without which the front's pull-quote and the
  answer's 예문 block are empty on every offline card

**Render**
- `ui_track()` / `ui_eyebrow()` and the pruning of the sheet API
- `ui_card_front.c` (the plate) and `ui_card_back.c` (the spread)
- `test_kanji_layout.c` and `test_kanji_nav.c` rewritten against the new contracts
- `ui_kanji.c` router, the three sheet files deleted, every CMakeLists corrected
- the simulator rewritten to assert what this redesign is *for*: an ink floor, a no-overlap
  walk, column containment, and the spoiler check

## Rulings

- **The four buttons are the four grades.** The board has four buttons and FSRS has four
  ratings; spending three presses walking a cursor was the shipped design's largest single
  cost. If wrong, the dock needs a cursor back and with it the three-press commit.
- **The front may print only Japanese and the learner's own history.** `examples[].gloss` reads
  as harmless context and is the answer. Enforced by a simulator assertion, not by care.
- **The 성립 block prefers `hook_body` over `description`.** Measured over the catalog,
  `hint.reason` averages 54–62 characters on kanji cards and fits the 3×24-character slot;
  `shape_explanation` averages 111–137 and runs to 371 and does not. This is arithmetic, not
  taste. If wrong, the block needs a page turn and the design is back to five screens.
- **No fifth font face.** The four cost ~4.7 MB of an 8 MB app partition; the scale
  16/20/28/56 has no gap a fifth would fill.
- **On-device FSRS runs unfuzzed.** The backend fuzzes intervals with a RNG. The board must not:
  the preview a learner reads has to be the interval they get, and a deterministic scheduler is
  one a host test can pin exactly. The backend's own preview path also disables fuzzing.
- **`KANJI_CLOCK_UNKNOWN` is a real state.** The offline-catalog record declined local
  scheduling because the board has no trusted power-off clock. That was right about the clock
  and wrong to conclude the scheduler was impossible: scheduling needs to know what *day* it is.
  A board that has never synced says so rather than scheduling against the epoch.
- **Comments lose their screen, not their model.** They are the least valuable thing a frame can
  show and there is no room beside the senses. `comments[]` stays on the wire.

## What deletion looks like

`ui_sheet_desc.c`, `ui_sheet_comments.c`, `ui_sheet_fsrs.c`, `kanji_sheet_layout_t`,
`kanji_chrome_t`'s rail, `ui_sheet_band_*`, `ui_pager_set`, the sheet enums, the page indices,
the grade cursor, and roughly 30 now-dead strings.

## Integration surface

Small, and known before the fan-out: `user_app.cpp:355-357` (a sheet-change comparison),
`user_app.cpp:561` (`KANJI_ACT_DRAW_DOCK`, which survives but now means "acknowledge a press"),
`test_study_source.c:330-354`, and a stale `0..4` range comment in `device_api_model.h`.
