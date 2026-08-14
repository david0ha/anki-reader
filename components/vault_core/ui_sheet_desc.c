/*
 * ui_sheet_desc.c — 설명: why the headword is shaped the way it is.
 *
 * The web app opens this by tapping the caption, the way YouTube opens a
 * video's description. Here it is KEY1, and it is deliberately reachable from
 * the QUESTION side as well as the answer: the shape story is a hint, and a
 * learner who reads it and still fails has given the scheduler a more honest
 * 다시 than one who did not.
 *
 * One page. The three blocks are each bounded by the model's own byte limits
 * and were sized together against the content area, so nothing here pages —
 * see kanji_sheet_pages().
 */
#include "ui_internal.h"

/* The body is three stacked blocks. Their heights are fixed rather than
 * flowed: LVGL has no layout pass here, and a block that grew into the one
 * below it would be a collision nobody sees until the glass. */
#define SHAPE_H     120
#define HOOK_H      100
#define PART_STEP    26

typedef struct {
    lv_obj_t *root;
    ui_sheet_band_t band;
    lv_obj_t *shape_cap;
    lv_obj_t *shape;
    lv_obj_t *hook_cap;
    lv_obj_t *hook;
    lv_obj_t *parts_cap;
    lv_obj_t *part[KANJI_PARTS_MAX];
    lv_obj_t *empty;
} desc_ui_t;

static desc_ui_t d;

lv_obj_t *ui_sheet_desc_create(lv_obj_t *par)
{
    const kanji_sheet_layout_t *l = kanji_sheet_layout(false);
    const kanji_chrome_t *c = kanji_chrome_layout();

    d.root = ui_fill_white(par, 0, 0, c->content.w, c->content.h);
    ui_sheet_band_create(d.root, &d.band, S_SHEET_DESC);

    const int x = l->body.x;
    const int w = l->body.w;
    int y = LOCAL_Y(l->body.y);

    d.shape_cap = ui_lab(d.root, x, y, UI_F_HEAD, S_SHAPE);
    d.shape = ui_lab_w(d.root, x, y + 28, w, UI_F_BODY, LV_TEXT_ALIGN_LEFT, "");
    ui_lab_wrap(d.shape, SHAPE_H - 28);
    y += SHAPE_H;

    d.hook_cap = ui_lab(d.root, x, y, UI_F_HEAD, S_HOOK_DEFAULT);
    d.hook = ui_lab_w(d.root, x, y + 28, w, UI_F_BODY, LV_TEXT_ALIGN_LEFT, "");
    ui_lab_wrap(d.hook, HOOK_H - 28);
    y += HOOK_H;

    d.parts_cap = ui_lab(d.root, x, y, UI_F_HEAD, S_PARTS);
    for (int i = 0; i < KANJI_PARTS_MAX; i++) {
        d.part[i] = ui_lab_w(d.root, x, y + 28 + i * PART_STEP, w, UI_F_BODY,
                             LV_TEXT_ALIGN_LEFT, "");
    }

    d.empty = ui_lab_w(d.root, x, LOCAL_Y(l->body.y) + 40, w, UI_F_HEAD,
                       LV_TEXT_ALIGN_CENTER, S_NO_DESC);
    return d.root;
}

void ui_sheet_desc_update(const kanji_t *k)
{
    const bool have = k && k->card.valid;
    ui_sheet_band_update(&d.band, k);

    const bool has_shape = have && k->card.description[0];
    const bool has_hook  = have && k->card.hook_body[0];
    const bool has_parts = have && k->card.part_count > 0;

    ui_set(d.shape, has_shape ? k->card.description : "");
    ui_show(d.shape_cap, has_shape);
    ui_show(d.shape, has_shape);

    /* The hook's own title comes from the card (hint.principle) when it has
     * one, because "기억 힌트" over a hook the author already named is a label
     * telling you less than the thing it labels. */
    ui_set(d.hook_cap, has_hook && k->card.hook_title[0] ? k->card.hook_title
                                                         : S_HOOK_DEFAULT);
    ui_set(d.hook, has_hook ? k->card.hook_body : "");
    ui_show(d.hook_cap, has_hook);
    ui_show(d.hook, has_hook);

    ui_show(d.parts_cap, has_parts);
    for (int i = 0; i < KANJI_PARTS_MAX; i++) {
        const bool on = has_parts && i < k->card.part_count;
        if (on) {
            const kanji_part_t *p = &k->card.parts[i];
            if (p->reading[0]) {
                ui_setf(d.part[i], "%s  %s · %s", p->glyph, p->meaning, p->reading);
            } else {
                ui_setf(d.part[i], "%s  %s", p->glyph, p->meaning);
            }
        } else {
            ui_set(d.part[i], "");
        }
        ui_show(d.part[i], on);
    }

    ui_show(d.empty, !has_shape && !has_hook && !has_parts);
}
