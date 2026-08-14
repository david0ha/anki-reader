/* Pure integer layout shared by firmware, simulator, and host tests. */
#include "ui_kanji_layout.h"

#define EDGE      16
#define RAIL_W    80
#define GUTTER    16
#define MAIN_X   112
#define MAIN_W   520
#define MAIN_Y    56
#define MAIN_H   368
#define FOOTER_Y 440
#define FOOTER_H  40

#define FOOTER_SLOT_W 154
#define DOCK_Y        344
#define DOCK_H         80
#define CELL_W        (MAIN_W / KANJI_GRADE_COUNT)

#define KEYCAP(i) { EDGE + (i) * FOOTER_SLOT_W, FOOTER_Y + 8, 24, 24 }
#define ACTION(i) { EDGE + (i) * FOOTER_SLOT_W + 30, FOOTER_Y + 10, \
                    FOOTER_SLOT_W - 30, 22 }
#define CELL(i)   { MAIN_X + (i) * CELL_W, DOCK_Y, CELL_W, DOCK_H }
#define LABEL(i)  { MAIN_X + (i) * CELL_W + 8, DOCK_Y + 10, CELL_W - 16, 24 }
#define SPAN(i)   { MAIN_X + (i) * CELL_W + 8, DOCK_Y + 44, CELL_W - 16, 20 }

static const kanji_chrome_t CHROME = {
    .rail          = { EDGE, EDGE, RAIL_W, 408 },
    .rail_rule     = { EDGE + RAIL_W, EDGE, 1, 408 },
    .rail_identity = { EDGE, EDGE, RAIL_W, 96 },
    .rail_progress = { EDGE, 328, RAIL_W, 96 },
    .main          = { MAIN_X, MAIN_Y, MAIN_W, MAIN_H },
    .masthead      = { MAIN_X, EDGE, MAIN_W, 24 },
    .brand         = { MAIN_X, EDGE, 96, 24 },
    .session       = { 224, EDGE, 288, 24 },
    .battery       = { 520, EDGE, 112, 24 },
    .footer        = { EDGE, FOOTER_Y, KANJI_SCREEN_W - 2 * EDGE, FOOTER_H },
    .keycap        = { KEYCAP(0), KEYCAP(1), KEYCAP(2), KEYCAP(3) },
    .key_action    = { ACTION(0), ACTION(1), ACTION(2), ACTION(3) },
    .header        = { MAIN_X, EDGE, MAIN_W, 24 },
    .key           = { ACTION(0), ACTION(1), ACTION(2), ACTION(3) },
};

static const kanji_question_layout_t QUESTION = {
    .hero      = { MAIN_X, 88, MAIN_W, 105 },
    .prompt    = { MAIN_X, 224, MAIN_W, 28 },
    .secondary = { MAIN_X, 264, MAIN_W, 48 },
    .counts    = { MAIN_X, 328, MAIN_W, 24 },
    .player    = { MAIN_X, MAIN_Y, MAIN_W, MAIN_H },
    .caption   = { MAIN_X, 264, MAIN_W, 48 },
    .queue     = { MAIN_X, 328, MAIN_W, 24 },
    .rail      = { EDGE, EDGE, RAIL_W, 408 },
    .scrubber  = { MAIN_X, 423, MAIN_W, 1 },
};

static const kanji_answer_layout_t ANSWER = {
    .hero         = { MAIN_X, MAIN_Y, MAIN_W, 105 },
    .reading      = { MAIN_X, 164, MAIN_W, 24 },
    .meaning      = { MAIN_X, 194, MAIN_W, 35 },
    .examples     = { MAIN_X, 236, MAIN_W, 72 },
    .example_step = 24,
    .example_rows = KANJI_EXAMPLES_MAX,
    .prompt       = { MAIN_X, 312, MAIN_W, 24 },
    .dock         = { MAIN_X, DOCK_Y, MAIN_W, DOCK_H },
    .cell         = { CELL(0), CELL(1), CELL(2), CELL(3) },
    .cell_label   = { LABEL(0), LABEL(1), LABEL(2), LABEL(3) },
    .cell_span    = { SPAN(0), SPAN(1), SPAN(2), SPAN(3) },
    .band         = { MAIN_X, MAIN_Y, MAIN_W, 105 },
    .level        = { MAIN_X, 164, MAIN_W, 24 },
};

#define SHEET_COMMON \
    .headword = { MAIN_X, MAIN_Y, 280, 32 }, \
    .title    = { 400, 60, 160, 28 }

static const kanji_sheet_layout_t SHEET_PLAIN = {
    SHEET_COMMON,
    .body  = { MAIN_X, 104, MAIN_W, 320 },
    .stats = { 0, 0, 0, 0 },
    .stat  = { {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0} },
    .pager = { 572, 64, 60, 20 },
    .band = { MAIN_X, MAIN_Y, MAIN_W, 32 },
    .band_word = { MAIN_X, MAIN_Y, 280, 32 },
    .band_title = { 400, 60, 160, 28 },
};

#define STAT_W (MAIN_W / KANJI_STAT_CELLS)
#define STAT(i) { MAIN_X + (i) * STAT_W, 356, STAT_W, 68 }

static const kanji_sheet_layout_t SHEET_WITH_STATS = {
    SHEET_COMMON,
    .body  = { MAIN_X, 104, MAIN_W, 232 },
    .stats = { MAIN_X, 348, MAIN_W, 76 },
    .stat  = { STAT(0), STAT(1), STAT(2), STAT(3), STAT(4) },
    .pager = { 572, 64, 60, 20 },
    .band = { MAIN_X, MAIN_Y, MAIN_W, 32 },
    .band_word = { MAIN_X, MAIN_Y, 280, 32 },
    .band_title = { 400, 60, 160, 28 },
};

const kanji_chrome_t *kanji_chrome_layout(void) { return &CHROME; }
const kanji_question_layout_t *kanji_question_layout(void) { return &QUESTION; }
const kanji_answer_layout_t *kanji_answer_layout(void) { return &ANSWER; }
const kanji_sheet_layout_t *kanji_sheet_layout(bool with_stats)
{
    return with_stats ? &SHEET_WITH_STATS : &SHEET_PLAIN;
}

void kanji_rect_to_half_open(const kanji_rect_t *r,
                             int *x1, int *y1, int *x2, int *y2)
{
    if (!r) return;
    if (x1) *x1 = r->x;
    if (y1) *y1 = r->y;
    if (x2) *x2 = r->x + r->w;
    if (y2) *y2 = r->y + r->h;
}

#define HERO_LARGE_MAX_CHARS 5

bool kanji_hero_is_large(const char *front)
{
    return kanji_utf8_len(front) <= HERO_LARGE_MAX_CHARS;
}

int kanji_center_x(const kanji_rect_t *outer, int w)
{
    if (!outer) return 0;
    if (w >= outer->w) return outer->x;
    return outer->x + (outer->w - w) / 2;
}
