/* Pure integer layout shared by firmware, simulator, and host tests.
 *
 * The horizontal grid and the reasoning behind it are in ui_kanji_layout.h. This file is the
 * vertical rhythm, and every height in it is a face's real line height plus its leading —
 * 16->20, 20->24, 28->35, 56->66 — never a round number. A slot sized to a round number either
 * clips a descender or leaves a gap that reads as a mistake in a stack of five. */
#include "ui_kanji_layout.h"

/* --- 문제, the plate -----------------------------------------------------------------------
 *
 *   y      what                                             h
 *   16     header band (badge, brand, status, counters)     24
 *   48     hairline                                          1
 *   80     the headword                                     72
 *  176     hairline, ornament, hairline                       1
 *  200     the pull-quote and its かな                       72
 *  288     the plate, four rows of 34                       136
 *  436     hairline                                          1
 *  446     queue counters and the reveal prompt             20
 *
 * The air between the hero and the ornament (24 px) and between the ornament and the quote
 * (24 px) is equal, but the air ABOVE the ornament rule is measured to the headword's box and
 * not to its ink, so optically the mark still belongs to the rule under the word rather than
 * floating between the two.
 *
 * The plate's row pitch is 34 rather than a line of 16 px type, because its VALUES are set in
 * the 28 px face. That is the one place the front spends scale on something other than the
 * headword, and it is what stops the lower third reading as a footnote: this block is the
 * learner's own record with the card, and on a frame that is looked at far more often than it
 * is pressed, it is most of the reason to look. */
#define F_HEAD_Y        16
#define F_HEAD_H        24
#define F_HEAD_RULE_Y   48
#define F_HERO_Y        80
#define F_HERO_H        72
#define F_ORN_Y        176
#define F_ORN_MARK      10
#define F_ORN_SEG_W    120
#define F_QUOTE_Y      200
#define F_QUOTE_LEAD    40     /* the word, KR 28 */
#define F_QUOTE_H       72     /* the word and its かな together */
#define F_QUOTE_INSET   60     /* a pull-quote wants a shorter measure than the page */
#define F_PLATE_Y      284
#define F_PLATE_STEP    36     /* kr_28 sets a 35 px line; 34 overlapped by one */
#define F_PLATE_COL_W  124
#define F_PLATE_VAL_W  200
#define F_PLATE_GAP     16     /* paper either side of the plate's vertical hairline */
#define F_FOOT_RULE_Y  436
#define F_FOOT_Y       446
#define F_FOOT_H        20

#define F_CENTER       (KANJI_SCREEN_W / 2)                             /* 324 */
#define F_PLATE_H      (KANJI_PLATE_ROWS * F_PLATE_STEP)                /* 104 */
#define F_PLATE_L_X    (F_CENTER - F_PLATE_GAP - F_PLATE_COL_W)         /* 184 */
#define F_PLATE_V_X    (F_CENTER + F_PLATE_GAP)                         /* 340 */

/* The label is set in the 16 px face beside a value in the 28 px one, so its box is pushed down
 * to put the two on a SHARED BASELINE. The numbers are the faces' own: kr_16 is 20/5 (ascent 15)
 * and kr_28 is 35/8 (ascent 27), so 27 - 15 = 12. Top-aligning the pair instead — which is what
 * happens if nobody thinks about it — floats the caption a third of a line above the figure it
 * captions, and in a stack of four that reads as four separate rendering faults rather than as
 * one block. */
#define F_PLATE_LABEL_DY 12

#define F_PLATE_LABEL(i) { F_PLATE_L_X, F_PLATE_Y + (i) * F_PLATE_STEP + F_PLATE_LABEL_DY, \
                           F_PLATE_COL_W, F_PLATE_STEP - F_PLATE_LABEL_DY }
#define F_PLATE_VALUE(i) { F_PLATE_V_X, F_PLATE_Y + (i) * F_PLATE_STEP, \
                           F_PLATE_VAL_W, F_PLATE_STEP }

static const kanji_front_layout_t FRONT = {
    .badge      = { KANJI_CONTENT_X, F_HEAD_Y, 48, F_HEAD_H },
    .brand      = { 84,  F_HEAD_Y, 200, F_HEAD_H },
    .status     = { 296, F_HEAD_Y, 104, F_HEAD_H },
    .counters   = { 412, F_HEAD_Y, 212, F_HEAD_H },
    .head_rule  = { KANJI_CONTENT_X, F_HEAD_RULE_Y, KANJI_CONTENT_W, KANJI_RULE_HAIR },

    .hero       = { KANJI_CONTENT_X, F_HERO_Y, KANJI_CONTENT_W, F_HERO_H },

    .orn_left   = { F_CENTER - F_ORN_MARK / 2 - 20 - F_ORN_SEG_W, F_ORN_Y,
                    F_ORN_SEG_W, KANJI_RULE_HAIR },
    .orn_mark   = { F_CENTER - F_ORN_MARK / 2, F_ORN_Y - F_ORN_MARK / 2,
                    F_ORN_MARK, F_ORN_MARK },
    .orn_right  = { F_CENTER + F_ORN_MARK / 2 + 20, F_ORN_Y,
                    F_ORN_SEG_W, KANJI_RULE_HAIR },

    .quote         = { KANJI_CONTENT_X + F_QUOTE_INSET, F_QUOTE_Y,
                       KANJI_CONTENT_W - 2 * F_QUOTE_INSET, F_QUOTE_LEAD },
    .quote_reading = { KANJI_CONTENT_X + F_QUOTE_INSET, F_QUOTE_Y + F_QUOTE_LEAD + 6,
                       KANJI_CONTENT_W - 2 * F_QUOTE_INSET, 26 },

    .plate_label = { F_PLATE_LABEL(0), F_PLATE_LABEL(1),
                     F_PLATE_LABEL(2), F_PLATE_LABEL(3) },
    .plate_rule  = { F_CENTER, F_PLATE_Y, KANJI_RULE_HAIR, F_PLATE_H },
    .plate_value = { F_PLATE_VALUE(0), F_PLATE_VALUE(1),
                     F_PLATE_VALUE(2), F_PLATE_VALUE(3) },
    .plate_empty = { KANJI_CONTENT_X, F_PLATE_Y + F_PLATE_STEP,
                     KANJI_CONTENT_W, F_PLATE_STEP },

    .foot_rule  = { KANJI_CONTENT_X, F_FOOT_RULE_Y, KANJI_CONTENT_W, KANJI_RULE_HAIR },
    .queue      = { KANJI_CONTENT_X, F_FOOT_Y, 360, F_FOOT_H },
    .prompt     = { 400, F_FOOT_Y, 224, F_FOOT_H },
};

/* --- 정답, the spread ----------------------------------------------------------------------
 *
 *   y      what
 *   14     the headword, with the status, the due span, the reading and the level beside it
 *   96     the band rule — 2 px, the only one on the board
 *  112     the two columns begin
 *  412     the columns end and the dock begins
 *  464     the dock ends, 16 px of paper below it
 *
 * The masthead carries the due span rather than the rail because 안정 and 다음 are very nearly
 * the same number — at the backend's 0.9 desired retention an FSRS interval is within a percent
 * of the stability it came from — so printing both is printing one fact twice. The rail keeps
 * the three that are independent: 반복, 실패, 난이도. */
#define B_HERO_Y         14
#define B_HERO_H         72
#define B_HERO_W        300     /* 5 chars at 56 px is 280; 10 at the 28 px fallback is also 280 */
#define B_SIDE_X        372     /* everything to the right of the hero */
#define B_META_Y         20
#define B_META_H         20
#define B_READ_Y         52
#define B_READ_H         26
#define B_BADGE_W        48
#define B_BAND_RULE_Y    96

#define B_TOP           112     /* the columns */
#define B_BOT           412

#define B_EYEBROW_H      18
#define B_SENSE_Y       132
#define B_SENSE_H        70     /* KR 28 x 2 */
#define B_SENSE_RULE_Y  212
#define B_BUILD_EB_Y    222
#define B_BUILD_Y       242
#define B_BUILD_H        80     /* KR 16 x 4 */
#define B_BUILD_RULE_Y  332
#define B_EX_EB_Y       342
#define B_EX_Y          362
#define B_EX_STEP        22
#define B_PRINCIPLE_W    72

#define B_ON_Y          132
#define B_KUN_Y         158
#define B_ROW_H          24
#define B_LABEL_W        40     /* 음독/훈독 at 16 px */
#define B_READ_RULE_Y   192
#define B_PART_EB_Y     202
#define B_PART_Y        222
#define B_PART_STEP      26
#define B_PART_GLYPH_W   44
#define B_PART_RULE_Y   310
#define B_STAT_EB_Y     318
#define B_STAT_Y        338
#define B_STAT_STEP      22
#define B_STAT_H         20
#define B_STAT_LABEL_W   76

#define B_DOCK_Y        412
#define B_DOCK_H         52
#define B_CELL_W        (KANJI_CONTENT_W / KANJI_GRADE_COUNT)           /* 150 */
#define B_CELL_X(i)     (KANJI_CONTENT_X + (i) * B_CELL_W)

#define B_R_VALUE_X     (KANJI_COL_R_X + B_LABEL_W + 4)                 /* 492 */
#define B_R_VALUE_W     (KANJI_CONTENT_R - B_R_VALUE_X)                 /* 132 */
#define B_PART_GLOSS_X  (KANJI_COL_R_X + B_PART_GLYPH_W + 6)            /* 490 */
#define B_PART_GLOSS_W  (KANJI_CONTENT_R - B_PART_GLOSS_X)              /* 134 */
#define B_STAT_VALUE_X  (KANJI_COL_R_X + B_STAT_LABEL_W + 4)            /* 520 */
#define B_STAT_VALUE_W  (KANJI_CONTENT_R - B_STAT_VALUE_X)              /* 104 */

#define B_EXAMPLE(i)     { KANJI_COL_L_X, B_EX_Y + (i) * B_EX_STEP, \
                           KANJI_COL_L_W, B_EX_STEP }
#define B_PART_GLYPH(i)  { KANJI_COL_R_X, B_PART_Y + (i) * B_PART_STEP, \
                           B_PART_GLYPH_W, B_ROW_H }
#define B_PART_GLOSS(i)  { B_PART_GLOSS_X, B_PART_Y + (i) * B_PART_STEP, \
                           B_PART_GLOSS_W, B_ROW_H }
#define B_STAT_LABEL(i)  { KANJI_COL_R_X, B_STAT_Y + (i) * B_STAT_STEP, \
                           B_STAT_LABEL_W, B_STAT_H }
#define B_STAT_VALUE(i)  { B_STAT_VALUE_X, B_STAT_Y + (i) * B_STAT_STEP, \
                           B_STAT_VALUE_W, B_STAT_H }
#define B_CELL(i)        { B_CELL_X(i), B_DOCK_Y, B_CELL_W, B_DOCK_H }
#define B_CELL_KEY(i)    { B_CELL_X(i) + 12, B_DOCK_Y + 8, 24, 24 }
#define B_CELL_NAME(i)   { B_CELL_X(i) + 42, B_DOCK_Y + 8, B_CELL_W - 54, 24 }
#define B_CELL_SPAN(i)   { B_CELL_X(i) + 12, B_DOCK_Y + 32, B_CELL_W - 24, B_STAT_H }

static const kanji_back_layout_t BACK = {
    .hero      = { KANJI_CONTENT_X, B_HERO_Y, B_HERO_W, B_HERO_H },
    .status    = { B_SIDE_X, B_META_Y, 100, B_META_H },
    .due       = { 480, B_META_Y, KANJI_CONTENT_R - 480, B_META_H },
    .reading   = { 340, B_READ_Y, KANJI_CONTENT_R - B_BADGE_W - 8 - 340, B_READ_H },
    .badge     = { KANJI_CONTENT_R - B_BADGE_W, B_READ_Y, B_BADGE_W, B_READ_H },
    .band_rule = { KANJI_CONTENT_X, B_BAND_RULE_Y, KANJI_CONTENT_W, KANJI_RULE_BAND },

    .col_rule  = { KANJI_COL_RULE_X, B_TOP, KANJI_RULE_HAIR, B_BOT - B_TOP },

    .sense_eyebrow   = { KANJI_COL_L_X, B_TOP, KANJI_COL_L_W, B_EYEBROW_H },
    .senses          = { KANJI_COL_L_X, B_SENSE_Y, KANJI_COL_L_W, B_SENSE_H },
    .sense_rule      = { KANJI_COL_L_X, B_SENSE_RULE_Y, KANJI_COL_L_W, KANJI_RULE_HAIR },
    .build_eyebrow   = { KANJI_COL_L_X, B_BUILD_EB_Y,
                         KANJI_COL_L_W - B_PRINCIPLE_W - 8, B_EYEBROW_H },
    .principle       = { KANJI_COL_L_X + KANJI_COL_L_W - B_PRINCIPLE_W, B_BUILD_EB_Y,
                         B_PRINCIPLE_W, B_EYEBROW_H },
    .build           = { KANJI_COL_L_X, B_BUILD_Y, KANJI_COL_L_W, B_BUILD_H },
    .build_rule      = { KANJI_COL_L_X, B_BUILD_RULE_Y, KANJI_COL_L_W, KANJI_RULE_HAIR },
    .example_eyebrow = { KANJI_COL_L_X, B_EX_EB_Y, KANJI_COL_L_W, B_EYEBROW_H },
    .example         = { B_EXAMPLE(0), B_EXAMPLE(1) },

    .read_eyebrow = { KANJI_COL_R_X, B_TOP, KANJI_COL_R_W, B_EYEBROW_H },
    .on_label     = { KANJI_COL_R_X, B_ON_Y,  B_LABEL_W, B_ROW_H },
    .on_value     = { B_R_VALUE_X,   B_ON_Y,  B_R_VALUE_W, B_ROW_H },
    .kun_label    = { KANJI_COL_R_X, B_KUN_Y, B_LABEL_W, B_ROW_H },
    .kun_value    = { B_R_VALUE_X,   B_KUN_Y, B_R_VALUE_W, B_ROW_H },
    .read_rule    = { KANJI_COL_R_X, B_READ_RULE_Y, KANJI_COL_R_W, KANJI_RULE_HAIR },
    .part_eyebrow = { KANJI_COL_R_X, B_PART_EB_Y, KANJI_COL_R_W, B_EYEBROW_H },
    .part_glyph   = { B_PART_GLYPH(0), B_PART_GLYPH(1), B_PART_GLYPH(2) },
    .part_gloss   = { B_PART_GLOSS(0), B_PART_GLOSS(1), B_PART_GLOSS(2) },
    .part_rule    = { KANJI_COL_R_X, B_PART_RULE_Y, KANJI_COL_R_W, KANJI_RULE_HAIR },
    .stat_eyebrow = { KANJI_COL_R_X, B_STAT_EB_Y, KANJI_COL_R_W, B_EYEBROW_H },
    .stat_label   = { B_STAT_LABEL(0), B_STAT_LABEL(1), B_STAT_LABEL(2) },
    .stat_value   = { B_STAT_VALUE(0), B_STAT_VALUE(1), B_STAT_VALUE(2) },

    .dock      = { KANJI_CONTENT_X, B_DOCK_Y, KANJI_CONTENT_W, B_DOCK_H },
    .cell      = { B_CELL(0), B_CELL(1), B_CELL(2), B_CELL(3) },
    .cell_key  = { B_CELL_KEY(0), B_CELL_KEY(1), B_CELL_KEY(2), B_CELL_KEY(3) },
    .cell_name = { B_CELL_NAME(0), B_CELL_NAME(1), B_CELL_NAME(2), B_CELL_NAME(3) },
    .cell_span = { B_CELL_SPAN(0), B_CELL_SPAN(1), B_CELL_SPAN(2), B_CELL_SPAN(3) },
};

/* --- the vertical invariants ---------------------------------------------------------------
 * The horizontal sums are asserted in the header. These are the ones that go wrong when a block
 * grows by one line: a column that overruns the dock, or two blocks that overlap. Both are
 * invisible in a screenshot — the second block simply draws over the first — which is exactly
 * why they are checked here instead of by eye. */

_Static_assert(F_HEAD_Y + F_HEAD_H < F_HEAD_RULE_Y
               && F_HEAD_RULE_Y < F_HERO_Y
               && F_HERO_Y + F_HERO_H < F_ORN_Y - F_ORN_MARK / 2
               && F_ORN_Y + F_ORN_MARK / 2 < F_QUOTE_Y
               && F_QUOTE_Y + F_QUOTE_H < F_PLATE_Y
               && F_PLATE_Y + F_PLATE_H < F_FOOT_RULE_Y
               && F_FOOT_RULE_Y < F_FOOT_Y
               && F_FOOT_Y + F_FOOT_H < KANJI_SCREEN_H,
               "the front's bands must stack down the page without overlapping");

_Static_assert(F_PLATE_L_X + F_PLATE_COL_W + F_PLATE_GAP == F_CENTER
               && F_PLATE_V_X - F_CENTER == F_PLATE_GAP
               && F_PLATE_V_X + F_PLATE_VAL_W <= KANJI_CONTENT_R,
               "the plate hangs off its hairline with equal paper either side. It is NOT "
               "mirror-symmetric: the value column is wider than the label column, because the "
               "axis is the rule and not the midpoint of the pair — which is how a dictionary "
               "sets a label|value block, and how the print this borrows from sets ORIGIN|text");

_Static_assert(B_HERO_Y + B_HERO_H < B_BAND_RULE_Y
               && B_BAND_RULE_Y + KANJI_RULE_BAND < B_TOP
               && KANJI_CONTENT_X + B_HERO_W < B_SIDE_X,
               "the back's masthead must clear the band rule, and the hero must clear the "
               "reading beside it");

_Static_assert(B_TOP + B_EYEBROW_H < B_SENSE_Y
               && B_SENSE_Y + B_SENSE_H < B_SENSE_RULE_Y
               && B_SENSE_RULE_Y < B_BUILD_EB_Y
               && B_BUILD_EB_Y + B_EYEBROW_H < B_BUILD_Y
               && B_BUILD_Y + B_BUILD_H < B_BUILD_RULE_Y
               && B_BUILD_RULE_Y < B_EX_EB_Y
               && B_EX_EB_Y + B_EYEBROW_H < B_EX_Y
               && B_EX_Y + KANJI_EXAMPLES_SHOWN * B_EX_STEP <= B_BOT,
               "the left column's blocks must stack and the last example must not reach the dock");

_Static_assert(B_TOP + B_EYEBROW_H < B_ON_Y
               && B_ON_Y + B_ROW_H <= B_KUN_Y
               && B_KUN_Y + B_ROW_H < B_READ_RULE_Y
               && B_READ_RULE_Y < B_PART_EB_Y
               && B_PART_EB_Y + B_EYEBROW_H < B_PART_Y
               && B_PART_Y + (KANJI_PARTS_SHOWN - 1) * B_PART_STEP + B_ROW_H < B_PART_RULE_Y
               && B_PART_RULE_Y < B_STAT_EB_Y
               && B_STAT_EB_Y + B_EYEBROW_H < B_STAT_Y
               && B_STAT_Y + (KANJI_STATS_SHOWN - 1) * B_STAT_STEP + B_STAT_H <= B_BOT,
               "the right rail's blocks must stack and the last figure must not reach the dock");

_Static_assert(B_EX_STEP >= 20 && B_PART_STEP >= B_ROW_H && B_STAT_STEP >= B_STAT_H,
               "a repeating row's pitch must be at least its own height or it clips the row "
               "below — LVGL will not complain, it will simply overlap");

_Static_assert(B_BOT == B_DOCK_Y && B_DOCK_Y + B_DOCK_H < KANJI_SCREEN_H,
               "the dock must begin exactly where the columns end and leave paper beneath it");

_Static_assert(B_CELL_X(KANJI_GRADE_COUNT - 1) + B_CELL_W == KANJI_CONTENT_R,
               "the four grade cells must tile the dock exactly — a rounding crack between two "
               "cells is a white stripe down a ruled row");

_Static_assert(KANJI_CONTENT_X % KANJI_BYTE_ALIGN == 0
               && (KANJI_CONTENT_X + KANJI_CONTENT_W) % KANJI_BYTE_ALIGN == 0,
               "the dock rectangle goes verbatim to epd_refresh_partial_area(), which refreshes "
               "whole framebuffer bytes; a dock that is not byte-aligned refreshes a strip that "
               "does not contain the thing that changed, and nothing logs it");

const kanji_front_layout_t *kanji_front_layout(void) { return &FRONT; }
const kanji_back_layout_t  *kanji_back_layout(void)  { return &BACK; }

void kanji_rect_to_half_open(const kanji_rect_t *r,
                             int *x1, int *y1, int *x2, int *y2)
{
    if (!r) return;
    if (x1) *x1 = r->x;
    if (y1) *y1 = r->y;
    if (x2) *x2 = r->x + r->w;
    if (y2) *y2 = r->y + r->h;
}

/* The hero face is Japanese-only, and it is chosen by LENGTH: five characters at 56 px is
 * 280 px, which fits the 336 px masthead slot and the 600 px centred slot on the front with
 * room either side. A longer headword falls back to the 28 px face, where ten characters —
 * the catalog's longest — is also 280 px. Both faces therefore always set the headword on one
 * line, which is why neither slot needs to be tall enough for two. */
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

size_t kanji_headword_display_text(char dst[KANJI_FRONT_MAX],
                                   const char *front)
{
    return kanji_text_collapse_whitespace(dst, KANJI_FRONT_MAX, front);
}
