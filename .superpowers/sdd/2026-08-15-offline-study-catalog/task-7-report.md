# Task 7 report — Full-catalog and firmware acceptance

## Scope and owned files

Task 7 added the opt-in full-database decoder sweep, closed the three deferred
minor test/documentation items, corrected offline-source operator guidance,
and resolved the full ESP-IDF build failure in project-owned configuration.
The existing untracked `managed_components` symlink was never edited, replaced,
removed, or staged. Generated catalogs and every build tree remained under
`/tmp`.

## RED evidence and root causes

### Managed LVGL examples / Montserrat 14

Using `/Users/ggrrm/esp/v5.4.3/esp-idf/export.sh` (`ESP-IDF v5.4.3-dirty`), a
fresh configure produced:

```text
CONFIG_LV_BUILD_EXAMPLES=y
CONFIG_LV_BUILD_DEMOS=y
# CONFIG_LV_FONT_MONTSERRAT_14 is not set
CONFIG_LV_FONT_MONTSERRAT_18=y
```

`managed_components/lvgl__lvgl/env_support/cmake/esp.cmake` globs every
`examples/*.c` when examples are enabled. The full build then failed at the
unused upstream example:

```text
managed_components/lvgl__lvgl/examples/styles/lv_example_style_21.c:127:39:
error: 'lv_font_montserrat_14' undeclared
```

The complete RED output is `/tmp/task7-idf-build-red.log`. The root cause was
not an application font dependency: LVGL 9.5 defaults its unused example/demo
source sets on for a non-minimal configuration, while this application
deliberately ships only Montserrat 18. Current LVGL documentation also exposes
`CONFIG_LV_BUILD_EXAMPLES` and `CONFIG_LV_BUILD_DEMOS` as the build switches.

The minimal fix is in `sdkconfig.defaults`:

```text
# CONFIG_LV_BUILD_EXAMPLES is not set
# CONFIG_LV_BUILD_DEMOS is not set
```

No dependency source was changed, and Montserrat 14 was not re-enabled merely
to make an unused example compile.

### `idf.py fullclean` and the protected symlink

Both the repository build directory and a brand-new external `-B` directory
reported the same component-manager failure after the build-directory clean:

```text
OSError: Cannot call rmtree on a symbolic link
.../lexicographic-instrument-ui/managed_components
```

Logs: `/tmp/task7-idf-fullclean-red.log` and
`/tmp/task7-idf-fullclean-external.log`. `idf.py fullclean` always invokes
`remove_managed_components` at the project root; Python refuses to recursively
remove this workspace-managed symlink. Per task ownership, the symlink was
preserved. The clean-build equivalent used a brand-new external build directory
and a brand-new external `SDKCONFIG`, so no object, generated config, catalog,
or CMake cache could be reused.

### Live sweep RED

The test first called the not-yet-defined integration function under an
explicit live database:

```sh
KANJIS_DB=/Users/ggrrm/Documents/kanjis-backend/data/kanjis-backend.sqlite3 \
  python3 tools/test_offline_catalog.py
```

It failed only for the missing feature:

```text
NameError: name 'run_live_database_sweep' is not defined
```

Log: `/tmp/task7-live-sweep-red.log`.

### State reserved-tail RED

The new test constructs a valid committed schema-1 bank, programs byte 44
outside the header CRC, and requires that unsupported header to be rejected.
Before the validator change it replayed the record and selected generation 7:

```text
got 1, want 0
CHECK(summary_is_zero(&summaries[0])) failed
got 7, want 0
3 state test failure(s)
```

Log: `/tmp/task7-state-reserved-red.log`. The implementation now validates
bytes 44–59 as erased (`0xFF`); the existing bytes 38–39 check remains, and the
first-boot encoding test independently asserts both exact reserved ranges.

### Hand-derived SHA-256 ordering mutation RED

The ordering test contains literal seed-7 key-material hex, digest prefixes,
and the exact interleaved order `a1,b0,a0,b1,a2`; it does not call a test-side
key builder. A temporary production mutation swapped deck id and card id in the
hash material. The test failed as intended:

```text
got ['a2', 'b0', 'a1', 'b1', 'a0'], expected ['a1', 'b0', 'a0', 'b1', 'a2']
```

Log: `/tmp/task7-sha-order-mutation-red.log`. Restoring the specified
`seed_u64_le + NUL + deck_id + NUL + card_id` order returned `ok: 71 checks`.

### Flash-metadata RED

The opt-in ESP-IDF metadata check was first invoked before its implementation:

```sh
IDF_BUILD_DIR=/tmp/offline-task7-idf-green.gfgFgA \
  python3 tools/test_offline_catalog.py
```

It failed with `NameError: name 'assert_idf_flash_metadata' is not defined`
(`/tmp/task7-flash-metadata-red.log`). The GREEN helper parses generated
`flasher_args.json` and `app-flash_args` as data, checks exact catalog
registration, rejects any state payload, and enforces both binary ceilings.

## Full live-database proof

The `KANJIS_DB` mode opens the supplied database read-only, selects the source
user using the production deterministic rule, generates an in-memory image,
runs the independent decoder/verifier, and compares every decoded card object
and every composition formula with the projected source in ordinal order. An
unset variable prints a clear skip line; an explicitly supplied missing or
mismatched database exits nonzero.

Observed GREEN against the required database:

```text
LIVE: decks=10 cards=9956 blocks=156 bytes=3441578 formulas=9956
deck_counts=kanji:N1=135,kanji:N2=245,kanji:N3=324,kanji:N4=181,kanji:N5=104,
vocab:N1=3205,vocab:N2=2561,vocab:N3=1516,vocab:N4=1008,vocab:N5=677
maxima={"composition_bytes":63,"compressed_block_bytes":23618,
"description_bytes":819,"front_bytes":30,"gloss_bytes":50,
"hook_body_bytes":615,"hook_title_bytes":6,"id_bytes":36,
"kun_reading_bytes":130,"level_bytes":2,"on_reading_bytes":57,
"part_glyph_bytes":30,"part_meaning_bytes":122,"part_reading_bytes":90,
"parts":6,"raw_block_bytes":74105,"reading_bytes":66,
"sense_bytes":140,"senses":5}
ok: 19996 checks
```

The last count includes the ESP flash-metadata assertions. Without an
`IDF_BUILD_DIR` it is `19987` checks.

## Clean host, component, and simulator verification

All brief commands were run from new `mktemp -d` host/simulator directories:

```text
python3 tools/test_kanji_server.py
  kanji_server: 242 checks, 0 failures
python3 tools/test_mock_kanji_server.py
  mock_kanji_server: 150 checks, 0 failures
python3 tools/test_offline_catalog.py
  SKIP: live catalog decoder sweep (set KANJIS_DB)
  SKIP: ESP-IDF flash metadata check (set IDF_BUILD_DIR)
  ok: 71 checks
KANJIS_DB=... IDF_BUILD_DIR=... python3 tools/test_offline_catalog.py
  ok: 19996 checks
cmake -S components/vault_core/test/host -B /tmp/offline-final-host.mKgesB
cmake --build /tmp/offline-final-host.mKgesB -j8
ctest --test-dir /tmp/offline-final-host.mKgesB --output-on-failure
  9/9 passed, 0 failed
sh components/user_app/test/run.sh
  source guard / study source / startup delivery: 0 failures
sh components/provisioning/test/run.sh
  40 tests, 87 checks, 0 failures
cmake -S sim -B /tmp/offline-final-sim.gHv1NL
cmake --build /tmp/offline-final-sim.gHv1NL -j8
  kanji_sim linked successfully
```

The focused state GREEN log is `/tmp/task7-state-reserved-green.log`; aggregate
final logs use `/tmp/task7-final-*`.

## Clean ESP-IDF 5.4.3 verification and sizes

The external build directory was
`/tmp/offline-task7-idf-green.gfgFgA`, with its own
`/tmp/offline-task7-idf-green.gfgFgA/sdkconfig`. After recording the unavoidable
`fullclean` symlink error, the clean acceptance sequence was:

```sh
idf.py -B /tmp/offline-task7-idf-green.gfgFgA \
  -DSDKCONFIG=/tmp/offline-task7-idf-green.gfgFgA/sdkconfig \
  -DKANJI_CATALOG_DB=/Users/ggrrm/Documents/kanjis-backend/data/kanjis-backend.sqlite3 \
  reconfigure
idf.py -B /tmp/offline-task7-idf-green.gfgFgA partition-table
idf.py -B /tmp/offline-task7-idf-green.gfgFgA catalog_image
idf.py -B /tmp/offline-task7-idf-green.gfgFgA build
idf.py -B /tmp/offline-task7-idf-green.gfgFgA size
```

All five commands exited 0. The generated config independently confirmed both
LVGL example/demo options disabled and only Montserrat 18 enabled. The build log
shows the real `catalog_store.c` and `catalog_store_core.c` translation units
compiled and `libcatalog_store.a` linked into the full firmware.

Exact binary assertions:

| Artifact | Actual | Ceiling | Spare |
|---|---:|---:|---:|
| application `.bin` | `0x56FA60` (5,700,192) | `0x780000` | `0x2105A0` (2,164,128) |
| catalog `.bin` | `0x3483AA` (3,441,578) | `0x770000` | `0x427C56` (4,357,206) |

`idf.py size` also reported the full 8 MiB application slot with `0x2905A0`
(32%) free and total ELF image usage of 5,700,072 bytes.

The CSV parser and IDF partition tool agreed on all exact endpoints:

```text
factory:     end 0x810000
catalog:     0x810000 + 0x770000 = 0xF80000 (readonly)
study_state: 0xF80000 + 0x080000 = 0x1000000
```

Generated normal-flash metadata contains `0x810000 kanji-catalog.bin` and no
`0xF80000`/`study_state` payload. Generated `app-flash_args` contains neither
catalog nor state, proving those partitions are preserved by app-only flashing.
No hardware was flashed.

## Deferred-minor and documentation triage

- Exact SHA-256 key-material order: closed with a hand-derived literal order
  assertion and mutation RED.
- State reserved bytes 38–39 and 44–59: closed with independent encoding
  assertions and rejection of a programmed out-of-CRC tail.
- Torn-tail wear: already accurately disclosed in `docs/board-hardware.md` and
  now repeated in `docs/esp-idf-development.md`. A torn slot forces recovery
  compaction; repeated cuts can accelerate erase wear but cannot authorize a
  torn record.
- Provisioning comments: empty URL now says offline catalog, with demo only for
  corrupt/missing catalog fallback, in both header and implementation comment.
- Operator docs now cover catalog inputs and deterministic selection, normal
  versus catalog-only/app-only flashing, state preservation, immediate offline
  boot, KEY2 long-press portal entry, demo fallback, deliberate lack of trusted
  local wall-clock FSRS scheduling, and rights review for generated artifacts.

## Review findings and concerns

An independent whole-branch pre-review found no catalog bounds, zlib, journal,
or partition defect. Its Task 7 flash-metadata acceptance gap is closed here.
Its two Important `user_app` findings (unchecked startup resource creation and
production arbitration coverage) are owned by the separate runtime review-fix
lane and are not silently altered in this commit. A fresh post-commit Task 7
review is parent-coordinated against the resulting commit.

The only remaining Task 7 environment concern is the protected
`managed_components` symlink incompatibility with component-manager
`fullclean`; the brand-new external build is clean evidence without violating
the explicit instruction not to touch that workspace-owned symlink.

No hardware-in-the-loop claim is made, and no physical flash command was run.
