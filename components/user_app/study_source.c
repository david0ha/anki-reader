#include "study_source.h"

#include <string.h>

static bool valid_grade(kanji_grade_t grade)
{
    return grade >= KANJI_GRADE_AGAIN && grade <= KANJI_GRADE_EASY;
}

study_source_state_t study_source_boot(bool catalog_available,
                                       uint16_t catalog_ordinal)
{
    return (study_source_state_t){
        .source = catalog_available ? KANJI_SOURCE_CATALOG : KANJI_SOURCE_DEMO,
        .catalog_ordinal = catalog_available ? catalog_ordinal : 0,
        .answer_visible = false,
    };
}

study_source_state_t study_source_remote_result(study_source_state_t current,
                                                bool succeeded)
{
    if (succeeded) {
        current.source = KANJI_SOURCE_REMOTE;
        current.answer_visible = false;
    }
    return current;
}

study_source_state_t study_source_clear_url(bool catalog_available,
                                            uint16_t catalog_ordinal)
{
    return study_source_boot(catalog_available, catalog_ordinal);
}

study_source_state_t study_source_local_grade_result(study_source_state_t current,
                                                     bool persisted,
                                                     uint16_t next_ordinal)
{
    if (persisted && current.source == KANJI_SOURCE_CATALOG) {
        current.catalog_ordinal = next_ordinal;
        current.answer_visible = false;
    }
    return current;
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

study_grade_route_t study_grade_capture(study_grade_request_t *out,
                                        kanji_source_t source,
                                        kanji_grade_t grade,
                                        uint16_t catalog_ordinal,
                                        const char *remote_card_id)
{
    if (out == NULL) {
        return STUDY_GRADE_NONE;
    }

    memset(out, 0, sizeof(*out));
    out->source = source;
    out->grade = grade;
    out->catalog_ordinal = catalog_ordinal;
    if (remote_card_id != NULL) {
        size_t length = strlen(remote_card_id);
        if (length >= sizeof(out->remote_card_id)) {
            length = sizeof(out->remote_card_id) - 1;
        }
        memcpy(out->remote_card_id, remote_card_id, length);
        out->remote_card_id[length] = '\0';
    }
    return study_grade_route(out);
}
