# 오미쿠지 — the fortune draw

Seven ranks, drawn at random, printed large in Hanja with a Hangul reading and one line of advice —
the shape of a paper omikuji slip from a shrine. Implemented in `components/fortune_core/omikuji.c`,
tested in `test/host/test_omikuji.c`.

## The ranks

Ordered worst → best, so the enum value doubles as a luck score:

| # | Hanja | 한글 | Default weight |
|---|-------|------|---------------|
| 0 | 大凶 | 대흉 | 5% |
| 1 | 凶   | 흉   | 15% |
| 2 | 小凶 | 소흉 | 15% |
| 3 | 吉   | 길   | 25% |
| 4 | 小吉 | 소길 | 20% |
| 5 | 中吉 | 중길 | 15% |
| 6 | 大吉 | 대길 | 5% |

The distribution is deliberately not uniform. A fortune that lands on 大吉 one time in seven stops
meaning anything; these weights are roughly the mix of a real shrine's box — mostly middling, the
extremes rare. All seven are configurable under **Fortune Board → Omikuji draw weights** in
menuconfig; they are relative and normalised by their sum, so they do not have to add to 100.

## Injected randomness

```c
void omikuji_draw_weighted(uint32_t (*rng)(void), const omikuji_weights_t *w, omikuji_result_t *out);
```

The randomness source is a parameter, not a global. On the device it is `esp_random`. In the tests
it is whatever the test needs:

- a **counting** source fed 0…99, which walks every bucket in order and proves each rank receives
  exactly its weight in draws — a boundary test that a real PRNG would only reach by luck;
- a **fixed** source pinned to the first and last values of the range, and to `0xFFFFFFFF`;
- a **seeded xorshift32** over 100,000 draws, asserting each rank lands within ±2 percentage points
  of its configured weight.

Degenerate inputs are covered too: a single non-zero weight makes that rank certain, and all-zero or
negative weights fall back to the defaults rather than dividing by zero.

`rng()` is uniform over 2³², so the modulo bias against a total of ~100 is about one part in 4×10⁷.
That is far below anything a fortune needs to care about, and it buys exact boundary tests.

## The text

Every user-visible string lives in `components/fortune_core/include/omikuji_messages.h`, in one
X-macro:

```c
X(OMIKUJI_DAIKICHI, "大吉", "대길", "바라던 일이\n이루어집니다")
```

Two things depend on that file being the single source of truth:

1. **`tools/gen_fonts.py` scrapes it** to derive the exact glyph set the subset fonts must contain.
   Edit a message without regenerating and the new syllable renders as a tofu box (□) — visible only
   once the firmware is on the glass. Keeping the strings here makes the font a mechanical function
   of the text. See [the font section of CLAUDE.md](../CLAUDE.md#working-rules).
2. **Line breaks are explicit, and budgeted in pixels.** Automatic word wrap would leave every break
   point at the mercy of font metrics, so `'\n'` is part of the copy. `test_omikuji.c` enforces the
   budgets mechanically — the horizontal `message` (2 lines × 8 codepoints), the vertical verses
   (per-column pixel math below), and the table values (exactly 2 syllables).

A rank now carries more than its message. The X-macro row is:

```c
X(id, hanja, hangul, message, haeseok, joeon, jae, sa, dae, geon)
```

`haeseok`(해석) and `joeon`(조언) are the 만세력 page's vertical verses; `jae/sa/dae/geon` are the
財運/事業/對人/健康 table values. A second table, `OMIKUJI_FLOW_TABLE`, holds the five 흐름 verses,
picked not by the draw but by the day stem's element (`saju_element_of_gan`) — so the verse block is
the meet of the calendar and the draw. `message` no longer renders on the device; it is kept for the
app's `/api/state`.

## On screen — the 만세력 slip

```
╔══════════════╗   double frame: 2px border, gap, 1px border
║  今 日 運 勢  ║   inverted masthead, 16px Bold Hanja
║ 2026. 8. 9(일)║   12px date line
║ ┌병┐ 中吉 ┌을┐║   pillar boxes 병오년/을묘일 (12px, vertical),
║ └년┘      └일┘║   grade 34px Bold — one size for every rank
║ ──────◆────── ║   rule with centre diamond
║   세로쓰기     ║   vertical verse, 12px, columns right→left and
║   [흐름][해석] ║   JUSTIFIED across the full width (6 cols → 18px
║   [조언]      ║   stride); inverted 2-glyph tags head each section
║  ⊕吉          ║   the seal closes the text below the last column
║ ┌財運┬事業┬…┐ ║   fortune table, headers inverted 12px Bold,
║ └안정┴순항┴…┘ ║   values 12px, columns run right→left
╚══════════════╝
```

The vertical verse is drawn by `ui_vtext.c` (LVGL has no vertical text): columns advance right to
left, glyphs top to bottom on a 13 px pitch, a space is a 5 px half-gap, and a section's inverted
tag costs 30 px of its first column. Those constants live in `omikuji_messages.h` as `MANSE_VERSE_*`
because the *test* uses the same numbers: every verse line must satisfy

```
13·glyphs + 5·spaces + (line 0 ? 30 : 0)  ≤  104   (the column height)
```

and every element×rank combination (5 × 7) must fit `MANSE_VERSE_MAX_COLS` columns. Copy that
overflows the panel fails `test_omikuji`, before anything renders.

## When it draws

- Once at boot.
- Again at local midnight, alongside the 일진 recalculation.
- On a short press of **USER**.
- On `POST /api/fortune/draw`.

Each of those is a full panel refresh (~2s, flashes) because the whole screen changes. The result is
not persisted across reboots — a power cycle draws a new slip, which seemed truer to the object than
restoring yesterday's.
