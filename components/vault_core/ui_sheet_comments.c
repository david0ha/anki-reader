/* Paper comments sheet, two fixed rows per page. */
#include "ui_internal.h"

#define PER_PAGE KANJI_COMMENTS_PER_PAGE
#define BLOCK_H  136
#define BODY_H    92
_Static_assert(PER_PAGE == 2, "comment geometry is two rows");

typedef struct {
    lv_obj_t *author;
    lv_obj_t *likes;
    lv_obj_t *body;
} comment_row_t;

typedef struct {
    lv_obj_t *root;
    ui_sheet_band_t band;
    lv_obj_t *count;
    comment_row_t row[PER_PAGE];
    lv_obj_t *empty;
    lv_obj_t *pager;
} comments_ui_t;

static comments_ui_t c;

lv_obj_t *ui_sheet_comments_create(lv_obj_t *par)
{
    const kanji_chrome_t *ch = kanji_chrome_layout();
    const kanji_sheet_layout_t *l = kanji_sheet_layout(false);
    c.root = ui_fill_white(par, 0, 0, ch->main.w, ch->main.h);
    ui_sheet_band_create(c.root, &c.band, S_SHEET_COMMENTS);
    const int x = LOCAL_X(l->body.x);
    const int y0 = LOCAL_Y(l->body.y);
    c.count = ui_lab_w(c.root, x, y0, l->body.w, UI_F_HEAD,
                       LV_TEXT_ALIGN_LEFT, "");
    for (int i = 0; i < PER_PAGE; i++) {
        const int y = y0 + 32 + i * BLOCK_H;
        c.row[i].author = ui_lab_w(c.root, x, y, l->body.w - 96, UI_F_HEAD,
                                   LV_TEXT_ALIGN_LEFT, "");
        c.row[i].likes = ui_lab_w(c.root, x + l->body.w - 96, y, 96, UI_F_BODY,
                                  LV_TEXT_ALIGN_RIGHT, "");
        c.row[i].body = ui_lab_w(c.root, x, y + 28, l->body.w, UI_F_BODY,
                                 LV_TEXT_ALIGN_LEFT, "");
        ui_lab_wrap(c.row[i].body, BODY_H);
        ui_rule(c.root, x, y + BLOCK_H - 8, l->body.w, 1);
    }
    c.empty = ui_lab_w(c.root, x, y0 + 48, l->body.w, UI_F_HEAD,
                       LV_TEXT_ALIGN_LEFT, S_NO_COMMENTS);
    c.pager = ui_lab_w(c.root, LOCAL_X(l->pager.x), LOCAL_Y(l->pager.y),
                       l->pager.w, UI_F_BODY, LV_TEXT_ALIGN_RIGHT, "");
    return c.root;
}

void ui_sheet_comments_update(const kanji_t *k, int page)
{
    const bool have = k && k->card.valid;
    const int count = have ? k->card.comment_count : 0;
    const int pages = kanji_sheet_pages(k, KANJI_SHEET_COMMENTS);
    if (page < 0 || page >= pages) page = 0;
    ui_sheet_band_update(&c.band, k);
    ui_set(c.band.title, S_SHEET_COMMENTS);
    if (count > 0) ui_setf(c.count, "%s %d", S_SHEET_COMMENTS, k->card.comment_total);
    else ui_set(c.count, "");

    const int first = page * PER_PAGE;
    for (int i = 0; i < PER_PAGE; i++) {
        const int idx = first + i;
        const bool on = idx < count;
        if (on) {
            const kanji_comment_t *comment = &k->card.comments[idx];
            ui_set(c.row[i].author, comment->author);
            ui_setf(c.row[i].likes, "%s %d", S_LIKES, comment->likes);
            ui_set(c.row[i].body, comment->body);
        } else {
            ui_set(c.row[i].author, "");
            ui_set(c.row[i].likes, "");
            ui_set(c.row[i].body, "");
        }
        ui_show(c.row[i].author, on);
        ui_show(c.row[i].likes, on);
        ui_show(c.row[i].body, on);
    }
    ui_show(c.empty, count == 0);
    ui_pager_set(c.pager, page, pages);
}
