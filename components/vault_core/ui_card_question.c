/* Paper-dominant question specimen. */
#include "ui_internal.h"

typedef struct {
    lv_obj_t *root;
    lv_obj_t *hero;
    lv_obj_t *prompt;
    lv_obj_t *secondary;
    lv_obj_t *counts;
} question_ui_t;

static question_ui_t q;

lv_obj_t *ui_card_question_create(lv_obj_t *par)
{
    const kanji_chrome_t *c = kanji_chrome_layout();
    const kanji_question_layout_t *l = kanji_question_layout();
    q.root = ui_fill_white(par, 0, 0, c->main.w, c->main.h);
    q.hero = ui_lab_headword(q.root, LOCAL_X(l->hero.x), LOCAL_Y(l->hero.y),
                             l->hero.w, l->hero.h, UI_F_HERO, "");
    q.prompt = ui_lab_w(q.root, LOCAL_X(l->prompt.x), LOCAL_Y(l->prompt.y),
                        l->prompt.w, UI_F_HEAD, LV_TEXT_ALIGN_LEFT,
                        S_TAP_TO_REVEAL);
    q.secondary = ui_lab_w(q.root, LOCAL_X(l->secondary.x),
                           LOCAL_Y(l->secondary.y), l->secondary.w,
                           UI_F_HEAD, LV_TEXT_ALIGN_LEFT, "");
    ui_lab_wrap(q.secondary, l->secondary.h);
    q.counts = ui_lab_w(q.root, LOCAL_X(l->counts.x), LOCAL_Y(l->counts.y),
                        l->counts.w, UI_F_HEAD, LV_TEXT_ALIGN_LEFT, "");
    return q.root;
}

void ui_card_question_update(const kanji_t *k)
{
    const bool have = k && k->card.valid;
    const lv_font_t *face = ui_hero_face(have ? k->card.front : "");
    lv_obj_set_style_text_font(q.hero, face, 0);
    ui_set(q.hero, have ? k->card.front : "");

    if (have) {
        ui_set(q.prompt, S_TAP_TO_REVEAL);
        ui_set(q.secondary, "");
    } else if (k && k->session.complete) {
        ui_set(q.prompt, S_SESSION_DONE);
        ui_set(q.secondary, S_SESSION_DONE_SUB);
    } else {
        ui_set(q.prompt, S_NO_DATA);
        ui_set(q.secondary, S_NO_DATA_SUB);
    }

    if (k) {
        ui_setf(q.counts, "%s %d · %s %d · %s %d",
                S_LEFT_NEW, k->session.left_new,
                S_LEFT_REVIEW, k->session.left_review,
                S_RETRY, k->session.retry);
    } else {
        ui_set(q.counts, "");
    }
}
