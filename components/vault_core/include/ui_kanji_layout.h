/*
 * ui_kanji_layout.h — every rectangle on the 648x480 glass, as pure integers.
 *
 * No LVGL, no floats, no measurement. The whole page is a compile-time constant, which is what
 * lets test_kanji_layout.c prove the grid holds together before a widget exists and lets the
 * simulator assert on the same numbers the firmware draws with.
 *
 * ## Two faces
 *
 * A card has a front and a back and nothing else. The five-screen routing this replaced put
 * 유래, 구성요소 and the FSRS numbers behind their own button-paged sheets, so seeing what a
 * character is made of cost three presses and three full refreshes — nine seconds of a panel
 * strobing to read two lines that fit beside the senses all along. Every field those sheets
 * paged through is already in kanji_t; spreading them over five screens was never a data
 * problem.
 *
 * ## The grid, and the arithmetic that makes it a rule rather than a taste
 *
 *     24  +  384  +  16 │1│ 15  +  184  +  24   =  648
 *    edge    left      gutter      right    edge
 *
 * A CJK glyph is full-width, so a column's measure in characters is simply w / font_px:
 *
 *              px    KR 16    KR 20    KR 28
 *     left    384     24 자    19 자    13 자
 *     right   184     11 자     9 자     6 자
 *
 * Korean and Japanese prose read comfortably at 25-35 characters. THE LEFT COLUMN AT 16 PX IS
 * THE ONLY MEASURE ON THIS PANEL THAT SETS PROSE, and that constraint is the design rather than
 * a consequence of it: 성립 and 예문 are prose and go left; 읽기, 구성 and 기억 are two-word
 * rows and figures, so the right column is a RAIL and not a second column of text.
 *
 * Both 24 and 384 are multiples of 8, and that is load-bearing rather than tidy — see the dock.
 *
 * ## The scale
 *
 * Four faces, 16 / 20 / 28 / 56, a 1 : 1.25 : 1.75 : 3.5 scale. No fifth face: compiled bitmap
 * data runs about source/5.6, so the four already cost ~4.7 MB of an 8 MB app partition
 * measured at 0x56FA60, and a fifth would spend most of the remainder on a size the scale has
 * no job for. Where this file needs a height it is the face's real line height (16->20, 20->24,
 * 28->35, 56->66) and never a round number, because a slot sized to a round number either clips
 * a descender or leaves a gap that reads as a mistake in a stack of five.
 */
#pragma once

#include <stdbool.h>

#include "kanji_model.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KANJI_SCREEN_W   648
#define KANJI_SCREEN_H   480
#define KANJI_BYTE_ALIGN 8

/* --- the grid ----------------------------------------------------------------------------- */
#define KANJI_MARGIN      24
#define KANJI_CONTENT_X   KANJI_MARGIN                                  /*  24 */
#define KANJI_CONTENT_W   (KANJI_SCREEN_W - 2 * KANJI_MARGIN)           /* 600 */
#define KANJI_CONTENT_R   (KANJI_CONTENT_X + KANJI_CONTENT_W)           /* 624, exclusive */

/* --- rule weights: exactly two ------------------------------------------------------------
 * A hairline separates blocks inside a column, a band rule closes the masthead, and nothing
 * else. A third weight is how a page grows a hierarchy the eye reads as a mistake. */
#define KANJI_RULE_HAIR   1
#define KANJI_RULE_BAND   2

#define KANJI_COL_L_X     KANJI_CONTENT_X                               /*  24 */
#define KANJI_COL_L_W     384
#define KANJI_GUTTER      32    /* the whole gap: 16 of paper, the rule, 15 of paper */
#define KANJI_COL_R_W     184
#define KANJI_COL_R_X     (KANJI_CONTENT_R - KANJI_COL_R_W)             /* 440 */

/* The rule sits in the gutter biased left by the truncation, so the paper either side of it is
 * 16 and 15 rather than a fraction each. */
#define KANJI_COL_RULE_X  (KANJI_COL_L_X + KANJI_COL_L_W \
                           + (KANJI_GUTTER - KANJI_RULE_HAIR) / 2)      /* 424 */

/* How many of a repeating thing the page has room for. These are DROP limits, not squeeze
 * limits: a card carrying six components prints three and drops the rest, because six rows
 * squeezed into a slot that holds three ellipsize into unreadable stubs, and a rail of stubs is
 * worse than a rail of three honest rows. */
#define KANJI_PARTS_SHOWN     3
#define KANJI_EXAMPLES_SHOWN  2
#define KANJI_STATS_SHOWN     3
#define KANJI_PLATE_ROWS      4

typedef struct {
    int x, y, w, h;
} kanji_rect_t;

/* Convert an origin-and-size rectangle to half-open panel bounds [x1, x2) x [y1, y2).
 * NULL output pointers are ignored. */
void kanji_rect_to_half_open(const kanji_rect_t *r,
                             int *x1, int *y1, int *x2, int *y2);

/* --- 문제, the plate -----------------------------------------------------------------------
 * An art print: one centred axis, a 3.5x scale step, a hairline broken by an ornament, and a
 * label|value plate at the foot.
 *
 * THE FRONT IS SPOILER-BOUND. It may print only Japanese and the learner's own history with
 * this card. Never senses, never parts[].meaning, never examples[].gloss — each of those is
 * Korean and each is the answer. examples[].gloss is the trap: it reads as innocuous context
 * right up until it prints 우연히 만나다 under 会う.
 *
 * What is left is richer than it sounds. The plate carries 단계 / 반복 / 안정 / 실패 — the
 * learner's own record with this exact character. It spoils nothing, and it is the reason the
 * frame is worth looking at when nobody is studying. */
typedef struct {
    kanji_rect_t badge;         /* inverted level chip — the one inverted block on this face */
    kanji_rect_t brand;         /* KANJIS · deck                                             */
    kanji_rect_t status;        /* 오프라인 / 오래됨 / DEMO, or empty                         */
    kanji_rect_t counters;      /* 연속 12 · 오늘 34, right-aligned                           */
    kanji_rect_t head_rule;

    kanji_rect_t hero;          /* the headword, centred                                     */

    kanji_rect_t orn_left;      /* hairline, ornament, hairline — one optical unit            */
    kanji_rect_t orn_mark;
    kanji_rect_t orn_right;

    kanji_rect_t quote;         /* a Japanese example set as a pull-quote. No Korean.         */
    kanji_rect_t quote_reading; /* its かな, a step down the scale                            */

    kanji_rect_t plate_label[KANJI_PLATE_ROWS];   /* right-aligned                            */
    kanji_rect_t plate_rule;                      /* the vertical hairline between them       */
    kanji_rect_t plate_value[KANJI_PLATE_ROWS];   /* left-aligned                             */
    kanji_rect_t plate_empty;                     /* a new card has no history: one row here  */

    kanji_rect_t foot_rule;
    kanji_rect_t queue;         /* 새 7 · 복습 18 · 다시 2                                    */
    kanji_rect_t prompt;        /* 뜻 보기, right-aligned                                     */
} kanji_front_layout_t;

/* --- 정답, the spread ----------------------------------------------------------------------
 * A dictionary spread: masthead, then two columns divided by a hairline, then the dock. Dense
 * and ordered, and everything at once — there is nothing behind a button.
 *
 * Vertical budget: the columns run y=112..412, which is 300 px. The left column spends 300 and
 * the right 292. Both are checked by _Static_assert below rather than by eye. */
typedef struct {
    /* masthead */
    kanji_rect_t hero;
    kanji_rect_t status;        /* 오프라인 / 오래됨 / DEMO                                   */
    kanji_rect_t due;           /* 복습 · 9일 뒤, right-aligned                                */
    kanji_rect_t reading;       /* the glance reading; the rail carries 음독/훈독 in full      */
    kanji_rect_t badge;         /* inverted level chip                                        */
    kanji_rect_t band_rule;     /* 2 px — the only band rule on the board                     */

    kanji_rect_t col_rule;      /* the hairline down the gutter                               */

    /* left column — the prose */
    kanji_rect_t sense_eyebrow;
    kanji_rect_t senses;        /* KR 28, two lines: the one thing read from across the room  */
    kanji_rect_t sense_rule;
    kanji_rect_t build_eyebrow;
    kanji_rect_t principle;     /* 상형 / 회의 / 형성 chip, right-aligned on the eyebrow row   */
    /* KR 16, FOUR lines. There is no composition row beside it, and its absence is what pays
     * for the fourth line. `composition` is the bare equation 言 + 口 + 五 = 語, which the
     * 구성 rail two columns to the right already prints WITH each part's Korean meaning — the
     * rail strictly dominates it — and the backend leaves the field null on most cards anyway.
     * Meanwhile the story that does the teaching did not fit: measured over the catalog,
     * hint.reason averages 54-62 characters on kanji cards but runs to 85 on 語, against a
     * three-line slot that held 72. Spending a redundant row on a needed line is arithmetic,
     * not taste. */
    kanji_rect_t build;
    kanji_rect_t build_rule;
    kanji_rect_t example_eyebrow;
    kanji_rect_t example[KANJI_EXAMPLES_SHOWN];

    /* right column — the rail */
    kanji_rect_t read_eyebrow;
    kanji_rect_t on_label,  on_value;
    kanji_rect_t kun_label, kun_value;
    kanji_rect_t read_rule;
    kanji_rect_t part_eyebrow;
    kanji_rect_t part_glyph[KANJI_PARTS_SHOWN];
    kanji_rect_t part_gloss[KANJI_PARTS_SHOWN];
    kanji_rect_t part_rule;
    kanji_rect_t stat_eyebrow;
    kanji_rect_t stat_label[KANJI_STATS_SHOWN];
    kanji_rect_t stat_value[KANJI_STATS_SHOWN];

    /* the dock: four buttons, four grades, no cursor */
    kanji_rect_t dock;
    kanji_rect_t cell[KANJI_GRADE_COUNT];
    kanji_rect_t cell_key[KANJI_GRADE_COUNT];    /* the button glyph — the legend is the dock */
    kanji_rect_t cell_name[KANJI_GRADE_COUNT];
    kanji_rect_t cell_span[KANJI_GRADE_COUNT];
} kanji_back_layout_t;

const kanji_front_layout_t *kanji_front_layout(void);
const kanji_back_layout_t  *kanji_back_layout(void);

bool kanji_hero_is_large(const char *front);
int  kanji_center_x(const kanji_rect_t *outer, int w);

/* Canonical display-only headword text. Raw model/parser/hash data is left untouched; ASCII
 * display whitespace is collapsed and trimmed into the model-sized destination. */
size_t kanji_headword_display_text(char dst[KANJI_FRONT_MAX],
                                   const char *front);

/* --- the invariants ------------------------------------------------------------------------
 * These are here rather than in a test because a layout fault is cheapest at compile time, and
 * because the numbers above are edited by hand: the sum that has to hold should fail the build
 * the moment it stops holding, not the next time somebody runs the simulator. */

_Static_assert(KANJI_COL_L_X + KANJI_COL_L_W + KANJI_GUTTER + KANJI_COL_R_W
               == KANJI_CONTENT_R,
               "the two columns, the gutter and the margins must be the panel width");

_Static_assert(KANJI_COL_L_W / 16 >= 20,
               "the left column must set at least 20 CJK characters or it is not prose");

_Static_assert(KANJI_MARGIN % KANJI_BYTE_ALIGN == 0
               && KANJI_CONTENT_W % KANJI_BYTE_ALIGN == 0,
               "the dock spans the content width and goes verbatim to a partial refresh, so "
               "both its origin and its width must be whole framebuffer bytes");

#ifdef __cplusplus
}
#endif
