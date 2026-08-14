/* Fixed-grid contract for the 648x480 Lexicographic Instrument. */
#include "th.h"

#include <string.h>

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

static void test_maximum_valid_headword_has_full_fallback_wrap_capacity(void)
{
    const kanji_question_layout_t *q = kanji_question_layout();
    const kanji_answer_layout_t *a = kanji_answer_layout();
    /* Generated 28 px fallback: 35 px line height, <=29 px advance. A valid
     * front has at most 39 bytes, so even 39 single-byte widest glyphs need no
     * more than three 520 px lines. These are hand-measured font bounds, not
     * values computed by the layout under test. */
    const int max_lines = 3;
    const int line_height = 35;
    const int max_advance = 29;
    CHECK((KANJI_FRONT_MAX - 1) * max_advance <= q->hero.w * max_lines);
    CHECK((KANJI_FRONT_MAX - 1) * max_advance <= a->hero.w * max_lines);
    CHECK(q->hero.h >= max_lines * line_height);
    CHECK(a->hero.h >= max_lines * line_height);
    CHECK(q->hero.y + q->hero.h <= q->prompt.y);
    CHECK(a->hero.y + a->hero.h <= a->reading.y);
}

static void test_display_headword_canonicalizes_maximum_whitespace_content(void)
{
    char raw[KANJI_FRONT_MAX];
    static const char whitespace[] = { ' ', '\n', '\r', '\t', '\f', '\v' };
    const size_t whitespace_count = sizeof whitespace / sizeof whitespace[0];
    for (int i = 0; i < KANJI_FRONT_MAX - 1; i++) {
        raw[i] = (i % 2 == 0)
                     ? 'W'
                     : whitespace[(i / 2) % whitespace_count];
    }
    raw[KANJI_FRONT_MAX - 1] = '\0';
    CHECK_INT((int)strlen(raw), KANJI_FRONT_MAX - 1);

    char display[KANJI_FRONT_MAX];
    const size_t written = kanji_headword_display_text(display, raw);
    CHECK_STR(display,
              "W W W W W W W W W W W W W W W W W W W W");
    CHECK_INT((int)written, 39);
    CHECK(written < KANJI_FRONT_MAX);
    CHECK(strpbrk(display, "\n\r\t\f\v") == NULL);
    CHECK((int)written * 29 <= kanji_question_layout()->hero.w * 3);

    const char face_raw[] = "一\n\n二\t\r三";
    char face_display[KANJI_FRONT_MAX];
    kanji_headword_display_text(face_display, face_raw);
    CHECK_STR(face_display, "一 二 三");
    CHECK(!kanji_hero_is_large(face_raw));
    CHECK(kanji_hero_is_large(face_display));

    kanji_headword_display_text(face_display, " \t会\r\nう\v ");
    CHECK_STR(face_display, "会 う");
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
    test_maximum_valid_headword_has_full_fallback_wrap_capacity();
    test_display_headword_canonicalizes_maximum_whitespace_content();
    test_dock_converts_to_exact_half_open_bounds();
    test_content_dependent_helpers();
    TH_REPORT("kanji_layout");
}
