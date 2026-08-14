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

static void catalog_boots_at_the_restored_card(void)
{
    study_source_state_t state = study_source_boot(true, 73);

    CHECK(state.source == KANJI_SOURCE_CATALOG);
    CHECK(state.catalog_ordinal == 73);
    CHECK(!state.answer_visible);
}

static void catalog_and_remote_grades_choose_distinct_routes(void)
{
    study_grade_request_t local;
    study_grade_request_t remote;

    CHECK(study_grade_capture(&local, KANJI_SOURCE_CATALOG,
                              KANJI_GRADE_HARD, 29, "catalog-card") ==
          STUDY_GRADE_LOCAL);
    CHECK(local.source == KANJI_SOURCE_CATALOG);
    CHECK(local.grade == KANJI_GRADE_HARD);
    CHECK(local.catalog_ordinal == 29);
    CHECK(strcmp(local.remote_card_id, "catalog-card") == 0);

    CHECK(study_grade_capture(&remote, KANJI_SOURCE_REMOTE,
                              KANJI_GRADE_EASY, 41, "remote-17") ==
          STUDY_GRADE_REMOTE);
    CHECK(remote.source == KANJI_SOURCE_REMOTE);
    CHECK(remote.grade == KANJI_GRADE_EASY);
    CHECK(remote.catalog_ordinal == 41);
    CHECK(strcmp(remote.remote_card_id, "remote-17") == 0);
}

static void a_captured_request_cannot_be_rerouted_by_a_source_change(void)
{
    study_grade_request_t local;
    study_grade_request_t remote;
    study_source_state_t current = study_source_boot(true, 11);

    (void)study_grade_capture(&local, current.source, KANJI_GRADE_GOOD,
                              current.catalog_ordinal, "local-id");
    current = study_source_remote_result(current, true);
    CHECK(current.source == KANJI_SOURCE_REMOTE);
    CHECK(study_grade_route(&local) == STUDY_GRADE_LOCAL);

    (void)study_grade_capture(&remote, current.source, KANJI_GRADE_AGAIN,
                              current.catalog_ordinal, "remote-id");
    current = study_source_clear_url(true, 11);
    CHECK(current.source == KANJI_SOURCE_CATALOG);
    CHECK(study_grade_route(&remote) == STUDY_GRADE_REMOTE);
}

static void remote_success_takes_over_but_failure_preserves_catalog(void)
{
    study_source_state_t catalog = study_source_boot(true, 88);
    catalog.answer_visible = true;

    study_source_state_t failed = study_source_remote_result(catalog, false);
    CHECK(failed.source == KANJI_SOURCE_CATALOG);
    CHECK(failed.catalog_ordinal == 88);
    CHECK(failed.answer_visible);

    study_source_state_t fetched = study_source_remote_result(catalog, true);
    CHECK(fetched.source == KANJI_SOURCE_REMOTE);
    CHECK(fetched.catalog_ordinal == 88);
    CHECK(!fetched.answer_visible);
}

static void clearing_the_url_restores_catalog_or_uses_demo_as_last_resort(void)
{
    study_source_state_t restored = study_source_clear_url(true, 902);
    CHECK(restored.source == KANJI_SOURCE_CATALOG);
    CHECK(restored.catalog_ordinal == 902);

    study_source_state_t no_catalog_boot = study_source_boot(false, 77);
    study_source_state_t no_catalog_clear = study_source_clear_url(false, 77);
    CHECK(no_catalog_boot.source == KANJI_SOURCE_DEMO);
    CHECK(no_catalog_clear.source == KANJI_SOURCE_DEMO);
}

static void failed_local_persistence_keeps_the_answer_and_ordinal(void)
{
    study_source_state_t answer = study_source_boot(true, 123);
    answer.answer_visible = true;

    study_source_state_t failed =
        study_source_local_grade_result(answer, false, 124);
    CHECK(failed.source == KANJI_SOURCE_CATALOG);
    CHECK(failed.catalog_ordinal == 123);
    CHECK(failed.answer_visible);

    study_source_state_t saved =
        study_source_local_grade_result(answer, true, 124);
    CHECK(saved.catalog_ordinal == 124);
    CHECK(!saved.answer_visible);
}

int main(void)
{
    catalog_boots_at_the_restored_card();
    catalog_and_remote_grades_choose_distinct_routes();
    a_captured_request_cannot_be_rerouted_by_a_source_change();
    remote_success_takes_over_but_failure_preserves_catalog();
    clearing_the_url_restores_catalog_or_uses_demo_as_last_resort();
    failed_local_persistence_keeps_the_answer_and_ordinal();

    printf("study source: %d failures\n", failures);
    return failures ? 1 : 0;
}
