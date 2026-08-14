#include "../study_source.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(expr) do { \
    if (!(expr)) { \
        failures++; \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); \
    } \
} while (0)

enum {
    EVENT_DECODE = 1,
    EVENT_PERSIST,
    EVENT_CURRENT,
    EVENT_PUBLISH,
};

typedef struct {
    bool available;
    bool decode_ok;
    bool persist_ok;
    uint16_t ordinal;
    kanji_t current;
    int events[8];
    int event_count;
} fake_catalog_t;

static kanji_t card(const char *id, kanji_source_t source)
{
    kanji_t value;
    memset(&value, 0, sizeof(value));
    value.valid = true;
    value.card.valid = true;
    value.source = source;
    kanji_str_copy(value.card.id, sizeof(value.card.id), id);
    kanji_str_copy(value.card.front, sizeof(value.card.front), id);
    return value;
}

static bool fake_available(void *context)
{
    return ((fake_catalog_t *)context)->available;
}

static const kanji_t *fake_current(void *context)
{
    fake_catalog_t *catalog = context;
    catalog->events[catalog->event_count++] = EVENT_CURRENT;
    return catalog->available ? &catalog->current : NULL;
}

static uint16_t fake_ordinal(void *context)
{
    return ((fake_catalog_t *)context)->ordinal;
}

static bool fake_grade(void *context, kanji_grade_t grade)
{
    fake_catalog_t *catalog = context;
    (void)grade;
    catalog->events[catalog->event_count++] = EVENT_DECODE;
    if (!catalog->decode_ok) {
        return false;
    }
    catalog->events[catalog->event_count++] = EVENT_PERSIST;
    if (!catalog->persist_ok) {
        return false;
    }
    catalog->ordinal++;
    catalog->current = card("next", KANJI_SOURCE_CATALOG);
    return true;
}

static void fake_lock(void *context)
{
    fake_catalog_t *catalog = context;
    catalog->events[catalog->event_count++] = EVENT_PUBLISH;
}

static void fake_unlock(void *context)
{
    (void)context;
}

static study_catalog_ops_t ops(fake_catalog_t *catalog)
{
    study_catalog_ops_t value = {
        .context = catalog,
        .available = fake_available,
        .current = fake_current,
        .ordinal = fake_ordinal,
        .grade = fake_grade,
    };
    return value;
}

static study_state_lock_t lock_ops(fake_catalog_t *catalog)
{
    study_state_lock_t value = {
        .context = catalog,
        .lock = fake_lock,
        .unlock = fake_unlock,
    };
    return value;
}

static void catalog_boot_and_url_clear_use_catalog_then_demo(void)
{
    fake_catalog_t catalog = {
        .available = true,
        .current = card("restored", KANJI_SOURCE_CATALOG),
        .ordinal = 73,
    };
    study_runtime_t runtime;
    study_runtime_init(&runtime);

    CHECK(study_runtime_restore(&runtime, ops(&catalog), lock_ops(&catalog)) ==
          STUDY_RESTORE_CATALOG);
    CHECK(runtime.data.source == KANJI_SOURCE_CATALOG);
    CHECK(strcmp(runtime.data.card.id, "restored") == 0);
    CHECK(runtime.catalog_ordinal == 73);
    CHECK(!runtime.nav.revealed);

    runtime.data = card("remote", KANJI_SOURCE_REMOTE);
    runtime.nav.revealed = true;
    catalog.current = card("restored-after-clear", KANJI_SOURCE_CATALOG);
    catalog.ordinal = 74;
    CHECK(study_runtime_restore(&runtime, ops(&catalog), lock_ops(&catalog)) ==
          STUDY_RESTORE_CATALOG);
    CHECK(strcmp(runtime.data.card.id, "restored-after-clear") == 0);
    CHECK(runtime.catalog_ordinal == 74);
    CHECK(!runtime.nav.revealed);

    catalog.available = false;
    CHECK(study_runtime_restore(&runtime, ops(&catalog), lock_ops(&catalog)) ==
          STUDY_RESTORE_DEMO);
    CHECK(runtime.data.source == KANJI_SOURCE_DEMO);
    CHECK(runtime.data.card.valid);
}

static void local_grade_orders_decode_persist_then_publish(void)
{
    fake_catalog_t catalog = {
        .available = true,
        .decode_ok = true,
        .persist_ok = true,
        .current = card("answer", KANJI_SOURCE_CATALOG),
        .ordinal = 18,
    };
    study_runtime_t runtime;
    study_runtime_init(&runtime);
    CHECK(study_runtime_restore(&runtime, ops(&catalog), lock_ops(&catalog)) ==
          STUDY_RESTORE_CATALOG);
    catalog.event_count = 0;
    runtime.nav.revealed = true;

    CHECK(study_runtime_capture_grade(&runtime, KANJI_GRADE_GOOD) ==
          STUDY_GRADE_LOCAL);
    study_grade_request_t request = runtime.pending_grade;
    CHECK(study_runtime_process_local_grade(
              &runtime, &request, ops(&catalog), lock_ops(&catalog)) ==
          STUDY_LOCAL_PUBLISHED);
    CHECK(catalog.event_count == 4);
    CHECK(catalog.events[0] == EVENT_DECODE);
    CHECK(catalog.events[1] == EVENT_PERSIST);
    CHECK(catalog.events[2] == EVENT_CURRENT);
    CHECK(catalog.events[3] == EVENT_PUBLISH);
    CHECK(strcmp(runtime.data.card.id, "next") == 0);
    CHECK(runtime.catalog_ordinal == 19);
    CHECK(!runtime.nav.revealed);
    CHECK(!runtime.pending_grade_valid);
}

static void local_decode_or_state_failure_keeps_card_and_ordinal(void)
{
    for (int persist_ok = 0; persist_ok <= 1; persist_ok++) {
        fake_catalog_t catalog = {
            .available = true,
            .decode_ok = persist_ok != 0,
            .persist_ok = false,
            .current = card("answer", KANJI_SOURCE_CATALOG),
            .ordinal = 41,
        };
        study_runtime_t runtime;
        study_runtime_init(&runtime);
        CHECK(study_runtime_restore(&runtime, ops(&catalog), lock_ops(&catalog)) ==
              STUDY_RESTORE_CATALOG);
        catalog.event_count = 0;
        runtime.nav.revealed = true;
        CHECK(study_runtime_capture_grade(&runtime, KANJI_GRADE_HARD) ==
              STUDY_GRADE_LOCAL);
        const study_grade_request_t request = runtime.pending_grade;

        CHECK(study_runtime_process_local_grade(
                  &runtime, &request, ops(&catalog), lock_ops(&catalog)) ==
              STUDY_LOCAL_FAILED);
        CHECK(strcmp(runtime.data.card.id, "answer") == 0);
        CHECK(runtime.catalog_ordinal == 41);
        CHECK(runtime.nav.revealed);
        CHECK(!runtime.pending_grade_valid);
        CHECK(catalog.events[catalog.event_count - 1] == EVENT_PUBLISH);
    }
}

static void captured_source_prevents_remote_local_cross_consumption(void)
{
    fake_catalog_t catalog = {
        .available = true,
        .decode_ok = true,
        .persist_ok = true,
        .current = card("local", KANJI_SOURCE_CATALOG),
        .ordinal = 9,
    };
    study_runtime_t runtime;
    study_runtime_init(&runtime);
    (void)study_runtime_restore(&runtime, ops(&catalog), lock_ops(&catalog));
    CHECK(study_runtime_capture_grade(&runtime, KANJI_GRADE_EASY) ==
          STUDY_GRADE_LOCAL);
    study_grade_request_t local = runtime.pending_grade;

    kanji_t remote = card("remote", KANJI_SOURCE_REMOTE);
    const uint32_t generation = source_guard_capture(&runtime.source_guard);
    CHECK(study_runtime_commit_remote(&runtime, &remote, generation, false) ==
          STUDY_REMOTE_PUBLISHED);
    CHECK(study_grade_route(&local) == STUDY_GRADE_LOCAL);
    CHECK(study_runtime_process_local_grade(
              &runtime, &local, ops(&catalog), lock_ops(&catalog)) ==
          STUDY_LOCAL_HIDDEN);
    CHECK(strcmp(runtime.data.card.id, "remote") == 0);
    CHECK(catalog.ordinal == 10);

    CHECK(study_runtime_capture_grade(&runtime, KANJI_GRADE_AGAIN) ==
          STUDY_GRADE_REMOTE);
    const study_grade_request_t captured_remote = runtime.pending_grade;
    const uint32_t remote_generation = runtime.pending_grade_generation;
    CHECK(study_runtime_remote_grade_ready(&runtime, remote_generation,
                                           "http://study"));
    source_guard_advance(&runtime.source_guard);
    CHECK(!study_runtime_remote_grade_ready(&runtime, remote_generation,
                                            "http://study"));
    CHECK(!study_runtime_remote_grade_ready(&runtime, remote_generation, ""));
    (void)study_runtime_restore(&runtime, ops(&catalog), lock_ops(&catalog));
    CHECK(study_grade_route(&captured_remote) == STUDY_GRADE_REMOTE);
}

static void stale_remote_result_preserves_catalog(void)
{
    fake_catalog_t catalog = {
        .available = true,
        .current = card("catalog", KANJI_SOURCE_CATALOG),
        .ordinal = 5,
    };
    study_runtime_t runtime;
    study_runtime_init(&runtime);
    (void)study_runtime_restore(&runtime, ops(&catalog), lock_ops(&catalog));
    const uint32_t stale = source_guard_capture(&runtime.source_guard);
    source_guard_advance(&runtime.source_guard);
    const kanji_t remote = card("remote", KANJI_SOURCE_REMOTE);
    CHECK(study_runtime_commit_remote(&runtime, &remote, stale, false) ==
          STUDY_REMOTE_STALE);
    CHECK(strcmp(runtime.data.card.id, "catalog") == 0);
}

int main(void)
{
    catalog_boot_and_url_clear_use_catalog_then_demo();
    local_grade_orders_decode_persist_then_publish();
    local_decode_or_state_failure_keeps_card_and_ordinal();
    captured_source_prevents_remote_local_cross_consumption();
    stale_remote_result_preserves_catalog();

    printf("study source: %d failures\n", failures);
    return failures ? 1 : 0;
}
