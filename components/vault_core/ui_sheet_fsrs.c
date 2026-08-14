/*
 * ui_sheet_fsrs.c — what the scheduler is, and what it currently thinks of this
 * card.
 *
 * This screen exists because the web app shows the numbers and never says what
 * they mean. A learner grading four times a day deserves to know why 쉬움 costs
 * three weeks and 다시 costs ten minutes, and the board is a better place to
 * put that than a settings page nobody opens: it is the only screen here you
 * reach by pressing a button while looking at a card you just got wrong.
 *
 * Three pages of copy (ui_strings.h), with the card's own numbers along the
 * bottom of every one of them — so the explanation and the thing it explains
 * are never on different pages.
 */
#include <stdio.h>

#include "ui_internal.h"

typedef struct {
    lv_obj_t *root;
    ui_sheet_band_t band;
    lv_obj_t *title;
    lv_obj_t *body;
    lv_obj_t *stat_val[KANJI_STAT_CELLS];
    lv_obj_t *pager;
} fsrs_ui_t;

static fsrs_ui_t f;

static const struct {
    const char *title;
    const char *body;
} PAGES[] = {
    { S_FSRS_P1_TITLE, S_FSRS_P1_BODY },
    { S_FSRS_P2_TITLE, S_FSRS_P2_BODY },
    { S_FSRS_P3_TITLE, S_FSRS_P3_BODY },
};
/* KEY0 walks kanji_sheet_pages(), which answers KANJI_FSRS_PAGES. If this array
 * ever holds a different number of pages, the extra copy is unreachable and the
 * pager lies about how much there is to read — so it is a compile error, not a
 * screen someone has to notice is missing. */
_Static_assert(sizeof PAGES / sizeof PAGES[0] == KANJI_FSRS_PAGES,
               "add or remove a page in kanji_nav.h's KANJI_FSRS_PAGES too");
#define PAGE_COUNT KANJI_FSRS_PAGES

static const char *STAT_CAPS[KANJI_STAT_CELLS] = {
    S_FSRS_STATE, S_FSRS_STABILITY, S_FSRS_DIFFICULTY, S_FSRS_REPS, S_FSRS_DUE,
};

lv_obj_t *ui_sheet_fsrs_create(lv_obj_t *par)
{
    const kanji_sheet_layout_t *l = kanji_sheet_layout(true);
    const kanji_chrome_t *ch = kanji_chrome_layout();

    f.root = ui_pane(par, 0, 0, ch->content.w, ch->content.h);
    lv_obj_set_style_bg_color(f.root, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(f.root, LV_OPA_COVER, 0);
    ui_sheet_band_create(f.root, &f.band, S_SHEET_FSRS);

    f.title = ui_lab_w(f.root, l->body.x, LOCAL_Y(l->body.y), l->body.w,
                       UI_F_TITLE, LV_TEXT_ALIGN_LEFT, "");
    f.body = ui_lab_w(f.root, l->body.x, LOCAL_Y(l->body.y) + 42, l->body.w,
                      UI_F_BODY, LV_TEXT_ALIGN_LEFT, "");
    ui_lab_wrap(f.body, l->body.h - 42);

    /* A rule over the card's own numbers: they are a different kind of thing
     * from the copy above them, and on a page with no colour a rule is the
     * only way to say so. */
    ui_fill(f.root, l->stats.x, LOCAL_Y(l->stats.y) - 6, l->stats.w, 2);
    for (int i = 0; i < KANJI_STAT_CELLS; i++) {
        ui_lab_w(f.root, l->stat[i].x, LOCAL_Y(l->stat[i].y),
                 l->stat[i].w, UI_F_BODY, LV_TEXT_ALIGN_LEFT, STAT_CAPS[i]);
        f.stat_val[i] = ui_lab_w(f.root, l->stat[i].x,
                                 LOCAL_Y(l->stat[i].y) + 24, l->stat[i].w,
                                 UI_F_HEAD, LV_TEXT_ALIGN_LEFT, "");
    }

    f.pager = ui_lab_w(f.root, l->pager.x, LOCAL_Y(l->pager.y), l->pager.w,
                       UI_F_BODY, LV_TEXT_ALIGN_RIGHT, "");
    return f.root;
}

/* -1 means the scheduler has no value yet, which is not the same as zero: a new
 * card has no stability, a same-day card has one that rounds to zero. Printing
 * "0일" for the first would be the board claiming to know something it does
 * not. */
static void set_measure(lv_obj_t *label, int value, const char *unit)
{
    if (value < 0) {
        ui_set(label, S_VALUE_UNKNOWN);
    } else if (unit && unit[0]) {
        ui_setf(label, "%d%s", value, unit);
    } else {
        ui_setf(label, "%d", value);
    }
}

void ui_sheet_fsrs_update(const kanji_t *k, int page)
{
    if (page < 0 || page >= PAGE_COUNT) page = 0;

    ui_sheet_band_update(&f.band, k);
    ui_set(f.title, PAGES[page].title);
    ui_set(f.body, PAGES[page].body);

    const bool have = k && k->card.valid;
    const kanji_fsrs_t *st = have ? &k->card.fsrs : NULL;

    ui_set(f.stat_val[0], st && st->state_label[0] ? st->state_label
                                                   : S_VALUE_UNKNOWN);
    set_measure(f.stat_val[1], st ? st->stability_days : -1, S_UNIT_DAYS);

    if (st && st->difficulty_pct >= 0) {
        ui_setf(f.stat_val[2], "%d%%", st->difficulty_pct);
    } else {
        ui_set(f.stat_val[2], S_VALUE_UNKNOWN);
    }

    /* Reviews and lapses ride in one cell: "12회 (2)" is the shape a learner
     * reads as "twelve reviews, two of which I lost", and five cells is all the
     * strip has. */
    if (st) {
        ui_setf(f.stat_val[3], "%d%s (%d)", st->reps, S_UNIT_TIMES, st->lapses);
    } else {
        ui_set(f.stat_val[3], S_VALUE_UNKNOWN);
    }

    ui_set(f.stat_val[4], st && st->due[0] ? st->due : S_VALUE_UNKNOWN);

    ui_pager_set(f.pager, page, PAGE_COUNT);
}
