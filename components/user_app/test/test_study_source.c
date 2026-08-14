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
    int events[32];
    int event_count;
} fake_catalog_t;

static void record_event(fake_catalog_t *catalog, int event)
{
    CHECK(catalog->event_count <
          (int)(sizeof(catalog->events) / sizeof(catalog->events[0])));
    if (catalog->event_count <
        (int)(sizeof(catalog->events) / sizeof(catalog->events[0]))) {
        catalog->events[catalog->event_count++] = event;
    }
}

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
    record_event(catalog, EVENT_CURRENT);
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
    record_event(catalog, EVENT_DECODE);
    if (!catalog->decode_ok) {
        return false;
    }
    record_event(catalog, EVENT_PERSIST);
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
    record_event(catalog, EVENT_PUBLISH);
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
    study_runtime_advance_source(&runtime);
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
    study_runtime_advance_source(&runtime);
    const kanji_t remote = card("remote", KANJI_SOURCE_REMOTE);
    CHECK(study_runtime_commit_remote(&runtime, &remote, stale, false) ==
          STUDY_REMOTE_STALE);
    CHECK(strcmp(runtime.data.card.id, "catalog") == 0);
}

static void publication_revision_tracks_draws_not_source_only_changes(void)
{
    fake_catalog_t catalog = {
        .available = true,
        .decode_ok = true,
        .persist_ok = true,
        .current = card("local-answer", KANJI_SOURCE_CATALOG),
        .ordinal = 27,
    };
    study_runtime_t runtime;
    study_runtime_init(&runtime);
    (void)study_runtime_restore(&runtime, ops(&catalog), lock_ops(&catalog));
    const uint32_t boot_revision =
        study_runtime_publication_revision(&runtime);

    CHECK(study_runtime_capture_grade(&runtime, KANJI_GRADE_GOOD) ==
          STUDY_GRADE_LOCAL);
    const study_grade_request_t local = runtime.pending_grade;
    CHECK(study_runtime_process_local_grade(
              &runtime, &local, ops(&catalog), lock_ops(&catalog)) ==
          STUDY_LOCAL_PUBLISHED);
    const study_draw_token_t local_draw = study_runtime_capture_draw(
        &runtime, STUDY_DRAW_PUBLICATION_ONLY);
    CHECK(local_draw.publication_revision != boot_revision);

    /* Exact queue order under review: the local CARD_ADVANCED token is made,
     * then an older SET_URL(non-empty) command runs first. That source-only
     * change must not invalidate the card publication waiting behind it. */
    study_runtime_advance_source(&runtime);
    CHECK(study_runtime_accepts_draw(&runtime, &local_draw));

    /* URL clear restores another card/nav snapshot and invalidates local draw. */
    catalog.current = card("restored", KANJI_SOURCE_CATALOG);
    catalog.ordinal = 28;
    (void)study_runtime_restore(&runtime, ops(&catalog), lock_ops(&catalog));
    CHECK(!study_runtime_accepts_draw(&runtime, &local_draw));

    /* Exact remote queue order: remote data (or its status/error draw) commits
     * and captures both tokens, SET_URL(non-empty) runs ahead of the draw, and
     * the old-source command must be rejected even though the card revision is
     * otherwise still current. */
    uint32_t generation = source_guard_capture(&runtime.source_guard);
    kanji_t remote = card("remote-before-url", KANJI_SOURCE_REMOTE);
    CHECK(study_runtime_commit_remote(&runtime, &remote, generation, false) ==
          STUDY_REMOTE_PUBLISHED);
    const study_draw_token_t old_remote_draw = study_runtime_capture_draw(
        &runtime, STUDY_DRAW_PUBLICATION_AND_SOURCE);
    const study_draw_token_t old_remote_status = study_runtime_capture_draw(
        &runtime, STUDY_DRAW_PUBLICATION_AND_SOURCE);
    CHECK(study_runtime_accepts_draw(&runtime, &old_remote_draw));
    CHECK(study_runtime_accepts_draw(&runtime, &old_remote_status));
    runtime.nav.revealed = true;
    runtime.nav.sheet = KANJI_SHEET_DESCRIPTION;
    runtime.nav.sheet_page = 2;
    runtime.nav.grade = KANJI_GRADE_EASY;
    study_runtime_advance_source(&runtime);
    CHECK(!study_runtime_accepts_draw(&runtime, &old_remote_draw));
    CHECK(!study_runtime_accepts_draw(&runtime, &old_remote_status));

    /* URL B has no card yet. A B failure/status token must not borrow URL A's
     * still-current publication revision to redraw A, and A must not become a
     * grade request addressed to B. */
    const study_draw_token_t failed_b_status = study_runtime_capture_draw(
        &runtime, STUDY_DRAW_PUBLICATION_AND_SOURCE);
    CHECK(!study_runtime_accepts_draw(&runtime, &failed_b_status));
    CHECK(study_runtime_capture_grade(&runtime, KANJI_GRADE_HARD) ==
          STUDY_GRADE_NONE);
    CHECK(!runtime.pending_grade_valid);

    /* The first successful B response is a new publication even when its card
     * bytes equal A. Only then may B draw and grade that remote card. */
    generation = source_guard_capture(&runtime.source_guard);
    CHECK(study_runtime_commit_remote(&runtime, &remote, generation, false) ==
          STUDY_REMOTE_PUBLISHED);
    CHECK(!runtime.nav.revealed);
    CHECK(runtime.nav.sheet == KANJI_SHEET_NONE);
    CHECK(runtime.nav.sheet_page == 0);
    CHECK(runtime.nav.grade == KANJI_GRADE_GOOD);
    const study_draw_token_t current_remote_draw = study_runtime_capture_draw(
        &runtime, STUDY_DRAW_PUBLICATION_AND_SOURCE);
    CHECK(study_runtime_accepts_draw(&runtime, &current_remote_draw));
    CHECK(study_runtime_commit_remote(&runtime, &remote, generation, false) ==
          STUDY_REMOTE_UNCHANGED);
    CHECK(study_runtime_accepts_draw(&runtime, &current_remote_draw));
    CHECK(study_runtime_capture_grade(&runtime, KANJI_GRADE_HARD) ==
          STUDY_GRADE_REMOTE);
    runtime.pending_grade_valid = false;

    /* A later remote publication also supersedes a queued local-only draw. */
    catalog.current = card("local-again", KANJI_SOURCE_CATALOG);
    (void)study_runtime_restore(&runtime, ops(&catalog), lock_ops(&catalog));
    CHECK(study_runtime_capture_grade(&runtime, KANJI_GRADE_EASY) ==
          STUDY_GRADE_LOCAL);
    const study_grade_request_t second_local = runtime.pending_grade;
    CHECK(study_runtime_process_local_grade(
              &runtime, &second_local, ops(&catalog), lock_ops(&catalog)) ==
          STUDY_LOCAL_PUBLISHED);
    const study_draw_token_t second_local_draw = study_runtime_capture_draw(
        &runtime, STUDY_DRAW_PUBLICATION_ONLY);
    generation = source_guard_capture(&runtime.source_guard);
    remote = card("remote-after-local", KANJI_SOURCE_REMOTE);
    CHECK(study_runtime_commit_remote(&runtime, &remote, generation, false) ==
          STUDY_REMOTE_PUBLISHED);
    CHECK(!study_runtime_accepts_draw(&runtime, &second_local_draw));
}

int main(void)
{
    catalog_boot_and_url_clear_use_catalog_then_demo();
    local_grade_orders_decode_persist_then_publish();
    local_decode_or_state_failure_keeps_card_and_ordinal();
    captured_source_prevents_remote_local_cross_consumption();
    stale_remote_result_preserves_catalog();
    publication_revision_tracks_draws_not_source_only_changes();

    printf("study source: %d failures\n", failures);
    return failures ? 1 : 0;
}
