/* Portable lifecycle/orchestration core behind the ESP partition adapter. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kanji_catalog.h"
#include "kanji_state.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CATALOG_STORE_PARTITION_CATALOG = 0,
    CATALOG_STORE_PARTITION_STATE,
} catalog_store_partition_kind_t;

typedef struct {
    void *context;
    uint8_t type;
    uint8_t subtype;
    uint32_t address;
    uint32_t size;
    bool readonly;
} catalog_store_partition_t;

typedef void *(*catalog_store_alloc_fn)(void *context, size_t size);
typedef void (*catalog_store_free_fn)(void *context, void *memory);
typedef bool (*catalog_store_find_partition_fn)(
    void *context, catalog_store_partition_kind_t kind,
    catalog_store_partition_t *out);

/* Wall-clock seconds now, or false when the board has never been told the time.
 *
 * This is kanji_clock.h's KANJI_CLOCK_UNKNOWN tier crossing the seam as a
 * single bool, and it is injected for exactly the reason the flash callbacks
 * are: catalog_store_core.c must not call esp_timer, time() or SNTP, so the
 * host suite can move a decade in one line and can drive the unsynced board —
 * the branch that must schedule nothing at all — without a clock to break.
 *
 * A false return means the core writes NO due date and words NO span. It does
 * not mean "assume the epoch": scheduling the 9,956-card catalog from 1970
 * makes every card overdue by half a century and says nothing on the glass
 * about why. `out_epoch` is left untouched on false. */
typedef bool (*catalog_store_now_fn)(void *context, int64_t *out_epoch);

typedef struct {
    void *context;
    catalog_store_alloc_fn alloc;
    catalog_store_free_fn dealloc;
    catalog_store_find_partition_fn find_partition;
    kanji_catalog_read_fn read;
    kanji_state_write_fn write;
    kanji_state_erase_fn erase;
    kanji_catalog_inflate_fn inflate;
    kanji_catalog_crc32_fn crc32;
    catalog_store_now_fn now;
    size_t compressed_capacity;
} catalog_store_ops_t;

typedef struct catalog_store_runtime catalog_store_runtime_t;

/* Zero-initialize before first use. The active runtime is opaque so callers
 * cannot accidentally put its two full card snapshots on a task stack. */
typedef struct {
    catalog_store_runtime_t *active;
} catalog_store_core_t;

/* Build an entirely new runtime and pointer-swap it into core only after
 * partition, catalog, state, and restored-card initialization all succeed. */
bool catalog_store_core_init(catalog_store_core_t *core,
                             const catalog_store_ops_t *ops);
void catalog_store_core_release(catalog_store_core_t *core);

/* Overlay a decoded card's own FSRS figures and its four grade previews from
 * the board's rating journal.  Pure: no flash, no clock, no allocation.
 *
 * REMOTE STILL WINS.  This is the one place local scheduling may touch a card,
 * and it refuses every card that is not KANJI_SOURCE_CATALOG.  A payload from
 * the proxy carries fsrs{} and preview{} computed against the server's clock
 * and the server's own review history; recomputing either of those locally
 * would overwrite the authority with a guess made by a board that may not know
 * what day it is.  Local figures fill in only where there is no proxy to ask.
 *
 * `clock_known` false is kanji_clock.h's UNKNOWN tier: reps, lapses, stability,
 * difficulty and the state label are all still true — none of them needs a
 * clock — but `due` and all four preview spans are left EMPTY, which is the
 * contract's spelling for "no date to show" and what the UI renders as blank.
 * There is no fallback date, because a fabricated one is indistinguishable on
 * the glass from a real one.
 *
 * A never-scheduled card reports stability_days and difficulty_pct as -1 and
 * not 0; docs/kanji-contract.md is explicit that the panel prints those two
 * differently and that 0 makes the board claim to know something it does not.
 *
 * `summary` may be NULL, which is treated as an unreviewed card. */
void catalog_store_core_project(kanji_t *card,
                                const kanji_rating_summary_t *summary,
                                bool clock_known, int64_t now_epoch);

bool catalog_store_core_available(const catalog_store_core_t *core);
const kanji_t *catalog_store_core_current(const catalog_store_core_t *core);
uint16_t catalog_store_core_ordinal(const catalog_store_core_t *core);
bool catalog_store_core_grade(catalog_store_core_t *core, kanji_grade_t grade);

#ifdef __cplusplus
}
#endif
