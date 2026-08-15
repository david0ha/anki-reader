#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "catalog_store.h"
#include "catalog_store_core.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"

static int failures;
static bool init_result;
static size_t init_calls;
static size_t release_calls;

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    failures++; \
} } while (0)

bool catalog_store_core_init(catalog_store_core_t *core,
                             const catalog_store_ops_t *ops)
{
    (void)ops;
    init_calls++;
    if (!init_result) return false;
    core->active = (catalog_store_runtime_t *)(uintptr_t)1;
    return true;
}

void catalog_store_core_release(catalog_store_core_t *core)
{
    release_calls++;
    if (core != NULL) core->active = NULL;
}

bool catalog_store_core_available(const catalog_store_core_t *core)
{
    return core != NULL && core->active != NULL;
}

const kanji_t *catalog_store_core_current(const catalog_store_core_t *core)
{
    (void)core;
    return NULL;
}

uint16_t catalog_store_core_ordinal(const catalog_store_core_t *core)
{
    (void)core;
    return 0;
}

bool catalog_store_core_grade(catalog_store_core_t *core, kanji_grade_t grade)
{
    (void)core;
    (void)grade;
    return false;
}

int64_t esp_timer_get_time(void)
{
    return 0;
}

void *heap_caps_malloc(size_t size, uint32_t capabilities)
{
    (void)capabilities;
    return malloc(size);
}

const esp_partition_t *esp_partition_find_first(
    esp_partition_type_t type, esp_partition_subtype_t subtype,
    const char *label)
{
    (void)type;
    (void)subtype;
    (void)label;
    return NULL;
}

esp_err_t esp_partition_read(const esp_partition_t *partition, size_t offset,
                             void *dst, size_t size)
{
    (void)partition;
    (void)offset;
    (void)dst;
    (void)size;
    return -1;
}

esp_err_t esp_partition_write(const esp_partition_t *partition, size_t offset,
                              const void *src, size_t size)
{
    (void)partition;
    (void)offset;
    (void)src;
    (void)size;
    return -1;
}

esp_err_t esp_partition_erase_range(const esp_partition_t *partition,
                                    size_t offset, size_t size)
{
    (void)partition;
    (void)offset;
    (void)size;
    return -1;
}

static void test_public_release_lifecycle(void)
{
    CHECK(!catalog_store_available());
    catalog_store_release();
    CHECK(release_calls == 1);
    CHECK(!catalog_store_available());

    init_result = false;
    CHECK(!catalog_store_init());
    CHECK(init_calls == 1);
    catalog_store_release();
    CHECK(release_calls == 2);
    CHECK(!catalog_store_available());

    init_result = true;
    CHECK(catalog_store_init());
    CHECK(catalog_store_available());
    catalog_store_release();
    CHECK(!catalog_store_available());
    catalog_store_release();
    CHECK(!catalog_store_available());

    CHECK(catalog_store_init());
    CHECK(catalog_store_available());
    catalog_store_release();
    CHECK(!catalog_store_available());
}

int main(void)
{
    test_public_release_lifecycle();
    if (failures != 0) {
        fprintf(stderr, "%d catalog-store adapter failure(s)\n", failures);
        return 1;
    }
    puts("ok: catalog-store adapter release lifecycle");
    return 0;
}
