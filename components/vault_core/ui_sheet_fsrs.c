/* FSRS explanation pages with a fixed five-stat strip. */
#include "ui_internal.h"

typedef struct {
    lv_obj_t *root;
    ui_sheet_band_t band;
    lv_obj_t *body;
    lv_obj_t *stat_val[KANJI_STAT_CELLS];
    lv_obj_t *pager;
} fsrs_ui_t;

static fsrs_ui_t f;
static const struct { const char *title; const char *body; } PAGES[] = {
    { S_FSRS_P1_TITLE, S_FSRS_P1_BODY },
    { S_FSRS_P2_TITLE, S_FSRS_P2_BODY },
    { S_FSRS_P3_TITLE, S_FSRS_P3_BODY },
};
_Static_assert(sizeof PAGES / sizeof PAGES[0] == KANJI_FSRS_PAGES,
               "FSRS copy and navigation page counts must match");
static const char *STAT_CAPS[KANJI_STAT_CELLS] = {
    S_FSRS_STATE, S_FSRS_STABILITY, S_FSRS_DIFFICULTY, S_FSRS_REPS, S_FSRS_DUE,
};

lv_obj_t *ui_sheet_fsrs_create(lv_obj_t *par)
{
    const kanji_chrome_t *ch = kanji_chrome_layout();
    const kanji_sheet_layout_t *l = kanji_sheet_layout(true);
    f.root = ui_fill_white(par, 0, 0, ch->main.w, ch->main.h);
    ui_sheet_band_create(f.root, &f.band, S_SHEET_FSRS);
    f.body = ui_lab_w(f.root, LOCAL_X(l->body.x), LOCAL_Y(l->body.y),
                      l->body.w, UI_F_BODY, LV_TEXT_ALIGN_LEFT, "");
    ui_lab_wrap(f.body, l->body.h);
    ui_rule(f.root, LOCAL_X(l->stats.x), LOCAL_Y(l->stats.y), l->stats.w, 1);
    for (int i = 0; i < KANJI_STAT_CELLS; i++) {
        ui_lab_w(f.root, LOCAL_X(l->stat[i].x), LOCAL_Y(l->stat[i].y),
                 l->stat[i].w, UI_F_BODY, LV_TEXT_ALIGN_LEFT, STAT_CAPS[i]);
        f.stat_val[i] = ui_lab_w(f.root, LOCAL_X(l->stat[i].x),
                                 LOCAL_Y(l->stat[i].y) + 24, l->stat[i].w,
                                 UI_F_HEAD, LV_TEXT_ALIGN_LEFT, "");
    }
    f.pager = ui_lab_w(f.root, LOCAL_X(l->pager.x), LOCAL_Y(l->pager.y),
                       l->pager.w, UI_F_BODY, LV_TEXT_ALIGN_RIGHT, "");
    return f.root;
}

static void set_measure(lv_obj_t *label, int value, const char *unit)
{
    if (value < 0) ui_set(label, S_VALUE_UNKNOWN);
    else if (unit && unit[0]) ui_setf(label, "%d%s", value, unit);
    else ui_setf(label, "%d", value);
}

void ui_sheet_fsrs_update(const kanji_t *k, int page)
{
    if (page < 0 || page >= KANJI_FSRS_PAGES) page = 0;
    ui_sheet_band_update(&f.band, k);
    ui_set(f.band.title, PAGES[page].title);
    ui_set(f.body, PAGES[page].body);
    const bool have = k && k->card.valid;
    const kanji_fsrs_t *st = have ? &k->card.fsrs : NULL;
    ui_set(f.stat_val[0], st && st->state_label[0] ? st->state_label : S_VALUE_UNKNOWN);
    set_measure(f.stat_val[1], st ? st->stability_days : -1, S_UNIT_DAYS);
    if (st && st->difficulty_pct >= 0) ui_setf(f.stat_val[2], "%d%%", st->difficulty_pct);
    else ui_set(f.stat_val[2], S_VALUE_UNKNOWN);
    if (st) ui_setf(f.stat_val[3], "%d%s (%d)", st->reps, S_UNIT_TIMES, st->lapses);
    else ui_set(f.stat_val[3], S_VALUE_UNKNOWN);
    ui_set(f.stat_val[4], st && st->due[0] ? st->due : S_VALUE_UNKNOWN);
    ui_pager_set(f.pager, page, KANJI_FSRS_PAGES);
}
