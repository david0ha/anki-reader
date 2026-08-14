# Offline catalog stack-frame fix

Date: 2026-08-15

## Outcome

The offline cold-start and grade paths no longer place a full `kanji_t` on a
firmware task stack. Parsing and catalog decoding now use an explicit,
caller-owned workspace. `catalog_store` allocates that third card-sized
workspace once through its existing PSRAM-first allocator, reuses it for every
decode, and releases it with the rest of the runtime.

The production `vault_core` component now has a component-wide hard gate:

```text
-Wframe-larger-than=2048 -Werror=frame-larger-than=2048 -fstack-usage
```

No task stack configuration was increased. The verified IDF configuration
still has `CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192`.

## Root cause and RED evidence

At the pre-fix implementation, both layers created a local `kanji_t`:

- `kanji_parse()` parsed into a local temporary before publishing it.
- `kanji_catalog_read_card()` created another local temporary and called
  `kanji_parse()` beneath it.

Adding the production hard gate first and rebuilding the actual ESP32-S3
objects in `/tmp/offline-root-final-idf.KL6r0t` produced the intended RED:

```text
kanji_parse.c:398:1: error: the frame size of 7280 bytes is larger than 2048 bytes
kanji_catalog.c:537:1: error: the frame size of 7360 bytes is larger than 2048 bytes
```

The corresponding Xtensa `.su` totals were 7,312 and 7,392 bytes. The nested
catalog path therefore had about 14,704 bytes live before accounting for its
callers, which cannot be safe on the 8 KiB main task.

Host RED tests then failed to compile because the workspace and public release
interfaces did not yet exist. The catalog-store allocation matrix also still
expected the old six-allocation runtime.

## Implementation

### Parser and catalog

- Added `kanji_parse_with_workspace(json, len, out, workspace)`.
- Missing workspace and `workspace == out` are rejected before either object is
  touched.
- Parsing may clobber only the scratch object on failure. The public output is
  still copied exactly once, after the entire envelope is accepted.
- Kept `kanji_parse()` as the remote-service/test compatibility API. It obtains
  a temporary through `cJSON_malloc()` (and releases it with `cJSON_free()`), so
  installed PSRAM-aware cJSON hooks continue to apply without shared static
  parser state.
- Changed `kanji_catalog_read_card()` to require a separate decode workspace.
  It validates the block, record, and deck before parsing/publishing; after the
  parser commits, only non-failing deck/source field copies remain. Thus every
  existing corruption and sentinel-output guarantee is preserved.

### Catalog-store lifetime

- Added one persistent `decode_workspace` to the opaque runtime.
- Init and grade both pass that workspace to the catalog reader.
- Grade performs zero allocator calls; current/pending publication still uses
  the existing pointer swap only after the state append succeeds.
- Every allocation-failure position, now 1 through 7, is exercised and must
  return to zero live allocations.
- Failed reinitialization still preserves the previous active card/runtime.
- Added public `catalog_store_release()`, which delegates to the already
  idempotent core release. It is safe before init, after failed init, on repeat
  calls, and permits a later `catalog_store_init()` retry.

## GREEN Xtensa frame evidence

After reconfiguring and rebuilding the same external IDF directory, the hard
gate passed for every `vault_core` and `catalog_store` production translation
unit. Fresh `.su` values are:

| Function | Xtensa stack usage |
|---|---:|
| `kanji_parse_with_workspace` | 48 B |
| `kanji_parse` compatibility wrapper | 32 B |
| `kanji_catalog_read_card` | 112 B |
| `catalog_store_core_init` | 80 B |
| `catalog_store_core_grade` | 32 B |
| ESP `catalog_store_init` | 80 B |
| ESP `catalog_store_grade` | 32 B |
| ESP `catalog_store_release` | 32 B |

The largest function anywhere in `vault_core` is now
`kanji_catalog_open` at 1,376 bytes, still below the enforced 2,048-byte cap.
The largest `catalog_store` frame is 96 bytes.

## Verification

- Portable vault-core host suite: **9/9 passed**.
- Catalog-store host suite (core plus ESP adapter lifecycle): **2/2 passed**.
- Vault-core ASan + UBSan suite: **9/9 passed**.
- Catalog-store ASan + UBSan suite: **2/2 passed**. ASan inflates the AppleClang
  host-only init frame, so that sanitizer invocation demoted only the host frame
  diagnostic; the normal host gate and actual Xtensa production hard gates
  remained enabled and passed.
- Fresh desktop simulator configure/build: `kanji_sim` linked successfully.
- Actual ESP32-S3 `__idf_vault_core` and `__idf_catalog_store` objects: passed
  the production hard gates.
- Full ESP-IDF link and image generation: passed; `obsidian_board.bin` was
  generated at 0x570610 bytes with 32% of the app partition free.
- Existing bad-JSON, corrupt block/index/CRC, and byte-identical sentinel tests
  remain enabled and passed.
- New tests cover missing/aliased workspaces, seven allocation failure points,
  no per-grade allocation churn, failed reinit preservation, idempotent release,
  and init retry after release.
