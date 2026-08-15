#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#include "catalog_store_core.h"
#include "kanji_fsrs.h"
#include "ui_strings.h"

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
    int64_t now_epoch;
    bool clock_unknown;
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
    /* 2026-02-02T00:00:00Z — inside kanji_clock.h's plausibility window, and a
     * round number so an interval printed by a failing assertion is readable
     * as a number of days rather than as an epoch. */
    env->now_epoch = 1770000000;
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

/* The injected clock. `clock_unknown` is the board that has never synced, and
 * it writes nothing through out_epoch — a core that ignores the return value
 * has to be caught here rather than on a learner's panel. */
static bool fake_now(void *context, int64_t *out_epoch)
{
    fake_env_t *env = context;
    if (env->clock_unknown) return false;
    *out_epoch = env->now_epoch;
    return true;
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
        .now = fake_now,
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

    /* A missing clock is a missing operation, not a silent demotion to "never
     * schedule anything": a board wired up without one would record a whole
     * study session's ratings with no schedule and never say so. */
    CHECK(env_init(&env));
    catalog_store_core_t clockless_store = {0};
    catalog_store_ops_t clockless = ops_for(&env);
    clockless.now = NULL;
    CHECK(!catalog_store_core_init(&clockless_store, &clockless));
    CHECK(!catalog_store_core_available(&clockless_store));
    CHECK(env.live_allocations == 0);
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

/* ---------------------------------------------------------------------------
 * The offline schedule.
 *
 * Everything below is about the two halves of one seam: a rating made with no
 * proxy in reach has to leave a real FSRS schedule on flash, and the card on
 * the glass has to show ITS OWN figures rather than an empty fsrs{}.
 *
 * The expected stabilities and difficulties are py-fsrs 6.3.1's own numbers for
 * these transitions, in the journal's milli units. They are written out rather
 * than recomputed through kanji_fsrs.h so that this file pins the WIRING — the
 * right prior state, the right clock, the right conversion — instead of
 * agreeing with the engine by construction.
 * ------------------------------------------------------------------------- */

#define STATE_HEADER_SIZE 64u
#define STATE_RECORD_SIZE 32u
#define DAY_SECONDS       ((int64_t)86400)

typedef struct {
    uint16_t card;
    uint16_t next;
    uint16_t reps;
    uint16_t lapses;
    uint8_t grade;
    uint32_t stability_milli;
    uint16_t difficulty_milli;
    uint32_t due_minutes;
} record_view_t;

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* The nth record of the active bank, decoded independently of kanji_state.c so
 * a transposed pair of fields shows up here rather than agreeing with itself. */
static record_view_t record_at(const fake_env_t *env, uint32_t index)
{
    const uint8_t *r =
        env->state_bytes + STATE_HEADER_SIZE + index * STATE_RECORD_SIZE;
    return (record_view_t){
        .card = rd16(r + 4),
        .next = rd16(r + 6),
        .reps = rd16(r + 8),
        .lapses = rd16(r + 10),
        .grade = r[12],
        .stability_milli = rd32(r + 16),
        .difficulty_milli = rd16(r + 20),
        .due_minutes = rd32(r + 24),
    };
}

static void check_text(const char *got, const char *want)
{
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__,
                got, want);
        failures++;
    }
}

/* Walk the whole five-card fixture once, so the store comes back to ordinal 0
 * with the ratings this test wants already on flash. */
static void grade_rest_of_catalog(catalog_store_core_t *store)
{
    for (int i = 0; i < 4; i++) {
        CHECK(catalog_store_core_grade(store, KANJI_GRADE_GOOD));
    }
    CHECK(catalog_store_core_ordinal(store) == 0);
}

static void test_first_rating_stores_a_real_schedule(void)
{
    fake_env_t env;
    CHECK(env_init(&env));
    catalog_store_core_t store = {0};
    catalog_store_ops_t ops = ops_for(&env);
    CHECK(catalog_store_core_init(&store, &ops));

    /* A card the scheduler has never touched. -1 and not 0: the panel prints
     * the two differently, and 0 would claim a stability the board has not
     * got. */
    const kanji_t *card = catalog_store_core_current(&store);
    CHECK(card != NULL);
    CHECK(card->source == KANJI_SOURCE_CATALOG);
    CHECK(card->card.fsrs.stability_days == -1);
    CHECK(card->card.fsrs.difficulty_pct == -1);
    CHECK(card->card.fsrs.reps == 0);
    CHECK(card->card.fsrs.lapses == 0);
    check_text(card->card.fsrs.state_label, S_STATE_NEW);
    check_text(card->card.fsrs.state, "new");
    check_text(card->card.fsrs.due, "");

    /* The four spans the dock promises before the learner presses anything:
     * the single 10-minute learning step, Hard at 1.5 steps, and S0(Good) /
     * S0(Easy) rounded to whole days. */
    check_text(card->card.preview.span[0], "10분 뒤");
    check_text(card->card.preview.span[1], "15분 뒤");
    check_text(card->card.preview.span[2], "2일 뒤");
    check_text(card->card.preview.span[3], "8일 뒤");

    CHECK(catalog_store_core_grade(&store, KANJI_GRADE_GOOD));

    const record_view_t first = record_at(&env, 0);
    CHECK(first.card == 0);
    CHECK(first.next == 1);
    CHECK(first.grade == (uint8_t)KANJI_GRADE_GOOD);
    CHECK(first.reps == 1);
    CHECK(first.lapses == 0);
    CHECK(first.stability_milli == 2307);   /* S0(Good) = w2 = 2.3065 days */
    CHECK(first.difficulty_milli == 2118);  /* D0(Good)                    */
    CHECK((int64_t)first.due_minutes * 60 == env.now_epoch + 2 * DAY_SECONDS);

    /* The card now on the glass is the NEXT one, and it must show its own
     * blank history rather than the figures of the card just graded. */
    const kanji_t *next = catalog_store_core_current(&store);
    CHECK(catalog_store_core_ordinal(&store) == 1);
    CHECK(next->card.fsrs.stability_days == -1);
    CHECK(next->card.fsrs.difficulty_pct == -1);
    CHECK(next->card.fsrs.reps == 0);
    check_text(next->card.fsrs.state_label, S_STATE_NEW);
    check_text(next->card.fsrs.due, "");

    /* ...and coming back round to it, the graded card carries its own. */
    grade_rest_of_catalog(&store);
    const kanji_t *graded = catalog_store_core_current(&store);
    CHECK(graded->card.fsrs.stability_days == 2);
    CHECK(graded->card.fsrs.difficulty_pct == 12);
    CHECK(graded->card.fsrs.reps == 1);
    CHECK(graded->card.fsrs.lapses == 0);
    check_text(graded->card.fsrs.state_label, S_STATE_REVIEW);
    check_text(graded->card.fsrs.state, "review");
    check_text(graded->card.fsrs.due, "2일 뒤");

    catalog_store_core_release(&store);
    env_destroy(&env);
}

static void test_second_rating_advances_the_schedule(void)
{
    fake_env_t env;
    CHECK(env_init(&env));
    catalog_store_core_t store = {0};
    catalog_store_ops_t ops = ops_for(&env);
    CHECK(catalog_store_core_init(&store, &ops));
    const int64_t start = env.now_epoch;

    CHECK(catalog_store_core_grade(&store, KANJI_GRADE_GOOD));
    grade_rest_of_catalog(&store);

    /* Two days on, exactly when the first Good said the card was due. */
    env.now_epoch = start + 2 * DAY_SECONDS;
    CHECK(catalog_store_core_grade(&store, KANJI_GRADE_GOOD));

    const record_view_t second = record_at(&env, 5);
    CHECK(second.card == 0);
    CHECK(second.reps == 2);
    CHECK(second.lapses == 0);
    /* Recomputed from the values that came BACK off flash (2.307 / 2.118), not
     * from the live doubles, which is what makes this a round-trip test. */
    CHECK(second.stability_milli == 10965);
    CHECK(second.difficulty_milli == 2111);

    const int64_t due = (int64_t)second.due_minutes * 60;
    CHECK(due - env.now_epoch == 11 * DAY_SECONDS);
    CHECK(due - env.now_epoch > 2 * DAY_SECONDS);

    catalog_store_core_release(&store);
    env_destroy(&env);
}

static void test_again_is_a_lapse_only_from_review(void)
{
    fake_env_t env;
    CHECK(env_init(&env));
    catalog_store_core_t store = {0};
    catalog_store_ops_t ops = ops_for(&env);
    CHECK(catalog_store_core_init(&store, &ops));
    const int64_t start = env.now_epoch;

    /* Card 0 graduates to Review; card 1 fails and stays in Learning. */
    CHECK(catalog_store_core_grade(&store, KANJI_GRADE_GOOD));
    CHECK(catalog_store_core_grade(&store, KANJI_GRADE_AGAIN));
    for (int i = 0; i < 3; i++) {
        CHECK(catalog_store_core_grade(&store, KANJI_GRADE_GOOD));
    }
    CHECK(catalog_store_core_ordinal(&store) == 0);

    /* Failing a card you have never graduated is not a lapse, it is still
     * learning it — the backend adapter's rule verbatim. */
    const record_view_t learning = record_at(&env, 1);
    CHECK(learning.card == 1);
    CHECK(learning.grade == (uint8_t)KANJI_GRADE_AGAIN);
    CHECK(learning.reps == 1);
    CHECK(learning.lapses == 0);
    CHECK(learning.stability_milli == 212);   /* S0(Again) = w0 */
    CHECK(learning.difficulty_milli == 6413); /* D0(Again) = w4 */
    CHECK((int64_t)learning.due_minutes * 60 == start + 600);

    env.now_epoch = start + 2 * DAY_SECONDS;
    CHECK(catalog_store_core_grade(&store, KANJI_GRADE_AGAIN));  /* card 0 */

    /* Card 1 is on the glass now, and one 다시 on a card that had never
     * graduated leaves it 학습 중 with nothing counted against it. */
    CHECK(catalog_store_core_ordinal(&store) == 1);
    const kanji_t *still_learning = catalog_store_core_current(&store);
    check_text(still_learning->card.fsrs.state_label, S_STATE_LEARNING);
    CHECK(still_learning->card.fsrs.reps == 1);
    CHECK(still_learning->card.fsrs.lapses == 0);

    CHECK(catalog_store_core_grade(&store, KANJI_GRADE_AGAIN));  /* card 1 */

    const record_view_t from_review = record_at(&env, 5);
    CHECK(from_review.card == 0);
    CHECK(from_review.reps == 2);
    CHECK(from_review.lapses == 1);
    CHECK(from_review.stability_milli == 608);
    CHECK(from_review.difficulty_milli == 7394);
    CHECK((int64_t)from_review.due_minutes * 60 == env.now_epoch + 600);

    const record_view_t from_learning = record_at(&env, 6);
    CHECK(from_learning.card == 1);
    CHECK(from_learning.reps == 2);
    CHECK(from_learning.lapses == 0);
    CHECK(from_learning.stability_milli == 113);
    CHECK(from_learning.difficulty_milli == 8806);
    CHECK((int64_t)from_learning.due_minutes * 60 == env.now_epoch + 600);

    /* Three presses bring the walk back to card 0, which the same rating did
     * drop out of Review — and the panel says so. */
    for (int i = 0; i < 3; i++) {
        CHECK(catalog_store_core_grade(&store, KANJI_GRADE_GOOD));
    }
    CHECK(catalog_store_core_ordinal(&store) == 0);
    const kanji_t *lapsed = catalog_store_core_current(&store);
    check_text(lapsed->card.fsrs.state_label, S_STATE_RELEARNING);
    CHECK(lapsed->card.fsrs.reps == 2);
    CHECK(lapsed->card.fsrs.lapses == 1);

    catalog_store_core_release(&store);
    env_destroy(&env);
}

static void test_unknown_clock_schedules_nothing(void)
{
    fake_env_t env;
    CHECK(env_init(&env));
    env.clock_unknown = true;
    catalog_store_core_t store = {0};
    catalog_store_ops_t ops = ops_for(&env);
    CHECK(catalog_store_core_init(&store, &ops));

    /* Everything that does not need a clock is still true; everything that
     * does is empty. There is no fallback date, because a made-up one is
     * indistinguishable on the glass from a real one. */
    const kanji_t *card = catalog_store_core_current(&store);
    CHECK(card->card.fsrs.stability_days == -1);
    CHECK(card->card.fsrs.difficulty_pct == -1);
    check_text(card->card.fsrs.state_label, S_STATE_NEW);
    check_text(card->card.fsrs.due, "");
    for (int i = 0; i < KANJI_GRADE_COUNT; i++) {
        check_text(card->card.preview.span[i], "");
    }

    /* The press is still recorded — the learner's rating is not the thing to
     * drop when the clock is missing — but with no schedule at all. */
    CHECK(catalog_store_core_grade(&store, KANJI_GRADE_GOOD));
    const record_view_t unscheduled = record_at(&env, 0);
    CHECK(unscheduled.card == 0);
    CHECK(unscheduled.grade == (uint8_t)KANJI_GRADE_GOOD);
    CHECK(unscheduled.reps == 1);
    CHECK(unscheduled.stability_milli == 0);
    CHECK(unscheduled.difficulty_milli == 0);
    CHECK(unscheduled.due_minutes == 0);

    grade_rest_of_catalog(&store);
    const kanji_t *rated = catalog_store_core_current(&store);
    CHECK(rated->card.fsrs.reps == 1);
    CHECK(rated->card.fsrs.stability_days == -1);
    CHECK(rated->card.fsrs.difficulty_pct == -1);
    check_text(rated->card.fsrs.state_label, S_STATE_NEW);
    check_text(rated->card.fsrs.due, "");
    for (int i = 0; i < KANJI_GRADE_COUNT; i++) {
        check_text(rated->card.preview.span[i], "");
    }

    catalog_store_core_release(&store);
    env_destroy(&env);
}

/* The projection on its own, where the source rule can be driven directly. */
static void test_projection_leaves_remote_cards_alone(void)
{
    kanji_t *card = malloc(sizeof *card);
    kanji_t *before = malloc(sizeof *before);
    CHECK(card != NULL && before != NULL);
    if (card == NULL || before == NULL) {
        free(before);
        free(card);
        return;
    }

    const kanji_rating_summary_t summary = {
        .due_epoch = 1770000000 + 9 * DAY_SECONDS,
        .sequence = 4,
        .stability_milli = 9000,
        .difficulty_milli = 5230,
        .reps = 7,
        .lapses = 2,
        .grade = (uint8_t)KANJI_GRADE_GOOD,
    };

    /* A card as the proxy sent it: worded against the SERVER's clock and the
     * server's own review history. */
    memset(card, 0, sizeof *card);
    card->valid = true;
    card->card.valid = true;
    card->source = KANJI_SOURCE_REMOTE;
    card->card.fsrs.reps = 41;
    card->card.fsrs.lapses = 3;
    card->card.fsrs.stability_days = 88;
    card->card.fsrs.difficulty_pct = 47;
    kanji_str_copy(card->card.fsrs.state, sizeof card->card.fsrs.state, "review");
    kanji_str_copy(card->card.fsrs.state_label,
                   sizeof card->card.fsrs.state_label, S_STATE_REVIEW);
    kanji_str_copy(card->card.fsrs.due, sizeof card->card.fsrs.due, "88일 뒤");
    for (int i = 0; i < KANJI_GRADE_COUNT; i++) {
        kanji_str_copy(card->card.preview.span[i],
                       sizeof card->card.preview.span[i], "3개월 뒤");
    }
    memcpy(before, card, sizeof *before);

    catalog_store_core_project(card, &summary, true, 1770000000);
    CHECK(memcmp(card, before, sizeof *card) == 0);

    card->source = KANJI_SOURCE_DEMO;
    memcpy(before, card, sizeof *before);
    catalog_store_core_project(card, &summary, true, 1770000000);
    CHECK(memcmp(card, before, sizeof *card) == 0);

    /* And the guard is the source and nothing else: the same call on a catalog
     * card does replace all of it, so the two checks above cannot be passing
     * because the projection does nothing at all. */
    card->source = KANJI_SOURCE_CATALOG;
    catalog_store_core_project(card, &summary, true, 1770000000);
    CHECK(memcmp(card, before, sizeof *card) != 0);
    CHECK(card->card.fsrs.reps == 7);
    CHECK(card->card.fsrs.lapses == 2);
    CHECK(card->card.fsrs.stability_days == 9);
    CHECK(card->card.fsrs.difficulty_pct == 47);
    check_text(card->card.fsrs.state_label, S_STATE_REVIEW);
    check_text(card->card.fsrs.due, "9일 뒤");

    /* A card rated by a board with no clock: reviewed, but carrying no
     * schedule. Its stability and difficulty are unknown, which is -1, and
     * never the 0 the record literally holds. */
    const kanji_rating_summary_t rated_only = {
        .sequence = 2,
        .reps = 3,
        .lapses = 1,
        .grade = (uint8_t)KANJI_GRADE_HARD,
    };
    catalog_store_core_project(card, &rated_only, true, 1770000000);
    CHECK(card->card.fsrs.reps == 3);
    CHECK(card->card.fsrs.lapses == 1);
    CHECK(card->card.fsrs.stability_days == -1);
    CHECK(card->card.fsrs.difficulty_pct == -1);
    check_text(card->card.fsrs.state_label, S_STATE_NEW);
    check_text(card->card.fsrs.due, "");

    /* The same schedule with the clock unsynced: the figures survive, the
     * dates do not. */
    catalog_store_core_project(card, &summary, false, 1770000000);
    CHECK(card->card.fsrs.stability_days == 9);
    CHECK(card->card.fsrs.difficulty_pct == 47);
    check_text(card->card.fsrs.state_label, S_STATE_REVIEW);
    check_text(card->card.fsrs.due, "");
    for (int i = 0; i < KANJI_GRADE_COUNT; i++) {
        check_text(card->card.preview.span[i], "");
    }

    free(before);
    free(card);
}

int main(void)
{
    test_init_failure_matrix();
    test_grade_order_and_reinitialization();
    test_first_rating_stores_a_real_schedule();
    test_second_rating_advances_the_schedule();
    test_again_is_a_lapse_only_from_review();
    test_unknown_clock_schedules_nothing();
    test_projection_leaves_remote_cards_alone();
    if (failures != 0) {
        fprintf(stderr, "%d catalog-store failure(s)\n", failures);
        return 1;
    }
    puts("ok: catalog-store failure atomicity and lifecycle");
    return 0;
}
