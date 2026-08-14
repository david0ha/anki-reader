#include "catalog_store_core.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define CATALOG_PARTITION_TYPE 0x40u
#define STATE_PARTITION_TYPE 0x41u
#define STORE_PARTITION_SUBTYPE 0x00u
#define CATALOG_PARTITION_OFFSET 0x810000u
#define CATALOG_PARTITION_SIZE 0x770000u
#define STATE_PARTITION_OFFSET 0xF80000u
#define STATE_PARTITION_SIZE 0x080000u

struct catalog_store_runtime {
    catalog_store_ops_t ops;
    catalog_store_partition_t catalog_partition;
    catalog_store_partition_t state_partition;
    void *compressed;
    void *raw;
    kanji_rating_summary_t *summaries;
    kanji_t *current;
    kanji_t *pending;
    kanji_t *decode_workspace;
    kanji_catalog_t catalog;
    kanji_state_t state;
    uint16_t ordinal;
    bool ready;
};

static bool ops_valid(const catalog_store_ops_t *ops)
{
    return ops != NULL && ops->alloc != NULL && ops->dealloc != NULL &&
           ops->find_partition != NULL && ops->read != NULL &&
           ops->write != NULL && ops->erase != NULL && ops->inflate != NULL &&
           ops->crc32 != NULL && ops->compressed_capacity != 0;
}

static bool catalog_partition_valid(const catalog_store_partition_t *partition)
{
    return partition->context != NULL &&
           partition->type == CATALOG_PARTITION_TYPE &&
           partition->subtype == STORE_PARTITION_SUBTYPE &&
           partition->address == CATALOG_PARTITION_OFFSET &&
           partition->size == CATALOG_PARTITION_SIZE && partition->readonly;
}

static bool state_partition_valid(const catalog_store_partition_t *partition)
{
    return partition->context != NULL &&
           partition->type == STATE_PARTITION_TYPE &&
           partition->subtype == STORE_PARTITION_SUBTYPE &&
           partition->address == STATE_PARTITION_OFFSET &&
           partition->size == STATE_PARTITION_SIZE && !partition->readonly;
}

static void runtime_release(catalog_store_runtime_t *runtime)
{
    if (runtime == NULL) return;
    runtime->ops.dealloc(runtime->ops.context, runtime->summaries);
    runtime->ops.dealloc(runtime->ops.context, runtime->raw);
    runtime->ops.dealloc(runtime->ops.context, runtime->compressed);
    runtime->ops.dealloc(runtime->ops.context, runtime->decode_workspace);
    runtime->ops.dealloc(runtime->ops.context, runtime->pending);
    runtime->ops.dealloc(runtime->ops.context, runtime->current);
    runtime->ops.dealloc(runtime->ops.context, runtime);
}

static catalog_store_runtime_t *runtime_alloc(const catalog_store_ops_t *ops)
{
    catalog_store_runtime_t *runtime = ops->alloc(ops->context, sizeof *runtime);
    if (runtime == NULL) return NULL;
    memset(runtime, 0, sizeof *runtime);
    runtime->ops = *ops;

    runtime->current = ops->alloc(ops->context, sizeof *runtime->current);
    runtime->pending = ops->alloc(ops->context, sizeof *runtime->pending);
    runtime->decode_workspace =
        ops->alloc(ops->context, sizeof *runtime->decode_workspace);
    runtime->compressed = ops->alloc(ops->context, ops->compressed_capacity);
    runtime->raw = ops->alloc(ops->context, KANJI_CATALOG_MAX_RAW_BLOCK);
    if (runtime->current == NULL || runtime->pending == NULL ||
        runtime->decode_workspace == NULL ||
        runtime->compressed == NULL || runtime->raw == NULL) {
        runtime_release(runtime);
        return NULL;
    }
    return runtime;
}

bool catalog_store_core_init(catalog_store_core_t *core,
                             const catalog_store_ops_t *ops)
{
    if (core == NULL || !ops_valid(ops)) return false;

    catalog_store_runtime_t *opened = runtime_alloc(ops);
    if (opened == NULL) return false;

    if (!ops->find_partition(ops->context, CATALOG_STORE_PARTITION_CATALOG,
                             &opened->catalog_partition) ||
        !catalog_partition_valid(&opened->catalog_partition) ||
        !ops->find_partition(ops->context, CATALOG_STORE_PARTITION_STATE,
                             &opened->state_partition) ||
        !state_partition_valid(&opened->state_partition)) {
        runtime_release(opened);
        return false;
    }

    const kanji_catalog_io_t catalog_io = {
        .context = opened->catalog_partition.context,
        .read = ops->read,
        .inflate = ops->inflate,
        .crc32 = ops->crc32,
    };
    if (!kanji_catalog_open(&opened->catalog, &catalog_io,
                            opened->catalog_partition.size,
                            opened->compressed, ops->compressed_capacity,
                            opened->raw, KANJI_CATALOG_MAX_RAW_BLOCK)) {
        runtime_release(opened);
        return false;
    }

    const uint32_t card_count = kanji_catalog_card_count(&opened->catalog);
    if (card_count == 0 || card_count > UINT16_MAX) {
        runtime_release(opened);
        return false;
    }
    opened->summaries = ops->alloc(
        ops->context, (size_t)card_count * sizeof *opened->summaries);
    if (opened->summaries == NULL) {
        runtime_release(opened);
        return false;
    }

    const kanji_state_io_t state_io = {
        .read_at = ops->read,
        .write_at = ops->write,
        .erase_range = ops->erase,
        .ctx = opened->state_partition.context,
    };
    if (!kanji_state_open(&opened->state, &state_io,
                          kanji_catalog_id(&opened->catalog),
                          (uint16_t)card_count, opened->summaries)) {
        runtime_release(opened);
        return false;
    }

    opened->ordinal = kanji_state_current_ordinal(&opened->state);
    if (!kanji_catalog_read_card(&opened->catalog, opened->ordinal,
                                 opened->current, opened->decode_workspace)) {
        runtime_release(opened);
        return false;
    }
    opened->ready = true;

    catalog_store_runtime_t *previous = core->active;
    core->active = opened;
    runtime_release(previous);
    return true;
}

void catalog_store_core_release(catalog_store_core_t *core)
{
    if (core == NULL) return;
    catalog_store_runtime_t *previous = core->active;
    core->active = NULL;
    runtime_release(previous);
}

bool catalog_store_core_available(const catalog_store_core_t *core)
{
    return core != NULL && core->active != NULL && core->active->ready;
}

const kanji_t *catalog_store_core_current(const catalog_store_core_t *core)
{
    return catalog_store_core_available(core) ? core->active->current : NULL;
}

uint16_t catalog_store_core_ordinal(const catalog_store_core_t *core)
{
    return catalog_store_core_available(core) ? core->active->ordinal : 0;
}

bool catalog_store_core_grade(catalog_store_core_t *core, kanji_grade_t grade)
{
    if (!catalog_store_core_available(core)) return false;
    catalog_store_runtime_t *runtime = core->active;
    const uint32_t count = kanji_catalog_card_count(&runtime->catalog);
    if (count == 0 || count > UINT16_MAX) return false;
    const uint16_t next_ordinal =
        (uint16_t)(((uint32_t)runtime->ordinal + 1u) % count);

    if (!kanji_catalog_read_card(&runtime->catalog, next_ordinal,
                                 runtime->pending,
                                 runtime->decode_workspace)) {
        return false;
    }
    if (!kanji_state_append_grade(&runtime->state, runtime->ordinal,
                                  next_ordinal, grade)) {
        return false;
    }

    kanji_t *previous = runtime->current;
    runtime->current = runtime->pending;
    runtime->pending = previous;
    runtime->ordinal = next_ordinal;
    return true;
}
