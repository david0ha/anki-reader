#include "study_source.h"

#include <string.h>

#include "kanji_mock.h"

static bool valid_grade(kanji_grade_t grade)
{
    return grade >= KANJI_GRADE_AGAIN && grade <= KANJI_GRADE_EASY;
}

static void lock_state(study_state_lock_t state)
{
    if (state.lock != NULL) {
        state.lock(state.context);
    }
}

static void unlock_state(study_state_lock_t state)
{
    if (state.unlock != NULL) {
        state.unlock(state.context);
    }
}

void study_runtime_init(study_runtime_t *runtime)
{
    if (runtime == NULL) {
        return;
    }
    memset(runtime, 0, sizeof(*runtime));
    kanji_nav_reset(&runtime->nav);
}

study_restore_result_t study_runtime_restore(study_runtime_t *runtime,
                                             study_catalog_ops_t catalog,
                                             study_state_lock_t state)
{
    const kanji_t *candidate = NULL;
    uint16_t ordinal = 0;
    if (catalog.available != NULL && catalog.current != NULL &&
        catalog.ordinal != NULL && catalog.available(catalog.context)) {
        candidate = catalog.current(catalog.context);
        if (candidate != NULL && candidate->valid) {
            ordinal = catalog.ordinal(catalog.context);
        } else {
            candidate = NULL;
        }
    }

    lock_state(state);
    if (candidate != NULL) {
        runtime->data = *candidate;
        runtime->catalog_ordinal = ordinal;
    } else {
        kanji_mock(&runtime->data);
        runtime->catalog_ordinal = 0;
    }
    runtime->hash = kanji_hash(&runtime->data);
    kanji_nav_reset(&runtime->nav);
    unlock_state(state);
    return candidate != NULL ? STUDY_RESTORE_CATALOG : STUDY_RESTORE_DEMO;
}

study_grade_route_t study_grade_route(const study_grade_request_t *request)
{
    if (request == NULL || !valid_grade(request->grade)) {
        return STUDY_GRADE_NONE;
    }
    if (request->source == KANJI_SOURCE_CATALOG) {
        return STUDY_GRADE_LOCAL;
    }
    if (request->source == KANJI_SOURCE_REMOTE) {
        return STUDY_GRADE_REMOTE;
    }
    return STUDY_GRADE_NONE;
}

study_grade_route_t study_runtime_capture_grade(study_runtime_t *runtime,
                                                kanji_grade_t grade)
{
    if (runtime == NULL || runtime->pending_grade_valid) {
        return STUDY_GRADE_NONE;
    }

    study_grade_request_t request;
    memset(&request, 0, sizeof(request));
    request.source = runtime->data.source;
    request.grade = grade;
    request.catalog_ordinal = runtime->catalog_ordinal;
    kanji_str_copy(request.remote_card_id, sizeof(request.remote_card_id),
                   runtime->data.card.id);
    const study_grade_route_t route = study_grade_route(&request);
    if (route != STUDY_GRADE_NONE) {
        runtime->pending_grade = request;
        runtime->pending_grade_generation =
            source_guard_capture(&runtime->source_guard);
        runtime->pending_grade_valid = true;
    }
    return route;
}

bool study_runtime_remote_grade_ready(const study_runtime_t *runtime,
                                      uint32_t captured_generation,
                                      const char *url)
{
    return runtime != NULL && url != NULL && url[0] != '\0' &&
           source_guard_accepts(&runtime->source_guard, captured_generation);
}

study_remote_result_t study_runtime_commit_remote(study_runtime_t *runtime,
                                                  const kanji_t *fetched,
                                                  uint32_t generation,
                                                  bool advanced)
{
    if (runtime == NULL || fetched == NULL ||
        !source_guard_accepts(&runtime->source_guard, generation)) {
        if (runtime != NULL && advanced) {
            runtime->pending_grade_valid = false;
        }
        return STUDY_REMOTE_STALE;
    }

    const uint32_t hash = kanji_hash(fetched);
    const bool transitioned = runtime->data.source != fetched->source;
    const bool changed = hash != runtime->hash || advanced || transitioned;
    if (advanced) {
        runtime->pending_grade_valid = false;
    }
    runtime->data = *fetched;
    runtime->hash = hash;
    if (advanced || transitioned) {
        kanji_nav_reset(&runtime->nav);
    }
    return changed ? STUDY_REMOTE_PUBLISHED : STUDY_REMOTE_UNCHANGED;
}

study_local_result_t study_runtime_process_local_grade(
    study_runtime_t *runtime,
    const study_grade_request_t *request,
    study_catalog_ops_t catalog,
    study_state_lock_t state)
{
    bool saved = false;
    const kanji_t *next = NULL;
    uint16_t next_ordinal = request != NULL ? request->catalog_ordinal : 0;

    if (runtime != NULL && study_grade_route(request) == STUDY_GRADE_LOCAL &&
        catalog.available != NULL && catalog.ordinal != NULL &&
        catalog.grade != NULL && catalog.current != NULL &&
        catalog.available(catalog.context) &&
        catalog.ordinal(catalog.context) == request->catalog_ordinal) {
        saved = catalog.grade(catalog.context, request->grade);
        if (saved) {
            next_ordinal = catalog.ordinal(catalog.context);
            next = catalog.current(catalog.context);
            saved = next != NULL && next->valid;
        }
    }

    study_local_result_t result = STUDY_LOCAL_FAILED;
    lock_state(state);
    if (runtime != NULL) {
        if (saved) {
            runtime->catalog_ordinal = next_ordinal;
            if (runtime->data.source == KANJI_SOURCE_CATALOG) {
                runtime->data = *next;
                runtime->hash = kanji_hash(&runtime->data);
                kanji_nav_reset(&runtime->nav);
                result = STUDY_LOCAL_PUBLISHED;
            } else {
                result = STUDY_LOCAL_HIDDEN;
            }
        }
        runtime->pending_grade_valid = false;
    }
    unlock_state(state);
    return result;
}
