#include "catalog_store.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "kanji_catalog.h"
#include "kanji_state.h"
#include "zlib.h"

#define CATALOG_PARTITION_TYPE ((esp_partition_type_t)0x40)
#define STATE_PARTITION_TYPE ((esp_partition_type_t)0x41)
#define STORE_PARTITION_SUBTYPE ((esp_partition_subtype_t)0x00)
#define CATALOG_PARTITION_OFFSET 0x810000u
#define CATALOG_PARTITION_SIZE 0x770000u
#define STATE_PARTITION_OFFSET 0xF80000u
#define STATE_PARTITION_SIZE 0x080000u

typedef struct {
    const esp_partition_t *catalog_partition;
    const esp_partition_t *state_partition;
    void *compressed;
    size_t compressed_capacity;
    void *raw;
    kanji_rating_summary_t *summaries;
    kanji_catalog_t catalog;
    kanji_state_t state;
    kanji_t current;
    uint16_t ordinal;
    bool ready;
} catalog_store_runtime_t;

static catalog_store_runtime_t s_store;

static void *workspace_alloc(size_t size)
{
    void *memory = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return memory != NULL ? memory : malloc(size);
}

static void runtime_release(catalog_store_runtime_t *runtime)
{
    free(runtime->summaries);
    free(runtime->raw);
    free(runtime->compressed);
    memset(runtime, 0, sizeof *runtime);
}

static bool partition_read(void *context, uint32_t offset,
                           void *dst, size_t length)
{
    const esp_partition_t *partition = context;
    return partition != NULL &&
           esp_partition_read(partition, offset, dst, length) == ESP_OK;
}

static bool partition_write(void *context, uint32_t offset,
                            const void *src, size_t length)
{
    const esp_partition_t *partition = context;
    return partition != NULL &&
           esp_partition_write(partition, offset, src, length) == ESP_OK;
}

static bool partition_erase(void *context, uint32_t offset, size_t length)
{
    const esp_partition_t *partition = context;
    return partition != NULL &&
           esp_partition_erase_range(partition, offset, length) == ESP_OK;
}

static bool inflate_exact(void *dst, size_t *dst_length,
                          const void *src, size_t src_length)
{
    if (dst == NULL || dst_length == NULL || src == NULL ||
        src_length > UINT_MAX || *dst_length > UINT_MAX) {
        return false;
    }

    z_stream stream = {0};
    stream.next_in = (Bytef *)src;
    stream.avail_in = (uInt)src_length;
    stream.next_out = dst;
    stream.avail_out = (uInt)*dst_length;
    if (inflateInit(&stream) != Z_OK) return false;

    const int result = inflate(&stream, Z_FINISH);
    const bool ok = result == Z_STREAM_END && stream.avail_in == 0 &&
                    stream.total_out == *dst_length;
    *dst_length = (size_t)stream.total_out;
    (void)inflateEnd(&stream);
    return ok;
}

static uint32_t zlib_crc32(const void *data, size_t length)
{
    const Bytef *cursor = data;
    uLong crc = crc32(0, Z_NULL, 0);
    while (length != 0) {
        const uInt chunk = length > UINT_MAX ? UINT_MAX : (uInt)length;
        crc = crc32(crc, cursor, chunk);
        cursor += chunk;
        length -= chunk;
    }
    return (uint32_t)crc;
}

static const esp_partition_t *find_catalog_partition(void)
{
    const esp_partition_t *partition = esp_partition_find_first(
        CATALOG_PARTITION_TYPE, STORE_PARTITION_SUBTYPE, "catalog");
    if (partition == NULL || partition->address != CATALOG_PARTITION_OFFSET ||
        partition->size != CATALOG_PARTITION_SIZE || !partition->readonly) {
        return NULL;
    }
    return partition;
}

static const esp_partition_t *find_state_partition(void)
{
    const esp_partition_t *partition = esp_partition_find_first(
        STATE_PARTITION_TYPE, STORE_PARTITION_SUBTYPE, "study_state");
    if (partition == NULL || partition->address != STATE_PARTITION_OFFSET ||
        partition->size != STATE_PARTITION_SIZE || partition->readonly) {
        return NULL;
    }
    return partition;
}

bool catalog_store_init(void)
{
    catalog_store_runtime_t opened = {0};
    opened.catalog_partition = find_catalog_partition();
    opened.state_partition = find_state_partition();
    if (opened.catalog_partition == NULL || opened.state_partition == NULL) {
        return false;
    }

    opened.compressed_capacity =
        (size_t)compressBound(KANJI_CATALOG_MAX_RAW_BLOCK);
    opened.compressed = workspace_alloc(opened.compressed_capacity);
    opened.raw = workspace_alloc(KANJI_CATALOG_MAX_RAW_BLOCK);
    if (opened.compressed == NULL || opened.raw == NULL) {
        runtime_release(&opened);
        return false;
    }

    const kanji_catalog_io_t catalog_io = {
        .context = (void *)opened.catalog_partition,
        .read = partition_read,
        .inflate = inflate_exact,
        .crc32 = zlib_crc32,
    };
    if (!kanji_catalog_open(&opened.catalog, &catalog_io,
                            opened.catalog_partition->size,
                            opened.compressed, opened.compressed_capacity,
                            opened.raw, KANJI_CATALOG_MAX_RAW_BLOCK)) {
        runtime_release(&opened);
        return false;
    }

    const uint32_t card_count = kanji_catalog_card_count(&opened.catalog);
    if (card_count == 0 || card_count > UINT16_MAX ||
        card_count > SIZE_MAX / sizeof *opened.summaries) {
        runtime_release(&opened);
        return false;
    }
    opened.summaries = workspace_alloc(card_count * sizeof *opened.summaries);
    if (opened.summaries == NULL) {
        runtime_release(&opened);
        return false;
    }

    const kanji_state_io_t state_io = {
        .read_at = partition_read,
        .write_at = partition_write,
        .erase_range = partition_erase,
        .ctx = (void *)opened.state_partition,
    };
    if (!kanji_state_open(&opened.state, &state_io,
                          kanji_catalog_id(&opened.catalog),
                          (uint16_t)card_count, opened.summaries)) {
        runtime_release(&opened);
        return false;
    }

    opened.ordinal = kanji_state_current_ordinal(&opened.state);
    if (!kanji_catalog_read_card(&opened.catalog, opened.ordinal,
                                 &opened.current)) {
        runtime_release(&opened);
        return false;
    }
    opened.ready = true;

    catalog_store_runtime_t previous = s_store;
    s_store = opened;
    runtime_release(&previous);
    return true;
}

bool catalog_store_available(void)
{
    return s_store.ready;
}

const kanji_t *catalog_store_current(void)
{
    return s_store.ready ? &s_store.current : NULL;
}

uint16_t catalog_store_ordinal(void)
{
    return s_store.ready ? s_store.ordinal : 0;
}

bool catalog_store_grade(kanji_grade_t grade)
{
    if (!s_store.ready) return false;

    const uint32_t count = kanji_catalog_card_count(&s_store.catalog);
    if (count == 0 || count > UINT16_MAX) return false;
    const uint16_t next_ordinal = (uint16_t)(((uint32_t)s_store.ordinal + 1u) % count);

    kanji_t next;
    if (!kanji_catalog_read_card(&s_store.catalog, next_ordinal, &next)) {
        return false;
    }
    if (!kanji_state_append_grade(&s_store.state, s_store.ordinal,
                                  next_ordinal, grade)) {
        return false;
    }

    s_store.ordinal = next_ordinal;
    s_store.current = next;
    return true;
}
