/*
 * ui_card_answer.c — the answer side, and the FSRS grade dock.
 *
 * The headword stays inverted in a band at the top so the eye can find it again
 * without re-reading the page; everything that has to be read as prose — the
 * Korean senses, the example — moves onto white below it, because at 16 px
 * after binarization white-on-black Hangul loses its thin strokes.
 *
 * The dock is the one part of this board that is refreshed on its own. Moving
 * the cursor must therefore change nothing outside kanji_answer_layout()->dock,
 * and ui_card_answer_dock() exists so the caller can say "only the cursor
 * moved" without the file having to guess.
 */
#include <stdio.h>

#include "ui_internal.h"

typedef struct {
    lv_obj_t *root;
    lv_obj_t *hero;
    lv_obj_t *reading;
    lv_obj_t *level;
    lv_obj_t *meaning_cap;
    lv_obj_t *meaning;
    lv_obj_t *example_cap;
    lv_obj_t *example[KANJI_EXAMPLES_MAX];

    /* One cell = a border, a label and a span. The fill is a separate object
     * that is shown behind the selected cell only, so moving the cursor is two
     * object changes rather than a rebuild. */
    lv_obj_t *cell_fill[KANJI_GRADE_COUNT];
    lv_obj_t *cell_label[KANJI_GRADE_COUNT];
    lv_obj_t *cell_span[KANJI_GRADE_COUNT];
} answer_ui_t;

static answer_ui_t a;

lv_obj_t *ui_card_answer_create(lv_obj_t *par)
{
    const kanji_answer_layout_t *l = kanji_answer_layout();
    const kanji_chrome_t *c = kanji_chrome_layout();

    a.root = ui_pane(par, 0, 0, c->content.w, c->content.h);
    lv_obj_set_style_bg_color(a.root, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(a.root, LV_OPA_COVER, 0);

    ui_fill(a.root, l->band.x, LOCAL_Y(l->band.y), l->band.w, l->band.h);

    a.hero = ui_lab_w(a.root, l->hero.x, LOCAL_Y(l->hero.y), l->hero.w,
                      UI_F_HERO, LV_TEXT_ALIGN_LEFT, "");
    lv_obj_set_style_text_color(a.hero, lv_color_white(), 0);

    a.reading = ui_lab_inv(a.root, l->reading.x, LOCAL_Y(l->reading.y),
                           l->reading.w, UI_F_HEAD, LV_TEXT_ALIGN_LEFT, "");
    a.level = ui_lab_inv(a.root, l->level.x, LOCAL_Y(l->level.y), l->level.w,
                         UI_F_HEAD, LV_TEXT_ALIGN_RIGHT, "");

    a.meaning_cap = ui_lab(a.root, l->meaning.x, LOCAL_Y(l->meaning.y),
                           UI_F_BODY, S_MEANING);
    a.meaning = ui_lab_w(a.root, l->meaning.x + 40, LOCAL_Y(l->meaning.y) - 2,
                         l->meaning.w - 40, UI_F_TITLE, LV_TEXT_ALIGN_LEFT, "");

    a.example_cap = ui_lab(a.root, l->examples.x, LOCAL_Y(l->examples.y),
                           UI_F_BODY, S_EXAMPLE);
    for (int i = 0; i < l->example_rows && i < KANJI_EXAMPLES_MAX; i++) {
        a.example[i] = ui_lab_w(a.root, l->examples.x + 46,
                                LOCAL_Y(l->examples.y) + i * l->example_step,
                                l->examples.w - 46, UI_F_BODY,
                                LV_TEXT_ALIGN_LEFT, "");
    }

    ui_lab_w(a.root, l->prompt.x, LOCAL_Y(l->prompt.y), l->prompt.w,
             UI_F_BODY, LV_TEXT_ALIGN_CENTER, S_GRADE_PROMPT);

    for (int i = 0; i < KANJI_GRADE_COUNT; i++) {
        ui_frame(a.root, l->cell[i].x, LOCAL_Y(l->cell[i].y),
                 l->cell[i].w, l->cell[i].h, 2);
        a.cell_fill[i] = ui_fill(a.root, l->cell[i].x + 2,
                                 LOCAL_Y(l->cell[i].y) + 2,
                                 l->cell[i].w - 4, l->cell[i].h - 4);
        a.cell_label[i] = ui_lab_w(a.root, l->cell_label[i].x,
                                   LOCAL_Y(l->cell_label[i].y),
                                   l->cell_label[i].w, UI_F_HEAD,
                                   LV_TEXT_ALIGN_CENTER,
                                   kanji_grade_label((kanji_grade_t)(i + 1)));
        a.cell_span[i] = ui_lab_w(a.root, l->cell_span[i].x,
                                  LOCAL_Y(l->cell_span[i].y),
                                  l->cell_span[i].w, UI_F_BODY,
                                  LV_TEXT_ALIGN_CENTER, "");
    }
    return a.root;
}

void ui_card_answer_dock(const kanji_t *k, kanji_grade_t cursor)
{
    for (int i = 0; i < KANJI_GRADE_COUNT; i++) {
        const kanji_grade_t g = (kanji_grade_t)(i + 1);
        const bool picked = (g == cursor);

        ui_show(a.cell_fill[i], picked);
        lv_obj_set_style_text_color(a.cell_label[i],
                                    picked ? lv_color_white() : lv_color_black(), 0);
        lv_obj_set_style_text_color(a.cell_span[i],
                                    picked ? lv_color_white() : lv_color_black(), 0);

        /* The span is what the rating actually costs, and it is the reason the
         * dock is worth four cells instead of one toggle. Blank rather than
         * "—" when the proxy could not compute it: an invented interval is
         * worse than an absent one. */
        ui_set(a.cell_span[i], k ? kanji_preview_span(k, g) : "");
    }
}

void ui_card_answer_update(const kanji_t *k, kanji_grade_t cursor)
{
    const kanji_answer_layout_t *l = kanji_answer_layout();
    const bool have = k && k->card.valid;

    const lv_font_t *face = ui_hero_face(have ? k->card.front : "");
    lv_obj_set_style_text_font(a.hero, face, 0);
    lv_obj_set_height(a.hero, lv_font_get_line_height(face));
    ui_set(a.hero, have ? k->card.front : "");

    ui_set(a.reading, have ? k->card.reading : "");
    ui_set(a.level, have ? k->card.level : "");

    /* The senses are joined into one line rather than stacked: three glosses of
     * the same word are one answer, and stacking them reads as three answers. */
    if (have && k->card.sense_count > 0) {
        char joined[KANJI_SENSES_MAX * (KANJI_SENSE_MAX + 3)];
        size_t at = 0;
        for (int i = 0; i < k->card.sense_count; i++) {
            const int n = snprintf(joined + at, sizeof joined - at, "%s%s",
                                   i ? ", " : "", k->card.senses[i]);
            if (n <= 0 || (size_t)n >= sizeof joined - at) break;
            at += (size_t)n;
        }
        ui_set(a.meaning, joined);
    } else {
        ui_set(a.meaning, "");
    }
    ui_show(a.meaning_cap, have && k->card.sense_count > 0);

    for (int i = 0; i < l->example_rows && i < KANJI_EXAMPLES_MAX; i++) {
        if (!a.example[i]) continue;
        if (have && i < k->card.example_count) {
            const kanji_example_t *e = &k->card.examples[i];
            if (e->reading[0] && e->gloss[0]) {
                ui_setf(a.example[i], "%s (%s) — %s", e->text, e->reading, e->gloss);
            } else if (e->gloss[0]) {
                ui_setf(a.example[i], "%s — %s", e->text, e->gloss);
            } else {
                ui_set(a.example[i], e->text);
            }
            ui_show(a.example[i], true);
        } else {
            ui_set(a.example[i], "");
            ui_show(a.example[i], false);
        }
    }
    ui_show(a.example_cap, have && k->card.example_count > 0);

    ui_card_answer_dock(k, cursor);
}
