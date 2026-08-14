#include "catalog_store.h"

#include <limits.h>
#include <stdlib.h>

#include "catalog_store_core.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "zlib.h"

#define CATALOG_PARTITION_TYPE ((esp_partition_type_t)0x40)
#define STATE_PARTITION_TYPE ((esp_partition_type_t)0x41)
#define STORE_PARTITION_SUBTYPE ((esp_partition_subtype_t)0x00)

static catalog_store_core_t s_store;

static void *workspace_alloc(void *context, size_t size)
{
    (void)context;
    void *memory = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return memory != NULL ? memory : malloc(size);
}

static void workspace_free(void *context, void *memory)
{
    (void)context;
    free(memory);
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

static bool find_partition(void *context, catalog_store_partition_kind_t kind,
                           catalog_store_partition_t *out)
{
    (void)context;
    const bool catalog = kind == CATALOG_STORE_PARTITION_CATALOG;
    const esp_partition_t *partition = esp_partition_find_first(
        catalog ? CATALOG_PARTITION_TYPE : STATE_PARTITION_TYPE,
        STORE_PARTITION_SUBTYPE, catalog ? "catalog" : "study_state");
    if (partition == NULL || out == NULL) return false;
    *out = (catalog_store_partition_t){
        .context = (void *)partition,
        .type = (uint8_t)partition->type,
        .subtype = (uint8_t)partition->subtype,
        .address = partition->address,
        .size = partition->size,
        .readonly = partition->readonly,
    };
    return true;
}

bool catalog_store_init(void)
{
    const catalog_store_ops_t ops = {
        .alloc = workspace_alloc,
        .dealloc = workspace_free,
        .find_partition = find_partition,
        .read = partition_read,
        .write = partition_write,
        .erase = partition_erase,
        .inflate = inflate_exact,
        .crc32 = zlib_crc32,
        .compressed_capacity =
            (size_t)compressBound(KANJI_CATALOG_MAX_RAW_BLOCK),
    };
    return catalog_store_core_init(&s_store, &ops);
}

bool catalog_store_available(void)
{
    return catalog_store_core_available(&s_store);
}

const kanji_t *catalog_store_current(void)
{
    return catalog_store_core_current(&s_store);
}

uint16_t catalog_store_ordinal(void)
{
    return catalog_store_core_ordinal(&s_store);
}

bool catalog_store_grade(kanji_grade_t grade)
{
    return catalog_store_core_grade(&s_store, grade);
}
