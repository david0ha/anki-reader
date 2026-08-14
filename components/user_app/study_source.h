/* Pure source arbitration and captured-grade policy for the study runtime. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "kanji_model.h"

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
    kanji_source_t source;
    uint16_t catalog_ordinal;
    bool answer_visible;
} study_source_state_t;

study_source_state_t study_source_boot(bool catalog_available,
                                       uint16_t catalog_ordinal);
study_source_state_t study_source_remote_result(study_source_state_t current,
                                                bool succeeded);
study_source_state_t study_source_clear_url(bool catalog_available,
                                            uint16_t catalog_ordinal);
study_source_state_t study_source_local_grade_result(study_source_state_t current,
                                                     bool persisted,
                                                     uint16_t next_ordinal);

study_grade_route_t study_grade_capture(study_grade_request_t *out,
                                        kanji_source_t source,
                                        kanji_grade_t grade,
                                        uint16_t catalog_ordinal,
                                        const char *remote_card_id);
study_grade_route_t study_grade_route(const study_grade_request_t *request);

#ifdef __cplusplus
}
#endif
