/*
 * ui_sheet_comments.c — 댓글.
 *
 * Two to a page. Three would fit only if all three were one line, and a comment
 * that silently loses its last line reads as a rendering bug rather than as a
 * long comment — so the page count comes from kanji_sheet_pages() and KEY0
 * walks it.
 *
 * Replies are already flattened away by the proxy: a thread indent inside two
 * rows reads as a rendering artefact, not as a conversation.
 */
#include "ui_internal.h"

/* How many rows this file draws. It is kanji_nav.h's number, not a second copy:
 * kanji_sheet_pages() decides how far KEY0 walks, and if the two disagree the
 * learner either cannot reach the last comment or pages onto a blank. The
 * assertion below is what stops that from ever being a runtime discovery. */
#define PER_PAGE     KANJI_COMMENTS_PER_PAGE
_Static_assert(PER_PAGE == 2, "the block geometry below is laid out for two rows");

#define BLOCK_H    130
#define BODY_H      (BLOCK_H - 34)

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
    const kanji_sheet_layout_t *l = kanji_sheet_layout(false);
    const kanji_chrome_t *ch = kanji_chrome_layout();

    c.root = ui_pane(par, 0, 0, ch->content.w, ch->content.h);
    lv_obj_set_style_bg_color(c.root, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(c.root, LV_OPA_COVER, 0);
    ui_sheet_band_create(c.root, &c.band, S_SHEET_COMMENTS);

    const int x = l->body.x;
    const int w = l->body.w;
    const int y0 = LOCAL_Y(l->body.y);

    c.count = ui_lab_w(c.root, x, y0, w, UI_F_HEAD, LV_TEXT_ALIGN_LEFT, "");

    for (int i = 0; i < PER_PAGE; i++) {
        const int y = y0 + 32 + i * BLOCK_H;
        c.row[i].author = ui_lab_w(c.root, x, y, w - 90, UI_F_HEAD,
                                   LV_TEXT_ALIGN_LEFT, "");
        c.row[i].likes = ui_lab_w(c.root, x + w - 88, y + 2, 88, UI_F_BODY,
                                  LV_TEXT_ALIGN_RIGHT, "");
        c.row[i].body = ui_lab_w(c.root, x, y + 30, w, UI_F_BODY,
                                 LV_TEXT_ALIGN_LEFT, "");
        ui_lab_wrap(c.row[i].body, BODY_H);
        /* A rule under each comment, the way a comment list separates its
         * rows. Drawn once at build time — it is furniture, not content. */
        ui_fill(c.root, x, y + BLOCK_H - 12, w, 1);
    }

    c.empty = ui_lab_w(c.root, x, y0 + 60, w, UI_F_HEAD,
                       LV_TEXT_ALIGN_CENTER, S_NO_COMMENTS);
    c.pager = ui_lab_w(c.root, l->pager.x, LOCAL_Y(l->pager.y), l->pager.w,
                       UI_F_BODY, LV_TEXT_ALIGN_RIGHT, "");
    return c.root;
}

void ui_sheet_comments_update(const kanji_t *k, int page)
{
    const bool have = k && k->card.valid;
    const int n = have ? k->card.comment_count : 0;

    ui_sheet_band_update(&c.band, k);

    if (n > 0) {
        ui_setf(c.count, "%s %d", S_SHEET_COMMENTS,
                have ? k->card.comment_total : 0);
    } else {
        ui_set(c.count, "");
    }

    if (page < 0) page = 0;
    const int first = page * PER_PAGE;

    for (int i = 0; i < PER_PAGE; i++) {
        const int idx = first + i;
        const bool on = idx < n;
        if (on) {
            const kanji_comment_t *m = &k->card.comments[idx];
            ui_set(c.row[i].author, m->author[0] ? m->author : "");
            ui_setf(c.row[i].likes, "%s %d", S_LIKES, m->likes);
            ui_set(c.row[i].body, m->body);
        } else {
            ui_set(c.row[i].author, "");
            ui_set(c.row[i].likes, "");
            ui_set(c.row[i].body, "");
        }
        ui_show(c.row[i].author, on);
        ui_show(c.row[i].likes, on);
        ui_show(c.row[i].body, on);
    }

    ui_show(c.empty, n == 0);
    ui_pager_set(c.pager, page,
                 kanji_sheet_pages(k, KANJI_SHEET_COMMENTS));
}
