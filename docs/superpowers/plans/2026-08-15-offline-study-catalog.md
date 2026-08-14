# Offline Study Catalog Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Boot the ESP32-S3 directly into any of 9,956 locally stored cards, persist every four-grade outcome and the current card across power loss, and keep the existing remote path intact.

**Architecture:** A deterministic Python exporter writes compact card-only JSON envelopes into 64-card zlib blocks in a raw read-only partition. Portable C validates and decodes one block at a time through injected I/O, while a separate two-bank append journal persists local rating summaries; an ESP adapter joins both to `user_app` before Wi-Fi starts.

**Tech Stack:** Python 3 standard library (`sqlite3`, `json`, `zlib`), C11, zlib 1.3.2, ESP-IDF 5.4.3, ESP32-S3 raw partitions, CMake host tests, LVGL 9.5 existing UI.

**Spec:** [`../specs/2026-08-15-offline-study-catalog-design.md`](../specs/2026-08-15-offline-study-catalog-design.md)

## Global Constraints

- Work only in the existing `codex/lexicographic-instrument-ui` worktree; do not touch `main` or the sibling backend/frontend repositories.
- Open the source SQLite database with URI `mode=ro`, set `PRAGMA query_only=ON`, and hold one read transaction. Never use `immutable=1` against its WAL-mode database.
- Export exactly one user's common card contents: all 10 decks and all 9,956 cards when the audited database is present. Never serialize comments, media, sessions, user settings, remote FSRS state, or duplicated study-card rows.
- Preserve full measured source data: body buffers 832 bytes including NUL, five senses, six parts, 144-byte reading/gloss/sense buffers, and a 96-byte composition buffer.
- Use a deterministic per-deck SHA-256 ordering followed by global round-robin interleaving. Never call `random`, `ORDER BY random()`, or use process-randomized hashes.
- Catalog image constants are fixed: 128-byte header, schema 1, 64 cards per block, 96 KiB maximum raw block, 12-byte card index, 16-byte block index, zlib-wrapped DEFLATE level 9, catalog partition limit `0x770000`.
- Partition layout is fixed: `catalog 0x40/0x00 @ 0x810000 size 0x770000 readonly`; `study_state 0x41/0x00 @ 0xF80000 size 0x080000`.
- State format is fixed: two 256 KiB banks, 64-byte bank header, 20-byte records, CRC32, commit marker written last, and no state-partition image in normal flashing.
- Offline v1 persists rating, repetitions, lapses, and current/next ordinal. It does not invent FSRS due dates, stability, or a trusted clock.
- A failed decode or failed state write never replaces the last valid card and never advances the study position.
- `UiTask` remains the only task that touches LVGL or starts an e-Paper refresh.
- Existing remote fetch/grade and stale-response behavior must remain backward compatible.
- Every production behavior follows RED → observed expected failure → GREEN → full focused suite. Tests assert behavior, not source text.

---

### Task 1: Full-fidelity card model and shared projection

**Files:**
- Modify: `components/vault_core/include/kanji_model.h`
- Modify: `components/vault_core/kanji_model.c`
- Modify: `components/vault_core/kanji_parse.c`
- Modify: `components/vault_core/kanji_mock.c`
- Modify: `components/vault_core/test/host/test_kanji_model.c`
- Modify: `components/vault_core/test/host/test_kanji_parse.c`
- Modify: `components/vault_core/test/host/test_kanji_mock.c`
- Modify: `tools/kanji_server.py`
- Modify: `tools/test_kanji_server.py`
- Modify: `docs/kanji-contract.md`

**Interfaces:**
- Produces `kanji_source_t { KANJI_SOURCE_NONE, KANJI_SOURCE_CATALOG, KANJI_SOURCE_REMOTE, KANJI_SOURCE_DEMO }` on `kanji_t`.
- Extends `kanji_card_t` with `gloss`, `on_reading`, `kun_reading`, and `composition`.
- Produces Python `project_card_content(card)`, `raw_card_parts(hint)`, and `safe_composition(front, composition_kanji, parts)` without display clipping.
- Preserves `flatten_card()` as the network/session envelope builder using the shared projection.

- [ ] **Step 1: Write failing C model and parser tests**

Add literal assertions that the public constants are `KANJI_READING_MAX=144`,
`KANJI_SENSE_MAX=144`, `KANJI_BODY_MAX=832`, `KANJI_FORMULA_MAX=96`,
`KANJI_SENSES_MAX=5`, and `KANJI_PARTS_MAX=6`. Parse one card-only envelope:

```json
{"v":1,"card":{"id":"wealth","front":"財","reading":"ザイ・サイ","on_reading":"ザイ・サイ","kun_reading":"","level":"N2","gloss":"재물 재","senses":["재물","재산"],"description":"財 = 貝 + 才","hook_title":"형성","hook_body":"재물과 재능을 함께 기억한다","composition":"貝 + 才 = 財","parts":[{"glyph":"貝","meaning":"재물","reading":"カイ"},{"glyph":"才","meaning":"재능","reading":"サイ"}]}}
```

Assert every new field exactly, `source == KANJI_SOURCE_REMOTE`, and a
parse failure leaves a sentinel output byte-identical. Add a 819-byte UTF-8-safe
description, 615-byte mnemonic, five senses, and six parts fixture that reaches
the model without truncation. Mutating any new field must change `kanji_hash()`.

- [ ] **Step 2: Run the focused C tests and observe RED**

```bash
cmake -S components/vault_core/test/host -B /tmp/offline-host
cmake --build /tmp/offline-host --target test_kanji_model test_kanji_parse test_kanji_mock -j8
```

Expected: compilation fails because the new capacities, fields, and source enum
do not exist.

- [ ] **Step 3: Expand the model and parser minimally**

Use these exact declarations:

```c
#define KANJI_READING_MAX      144
#define KANJI_SENSE_MAX        144
#define KANJI_BODY_MAX         832
#define KANJI_FORMULA_MAX       96
#define KANJI_SENSES_MAX         5
#define KANJI_PARTS_MAX          6

typedef enum {
    KANJI_SOURCE_NONE = 0,
    KANJI_SOURCE_CATALOG,
    KANJI_SOURCE_REMOTE,
    KANJI_SOURCE_DEMO,
} kanji_source_t;
```

Add the four card buffers, add `kanji_source_t source` beside `valid/demo`,
parse optional wire fields with the existing bounded helpers, set successful
JSON parses to REMOTE, set the mock to DEMO, and hash all new fields and source.
Keep every old wire field optional and keep failure atomic.

- [ ] **Step 4: Run the C tests to GREEN**

```bash
cmake --build /tmp/offline-host --target test_kanji_model test_kanji_parse test_kanji_mock -j8
/tmp/offline-host/test_kanji_model
/tmp/offline-host/test_kanji_parse
/tmp/offline-host/test_kanji_mock
```

Expected: all report `ok` with no warning.

- [ ] **Step 5: Write failing Python projection tests**

Add literal `財`, compound, and okurigana cases. Required expectations:

```python
eq(safe_composition("財", "財", raw_parts),
   ("貝 + 才 = 財", [raw_parts[1], raw_parts[2]]),
   "single-kanji self reference is removed")
eq(safe_composition("勉強", "勉強", compound_parts)[0],
   "勉 + 強 = 勉強", "compound sub-radicals are removed")
eq(safe_composition("懲らしめる", "懲", okurigana_parts)[0],
   "徴 + 心 = 懲", "okurigana result is the constituent kanji")
```

Also assert the unbounded projection retains 819/615 bytes, five senses, six
parts, structured readings, source principle, and an empty missing component
reading. Assert no image/comment/example/session/FSRS key is present.

- [ ] **Step 6: Run the Python server tests and observe RED**

```bash
python3 tools/test_kanji_server.py
```

Expected: import or assertion failure for the three absent projection helpers.

- [ ] **Step 7: Implement the shared projection and safe equation**

Port the tested single-kanji/compound filtering from
`../kanjis-front/src/reels/toReelInfo.ts` without copying its unreliable
semantic-role or `会意` inference. `project_card_content()` parses source JSON
once and returns full strings/arrays. `flatten_card()` wraps that result with
the current session, preview, examples, and comments behavior and applies only
the now-expanded model byte budgets. A missing principle stays `""`.

- [ ] **Step 8: Run all host and proxy tests**

```bash
cmake --build /tmp/offline-host -j8
ctest --test-dir /tmp/offline-host --output-on-failure
python3 tools/test_kanji_server.py
python3 tools/test_mock_kanji_server.py
```

Expected: all existing and new tests pass.

- [ ] **Step 9: Document and commit the expanded contract**

Document the optional new fields, source semantics, and full-fidelity limits.

```bash
git add components/vault_core tools/kanji_server.py tools/test_kanji_server.py docs/kanji-contract.md
git commit -m "feat: preserve full offline card content"
```

---

### Task 2: Deterministic compressed catalog generator

**Files:**
- Create: `tools/gen_offline_catalog.py`
- Create: `tools/test_offline_catalog.py`
- Create: `tools/fixtures/offline_catalog.json`
- Modify: `.gitignore`

**Interfaces:**
- Produces `select_user(conn, explicit_user_id=None)`.
- Produces `load_source(conn, user_id)` and `balanced_cards(decks, seed)`.
- Produces `encode_catalog(decks, cards, partition_size) -> bytes` and `verify_catalog(image) -> CatalogManifest`.
- CLI accepts `--db`, `--user-id`, `--seed`, `--partition-size`, `--output`, `--verify`, and `--fixture-json`.
- Binary header magic is `b"KJCAT01\0"`, schema is 1, header is 128 bytes.

- [ ] **Step 1: Create the fixture and failing generator tests**

The fixture contains two decks and at least five cards including the literal
`財` projection. Tests create a temporary WAL-mode SQLite database with the
four source tables and two users, then assert the maximum-active-coverage user
is selected and an explicit valid user overrides it. An unknown user must fail.

Add behavior tests for byte-identical repeated output, different seed changing
order but not membership, round-robin deck balance, safe equations, excluded
keys, header fields, 64-card boundary indexing, CRC rejection, and partition
overflow. Expected byte offsets are hand-derived literals, never read from the
encoder's own constants.

- [ ] **Step 2: Run the generator tests and observe RED**

```bash
python3 tools/test_offline_catalog.py
```

Expected: import failure because `gen_offline_catalog.py` does not exist.

- [ ] **Step 3: Implement read-only source selection and ordering**

Open production input as:

```python
conn = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
conn.execute("PRAGMA query_only=ON")
conn.execute("BEGIN")
```

Choose the user ordered by active card count descending, active deck count
descending, then user id ascending. Join active `study_decks` to
`deck_templates` and `card_templates`; never read duplicated `study_cards`
content. Sort each deck by `sha256(seed_u64_le + b"\0" + deck_id + b"\0" +
card_id)`, then take one card from each non-empty deck in manifest order until
all decks are empty.

- [ ] **Step 4: Implement the exact image format**

Use this 128-byte little-endian header:

```text
0   magic[8]              8   schema u16          10  header_size u16
12  flags u32             16  used_size u32       20  deck_count u16
22  block_cards u16       24  card_count u32      28  block_count u32
32  deck_off u32          36  deck_len u32        40  card_index_off u32
44  card_index_len u32    48  block_index_off u32 52  block_index_len u32
56  data_off u32          60  data_len u32        64  seed u64
72  catalog_id[16]        88  source_sha256[32]   120 header_crc32 u32
124 tables_crc32 u32
```

`header_crc32` covers bytes 0..119. `tables_crc32` covers the contiguous deck,
card-index, and block-index sections. A card index is `<IIB3x>` and a block
index is `<IIII>` containing compressed absolute offset, compressed length,
raw length, and raw CRC32. Raw blocks concatenate canonical compact JSON
(`sort_keys=True`, separators `(',', ':')`, UTF-8) and are compressed with
`zlib.compress(raw, 9)`. Derive block/slot from ordinal `/64` and `%64`.

- [ ] **Step 5: Implement independent verification and atomic output**

`verify_catalog()` must repeat every bounds/count/CRC/zlib/index/JSON check and
return decoded counts/maxima. The CLI writes a temporary sibling file, fsyncs,
and replaces the destination only when bytes changed. It hard-fails above
`0x770000`, above 96 KiB raw block, or when the production DB does not yield
10 decks/9,956 unique cards. Fixture mode uses fixture counts instead.

- [ ] **Step 6: Run fixture tests and the full live-database proof**

```bash
python3 tools/test_offline_catalog.py
python3 tools/gen_offline_catalog.py \
  --db /Users/ggrrm/Documents/kanjis-backend/data/kanjis-backend.sqlite3 \
  --output /tmp/kanji-catalog.bin --partition-size 0x770000 --seed 0 --verify
python3 tools/gen_offline_catalog.py \
  --db /Users/ggrrm/Documents/kanjis-backend/data/kanjis-backend.sqlite3 \
  --output /tmp/kanji-catalog-2.bin --partition-size 0x770000 --seed 0 --verify
cmp /tmp/kanji-catalog.bin /tmp/kanji-catalog-2.bin
```

Expected: manifest reports 10 decks, 9,956 cards, 156 blocks, image below
`0x770000`, max raw block below 96 KiB, and `cmp` succeeds.

- [ ] **Step 7: Commit the generator**

Ignore only developer-selected output names outside `build/`; do not ignore
source or test fixtures.

```bash
git add .gitignore tools/gen_offline_catalog.py tools/test_offline_catalog.py tools/fixtures/offline_catalog.json
git commit -m "feat: generate compressed offline catalog"
```

---

### Task 3: Bounds-checked portable catalog reader

**Files:**
- Create: `components/vault_core/include/kanji_catalog.h`
- Create: `components/vault_core/kanji_catalog.c`
- Create: `components/vault_core/test/host/test_kanji_catalog.c`
- Modify: `components/vault_core/CMakeLists.txt`
- Modify: `components/vault_core/test/host/CMakeLists.txt`

**Interfaces:**
- Consumes the Task 2 header/index format and Task 1 `kanji_parse(json,len)`.
- Produces `kanji_catalog_open()`, `kanji_catalog_deck()`, and `kanji_catalog_read_card()`.
- Caller supplies read, inflate, CRC, compressed workspace, and raw workspace.
- `kanji_catalog_read_card()` changes `*out` only after complete success and sets source CATALOG.

- [ ] **Step 1: Add a generated fixture dependency and failing reader tests**

At host-test configure time, run Task 2 with `--fixture-json` into the build
directory. In C, load that image through a memory `read_at`. Assert header and
deck metadata, first/last cards, every ordinal adjacent to a 64-card boundary,
the `財` fields, and source CATALOG.

For each corruption copy the image, change exactly one independent field, and
assert a specific non-OK status: magic, version, header CRC, table CRC,
partition truncation, offset addition overflow, deck index, block offset,
compressed data, raw CRC, record offset, record length, malformed JSON, and
invalid UTF-8. Seed `kanji_t out` with `0xA5`; every failed card read must leave
all bytes unchanged.

- [ ] **Step 2: Run the reader target and observe RED**

```bash
cmake -S components/vault_core/test/host -B /tmp/offline-host
cmake --build /tmp/offline-host --target test_kanji_catalog -j8
```

Expected: compile failure because `kanji_catalog.h` is absent.

- [ ] **Step 3: Define the portable API and status enum**

```c
typedef bool (*kanji_catalog_read_fn)(void *, uint32_t, void *, size_t);
typedef bool (*kanji_catalog_inflate_fn)(void *, size_t *, const void *, size_t);
typedef uint32_t (*kanji_catalog_crc32_fn)(const void *, size_t);

bool kanji_catalog_open(kanji_catalog_t *cat,
                        const kanji_catalog_io_t *io,
                        uint32_t partition_size,
                        void *compressed_workspace, size_t compressed_capacity,
                        void *raw_workspace, size_t raw_capacity);
bool kanji_catalog_read_card(kanji_catalog_t *cat, uint32_t ordinal,
                             kanji_t *out);
```

Expose count/catalog-id/deck-info accessors, not internal offsets. Keep a
distinct status accessor so tests and logs can distinguish corruption from an
out-of-range ordinal.

- [ ] **Step 4: Implement checked little-endian parsing and one-block decode**

Never cast the image to a struct. Check `offset <= limit` and `length <=
limit-offset` before every read. Cache one decoded block id. Read its 16-byte
entry, ensure compressed/raw lengths fit caller workspaces, inflate exactly,
verify raw CRC, read the 12-byte card index, validate its deck and record span,
and call `kanji_parse(raw+offset,length,&temporary)`. Overlay deck name/level,
source CATALOG, and catalog ordinal only after parse; then assign `*out`.

- [ ] **Step 5: Run the reader and whole host suite**

```bash
cmake --build /tmp/offline-host -j8
ctest --test-dir /tmp/offline-host --output-on-failure
```

Expected: reader corruption matrix and all prior tests pass.

- [ ] **Step 6: Commit the reader**

```bash
git add components/vault_core/include/kanji_catalog.h components/vault_core/kanji_catalog.c \
  components/vault_core/test/host/test_kanji_catalog.c \
  components/vault_core/CMakeLists.txt components/vault_core/test/host/CMakeLists.txt
git commit -m "feat: decode indexed offline catalog"
```

---

### Task 4: Power-loss-safe rating journal

**Files:**
- Create: `components/vault_core/include/kanji_state.h`
- Create: `components/vault_core/kanji_state.c`
- Create: `components/vault_core/test/host/test_kanji_state.c`
- Modify: `components/vault_core/CMakeLists.txt`
- Modify: `components/vault_core/test/host/CMakeLists.txt`

**Interfaces:**
- Produces injected `kanji_state_io_t { read_at, write_at, erase_range, ctx }`.
- Produces `kanji_state_open()`, `kanji_state_current_ordinal()`, `kanji_state_summary()`, and `kanji_state_append_grade()`.
- Caller supplies `kanji_rating_summary_t summaries[card_count]`.

- [ ] **Step 1: Write a real NOR-flash fake and failing state tests**

The fake starts at `0xFF`, permits writes only from 1 to 0, requires 4 KiB
aligned erase ranges, supports a write-byte cutoff, and survives by constructing
a new `kanji_state_t` over the same bytes. Test:

- erased first boot creates card 0 with zero summaries;
- grades 1..4 survive reopen with saturating reps and Again-only lapses;
- invalid grade/ordinal and callback failure leave RAM/flash/current unchanged;
- a torn 20-byte record restores the previous current ordinal;
- two valid banks select the highest generation with wrap-safe comparison;
- power loss before bank commit retains the old bank;
- power loss after commit selects the new bank;
- forced compaction retains the latest summary of every reviewed card;
- a different 16-byte catalog id starts fresh state.

- [ ] **Step 2: Run the state target and observe RED**

```bash
cmake --build /tmp/offline-host --target test_kanji_state -j8
```

Expected: compile failure because the state API is absent.

- [ ] **Step 3: Implement the exact bank and record encodings**

Each 256 KiB bank begins with:

```text
0 magic[8]="KJSTATE1"  8 schema u16  10 header_size u16
12 generation u32      16 catalog_id[16]
32 bank_size u32       36 record_size u16  38 reserved u16
40 header_crc32 u32    44 reserved[16]     60 commit u32
```

CRC covers bytes 0..39 and commit is `0x434F4D4D`, written last. Each record is
exactly:

```text
0 seq u32  4 card u16  6 next u16  8 reps u16  10 lapses u16
12 grade u8  13 flags u8  14 reserved u16  16 crc32(bytes 0..15)
```

Scan until an erased sequence or first invalid/torn record. Apply a record to a
card only when its sequence is newer than that card's current summary; derive
the global next ordinal from the newest sequence in the bank.

- [ ] **Step 4: Implement append verification and two-bank compaction**

An append calculates a complete absolute summary, writes one record, reads it
back, validates CRC/content, and only then mutates RAM/current. Before a bank
fills, erase the inactive bank, write an uncommitted next-generation header,
write one latest record per reviewed ordinal, verify them, write commit last,
switch active bank, then erase the former bank. Leave the old committed bank
untouched on any pre-commit failure.

- [ ] **Step 5: Run state tests with sanitizers and full host suite**

```bash
cmake --build /tmp/offline-host -j8
ctest --test-dir /tmp/offline-host --output-on-failure
cmake -S components/vault_core/test/host -B /tmp/offline-host-san -DSANITIZE=ON
cmake --build /tmp/offline-host-san --target test_kanji_state -j8
/tmp/offline-host-san/test_kanji_state
```

Expected: all state and existing tests pass without ASan/UBSan findings where
the platform permits the sanitizer process.

- [ ] **Step 6: Commit the journal**

```bash
git add components/vault_core/include/kanji_state.h components/vault_core/kanji_state.c \
  components/vault_core/test/host/test_kanji_state.c \
  components/vault_core/CMakeLists.txt components/vault_core/test/host/CMakeLists.txt
git commit -m "feat: persist offline rating state"
```

---

### Task 5: ESP partition adapter and flash image integration

**Files:**
- Modify: `partitions.csv`
- Modify: `main/CMakeLists.txt`
- Create: `components/catalog_store/CMakeLists.txt`
- Create: `components/catalog_store/idf_component.yml`
- Create: `components/catalog_store/catalog_store.h`
- Create: `components/catalog_store/catalog_store.c`
- Modify: `tools/test_offline_catalog.py`
- Modify: `docs/board-hardware.md`

**Interfaces:**
- Produces `catalog_store_init()`, `catalog_store_available()`, `catalog_store_current()`, `catalog_store_ordinal()`, and `catalog_store_grade()`.
- Uses raw partition callbacks for the portable catalog/state cores.
- `catalog_store_grade()` persists first and returns the next card only on full success.

- [ ] **Step 1: Add failing partition and image-integration tests**

Extend the Python test to parse `partitions.csv` as values, not grep text. Assert
the two names, custom types/subtypes, offsets/sizes, read-only catalog flag, no
overlap, 4 KiB alignment, and exact `0x1000000` end. Invoke the generator on the
fixture with `0x770000` and assert the image fits.

- [ ] **Step 2: Run the partition test and observe RED**

```bash
python3 tools/test_offline_catalog.py
```

Expected: partition assertions fail because neither partition exists.

- [ ] **Step 3: Add the exact partitions and official zlib component**

Append:

```csv
catalog,     0x40, 0x00, 0x810000, 0x770000, readonly
study_state, 0x41, 0x00, 0xF80000, 0x080000,
```

The component manifest pins `espressif/zlib: ^1.3.2`. The component requires
`vault_core`, `esp_partition`, and `zlib` and does not require LVGL.

- [ ] **Step 4: Implement the ESP adapter**

Find exact named/type/subtype partitions, allocate compressed/raw decoder
workspaces and card summaries from PSRAM with a normal-heap fallback, and wire
`esp_partition_read/write/erase_range`. Initialize catalog first, then state
with the catalog id/card count, then decode the restored ordinal. Return false
without altering the current card on every failure. `catalog_store_grade()`
computes `(ordinal+1)%count`, decodes next into a temporary, appends/verifies
state, and publishes internal current only after both succeed.

- [ ] **Step 5: Register deterministic catalog generation with flash**

In `main/CMakeLists.txt`, define cache variables with these defaults:

```cmake
set(KANJI_CATALOG_DB "${CMAKE_SOURCE_DIR}/../kanjis-backend/data/kanjis-backend.sqlite3" CACHE FILEPATH "Read-only kanjis backend SQLite path")
set(KANJI_CATALOG_USER_ID "" CACHE STRING "Optional source user UUID")
set(KANJI_CATALOG_SEED "0" CACHE STRING "Deterministic catalog ordering seed")
```

Create `${CMAKE_BINARY_DIR}/kanji-catalog.bin` through a custom command that
runs every requested catalog target but replaces unchanged output atomically.
Create `catalog_image`, make normal `flash` depend on it, and register only the
catalog image with
`esptool_py_flash_to_partition(flash catalog "${CMAKE_BINARY_DIR}/kanji-catalog.bin")`. Never
generate or flash a `study_state` image. Add a `catalog-flash` target if the
installed IDF helper supports a partition-only target; otherwise document
`idf.py flash` as the supported full-catalog operation rather than inventing a
shell flash command.

- [ ] **Step 6: Verify pack, partitions, and component compilation**

```bash
python3 tools/test_offline_catalog.py
python3 tools/gen_offline_catalog.py \
  --db /Users/ggrrm/Documents/kanjis-backend/data/kanjis-backend.sqlite3 \
  --output /tmp/kanji-catalog.bin --partition-size 0x770000 --seed 0 --verify
```

Then, in an ESP-IDF 5.4.3 shell:

```bash
idf.py reconfigure
idf.py partition-table
idf.py catalog_image
```

Expected: the partition table ends at 16 MiB and the full image verifies.

- [ ] **Step 7: Document and commit the storage adapter**

```bash
git add partitions.csv main/CMakeLists.txt components/catalog_store \
  tools/test_offline_catalog.py docs/board-hardware.md
git commit -m "feat: add offline catalog partitions"
```

---

### Task 6: Offline-first boot and local grading

**Files:**
- Create: `components/user_app/study_source.h`
- Create: `components/user_app/study_source.c`
- Create: `components/user_app/test/test_study_source.c`
- Modify: `components/user_app/test/run.sh`
- Modify: `components/user_app/CMakeLists.txt`
- Modify: `components/user_app/user_app.h`
- Modify: `components/user_app/user_app.cpp`
- Create: `components/provisioning/prov_boot_policy.h`
- Create: `components/provisioning/prov_boot_policy.c`
- Create: `components/provisioning/test/test_prov_boot_policy.c`
- Modify: `components/provisioning/test/run.sh`
- Modify: `components/provisioning/CMakeLists.txt`
- Modify: `components/provisioning/provisioning.h`
- Modify: `components/provisioning/provisioning.c`
- Modify: `main/main.cpp`

**Interfaces:**
- Pure source policy captures `{source, grade, catalog_ordinal, remote_card_id}` and returns LOCAL or REMOTE route without consulting mutable current source later.
- `UserApp_TaskInit()` initializes catalog before spawning tasks and accepts an empty initial network config.
- Produces `UserApp_SetNetworkConfig()` and `UserApp_SetOverlay()` queue-based calls.
- Provisioning returns offline for no config or failed join; only consumed force-portal state enters the blocking portal.

- [ ] **Step 1: Write failing pure source-policy tests**

Cover catalog cold boot, catalog submit routing local, remote submit routing HTTP,
source transition after capture not changing the request route, remote success
takeover, remote failure preserving catalog, URL clear restoring catalog, demo
only when catalog unavailable, and failed local persistence preserving the
answer/current ordinal.

- [ ] **Step 2: Write failing provisioning-policy tests**

Use a pure decision function with literal matrix:

```c
CHECK_EQ(prov_boot_decide(false, false, false), PROV_BOOT_OFFLINE);
CHECK_EQ(prov_boot_decide(true,  false, false), PROV_BOOT_PORTAL);
CHECK_EQ(prov_boot_decide(false, true,  false), PROV_BOOT_OFFLINE);
CHECK_EQ(prov_boot_decide(false, true,  true),  PROV_BOOT_ONLINE);
```

The first boolean is forced portal, second saved config, third successful join.

- [ ] **Step 3: Run both component suites and observe RED**

```bash
sh components/user_app/test/run.sh
sh components/provisioning/test/run.sh
```

Expected: missing-header/source failures.

- [ ] **Step 4: Implement pure policy and captured grade requests**

Add explicit CATALOG/REMOTE/DEMO policy without duplicating the model enum.
Replace `s_pending_grade` with a single pending request holding captured source,
grade, catalog ordinal, and remote id. A second request remains rejected while
one is pending. The worker branches from captured source, never the source that
happens to be current when it wakes.

- [ ] **Step 5: Initialize and grade the local store before networking**

`UserApp_TaskInit()` calls `catalog_store_init()`, loads the restored card into
`s_data`, source/hash/nav, and only uses `kanji_mock()` when the store is invalid.
For a captured CATALOG grade, call `catalog_store_grade()` on the worker task;
on success copy the next card, reset navigation, and post
`APP_CMD_CARD_ADVANCED`; on failure clear the pending slot, log, and leave the
current answer untouched. Clearing the remote URL reloads
`catalog_store_current()`.

- [ ] **Step 6: Make provisioning offline-first without violating panel ownership**

Keep forced portal behavior. Change no-config and failed saved join to return
`false` instead of auto-starting the portal. In `app_main()` build UI and call
`UserApp_TaskInit()` before `provisioning_run()`, so the main task may wait for
a saved-network attempt while `UiTask` already displays/responds offline.
Replace `OnProvisioningEvent()` direct LVGL/panel calls with
`UserApp_SetOverlay()` commands. On successful join, post the config through
`UserApp_SetNetworkConfig()`, sync time, and start `device_api`; otherwise
return with local study tasks running. The existing KEY2 long-press NVS flag
remains the only portal entry.

- [ ] **Step 7: Run component, host, and simulator regressions**

```bash
sh components/user_app/test/run.sh
sh components/provisioning/test/run.sh
cmake --build /tmp/offline-host -j8
ctest --test-dir /tmp/offline-host --output-on-failure
cmake -S sim -B /tmp/offline-sim
cmake --build /tmp/offline-sim -j8
```

Expected: every suite passes and the simulator links the expanded model.

- [ ] **Step 8: Commit offline runtime behavior**

```bash
git add components/user_app components/provisioning main/main.cpp
git commit -m "feat: boot and grade from offline catalog"
```

---

### Task 7: Full-catalog and firmware acceptance

**Files:**
- Modify: `README.md`
- Modify: `docs/app-control.md`
- Modify: `docs/bring-up.md`
- Modify: `docs/esp-idf-development.md`
- Modify: `docs/simulator.md` only if commands changed

**Interfaces:**
- Documents catalog generation, user selection, full vs app-only flashing,
  offline boot, state preservation, portal long-press, and the deliberate lack
  of local wall-clock FSRS scheduling.

- [ ] **Step 1: Add a full-catalog decoder sweep command**

Extend `tools/test_offline_catalog.py` with an opt-in live-DB integration mode
that generates the image, verifies all 9,956 JSON envelopes and every formula,
and prints exact decks/cards/blocks/bytes/maxima. It must skip with a clear line
when the DB is absent and fail on any count/data mismatch when `KANJIS_DB` is
explicitly supplied.

- [ ] **Step 2: Run every host and Python verification from clean temp builds**

```bash
python3 tools/test_kanji_server.py
python3 tools/test_mock_kanji_server.py
python3 tools/test_offline_catalog.py
KANJIS_DB=/Users/ggrrm/Documents/kanjis-backend/data/kanjis-backend.sqlite3 \
  python3 tools/test_offline_catalog.py
cmake -S components/vault_core/test/host -B /tmp/offline-final-host
cmake --build /tmp/offline-final-host -j8
ctest --test-dir /tmp/offline-final-host --output-on-failure
sh components/user_app/test/run.sh
sh components/provisioning/test/run.sh
cmake -S sim -B /tmp/offline-final-sim
cmake --build /tmp/offline-final-sim -j8
```

Expected: zero failures and exact live counts.

- [ ] **Step 3: Run a fresh ESP-IDF build and size checks**

Load the repository's documented ESP-IDF 5.4.3 environment, then:

```bash
idf.py fullclean
idf.py reconfigure
idf.py partition-table
idf.py catalog_image
idf.py build
idf.py size
```

Use the produced `.bin` and partition CSV parser to assert the app image is at
most `0x780000` (512 KiB spare in the 8 MiB slot), catalog image at most
`0x770000`, and all partition endpoints exact. Do not flash physical hardware
without a separate user request.

- [ ] **Step 4: Update operator documentation**

Document:

- `KANJI_CATALOG_DB`, optional user id, seed, and deterministic selection.
- `idf.py catalog_image`, normal `idf.py flash`, and that `app-flash` preserves
  both the existing catalog and `study_state`.
- First boot immediately studies offline; KEY2 long-press enters Wi-Fi setup.
- A corrupt/missing catalog falls back to the built-in mock.
- Offline ratings persist locally but do not yet upload or calculate trusted
  FSRS due dates.
- Generated catalog images are local build artifacts and should not be
  redistributed without rights review.

- [ ] **Step 5: Commit verification and docs**

```bash
git add README.md docs tools/test_offline_catalog.py
git commit -m "docs: verify offline study workflow"
```

- [ ] **Step 6: Request whole-branch review**

Review the complete spec-to-HEAD diff for corrupt-input safety, flash wear and
power-loss behavior, task/panel ownership, accidental online regressions, and
test quality. Resolve every Critical/Important finding through a reviewed fix
round, then rerun Step 2 and Step 3 before reporting completion.
