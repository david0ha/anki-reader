/* Production source arbitration and local-grade transaction orchestration. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "kanji_model.h"
#include "kanji_nav.h"
#include "source_guard.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STUDY_GRADE_NONE = 0,
    STUDY_GRADE_LOCAL,
    STUDY_GRADE_REMOTE,
} study_grade_route_t;

typedef struct {
    kanji_source_t source;
    kanji_grade_t grade;
    uint16_t catalog_ordinal;
    char remote_card_id[KANJI_ID_MAX];
} study_grade_request_t;

typedef struct {
    kanji_t data;
    uint32_t hash;
    uint16_t catalog_ordinal;
    source_guard_t source_guard;
    kanji_nav_t nav;
    study_grade_request_t pending_grade;
    bool pending_grade_valid;
    uint32_t pending_grade_generation;
} study_runtime_t;

typedef struct {
    void *context;
    bool (*available)(void *context);
    const kanji_t *(*current)(void *context);
    uint16_t (*ordinal)(void *context);
    /* Must decode the next card, persist/verify the grade, and publish the
     * store snapshot in that order, or return false without changing it. */
    bool (*grade)(void *context, kanji_grade_t grade);
} study_catalog_ops_t;

typedef struct {
    void *context;
    void (*lock)(void *context);
    void (*unlock)(void *context);
} study_state_lock_t;

typedef enum {
    STUDY_RESTORE_CATALOG = 0,
    STUDY_RESTORE_DEMO,
} study_restore_result_t;

typedef enum {
    STUDY_REMOTE_STALE = 0,
    STUDY_REMOTE_UNCHANGED,
    STUDY_REMOTE_PUBLISHED,
} study_remote_result_t;

typedef enum {
    STUDY_LOCAL_FAILED = 0,
    STUDY_LOCAL_PUBLISHED,
    STUDY_LOCAL_HIDDEN,
} study_local_result_t;

void study_runtime_init(study_runtime_t *runtime);

/* Used by both cold boot and URL clear. Catalog callbacks are invoked before
 * the state lock is taken; the final card/hash/ordinal/nav swap is atomic. */
study_restore_result_t study_runtime_restore(study_runtime_t *runtime,
                                             study_catalog_ops_t catalog,
                                             study_state_lock_t state);

study_grade_route_t study_runtime_capture_grade(study_runtime_t *runtime,
                                                kanji_grade_t grade);
study_grade_route_t study_grade_route(const study_grade_request_t *request);
bool study_runtime_remote_grade_ready(const study_runtime_t *runtime,
                                      uint32_t captured_generation,
                                      const char *url);

/* Commit a successful remote fetch while the caller holds the state lock. */
study_remote_result_t study_runtime_commit_remote(study_runtime_t *runtime,
                                                  const kanji_t *fetched,
                                                  uint32_t generation,
                                                  bool advanced);

/* The caller serializes catalog access. This routine performs store grade
 * first and takes the state lock only for the final publication/clear. */
study_local_result_t study_runtime_process_local_grade(
    study_runtime_t *runtime,
    const study_grade_request_t *request,
    study_catalog_ops_t catalog,
    study_state_lock_t state);

#ifdef __cplusplus
}
#endif
