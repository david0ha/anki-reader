/* Semantic description pages: shape, memory hook, or components. */
#include "ui_internal.h"

#define PART_STEP 72

typedef struct {
    lv_obj_t *root;
    ui_sheet_band_t band;
    lv_obj_t *prose;
    lv_obj_t *part[KANJI_PARTS_MAX];
    lv_obj_t *empty;
    lv_obj_t *pager;
} desc_ui_t;

static desc_ui_t d;

lv_obj_t *ui_sheet_desc_create(lv_obj_t *par)
{
    const kanji_chrome_t *c = kanji_chrome_layout();
    const kanji_sheet_layout_t *l = kanji_sheet_layout(false);
    d.root = ui_fill_white(par, 0, 0, c->main.w, c->main.h);
    ui_sheet_band_create(d.root, &d.band, S_SHEET_DESC);
    d.prose = ui_lab_w(d.root, LOCAL_X(l->body.x), LOCAL_Y(l->body.y),
                       l->body.w, UI_F_BODY, LV_TEXT_ALIGN_LEFT, "");
    ui_lab_wrap(d.prose, l->body.h);
    for (int i = 0; i < KANJI_PARTS_MAX; i++) {
        d.part[i] = ui_lab_w(d.root, LOCAL_X(l->body.x),
                             LOCAL_Y(l->body.y) + i * PART_STEP,
                             l->body.w, UI_F_HEAD, LV_TEXT_ALIGN_LEFT, "");
    }
    d.empty = ui_lab_w(d.root, LOCAL_X(l->body.x), LOCAL_Y(l->body.y),
                       l->body.w, UI_F_HEAD, LV_TEXT_ALIGN_LEFT, S_NO_DESC);
    d.pager = ui_lab_w(d.root, LOCAL_X(l->pager.x), LOCAL_Y(l->pager.y),
                       l->pager.w, UI_F_BODY, LV_TEXT_ALIGN_RIGHT, "");
    return d.root;
}

void ui_sheet_desc_update(const kanji_t *k, int page)
{
    const int pages = kanji_sheet_pages(k, KANJI_SHEET_DESCRIPTION);
    if (page < 0 || page >= pages) page = 0;
    const kanji_desc_page_t semantic = kanji_desc_page_at(k, page);
    const bool have = k && k->card.valid;
    char normalized[KANJI_BODY_MAX];
    normalized[0] = '\0';

    ui_sheet_band_update(&d.band, k);
    ui_show(d.prose, false);
    ui_show(d.empty, false);
    for (int i = 0; i < KANJI_PARTS_MAX; i++) ui_show(d.part[i], false);

    switch (semantic) {
    case KANJI_DESC_PAGE_SHAPE:
        ui_set(d.band.title, S_SHAPE);
        kanji_text_collapse_whitespace(normalized, sizeof normalized,
                                       have ? k->card.description : "");
        ui_set(d.prose, normalized);
        ui_show(d.prose, true);
        break;
    case KANJI_DESC_PAGE_HOOK:
        ui_set(d.band.title, have && k->card.hook_title[0]
                                   ? k->card.hook_title : S_HOOK_DEFAULT);
        kanji_text_collapse_whitespace(normalized, sizeof normalized,
                                       have ? k->card.hook_body : "");
        ui_set(d.prose, normalized);
        ui_show(d.prose, true);
        break;
    case KANJI_DESC_PAGE_PARTS:
        ui_set(d.band.title, S_PARTS);
        for (int i = 0; i < KANJI_PARTS_MAX; i++) {
            const bool on = have && i < k->card.part_count;
            if (on) {
                const kanji_part_t *part = &k->card.parts[i];
                if (part->reading[0])
                    ui_setf(d.part[i], "%s  %s · %s", part->glyph,
                            part->meaning, part->reading);
                else
                    ui_setf(d.part[i], "%s  %s", part->glyph, part->meaning);
            } else {
                ui_set(d.part[i], "");
            }
            ui_show(d.part[i], on);
        }
        break;
    case KANJI_DESC_PAGE_NONE:
    default:
        ui_set(d.band.title, S_SHEET_DESC);
        ui_show(d.empty, true);
        break;
    }
    ui_pager_set(d.pager, page, pages);
}
