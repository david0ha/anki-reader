/*
 * ui_kanji_layout.c — the geometry declared in ui_kanji_layout.h.
 *
 * Every number here was chosen against a 648x480 panel and is asserted by
 * test_kanji_layout.c. Nothing in this file draws, allocates, or calls libm:
 * the whole point is that the layout can be checked on a laptop in a
 * millisecond, and that the firmware and the simulator lay out from the same
 * bytes rather than from two copies of the same arithmetic.
 */
#include "ui_kanji_layout.h"

/* --- the grid ------------------------------------------------------------- */

#define PAD          14      /* content inset from the panel edge */
#define HEADER_H     44
#define FOOTER_H     34
#define RULE          2      /* the hairline under/over the chrome */

#define CONTENT_Y    (HEADER_H + RULE)                                  /* 46  */
#define CONTENT_H    (KANJI_SCREEN_H - CONTENT_Y - FOOTER_H - RULE)     /* 398 */

/* --- shared chrome -------------------------------------------------------- */

static const kanji_chrome_t CHROME = {
    .header = { 0, 0, KANJI_SCREEN_W, HEADER_H },

    /* The wordmark, then the two stat chips, then the track pill hard right —
     * the order the web app's header has, so a learner who uses both reads
     * them the same way. */
    .brand  = { PAD, 10, 120, 24 },
    .chips  = { 326, 13, 204, 18 },
    .track  = { KANJI_SCREEN_W - PAD - 96, 13, 96, 18 },

    .content = { 0, CONTENT_Y, KANJI_SCREEN_W, CONTENT_H },

    .footer = { 0, KANJI_SCREEN_H - FOOTER_H, KANJI_SCREEN_W, FOOTER_H },
    /* Four equal columns across the padded width: 648 - 2*14 = 620 = 4 * 155.
     * Equal on purpose — the legend's job is to be scanned, and a column that
     * is wider because its label is longer makes the eye stop. */
    .key = {
        { PAD + 0 * 155, KANJI_SCREEN_H - FOOTER_H + 6, 155, 22 },
        { PAD + 1 * 155, KANJI_SCREEN_H - FOOTER_H + 6, 155, 22 },
        { PAD + 2 * 155, KANJI_SCREEN_H - FOOTER_H + 6, 155, 22 },
        { PAD + 3 * 155, KANJI_SCREEN_H - FOOTER_H + 6, 155, 22 },
    },

    .rule_top    = HEADER_H,
    .rule_bottom = CONTENT_Y + CONTENT_H,
};

/* --- the question screen -------------------------------------------------- */

/* The rail lives in the player's right margin rather than floating over the
 * headword. On a phone the rail is a translucent overlay and the card slot
 * keeps symmetric padding so the glyph stays centred; on a 1-bit panel there is
 * no translucency, so an overlay would simply collide. The hero box is narrowed
 * to clear it — 464 px still holds sixteen characters at the small face and
 * eight at the large one, which is more than the catalog's longest headword. */
#define RAIL_X       560
#define HERO_W       464
#define HERO_X       ((KANJI_SCREEN_W - HERO_W) / 2)      /* 92 */

static const kanji_question_layout_t QUESTION = {
    .player   = { 0, CONTENT_Y, KANJI_SCREEN_W, CONTENT_H },

    /* The headword sits between the header and the caption block rather than
     * in the geometric middle of the player: the caption owns the bottom third
     * the way it does on a Short, so centring against the whole player would
     * leave the word floating high with a hole under it. */
    .hero     = { HERO_X, 140, HERO_W, 100 },
    .prompt   = { HERO_X, 252, HERO_W, 24 },

    /* Bottom left, the way a Short captions its channel. */
    .caption  = { 24, 300, 380, 52 },
    .queue    = { 24, 356, 380, 26 },

    .rail       = { RAIL_X, 140, 72, 200 },
    .rail_step  = 64,
    .rail_items = 3,

    /* A line along the player's foot: how far into today's queue this card is.
     * The web app's is a red scrubber; here it is the one place a filled bar
     * carries meaning, so it gets the panel's full width. */
    .scrubber = { 0, CONTENT_Y + CONTENT_H - 4, KANJI_SCREEN_W, 4 },
};

/* --- the answer screen ---------------------------------------------------- */

/* The dock is the only rectangle on this board that is refreshed on its own, so
 * its x bounds are multiples of 8 (81 bytes to a framebuffer row): the window
 * the driver refreshes is then exactly the window that was drawn. 632 = 4 x 158
 * keeps the four cells equal to the pixel, which matters because an unequal
 * dock reads as one rating being recommended — a claim FSRS does not make. */
#define DOCK_X        8
#define DOCK_W      632
#define DOCK_Y      358
#define DOCK_H       80
#define CELL_W      (DOCK_W / KANJI_GRADE_COUNT)          /* 158 */

#define CELL(i)      { DOCK_X + (i) * CELL_W, DOCK_Y, CELL_W, DOCK_H }
#define CELL_LABEL(i){ DOCK_X + (i) * CELL_W + 8, 368, CELL_W - 16, 26 }
#define CELL_SPAN(i) { DOCK_X + (i) * CELL_W + 8, 398, CELL_W - 16, 22 }

static const kanji_answer_layout_t ANSWER = {
    /* The headword stays inverted after the reveal so the eye can find it
     * again without re-reading the page; everything that has to be read as
     * prose moves onto white below it. */
    .band    = { 0, CONTENT_Y, KANJI_SCREEN_W, 124 },
    .hero    = { 24, 56, 440, 72 },
    .reading = { 24, 132, 440, 24 },
    .level   = { RAIL_X, 60, 64, 28 },

    .meaning  = { PAD, 178, KANJI_SCREEN_W - 2 * PAD, 48 },
    /* Three rows, because a card with three examples that silently shows two
     * is a card that lies about the catalog. The vertical budget below the
     * band is 178..438 and every block in it is sized to the pixel; there is
     * no slack, which is why the prompt is a rectangle here rather than a
     * number in the screen file where nothing would check it. */
    .examples = { PAD, 232, KANJI_SCREEN_W - 2 * PAD, 96 },
    .example_step = 32,
    .example_rows = 3,
    .prompt   = { DOCK_X, 332, DOCK_W, 22 },

    .dock = { DOCK_X, DOCK_Y, DOCK_W, DOCK_H },
    .cell       = { CELL(0), CELL(1), CELL(2), CELL(3) },
    .cell_label = { CELL_LABEL(0), CELL_LABEL(1), CELL_LABEL(2), CELL_LABEL(3) },
    .cell_span  = { CELL_SPAN(0), CELL_SPAN(1), CELL_SPAN(2), CELL_SPAN(3) },
};

/* --- the sheets ----------------------------------------------------------- */

/* A sheet is for reading, so its inverted strip is a strip: enough to say which
 * card this is about and which sheet you are on, and not one pixel more. */
#define SHEET_BAND_H   56
#define SHEET_BODY_X   PAD
#define SHEET_BODY_Y   (CONTENT_Y + SHEET_BAND_H + 10)                  /* 112 */
#define SHEET_BODY_W   (KANJI_SCREEN_W - 2 * PAD)                       /* 620 */

#define STATS_Y        364
#define STATS_H        74
#define STAT_W         (SHEET_BODY_W / KANJI_STAT_CELLS)                /* 124 */
#define STAT(i)        { PAD + (i) * STAT_W, 372, STAT_W, 58 }

#define PLAIN_BODY_H   (CONTENT_Y + CONTENT_H - 6 - SHEET_BODY_Y)       /* 326 */
#define STATS_BODY_H   (STATS_Y - 8 - SHEET_BODY_Y)                     /* 244 */

#define SHEET_COMMON \
    .band       = { 0, CONTENT_Y, KANJI_SCREEN_W, SHEET_BAND_H }, \
    .band_word  = { PAD, CONTENT_Y + 10, 300, 36 }, \
    .band_title = { 380, CONTENT_Y + 16, 254, 28 }

static const kanji_sheet_layout_t SHEET_PLAIN = {
    SHEET_COMMON,
    .body  = { SHEET_BODY_X, SHEET_BODY_Y, SHEET_BODY_W, PLAIN_BODY_H },
    /* No strip: zero height, so a caller that draws it unconditionally draws
     * nothing rather than into a stale rectangle. */
    .stats = { 0, 0, 0, 0 },
    .stat  = { {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0} },
    .pager = { SHEET_BODY_X + SHEET_BODY_W - 70,
               SHEET_BODY_Y + PLAIN_BODY_H - 26, 60, 22 },
};

static const kanji_sheet_layout_t SHEET_WITH_STATS = {
    SHEET_COMMON,
    .body  = { SHEET_BODY_X, SHEET_BODY_Y, SHEET_BODY_W, STATS_BODY_H },
    .stats = { PAD, STATS_Y, SHEET_BODY_W, STATS_H },
    .stat  = { STAT(0), STAT(1), STAT(2), STAT(3), STAT(4) },
    .pager = { SHEET_BODY_X + SHEET_BODY_W - 70,
               SHEET_BODY_Y + STATS_BODY_H - 26, 60, 22 },
};

/* --- accessors ------------------------------------------------------------ */

const kanji_chrome_t *kanji_chrome_layout(void)
{
    return &CHROME;
}

const kanji_question_layout_t *kanji_question_layout(void)
{
    return &QUESTION;
}

const kanji_answer_layout_t *kanji_answer_layout(void)
{
    return &ANSWER;
}

const kanji_sheet_layout_t *kanji_sheet_layout(bool with_stats)
{
    return with_stats ? &SHEET_WITH_STATS : &SHEET_PLAIN;
}

/* --- content-dependent choices -------------------------------------------- */

/* Five characters at 56 px is about 300 px, which clears the rail with room to
 * spare. Six would not, and a headword that has to be ellipsized has stopped
 * being a headword — so the face shrinks instead. */
#define HERO_LARGE_MAX_CHARS 5

bool kanji_hero_is_large(const char *front)
{
    const int n = kanji_utf8_len(front);
    /* Nothing to draw still picks the large face: the empty-card screen keeps
     * the hero's height rather than collapsing the layout around a gap. */
    return n <= HERO_LARGE_MAX_CHARS;
}

int kanji_center_x(const kanji_rect_t *outer, int w)
{
    if (!outer) return 0;
    if (w >= outer->w) return outer->x;
    return outer->x + (outer->w - w) / 2;
}
