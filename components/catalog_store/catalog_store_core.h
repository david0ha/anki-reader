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

bool catalog_store_core_available(const catalog_store_core_t *core);
const kanji_t *catalog_store_core_current(const catalog_store_core_t *core);
uint16_t catalog_store_core_ordinal(const catalog_store_core_t *core);
bool catalog_store_core_grade(catalog_store_core_t *core, kanji_grade_t grade);

#ifdef __cplusplus
}
#endif
