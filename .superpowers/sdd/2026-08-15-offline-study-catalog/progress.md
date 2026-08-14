# SDD ledger — plan: docs/superpowers/plans/2026-08-15-offline-study-catalog.md

Spec: `docs/superpowers/specs/2026-08-15-offline-study-catalog-design.md`
Start HEAD: `0c726fb`

## Preflight interface scan

| Tasks | Producer / consumer | Finding |
|---|---|---|
| 1 → 2 | `project_card_content`, safe composition, full wire fields → generator envelopes | Clean; Task 2 consumes Task 1 names and exact capacities. |
| 1 → 3 | expanded `kanji_t` and `kanji_parse(json,len)` → catalog decode | Clean; Task 3 overlays only source/deck/ordinal after atomic parse. |
| 1 → 6 | explicit `kanji_source_t` → source arbitration | Clean; Task 6 is forbidden to define a competing enum. |
| 2 → 3 | 128-byte header, 12-byte card index, 16-byte block index, zlib blocks → C reader | Clean; field positions and cardinalities match exactly. |
| 2 → 5 | generator CLI/image → CMake flash target | Clean; Task 5 uses `0x770000`, seed, user id, and output path specified by Task 2. |
| 2 → 7 | live catalog proof → final acceptance | Clean; explicit DB mode makes count mismatch fail. |
| 3 → 5 | injected catalog I/O/workspaces → ESP partition adapter | Clean; Task 5 supplies raw partition reads plus official zlib. |
| 4 → 5 | injected two-bank state engine → ESP state partition adapter | Clean; size and erase alignment match the 512 KiB partition. |
| 5 → 6 | `catalog_store_*` → offline boot/local grade | Clean; Task 6 decodes before grade persistence and advances only after both succeed. |
| 6 → 7 | offline-first runtime → final docs/verification | Clean; Task 7 checks but does not redesign runtime. |
| 1 | tests vs model/parser/projection implementation | Internally consistent; every new field and unbounded limit has a RED test. |
| 2 | fixture/live tests vs generator | Internally consistent; fixture bypasses production 10/9,956 gate explicitly. |
| 3 | corruption matrix vs checked reader | Internally consistent; output atomicity is tested with a byte sentinel. |
| 4 | NOR fake vs bank/journal implementation | Internally consistent; commit order and torn writes are observable behaviors. |
| 5 | partition tests vs CSV/adapter/CMake | Internally consistent; state is deliberately never registered as a flash image. |
| 6 | pure policies vs `user_app`/provisioning integration | Internally consistent; UI ownership is preserved through queued overlay calls. |
| 7 | acceptance commands vs preceding outputs | Internally consistent; no new production architecture is introduced. |

## Preflight rulings

- Ruling: all ten decks are shuffled independently and globally round-robin interleaved before fixed 64-card blocks — this maximizes deck balance and makes `ordinal/64` authoritative; if wrong, pack/reader order must be rebuilt together.
- Ruling: offline v1 persists four-grade outcomes, repetitions, lapses, and position but does not claim local FSRS due scheduling — the board lacks a trusted power-off clock; if wrong, a separate FSRS-6 and clock design is required.
- Ruling: the next card is decoded before the grade record is persisted — this guarantees either both position and drawable card advance or neither; if wrong, a decode failure after persistence could skip a card on reboot.
- Ruling: absent credentials and failed saved joins remain in study mode; only the existing long press opens the portal — this gives immediate offline operation; if wrong, first-time setup requires an extra deliberate button action.
- Ruling: generated catalog images remain local build artifacts and are not committed — source content lacks redistribution metadata; if wrong, another distribution/licensing decision is required.
- Ruling: `kanji_t` does not gain `catalog_ordinal`; `catalog_store` owns the current ordinal and the decoder overlays only deck/level/source — this avoids leaking storage addressing into the display model; if wrong, a later API consumer would require an explicit model field and hash update.

## Task status

- Task 1: external-check resolved — `kanjis-front/src/reels/toReelInfo.ts` was audited before the plan; the single/compound/okurigana goldens in the reviewed diff cover the adopted behavior. No implementation gap.
- Task 1: complete (commits 0c726fb..3096c6b, review clean)
- Task 2: minor (deferred): ordering test proves determinism/balance but not a hand-derived exact SHA-256 key-material order; final review must triage.
- Task 2: fix round 1/5 (2 addressed, 0 open — absolute ceiling/exact length; canonical JSON; commits 488f344..fbc069c)
- Task 2: complete (commits 3096c6b..fbc069c, review clean; 1 deferred minor)
- Task 3: fix round 1/5 (1 addressed, 1 open — runtime index structure fixed; missing repaired in-bounds record-gap test; commits dd60d67..4b2b038)
- Task 3: fix round 2/5 (1 addressed, 0 open — repaired in-bounds record-gap coverage; commits 4b2b038..9d0a308)
- Task 3: complete (commits fbc069c..9d0a308, review clean)
- Task 4: minor (deferred): exact reserved header bytes 38–39 and 44–59 are not independently asserted; final review must triage.
- Task 4: minor (deferred): repeated torn tails can accelerate one erase cycle per bank on recovery; Task 5/docs must disclose adapter wear behavior.
- Task 4: fix round 1/5 (3 addressed, 0 open — enum validation; verified commit headers; portable generation ordering; commits 2a2dd0a..f0fd996)
- Task 4: complete (commits 9d0a308..f0fd996, review clean; 2 deferred minors)
- Task 5: observation (deferred to Task 7): full firmware build exposes a pre-existing managed LVGL example that references disabled Montserrat 14; Task 5 did not cause it, but final firmware acceptance must resolve it.
- Task 5: fix round 1/5 (1 addressed, 1 open — pointer runtime/failure tests fixed; frame warning is not a hard error; commits 96ceb6d..7399380)
- Task 5: fix round 2/5 (1 addressed, 0 open — Xtensa stack-frame threshold is a hard compile error; commits 7399380..54de22e)
- Task 5: complete (commits f0fd996..54de22e, review clean; LVGL build observation deferred)
- Task 6: minor (deferred): `prov_config.h` comments still say empty URL selects demo; Task 7 docs/code-comment pass must update this to catalog-with-demo-fallback.
- Task 6: fix round 1/5 (1 addressed, 0 open — startup config/overlay queue delivery and API gate; commits 719fa90..31a227c)
- Task 6: complete (commits 54de22e..31a227c, review clean; comments/LVGL observations deferred)
- Task 7: deferred minor triage complete — hand-derived SHA-256 order mutation-tested; state bytes 38–39 and 44–59 independently asserted/validated; torn-tail recovery wear disclosed; provisioning comments corrected.
- Task 7: ESP-IDF observation resolved in project config — unused LVGL examples/demos disabled; fresh external ESP-IDF 5.4.3 build links with Montserrat 14 still intentionally disabled.
- Task 7: acceptance GREEN — live 10-deck/9,956-card/156-block sweep compares every envelope/formula; 9/9 host tests, component suites, simulator, partition/catalog targets, full firmware build, size ceilings, and flash metadata pass. App `0x56FA60`; catalog `0x3483AA`; exact flash end `0x1000000`; no hardware flashed.
- Task 7: environment note — `idf.py fullclean` cannot remove the protected `managed_components` symlink (`shutil.rmtree` rejects symlinks), so clean acceptance used a brand-new external build directory and SDKCONFIG without modifying the symlink.
- Task 7: independent pre-review found no catalog/journal/partition defect; flash-metadata gap closed here. Two Important runtime findings are assigned to the separate Task 6 review-fix lane; parent will run a fresh post-commit review.
