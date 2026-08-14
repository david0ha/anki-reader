#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#include "catalog_store_core.h"

static int failures;

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    failures++; \
} } while (0)

typedef struct fake_env fake_env_t;

typedef struct {
    fake_env_t *env;
    catalog_store_partition_kind_t kind;
} fake_partition_context_t;

struct fake_env {
    uint8_t *catalog_bytes;
    size_t catalog_length;
    uint8_t *state_bytes;
    fake_partition_context_t catalog_context;
    fake_partition_context_t state_context;
    size_t alloc_attempts;
    size_t alloc_total;
    size_t free_total;
    size_t live_allocations;
    size_t fail_alloc_attempt;
    bool fail_catalog_partition;
    bool fail_state_partition;
    bool fail_catalog_read;
    bool fail_catalog_after_state;
    bool state_read_started;
    bool fail_state_read;
    bool fail_state_write;
    bool fail_state_erase;
};

static uint8_t *load_file(const char *path, size_t *length)
{
    FILE *file = fopen(path, "rb");
    CHECK(file != NULL);
    if (file == NULL) return NULL;
    CHECK(fseek(file, 0, SEEK_END) == 0);
    long size = ftell(file);
    CHECK(size > 0);
    CHECK(fseek(file, 0, SEEK_SET) == 0);
    uint8_t *bytes = size > 0 ? malloc((size_t)size) : NULL;
    CHECK(bytes != NULL);
    if (bytes != NULL) CHECK(fread(bytes, 1, (size_t)size, file) == (size_t)size);
    CHECK(fclose(file) == 0);
    if (bytes != NULL) *length = (size_t)size;
    return bytes;
}

static bool env_init(fake_env_t *env)
{
    memset(env, 0, sizeof *env);
    env->catalog_bytes = load_file(CATALOG_FIXTURE, &env->catalog_length);
    env->state_bytes = malloc(KANJI_STATE_PARTITION_SIZE);
    if (env->catalog_bytes == NULL || env->state_bytes == NULL) {
        free(env->state_bytes);
        free(env->catalog_bytes);
        return false;
    }
    memset(env->state_bytes, 0xff, KANJI_STATE_PARTITION_SIZE);
    env->catalog_context = (fake_partition_context_t){env, CATALOG_STORE_PARTITION_CATALOG};
    env->state_context = (fake_partition_context_t){env, CATALOG_STORE_PARTITION_STATE};
    return true;
}

static void env_destroy(fake_env_t *env)
{
    CHECK(env->live_allocations == 0);
    CHECK(env->alloc_total == env->free_total);
    free(env->state_bytes);
    free(env->catalog_bytes);
}

static void *fake_alloc(void *context, size_t size)
{
    fake_env_t *env = context;
    env->alloc_attempts++;
    if (env->fail_alloc_attempt != 0 &&
        env->alloc_attempts == env->fail_alloc_attempt) {
        return NULL;
    }
    void *memory = malloc(size);
    if (memory != NULL) {
        env->alloc_total++;
        env->live_allocations++;
    }
    return memory;
}

static void fake_free(void *context, void *memory)
{
    fake_env_t *env = context;
    if (memory == NULL) return;
    CHECK(env->live_allocations != 0);
    env->free_total++;
    env->live_allocations--;
    free(memory);
}

static bool fake_find_partition(void *context, catalog_store_partition_kind_t kind,
                                catalog_store_partition_t *out)
{
    fake_env_t *env = context;
    if ((kind == CATALOG_STORE_PARTITION_CATALOG && env->fail_catalog_partition) ||
        (kind == CATALOG_STORE_PARTITION_STATE && env->fail_state_partition)) {
        return false;
    }
    if (kind == CATALOG_STORE_PARTITION_CATALOG) {
        *out = (catalog_store_partition_t){
            .context = &env->catalog_context,
            .type = 0x40,
            .subtype = 0,
            .address = 0x810000,
            .size = 0x770000,
            .readonly = true,
        };
    } else {
        *out = (catalog_store_partition_t){
            .context = &env->state_context,
            .type = 0x41,
            .subtype = 0,
            .address = 0xf80000,
            .size = 0x080000,
            .readonly = false,
        };
    }
    return true;
}

static bool fake_read(void *context, uint32_t offset, void *dst, size_t length)
{
    fake_partition_context_t *partition = context;
    fake_env_t *env = partition->env;
    if (partition->kind == CATALOG_STORE_PARTITION_CATALOG) {
        if (env->fail_catalog_read ||
            (env->fail_catalog_after_state && env->state_read_started) ||
            (size_t)offset > env->catalog_length ||
            length > env->catalog_length - (size_t)offset) {
            return false;
        }
        memcpy(dst, env->catalog_bytes + offset, length);
        return true;
    }
    env->state_read_started = true;
    if (env->fail_state_read || (size_t)offset > KANJI_STATE_PARTITION_SIZE ||
        length > KANJI_STATE_PARTITION_SIZE - (size_t)offset) {
        return false;
    }
    memcpy(dst, env->state_bytes + offset, length);
    return true;
}

static bool fake_write(void *context, uint32_t offset,
                       const void *src, size_t length)
{
    fake_partition_context_t *partition = context;
    fake_env_t *env = partition->env;
    if (partition->kind != CATALOG_STORE_PARTITION_STATE || env->fail_state_write ||
        (size_t)offset > KANJI_STATE_PARTITION_SIZE ||
        length > KANJI_STATE_PARTITION_SIZE - (size_t)offset) {
        return false;
    }
    const uint8_t *input = src;
    for (size_t i = 0; i < length; i++) {
        if ((env->state_bytes[offset + i] & input[i]) != input[i]) return false;
    }
    for (size_t i = 0; i < length; i++) env->state_bytes[offset + i] &= input[i];
    return true;
}

static bool fake_erase(void *context, uint32_t offset, size_t length)
{
    fake_partition_context_t *partition = context;
    fake_env_t *env = partition->env;
    if (partition->kind != CATALOG_STORE_PARTITION_STATE || env->fail_state_erase ||
        offset % 0x1000 != 0 || length % 0x1000 != 0 ||
        (size_t)offset > KANJI_STATE_PARTITION_SIZE ||
        length > KANJI_STATE_PARTITION_SIZE - (size_t)offset) {
        return false;
    }
    memset(env->state_bytes + offset, 0xff, length);
    return true;
}

static bool fake_inflate(void *dst, size_t *dst_length,
                         const void *src, size_t src_length)
{
    uLongf actual = (uLongf)*dst_length;
    int result = uncompress(dst, &actual, src, (uLong)src_length);
    *dst_length = (size_t)actual;
    return result == Z_OK;
}

static uint32_t fake_crc32(const void *data, size_t length)
{
    return (uint32_t)crc32(0, data, (uInt)length);
}

static catalog_store_ops_t ops_for(fake_env_t *env)
{
    return (catalog_store_ops_t){
        .context = env,
        .alloc = fake_alloc,
        .dealloc = fake_free,
        .find_partition = fake_find_partition,
        .read = fake_read,
        .write = fake_write,
        .erase = fake_erase,
        .inflate = fake_inflate,
        .crc32 = fake_crc32,
        .compressed_capacity = (size_t)compressBound(KANJI_CATALOG_MAX_RAW_BLOCK),
    };
}

static void check_init_failure(fake_env_t *env)
{
    catalog_store_core_t store = {0};
    catalog_store_ops_t ops = ops_for(env);
    CHECK(!catalog_store_core_init(&store, &ops));
    CHECK(!catalog_store_core_available(&store));
    CHECK(catalog_store_core_current(&store) == NULL);
    CHECK(catalog_store_core_ordinal(&store) == 0);
    CHECK(env->live_allocations == 0);
    catalog_store_core_release(&store);
}

static void test_init_failure_matrix(void)
{
    fake_env_t probe;
    CHECK(env_init(&probe));
    catalog_store_core_t store = {0};
    catalog_store_ops_t probe_ops = ops_for(&probe);
    CHECK(catalog_store_core_init(&store, &probe_ops));
    CHECK(probe.alloc_attempts == 7);
    catalog_store_core_release(&store);
    env_destroy(&probe);

    for (size_t fail_at = 1; fail_at <= 7; fail_at++) {
        fake_env_t env;
        CHECK(env_init(&env));
        env.fail_alloc_attempt = fail_at;
        check_init_failure(&env);
        env_destroy(&env);
    }

    fake_env_t env;
    CHECK(env_init(&env));
    env.fail_catalog_partition = true;
    check_init_failure(&env);
    env_destroy(&env);

    CHECK(env_init(&env));
    env.fail_state_partition = true;
    check_init_failure(&env);
    env_destroy(&env);

    CHECK(env_init(&env));
    env.fail_catalog_read = true;
    check_init_failure(&env);
    env_destroy(&env);

    CHECK(env_init(&env));
    env.fail_state_read = true;
    check_init_failure(&env);
    env_destroy(&env);

    CHECK(env_init(&env));
    env.fail_state_erase = true;
    check_init_failure(&env);
    env_destroy(&env);

    CHECK(env_init(&env));
    env.fail_state_write = true;
    check_init_failure(&env);
    env_destroy(&env);

    CHECK(env_init(&env));
    env.fail_catalog_after_state = true;
    check_init_failure(&env);
    CHECK(env.state_read_started);
    env_destroy(&env);
}

static void test_grade_order_and_reinitialization(void)
{
    fake_env_t env;
    CHECK(env_init(&env));
    catalog_store_core_t store = {0};
    catalog_store_ops_t ops = ops_for(&env);
    CHECK(catalog_store_core_init(&store, &ops));
    CHECK(catalog_store_core_available(&store));
    CHECK(catalog_store_core_ordinal(&store) == 0);
    const kanji_t *initial = catalog_store_core_current(&store);
    CHECK(initial != NULL);
    CHECK(initial->card.front[0] != '\0');
    kanji_t *published = malloc(sizeof *published);
    uint8_t *flash_before = malloc(KANJI_STATE_PARTITION_SIZE);
    CHECK(published != NULL && flash_before != NULL);
    memcpy(published, initial, sizeof *published);
    memcpy(flash_before, env.state_bytes, KANJI_STATE_PARTITION_SIZE);
    const size_t allocations_before_grades = env.alloc_attempts;

    env.fail_catalog_read = true;
    CHECK(!catalog_store_core_grade(&store, KANJI_GRADE_GOOD));
    CHECK(catalog_store_core_current(&store) == initial);
    CHECK(catalog_store_core_ordinal(&store) == 0);
    CHECK(memcmp(catalog_store_core_current(&store), published, sizeof *published) == 0);
    CHECK(memcmp(env.state_bytes, flash_before, KANJI_STATE_PARTITION_SIZE) == 0);

    env.fail_catalog_read = false;
    env.fail_state_write = true;
    CHECK(!catalog_store_core_grade(&store, KANJI_GRADE_HARD));
    CHECK(catalog_store_core_current(&store) == initial);
    CHECK(catalog_store_core_ordinal(&store) == 0);
    CHECK(memcmp(catalog_store_core_current(&store), published, sizeof *published) == 0);
    CHECK(memcmp(env.state_bytes, flash_before, KANJI_STATE_PARTITION_SIZE) == 0);

    env.fail_state_write = false;
    CHECK(catalog_store_core_grade(&store, KANJI_GRADE_EASY));
    const kanji_t *advanced = catalog_store_core_current(&store);
    CHECK(advanced != NULL && advanced != initial);
    CHECK(catalog_store_core_ordinal(&store) == 1);
    CHECK(memcmp(env.state_bytes, flash_before, KANJI_STATE_PARTITION_SIZE) != 0);
    CHECK(env.alloc_attempts == allocations_before_grades);
    memcpy(published, advanced, sizeof *published);

    const size_t live_before_failed_reinit = env.live_allocations;
    env.fail_catalog_partition = true;
    CHECK(!catalog_store_core_init(&store, &ops));
    CHECK(catalog_store_core_current(&store) == advanced);
    CHECK(catalog_store_core_ordinal(&store) == 1);
    CHECK(memcmp(catalog_store_core_current(&store), published, sizeof *published) == 0);
    CHECK(env.live_allocations == live_before_failed_reinit);

    env.fail_catalog_partition = false;
    env.fail_catalog_read = true;
    CHECK(!catalog_store_core_init(&store, &ops));
    CHECK(catalog_store_core_current(&store) == advanced);
    CHECK(catalog_store_core_ordinal(&store) == 1);
    CHECK(memcmp(catalog_store_core_current(&store), published, sizeof *published) == 0);
    CHECK(env.live_allocations == live_before_failed_reinit);

    env.fail_catalog_read = false;
    env.fail_state_read = true;
    CHECK(!catalog_store_core_init(&store, &ops));
    CHECK(catalog_store_core_current(&store) == advanced);
    CHECK(catalog_store_core_ordinal(&store) == 1);
    CHECK(memcmp(catalog_store_core_current(&store), published, sizeof *published) == 0);
    CHECK(env.live_allocations == live_before_failed_reinit);

    env.fail_state_read = false;
    env.state_read_started = false;
    env.fail_catalog_after_state = true;
    CHECK(!catalog_store_core_init(&store, &ops));
    CHECK(env.state_read_started);
    CHECK(catalog_store_core_current(&store) == advanced);
    CHECK(catalog_store_core_ordinal(&store) == 1);
    CHECK(memcmp(catalog_store_core_current(&store), published, sizeof *published) == 0);
    CHECK(env.live_allocations == live_before_failed_reinit);

    env.fail_catalog_after_state = false;
    env.alloc_attempts = 0;
    env.fail_alloc_attempt = 2;
    CHECK(!catalog_store_core_init(&store, &ops));
    CHECK(catalog_store_core_current(&store) == advanced);
    CHECK(catalog_store_core_ordinal(&store) == 1);
    CHECK(memcmp(catalog_store_core_current(&store), published, sizeof *published) == 0);
    CHECK(env.live_allocations == live_before_failed_reinit);

    env.fail_alloc_attempt = 0;
    env.alloc_attempts = 0;
    CHECK(catalog_store_core_init(&store, &ops));
    CHECK(catalog_store_core_available(&store));
    CHECK(catalog_store_core_ordinal(&store) == 1);
    CHECK(catalog_store_core_current(&store) != advanced);
    CHECK(memcmp(catalog_store_core_current(&store), published, sizeof *published) == 0);
    CHECK(env.live_allocations == live_before_failed_reinit);
    CHECK(env.alloc_total - env.free_total == env.live_allocations);

    catalog_store_core_release(&store);
    CHECK(!catalog_store_core_available(&store));
    CHECK(catalog_store_core_current(&store) == NULL);
    CHECK(catalog_store_core_ordinal(&store) == 0);
    catalog_store_core_release(&store);

    CHECK(catalog_store_core_init(&store, &ops));
    CHECK(catalog_store_core_available(&store));
    catalog_store_core_release(&store);
    CHECK(!catalog_store_core_available(&store));
    free(flash_before);
    free(published);
    env_destroy(&env);
}

int main(void)
{
    test_init_failure_matrix();
    test_grade_order_and_reinitialization();
    if (failures != 0) {
        fprintf(stderr, "%d catalog-store failure(s)\n", failures);
        return 1;
    }
    puts("ok: catalog-store failure atomicity and lifecycle");
    return 0;
}
