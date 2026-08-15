# Gloss fallback fix — full-card short meanings

## Finding and RED

The catalog projection copied only `back.meaning.gloss`. In the required live
database, 5,689 of 9,956 card templates have a null gloss while all 9,956 have
at least one nonempty sense. Those cards therefore reached the offline catalog
with an empty short meaning even though the source contained one.

The unit regression was written before the production change. It supplied a
null gloss, invalid/blank leading sense entries, and a padded first valid sense.
The focused run failed on the missing behavior:

```text
FAIL a missing short gloss uses the first nonempty normalized sense
  (got '', want '만나다')
FAIL a whitespace-only gloss is empty and falls back to a normalized sense
  (got ' \t ', want '만나다')
FAIL a nonempty explicit gloss wins after surrounding whitespace is removed
  (got '  모일 회\n', want '모일 회')
```

## Implementation

`project_card_content()` now trims and validates the explicit gloss and every
sense. A nonempty explicit gloss remains authoritative; otherwise the first
nonempty normalized sense becomes the short gloss. If neither exists, the
field remains empty. No content is synthesized. The existing projection still
excludes images, examples, comments, session state, and FSRS data, and the 財
golden continues to assert the explicit `재물 재` gloss independently from its
five senses.

The full catalog acceptance test also derives an independent gloss oracle
directly from each raw `back` JSON column. It does not call the production
projector. It checks all decoded records by stable id and includes a mutation
that blanks every encoded gloss, proving the oracle rejects the defect.

## GREEN evidence

```sh
KANJIS_SAMPLE=9956 \
KANJIS_DB=/Users/ggrrm/Documents/kanjis-backend/data/kanjis-backend.sqlite3 \
  python3 tools/test_kanji_server.py
```

Result: `kanji_server: 246 checks, 0 failures`, with all 9,956 raw rows
projected.

```sh
KANJIS_DB=/Users/ggrrm/Documents/kanjis-backend/data/kanjis-backend.sqlite3 \
  python3 tools/test_offline_catalog.py
```

Result: `ok: 10048 checks`; the independent oracle reported
`gloss_checks=9956 gloss_nonempty=9956` and the mutation was rejected.

```sh
python3 tools/test_mock_kanji_server.py
```

Result: `mock_kanji_server: 150 checks, 0 failures`.

An actual read-only database CLI generation with seed 0 and `--verify`
produced 10 decks, 9,956 cards, and 156 blocks. The image is 3,456,822 bytes,
well below the `0x770000` (7,798,784-byte) catalog partition ceiling; maximum
raw block size is 74,473 bytes against the 96 KiB guard. No repository catalog
artifact was written and no hardware was flashed.

`git diff --check` exits zero for all owned files.
