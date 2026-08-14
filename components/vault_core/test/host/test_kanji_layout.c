/* Fixed-grid contract for the 648x480 Lexicographic Instrument. */
#include "th.h"

#include "ui_kanji_layout.h"

#define CHECK_RECT(r, X, Y, W, H) do { \
    CHECK_INT((r).x, (X)); CHECK_INT((r).y, (Y)); \
    CHECK_INT((r).w, (W)); CHECK_INT((r).h, (H)); \
} while (0)

static bool on_screen(kanji_rect_t r)
{
    return r.x >= 0 && r.y >= 0 && r.w > 0 && r.h > 0 &&
           r.x + r.w <= KANJI_SCREEN_W && r.y + r.h <= KANJI_SCREEN_H;
}

static bool overlaps(kanji_rect_t a, kanji_rect_t b)
{
    return a.x < b.x + b.w && b.x < a.x + a.w &&
           a.y < b.y + b.h && b.y < a.y + a.h;
}

static bool contains(kanji_rect_t outer, kanji_rect_t inner)
{
    return inner.x >= outer.x && inner.y >= outer.y &&
           inner.x + inner.w <= outer.x + outer.w &&
           inner.y + inner.h <= outer.y + outer.h;
}

static void test_approved_primary_grid(void)
{
    const kanji_chrome_t *c = kanji_chrome_layout();
    const kanji_answer_layout_t *a = kanji_answer_layout();

    CHECK_RECT(c->rail,      16,  16,  80, 408);
    CHECK_RECT(c->rail_rule, 96,  16,   1, 408);
    CHECK_RECT(c->main,     112,  56, 520, 368);
    CHECK_RECT(c->footer,   16, 440, 616,  40);
    CHECK_RECT(a->dock,    112, 344, 520,  80);
    for (int i = 0; i < KANJI_GRADE_COUNT; i++) CHECK_INT(a->cell[i].w, 130);
}

static void test_every_rectangle_stays_on_screen(void)
{
    const kanji_chrome_t *c = kanji_chrome_layout();
    const kanji_question_layout_t *q = kanji_question_layout();
    const kanji_answer_layout_t *a = kanji_answer_layout();
    const kanji_sheet_layout_t *plain = kanji_sheet_layout(false);
    const kanji_sheet_layout_t *stats = kanji_sheet_layout(true);
    const kanji_rect_t rects[] = {
        c->rail, c->rail_rule, c->rail_identity, c->rail_progress, c->main,
        c->masthead, c->brand, c->session, c->battery, c->footer,
        q->hero, q->prompt, q->secondary, q->counts,
        a->hero, a->reading, a->meaning, a->examples, a->prompt, a->dock,
        plain->headword, plain->title, plain->body, plain->pager,
        stats->headword, stats->title, stats->body, stats->stats, stats->pager,
    };
    for (size_t i = 0; i < sizeof rects / sizeof rects[0]; i++) {
        CHECK(on_screen(rects[i]));
    }
    for (int i = 0; i < 4; i++) {
        CHECK(on_screen(c->keycap[i]));
        CHECK(on_screen(c->key_action[i]));
        CHECK(contains(c->footer, c->keycap[i]));
        CHECK(contains(c->footer, c->key_action[i]));
    }
    for (int i = 0; i < KANJI_GRADE_COUNT; i++) {
        CHECK(on_screen(a->cell[i]));
        CHECK(on_screen(a->cell_label[i]));
        CHECK(on_screen(a->cell_span[i]));
    }
    for (int i = 0; i < KANJI_STAT_CELLS; i++) CHECK(on_screen(stats->stat[i]));
}

static void test_question_regions_do_not_overlap(void)
{
    const kanji_question_layout_t *q = kanji_question_layout();
    CHECK(!overlaps(q->hero, q->prompt));
    CHECK(!overlaps(q->hero, q->counts));
    CHECK(!overlaps(q->prompt, q->counts));
}

static void test_all_three_answer_examples_are_reachable(void)
{
    const kanji_answer_layout_t *a = kanji_answer_layout();
    CHECK_INT(a->example_rows, KANJI_EXAMPLES_MAX);
    CHECK(a->example_step > 0);
    CHECK(a->examples.y + a->example_rows * a->example_step <= a->prompt.y);
    CHECK(!overlaps(a->examples, a->dock));
    CHECK(contains(a->dock, a->cell[0]));
    CHECK(contains(a->dock, a->cell[KANJI_GRADE_COUNT - 1]));
}

static void test_description_prose_has_a_full_reading_page(void)
{
    const kanji_sheet_layout_t *plain = kanji_sheet_layout(false);
    CHECK_INT(plain->body.w, 520);
    CHECK(plain->body.h >= 320);
}

static void test_dock_converts_to_exact_half_open_bounds(void)
{
    int x1 = -1, y1 = -1, x2 = -1, y2 = -1;
    kanji_rect_to_half_open(&kanji_answer_layout()->dock, &x1, &y1, &x2, &y2);
    CHECK_INT(x1, 112);
    CHECK_INT(y1, 344);
    CHECK_INT(x2, 632);
    CHECK_INT(y2, 424);
    CHECK_INT(x1 % KANJI_BYTE_ALIGN, 0);
    CHECK_INT(x2 % KANJI_BYTE_ALIGN, 0);
}

static void test_content_dependent_helpers(void)
{
    CHECK(kanji_hero_is_large(""));
    CHECK(kanji_hero_is_large("食べる"));
    CHECK(!kanji_hero_is_large("一二三四五六"));
    const kanji_rect_t r = { 112, 56, 520, 368 };
    CHECK_INT(kanji_center_x(&r, 120), 312);
    CHECK_INT(kanji_center_x(&r, 600), 112);
}

int main(void)
{
    test_approved_primary_grid();
    test_every_rectangle_stays_on_screen();
    test_question_regions_do_not_overlap();
    test_all_three_answer_examples_are_reachable();
    test_description_prose_has_a_full_reading_page();
    test_dock_converts_to_exact_half_open_bounds();
    test_content_dependent_helpers();
    TH_REPORT("kanji_layout");
}
