/* Pure integer geometry for the 648x480 Lexicographic Instrument UI. */
#pragma once

#include <stdbool.h>

#include "kanji_model.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KANJI_SCREEN_W   648
#define KANJI_SCREEN_H   480
#define KANJI_BYTE_ALIGN 8
#define KANJI_STAT_CELLS 5

typedef struct {
    int x, y, w, h;
} kanji_rect_t;

/* Convert an origin-and-size rectangle to half-open panel bounds
 * [x1, x2) x [y1, y2). NULL output pointers are ignored. */
void kanji_rect_to_half_open(const kanji_rect_t *r,
                             int *x1, int *y1, int *x2, int *y2);

typedef struct {
    kanji_rect_t rail;
    kanji_rect_t rail_rule;
    kanji_rect_t rail_identity;
    kanji_rect_t rail_progress;
    kanji_rect_t main;
    kanji_rect_t masthead;
    kanji_rect_t brand;
    kanji_rect_t session;
    kanji_rect_t battery;
    kanji_rect_t footer;
    kanji_rect_t keycap[4];
    kanji_rect_t key_action[4];
    /* Compile-only locations for the Task 5 replacement of legacy simulator
     * pixel probes. The UI never draws or consumes these rectangles. */
    kanji_rect_t header;
    kanji_rect_t key[4];
} kanji_chrome_t;

typedef struct {
    kanji_rect_t hero;
    kanji_rect_t prompt;
    kanji_rect_t secondary;
    kanji_rect_t counts;
    /* Legacy simulator probes, not presentation regions. */
    kanji_rect_t player, caption, queue, rail, scrubber;
} kanji_question_layout_t;

typedef struct {
    kanji_rect_t hero;
    kanji_rect_t reading;
    kanji_rect_t meaning;
    kanji_rect_t examples;
    int example_step;
    int example_rows;
    kanji_rect_t prompt;
    /* Origin/size geometry for drawing. Before panel refresh it is converted
     * to half-open bounds; the size rectangle is not passed verbatim. */
    kanji_rect_t dock;
    kanji_rect_t cell[KANJI_GRADE_COUNT];
    kanji_rect_t cell_label[KANJI_GRADE_COUNT];
    kanji_rect_t cell_span[KANJI_GRADE_COUNT];
    /* Legacy simulator probes, not presentation regions. */
    kanji_rect_t band, level;
} kanji_answer_layout_t;

typedef struct {
    kanji_rect_t headword;
    kanji_rect_t title;
    kanji_rect_t body;
    kanji_rect_t stats;
    kanji_rect_t stat[KANJI_STAT_CELLS];
    kanji_rect_t pager;
    /* Legacy simulator probes, not presentation regions. */
    kanji_rect_t band, band_word, band_title;
} kanji_sheet_layout_t;

const kanji_chrome_t          *kanji_chrome_layout(void);
const kanji_question_layout_t *kanji_question_layout(void);
const kanji_answer_layout_t   *kanji_answer_layout(void);
const kanji_sheet_layout_t    *kanji_sheet_layout(bool with_stats);

bool kanji_hero_is_large(const char *front);
int kanji_center_x(const kanji_rect_t *outer, int w);

/* Canonical display-only headword text. Raw model/parser/hash data is left
 * untouched; ASCII display whitespace is collapsed and trimmed into the
 * model-sized destination. */
size_t kanji_headword_display_text(char dst[KANJI_FRONT_MAX],
                                   const char *front);

#ifdef __cplusplus
}
#endif
