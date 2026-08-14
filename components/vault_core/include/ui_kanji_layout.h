/*
 * ui_kanji_layout.h — where everything goes, as pure integers. No LVGL, no
 * ESP-IDF, no libm.
 *
 * The geometry is separated from the drawing for two reasons, and both of them
 * have bitten this project before:
 *
 *   1. A rectangle is testable on a laptop in a millisecond; a rendered panel
 *      is not. Every "does this overlap", "is this on screen", "is the dock
 *      byte-aligned" question is answered here, by test_kanji_layout.c, before
 *      a single widget exists.
 *   2. The grade dock's rectangle is passed verbatim to
 *      epd_refresh_partial_area(). If it drifts by a pixel from what was drawn,
 *      the panel refreshes a strip that does not contain the thing that
 *      changed, and the board silently shows a stale rating.
 *
 * The design is the kanjis.ai Shorts player, redrawn for a 648x480 1-bit panel:
 * an inverted immersive "player" carrying the headword, and white rising sheets
 * carrying the long-form Korean. Black-on-white for anything that has to be
 * read as prose, white-on-black for the two or three characters that have to be
 * seen from across a room. That split is not decoration — at 16 px after
 * binarization, white-on-black Hangul loses its thin strokes.
 */
#pragma once

#include <stdbool.h>

#include "kanji_model.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KANJI_SCREEN_W   648
#define KANJI_SCREEN_H   480

/* The panel is 648 px wide, a multiple of 8, so a framebuffer row is exactly
 * 81 bytes. A partial-refresh window whose x bounds are byte-aligned needs no
 * read-modify-write on the edge columns; the driver snaps outward anyway, but
 * a dock that is already aligned refreshes exactly the strip that was drawn. */
#define KANJI_BYTE_ALIGN 8

/* How many of a card's own FSRS numbers the sheet's bottom strip prints. */
#define KANJI_STAT_CELLS 5

typedef struct {
    int x, y, w, h;
} kanji_rect_t;

/* Convert an origin-and-size rectangle to panel bounds [x1, x2) × [y1, y2).
 * The upper bounds are exclusive, matching epd_refresh_partial_area(); NULL
 * output pointers are ignored, and a NULL rectangle leaves every output
 * untouched. */
void kanji_rect_to_half_open(const kanji_rect_t *r,
                             int *x1, int *y1, int *x2, int *y2);

/* --- shared chrome -------------------------------------------------------- */

typedef struct {
    kanji_rect_t header;      /* the inverted band: brand, streak, track */
    kanji_rect_t brand;
    kanji_rect_t chips;       /* 연속 / 오늘, right-aligned inside the header */
    kanji_rect_t track;
    kanji_rect_t content;     /* everything between the two rules */
    kanji_rect_t footer;      /* the per-screen key legend */
    kanji_rect_t key[4];      /* KEY0 / KEY1 / KEY2 / BOOT, left to right */
    int rule_top;             /* y of the hairline under the header */
    int rule_bottom;          /* y of the hairline over the footer */
} kanji_chrome_t;

/* --- the question screen -------------------------------------------------- */

typedef struct {
    kanji_rect_t player;      /* the filled immersive area */
    kanji_rect_t hero;        /* the headword */
    kanji_rect_t prompt;      /* "KEY0 을 눌러 정답 보기" */
    kanji_rect_t caption;     /* deck + level, bottom left, the "channel" */
    kanji_rect_t queue;       /* the three queue counters under the caption */
    kanji_rect_t rail;        /* the right action column */
    int rail_step;            /* baseline-to-baseline inside the rail */
    int rail_items;
    kanji_rect_t scrubber;    /* the progress line along the player's foot */
} kanji_question_layout_t;

/* --- the answer screen ---------------------------------------------------- */

typedef struct {
    kanji_rect_t band;        /* the inverted headword strip */
    kanji_rect_t hero;
    kanji_rect_t reading;
    kanji_rect_t level;       /* the JLPT chip, right-aligned in the band */
    kanji_rect_t meaning;     /* 뜻 */
    kanji_rect_t examples;    /* 예문, the block */
    int example_step;         /* row pitch inside `examples` */
    int example_rows;         /* how many rows the block has room for */
    kanji_rect_t prompt;      /* the line above the dock that asks for a rating */
    kanji_rect_t dock;        /* THE partial-refresh rectangle */
    kanji_rect_t cell[KANJI_GRADE_COUNT];
    kanji_rect_t cell_label[KANJI_GRADE_COUNT];
    kanji_rect_t cell_span[KANJI_GRADE_COUNT];
} kanji_answer_layout_t;

/* --- the sheets ----------------------------------------------------------- */

typedef struct {
    kanji_rect_t band;        /* the compact inverted strip: which card */
    kanji_rect_t band_word;
    kanji_rect_t band_title;  /* 설명 / 댓글 / FSRS 복습 일정 */
    kanji_rect_t body;        /* the white page */
    kanji_rect_t stats;       /* FSRS only: this card's own numbers */
    kanji_rect_t stat[KANJI_STAT_CELLS];
    kanji_rect_t pager;       /* "2/3", bottom right of the body */
} kanji_sheet_layout_t;

/* --- accessors -----------------------------------------------------------
 * All four are compile-time constant in practice; they are functions so the
 * host test links against the same bytes the firmware draws from. */

const kanji_chrome_t          *kanji_chrome_layout(void);
const kanji_question_layout_t *kanji_question_layout(void);
const kanji_answer_layout_t   *kanji_answer_layout(void);

/* The sheets share one geometry; `with_stats` shortens the body to make room
 * for the FSRS card-state strip. */
const kanji_sheet_layout_t    *kanji_sheet_layout(bool with_stats);

/* --- content-dependent choices -------------------------------------------
 * Two decisions that look like drawing but are arithmetic, so they live here
 * where a test can drive every input. */

/* Whether the headword gets the 56 px hero face. A word longer than this runs
 * off the panel at 56 px, so it drops to the 28 px face rather than being
 * ellipsized — a truncated headword is not a headword. */
bool kanji_hero_is_large(const char *front);

/* Where a rectangle of `w` px goes to be centred in `outer`. Returns the x. */
int kanji_center_x(const kanji_rect_t *outer, int w);

#ifdef __cplusplus
}
#endif
