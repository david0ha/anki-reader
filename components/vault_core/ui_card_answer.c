/* Paper answer composition and the isolated partial-refresh grade dock. */
#include <stdio.h>

#include "ui_internal.h"

typedef struct {
    lv_obj_t *root;
    lv_obj_t *hero;
    lv_obj_t *reading;
    lv_obj_t *meaning;
    lv_obj_t *example[KANJI_EXAMPLES_MAX];
    lv_obj_t *cell_fill[KANJI_GRADE_COUNT];
    lv_obj_t *cell_label[KANJI_GRADE_COUNT];
    lv_obj_t *cell_span[KANJI_GRADE_COUNT];
} answer_ui_t;

static answer_ui_t a;

lv_obj_t *ui_card_answer_create(lv_obj_t *par)
{
    const kanji_chrome_t *c = kanji_chrome_layout();
    const kanji_answer_layout_t *l = kanji_answer_layout();
    a.root = ui_fill_white(par, 0, 0, c->main.w, c->main.h);

    a.hero = ui_lab_headword(a.root, LOCAL_X(l->hero.x), LOCAL_Y(l->hero.y),
                             l->hero.w, l->hero.h, UI_F_HERO, "");
    a.reading = ui_lab_w(a.root, LOCAL_X(l->reading.x), LOCAL_Y(l->reading.y),
                         l->reading.w, UI_F_HEAD, LV_TEXT_ALIGN_LEFT, "");
    a.meaning = ui_lab_w(a.root, LOCAL_X(l->meaning.x), LOCAL_Y(l->meaning.y),
                         l->meaning.w, UI_F_TITLE, LV_TEXT_ALIGN_LEFT, "");
    for (int i = 0; i < KANJI_EXAMPLES_MAX; i++) {
        a.example[i] = ui_lab_w(a.root, LOCAL_X(l->examples.x),
                                LOCAL_Y(l->examples.y) + i * l->example_step,
                                l->examples.w, UI_F_BODY,
                                LV_TEXT_ALIGN_LEFT, "");
    }
    ui_lab_w(a.root, LOCAL_X(l->prompt.x), LOCAL_Y(l->prompt.y), l->prompt.w,
             UI_F_HEAD, LV_TEXT_ALIGN_LEFT, S_GRADE_PROMPT);

    for (int i = 0; i < KANJI_GRADE_COUNT; i++) {
        ui_frame(a.root, LOCAL_X(l->cell[i].x), LOCAL_Y(l->cell[i].y),
                 l->cell[i].w, l->cell[i].h, 1);
        a.cell_fill[i] = ui_fill(a.root, LOCAL_X(l->cell[i].x) + 1,
                                 LOCAL_Y(l->cell[i].y) + 1,
                                 l->cell[i].w - 2, l->cell[i].h - 2);
        a.cell_label[i] = ui_lab_w(a.root, LOCAL_X(l->cell_label[i].x),
                                   LOCAL_Y(l->cell_label[i].y),
                                   l->cell_label[i].w, UI_F_HEAD,
                                   LV_TEXT_ALIGN_CENTER,
                                   kanji_grade_label((kanji_grade_t)(i + 1)));
        a.cell_span[i] = ui_lab_w(a.root, LOCAL_X(l->cell_span[i].x),
                                  LOCAL_Y(l->cell_span[i].y),
                                  l->cell_span[i].w, UI_F_BODY,
                                  LV_TEXT_ALIGN_CENTER, "");
    }
    return a.root;
}

void ui_card_answer_dock(const kanji_t *k, kanji_grade_t cursor)
{
    if (cursor < KANJI_GRADE_AGAIN || cursor > KANJI_GRADE_EASY) {
        cursor = KANJI_GRADE_GOOD;
    }
    for (int i = 0; i < KANJI_GRADE_COUNT; i++) {
        const kanji_grade_t grade = (kanji_grade_t)(i + 1);
        const bool selected = grade == cursor;
        ui_show(a.cell_fill[i], selected);
        lv_obj_set_style_text_color(a.cell_label[i],
                                    selected ? lv_color_white() : lv_color_black(), 0);
        lv_obj_set_style_text_color(a.cell_span[i],
                                    selected ? lv_color_white() : lv_color_black(), 0);
        ui_set(a.cell_span[i], k ? kanji_preview_span(k, grade) : "");
    }
}

void ui_card_answer_update(const kanji_t *k, kanji_grade_t cursor)
{
    const bool have = k && k->card.valid;
    ui_apply_headword(a.hero, have ? k->card.front : "");
    if (have && k->card.reading[0]) ui_setf(a.reading, "%s  %s", S_READING, k->card.reading);
    else ui_set(a.reading, "");

    if (have && k->card.sense_count > 0) {
        char joined[KANJI_SENSES_MAX * (KANJI_SENSE_MAX + 3)];
        size_t at = (size_t)snprintf(joined, sizeof joined, "%s  ", S_MEANING);
        for (int i = 0; i < k->card.sense_count && at < sizeof joined; i++) {
            const int n = snprintf(joined + at, sizeof joined - at, "%s%s",
                                   i ? ", " : "", k->card.senses[i]);
            if (n <= 0 || (size_t)n >= sizeof joined - at) break;
            at += (size_t)n;
        }
        ui_set(a.meaning, joined);
    } else {
        ui_set(a.meaning, "");
    }

    for (int i = 0; i < KANJI_EXAMPLES_MAX; i++) {
        const bool on = have && i < k->card.example_count;
        if (on) {
            const kanji_example_t *e = &k->card.examples[i];
            if (e->reading[0] && e->gloss[0])
                ui_setf(a.example[i], "%s %d  %s (%s) — %s", S_EXAMPLE, i + 1,
                        e->text, e->reading, e->gloss);
            else if (e->gloss[0])
                ui_setf(a.example[i], "%s %d  %s — %s", S_EXAMPLE, i + 1,
                        e->text, e->gloss);
            else
                ui_setf(a.example[i], "%s %d  %s", S_EXAMPLE, i + 1, e->text);
        } else {
            ui_set(a.example[i], "");
        }
        ui_show(a.example[i], on);
    }
    ui_card_answer_dock(k, cursor);
}
