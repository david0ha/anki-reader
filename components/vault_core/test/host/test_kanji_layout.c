/*
 * test_kanji_layout.c — the part of the 648x480 grid a _Static_assert cannot reach.
 *
 * ui_kanji_layout.h and ui_kanji_layout.c already fail the BUILD on the horizontal sums, on the
 * byte alignment of the content box, and on the vertical stacking of every band down each face.
 * None of that is repeated here. A test that restates a static assert costs a run and proves
 * nothing: the compiler got there first, and the second copy is one more thing to keep in step.
 *
 * What a static assert cannot see is the page as a whole. It walks one chain of y-coordinates at
 * a time, so it has no way to notice that the front's ornament and the plate's first row want the
 * same ten pixels, that a rail figure has drifted across the gutter into the prose column, or
 * that a rectangle added to either struct six months from now was never looked at by anything.
 *
 * So this file treats the two layouts as data — a table of {name, rect} per face — and checks:
 *
 *   - every rectangle is on the panel and inside the content box, by name;
 *   - every PAIR of rectangles drawn AT THE SAME TIME is disjoint. This is the assertion worth
 *     having. LVGL draws one label straight over another without a word, and black-on-black at
 *     16 px reads as a slightly bold label in a screenshot — the defect survives review precisely
 *     because looking at the panel cannot reveal it;
 *   - nothing in the columns crosses the gutter rule, and each block is in the column the grid
 *     assigns it: prose left, figures right;
 *   - the dock — the one rectangle handed verbatim to epd_refresh_partial_area() — keeps
 *     byte-aligned bounds and is tiled exactly by its four cells.
 *
 * Each table is checked against sizeof(struct) / sizeof(kanji_rect_t), so a field added to either
 * layout without a line in the table fails this test rather than quietly going untested. That is
 * the whole point of building the tables by hand: an omission has to be loud.
 */
#include "th.h"

#include <stdbool.h>
#include <string.h>

#include "ui_kanji_layout.h"

/* --- rectangles as data --------------------------------------------------------------------
 * A rect carries its field name so a failure names the two things that collided. A pairwise
 * report that says only "overlap at line 214" sends the reader back to count coordinates by
 * hand, which is the work the test was supposed to do. */

typedef struct {
    char         name[32];
    kanji_rect_t r;
} named_rect_t;

typedef struct {
    named_rect_t v[64];
    size_t       n;
} rect_set_t;

static void rs_add(rect_set_t *s, const char *name, kanji_rect_t r)
{
    if (s->n >= sizeof s->v / sizeof s->v[0]) {
        printf("  FATAL rect_set_t is too small for %s\n", name);
        exit(2);
    }
    snprintf(s->v[s->n].name, sizeof s->v[s->n].name, "%s", name);
    s->v[s->n].r = r;
    s->n++;
}

static void rs_add_i(rect_set_t *s, const char *name, int i, kanji_rect_t r)
{
    char buf[28];
    snprintf(buf, sizeof buf, "%s[%d]", name, i);
    rs_add(s, buf, r);
}

#define ADD(set, lay, field) rs_add((set), #field, (lay)->field)
#define ADD_N(set, lay, field, count) do {                                  \
        for (int _i = 0; _i < (count); _i++)                                \
            rs_add_i((set), #field, _i, (lay)->field[_i]);                  \
    } while (0)

/* How many kanji_rect_t a layout struct holds. Both structs are rectangles and nothing else, so
 * this is the field count — and it is what makes a forgotten table entry fail rather than pass. */
#define RECT_FIELDS(type) ((int)(sizeof(type) / sizeof(kanji_rect_t)))

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

static void check_all_on_panel(const rect_set_t *s, const char *face)
{
    for (size_t i = 0; i < s->n; i++) {
        const kanji_rect_t r = s->v[i].r;
        /* A zero-width or zero-height slot is worse than a missing one: the layout claims it
         * holds something, the screen file dutifully fills it, and nothing is ever drawn. */
        const bool ok = r.w > 0 && r.h > 0 &&
                        r.x >= 0 && r.y >= 0 &&
                        r.x + r.w <= KANJI_SCREEN_W &&
                        r.y + r.h <= KANJI_SCREEN_H;
        if (!ok) {
            printf("  %s: %s = {%d,%d,%d,%d} is not on the panel\n",
                   face, s->v[i].name, r.x, r.y, r.w, r.h);
        }
        CHECK(ok);
    }
}

static void check_all_inside_content_box(const rect_set_t *s, const char *face)
{
    for (size_t i = 0; i < s->n; i++) {
        const kanji_rect_t r = s->v[i].r;
        const bool ok = r.x >= KANJI_CONTENT_X && r.x + r.w <= KANJI_CONTENT_R;
        if (!ok) {
            printf("  %s: %s spans x %d..%d, outside the content box %d..%d\n",
                   face, s->v[i].name, r.x, r.x + r.w, KANJI_CONTENT_X, KANJI_CONTENT_R);
        }
        CHECK(ok);
    }
}

static void check_pairwise_disjoint(const rect_set_t *s, const char *what)
{
    for (size_t i = 0; i < s->n; i++) {
        for (size_t j = i + 1; j < s->n; j++) {
            const bool clash = overlaps(s->v[i].r, s->v[j].r);
            if (clash) {
                printf("  %s: %s {%d,%d,%d,%d} overlaps %s {%d,%d,%d,%d}\n", what,
                       s->v[i].name, s->v[i].r.x, s->v[i].r.y, s->v[i].r.w, s->v[i].r.h,
                       s->v[j].name, s->v[j].r.x, s->v[j].r.y, s->v[j].r.w, s->v[j].r.h);
            }
            CHECK(!clash);
        }
    }
}

static void check_within_span(const rect_set_t *s, const char *what, int x1, int x2)
{
    for (size_t i = 0; i < s->n; i++) {
        const kanji_rect_t r = s->v[i].r;
        const bool ok = r.x >= x1 && r.x + r.w <= x2;
        if (!ok) {
            printf("  %s: %s spans x %d..%d, outside %d..%d\n",
                   what, s->v[i].name, r.x, r.x + r.w, x1, x2);
        }
        CHECK(ok);
    }
}

/* --- 문제, the plate -----------------------------------------------------------------------
 * The plate has two mutually exclusive states and they occupy the same paper: four label|value
 * rows either side of a vertical hairline when the learner has a history with this card, or the
 * single centred "새 카드" row when they do not. Putting both in one disjointness set would fail
 * for a collision that can never appear on the glass, so each is checked as its own page. */

static void front_common(rect_set_t *s)
{
    const kanji_front_layout_t *f = kanji_front_layout();
    s->n = 0;
    ADD(s, f, badge);
    ADD(s, f, brand);
    ADD(s, f, status);
    ADD(s, f, counters);
    ADD(s, f, head_rule);
    ADD(s, f, hero);
    ADD(s, f, orn_left);
    ADD(s, f, orn_mark);
    ADD(s, f, orn_right);
    ADD(s, f, foot_rule);
    ADD(s, f, queue);
    ADD(s, f, prompt);
}

static void front_with_history(rect_set_t *s)
{
    const kanji_front_layout_t *f = kanji_front_layout();
    front_common(s);
    ADD_N(s, f, plate_label, KANJI_PLATE_ROWS);
    ADD(s, f, plate_rule);
    ADD_N(s, f, plate_value, KANJI_PLATE_ROWS);
}

static void front_new_card(rect_set_t *s)
{
    front_common(s);
    ADD(s, kanji_front_layout(), plate_empty);
}

static void front_all(rect_set_t *s)
{
    front_with_history(s);
    ADD(s, kanji_front_layout(), plate_empty);
}

/* --- 정답, the spread ----------------------------------------------------------------------
 * Everything on the back is drawn at once — that is the whole design — so there is exactly one
 * disjointness set, plus the dock's interior checked separately against its own cells. The dock
 * rectangle stands in for its cells in the spread set: it is the band they tile, and a cell that
 * collided with a column would have to collide with the dock first. */

static void back_spread(rect_set_t *s)
{
    const kanji_back_layout_t *b = kanji_back_layout();
    s->n = 0;

    ADD(s, b, hero);
    ADD(s, b, status);
    ADD(s, b, due);
    ADD(s, b, reading);
    ADD(s, b, badge);
    ADD(s, b, band_rule);

    ADD(s, b, col_rule);

    ADD(s, b, sense_eyebrow);
    ADD(s, b, senses);
    ADD(s, b, sense_rule);
    ADD(s, b, build_eyebrow);
    ADD(s, b, principle);
    ADD(s, b, build);
    ADD(s, b, build_rule);
    ADD(s, b, example_eyebrow);
    ADD_N(s, b, example, KANJI_EXAMPLES_SHOWN);

    ADD(s, b, read_eyebrow);
    ADD(s, b, on_label);
    ADD(s, b, on_value);
    ADD(s, b, kun_label);
    ADD(s, b, kun_value);
    ADD(s, b, read_rule);
    ADD(s, b, part_eyebrow);
    ADD_N(s, b, part_glyph, KANJI_PARTS_SHOWN);
    ADD_N(s, b, part_gloss, KANJI_PARTS_SHOWN);
    ADD(s, b, part_rule);
    ADD(s, b, stat_eyebrow);
    ADD_N(s, b, stat_label, KANJI_STATS_SHOWN);
    ADD_N(s, b, stat_value, KANJI_STATS_SHOWN);

    ADD(s, b, dock);
}

static void back_dock_contents(rect_set_t *s)
{
    const kanji_back_layout_t *b = kanji_back_layout();
    s->n = 0;
    ADD_N(s, b, cell_key,  KANJI_GRADE_COUNT);
    ADD_N(s, b, cell_name, KANJI_GRADE_COUNT);
    ADD_N(s, b, cell_span, KANJI_GRADE_COUNT);
}

static void back_all(rect_set_t *s)
{
    const kanji_back_layout_t *b = kanji_back_layout();
    back_spread(s);
    ADD_N(s, b, cell,      KANJI_GRADE_COUNT);
    ADD_N(s, b, cell_key,  KANJI_GRADE_COUNT);
    ADD_N(s, b, cell_name, KANJI_GRADE_COUNT);
    ADD_N(s, b, cell_span, KANJI_GRADE_COUNT);
}

/* The two columns, named. The sweep below proves nothing straddles the gutter; these two tables
 * prove each block is in the column the grid gives it — 성립 and 예문 are prose and belong left,
 * 읽기 · 구성 · 기억 are two-word rows and figures and belong right. A rail figure that drifted
 * into the prose column would still clear the gutter and still look plausible in a screenshot. */

static void back_left_column(rect_set_t *s)
{
    const kanji_back_layout_t *b = kanji_back_layout();
    s->n = 0;
    ADD(s, b, sense_eyebrow);
    ADD(s, b, senses);
    ADD(s, b, sense_rule);
    ADD(s, b, build_eyebrow);
    ADD(s, b, principle);
    ADD(s, b, build);
    ADD(s, b, build_rule);
    ADD(s, b, example_eyebrow);
    ADD_N(s, b, example, KANJI_EXAMPLES_SHOWN);
}

static void back_right_rail(rect_set_t *s)
{
    const kanji_back_layout_t *b = kanji_back_layout();
    s->n = 0;
    ADD(s, b, read_eyebrow);
    ADD(s, b, on_label);
    ADD(s, b, on_value);
    ADD(s, b, kun_label);
    ADD(s, b, kun_value);
    ADD(s, b, read_rule);
    ADD(s, b, part_eyebrow);
    ADD_N(s, b, part_glyph, KANJI_PARTS_SHOWN);
    ADD_N(s, b, part_gloss, KANJI_PARTS_SHOWN);
    ADD(s, b, part_rule);
    ADD(s, b, stat_eyebrow);
    ADD_N(s, b, stat_label, KANJI_STATS_SHOWN);
    ADD_N(s, b, stat_value, KANJI_STATS_SHOWN);
}

/* --- the tests ------------------------------------------------------------------------------ */

static void test_every_rectangle_is_covered_by_a_table(void)
{
    rect_set_t front, back;
    front_all(&front);
    back_all(&back);

    /* If either of these fails, a rectangle was added to a layout struct and to no table here,
     * which means nothing checks that it is on the panel or that it clears its neighbours. Add
     * the field to the table above rather than raising the number. */
    CHECK_INT((int)front.n, RECT_FIELDS(kanji_front_layout_t));
    CHECK_INT((int)back.n,  RECT_FIELDS(kanji_back_layout_t));
}

static void test_every_rectangle_is_on_the_panel(void)
{
    rect_set_t front, back;
    front_all(&front);
    back_all(&back);

    check_all_on_panel(&front, "front");
    check_all_on_panel(&back,  "back");

    /* Horizontally the margin is absolute: nothing on either face reaches into the 24 px of
     * paper at the edges. Vertically it is not — the front's masthead deliberately sits at
     * y = 16 — so only the x bounds are swept here. */
    check_all_inside_content_box(&front, "front");
    check_all_inside_content_box(&back,  "back");
}

static void test_nothing_drawn_together_overlaps(void)
{
    rect_set_t s;

    front_with_history(&s);
    check_pairwise_disjoint(&s, "front, card with a history");

    front_new_card(&s);
    check_pairwise_disjoint(&s, "front, new card");

    back_spread(&s);
    check_pairwise_disjoint(&s, "back");

    back_dock_contents(&s);
    check_pairwise_disjoint(&s, "dock contents");
}

static void test_the_columns_never_cross_the_gutter(void)
{
    const kanji_back_layout_t *b = kanji_back_layout();
    const int col_top = b->col_rule.y;
    const int col_bot = b->col_rule.y + b->col_rule.h;
    const int rule_l  = KANJI_COL_RULE_X;
    const int rule_r  = KANJI_COL_RULE_X + KANJI_RULE_HAIR;

    /* The rule lives in the gap and in neither column, so the gutter is real paper on both sides
     * rather than a line with type against it. */
    CHECK(KANJI_COL_L_X + KANJI_COL_L_W <= rule_l);
    CHECK(rule_r <= KANJI_COL_R_X);

    rect_set_t left, right;
    back_left_column(&left);
    back_right_rail(&right);
    check_within_span(&left,  "left column", KANJI_COL_L_X, KANJI_COL_L_X + KANJI_COL_L_W);
    check_within_span(&right, "right rail",  KANJI_COL_R_X, KANJI_CONTENT_R);

    /* And the sweep that needs no table: every rectangle between the band rule and the dock is
     * wholly in one column or the other. The masthead above and the dock below are full-width by
     * design and exempt; so is the gutter rule itself. */
    rect_set_t spread;
    back_spread(&spread);
    size_t in_column = 0;
    for (size_t i = 0; i < spread.n; i++) {
        const kanji_rect_t r = spread.v[i].r;
        if (r.y + r.h <= col_top) continue;                       /* masthead      */
        if (r.y >= col_bot) continue;                             /* the dock band */
        if (r.x == rule_l && r.w == KANJI_RULE_HAIR) continue;    /* the rule      */
        in_column++;

        const bool l = r.x >= KANJI_COL_L_X && r.x + r.w <= KANJI_COL_L_X + KANJI_COL_L_W;
        const bool rr = r.x >= KANJI_COL_R_X && r.x + r.w <= KANJI_CONTENT_R;
        if (!(l || rr)) {
            printf("  back: %s spans x %d..%d and is in neither column\n",
                   spread.v[i].name, r.x, r.x + r.w);
        }
        CHECK(l || rr);
    }
    /* Ties the two named tables to the sweep: a new column rectangle that reaches the sweep but
     * not a table shows up here as a count that no longer matches. */
    CHECK_INT((int)(left.n + right.n), (int)in_column);
}

static void test_dock_bounds_are_byte_aligned(void)
{
    const kanji_back_layout_t *b = kanji_back_layout();

    int x1 = -1, y1 = -1, x2 = -1, y2 = -1;
    kanji_rect_to_half_open(&b->dock, &x1, &y1, &x2, &y2);
    CHECK_INT(x1, 24);
    CHECK_INT(y1, 412);
    CHECK_INT(x2, 624);
    CHECK_INT(y2, 464);

    /* epd_refresh_partial_area() refreshes whole framebuffer bytes. A dock whose x bounds are not
     * multiples of 8 refreshes a window that does not contain the thing that changed, and nothing
     * logs it — the panel simply keeps showing the previous state. There is no partial refresh on
     * the board today, but the assertion is free and it keeps the invariant true for the day one
     * comes back. */
    CHECK_INT(x1 % KANJI_BYTE_ALIGN, 0);
    CHECK_INT(x2 % KANJI_BYTE_ALIGN, 0);
}

static void test_half_open_conversion_ignores_what_it_is_not_given(void)
{
    const kanji_rect_t r = { 100, 40, 20, 10 };

    int only = -1;
    kanji_rect_to_half_open(&r, NULL, NULL, &only, NULL);
    CHECK_INT(only, 120);

    /* A NULL rect must leave every output alone rather than writing zeroes: a caller that reads
     * back a 0x0 window would refresh the top-left corner instead of refreshing nothing. */
    int x1 = -7, y1 = -7, x2 = -7, y2 = -7;
    kanji_rect_to_half_open(NULL, &x1, &y1, &x2, &y2);
    CHECK_INT(x1, -7);
    CHECK_INT(y1, -7);
    CHECK_INT(x2, -7);
    CHECK_INT(y2, -7);
}

static void test_grade_cells_tile_the_dock_exactly(void)
{
    const kanji_back_layout_t *b = kanji_back_layout();

    CHECK_INT(b->cell[0].x, b->dock.x);
    CHECK_INT(b->cell[KANJI_GRADE_COUNT - 1].x + b->cell[KANJI_GRADE_COUNT - 1].w,
              b->dock.x + b->dock.w);
    for (int i = 0; i < KANJI_GRADE_COUNT; i++) {
        CHECK_INT(b->cell[i].y, b->dock.y);
        CHECK_INT(b->cell[i].h, b->dock.h);
        /* No gap and no overlap: the next cell starts exactly where this one ends. A one-pixel
         * crack between two cells is a white stripe down a ruled row, and a one-pixel overlap
         * puts two rules on the same column of pixels — neither is visible in a screenshot. */
        if (i + 1 < KANJI_GRADE_COUNT) CHECK_INT(b->cell[i].x + b->cell[i].w, b->cell[i + 1].x);
        CHECK(contains(b->dock, b->cell[i]));

        /* The button glyph, the grade name and the interval belong to their OWN cell. A span that
         * spilled one cell to the right would print 4일 뒤 under 보통, which is a wrong number the
         * learner has no way to know is wrong. */
        CHECK(contains(b->cell[i], b->cell_key[i]));
        CHECK(contains(b->cell[i], b->cell_name[i]));
        CHECK(contains(b->cell[i], b->cell_span[i]));
    }
}

static void test_hero_face_is_chosen_where_the_slot_can_hold_it(void)
{
    const kanji_front_layout_t *f = kanji_front_layout();
    const kanji_back_layout_t  *b = kanji_back_layout();

    /* Hand-measured font bounds, not values the layout computes: a CJK glyph is full-width, so
     * the 56 px hero advances 56 px per character on a 66 px line, and the 28 px fallback
     * advances 28 on a 35 px line. */
    const int hero_px = 56, hero_line = 66;
    const int fall_px = 28, fall_line = 35;

    CHECK(kanji_hero_is_large(""));
    CHECK(kanji_hero_is_large("食べる"));

    /* The 5/6 boundary, from both sides. Five is the largest headword the hero face is allowed,
     * and it is allowed because 5 x 56 fits the narrower of the two slots. */
    CHECK(kanji_hero_is_large("一二三四五"));
    CHECK(!kanji_hero_is_large("一二三四五六"));
    CHECK(5 * hero_px <= f->hero.w);
    CHECK(5 * hero_px <= b->hero.w);
    CHECK(hero_line <= f->hero.h);
    CHECK(hero_line <= b->hero.h);

    /* And the fallback holds the catalog's longest headword — ten characters — on one line, which
     * is why neither slot is tall enough for two. A 13-character headword is byte-legal in a
     * 40-byte front and does not exist in the catalog; it would ellipsize in the back's slot. */
    CHECK(10 * fall_px <= f->hero.w);
    CHECK(10 * fall_px <= b->hero.w);
    CHECK(fall_line <= f->hero.h);
    CHECK(fall_line <= b->hero.h);

    /* The face is chosen by character count and not by byte count: a headword of five kana is
     * five characters and fifteen bytes. */
    CHECK(kanji_hero_is_large("あいうえお"));
    CHECK(!kanji_hero_is_large("あいうえおか"));
}

static void test_center_x_never_leaves_its_box(void)
{
    const kanji_rect_t outer = { KANJI_CONTENT_X, 88, KANJI_CONTENT_W, 72 };

    CHECK_INT(kanji_center_x(&outer, 280), 184);      /* the hero at five characters */
    CHECK_INT(kanji_center_x(&outer, 0), 324);        /* the page centre             */

    /* Truncation must round toward the left edge rather than off it. */
    CHECK_INT(kanji_center_x(&outer, KANJI_CONTENT_W - 1), KANJI_CONTENT_X);

    /* Content wider than its box is pinned to the left edge, not centred to a negative x: LVGL
     * would happily place a label at x = -14 and clip the first glyph off the panel. */
    CHECK_INT(kanji_center_x(&outer, KANJI_CONTENT_W), KANJI_CONTENT_X);
    CHECK_INT(kanji_center_x(&outer, KANJI_CONTENT_W + 200), KANJI_CONTENT_X);

    CHECK_INT(kanji_center_x(NULL, 100), 0);
}

static void test_display_headword_canonicalizes_maximum_whitespace_content(void)
{
    /* The worst case the model can carry: 39 bytes alternating a glyph and a different flavour of
     * ASCII whitespace, so every collapse path runs and the result is exactly one byte short of
     * the destination. */
    char raw[KANJI_FRONT_MAX];
    static const char whitespace[] = { ' ', '\n', '\r', '\t', '\f', '\v' };
    const size_t whitespace_count = sizeof whitespace / sizeof whitespace[0];
    for (int i = 0; i < KANJI_FRONT_MAX - 1; i++) {
        raw[i] = (i % 2 == 0) ? 'W' : whitespace[(i / 2) % whitespace_count];
    }
    raw[KANJI_FRONT_MAX - 1] = '\0';
    CHECK_INT((int)strlen(raw), KANJI_FRONT_MAX - 1);

    char display[KANJI_FRONT_MAX];
    const size_t written = kanji_headword_display_text(display, raw);
    CHECK_STR(display, "W W W W W W W W W W W W W W W W W W W W");
    CHECK_INT((int)written, KANJI_FRONT_MAX - 1);
    CHECK(written < KANJI_FRONT_MAX);
    /* A tab or a newline reaching a label is not a wrong glyph, it is a second LINE — the
     * headword's slot is one line tall and the overflow lands on the rule beneath it. */
    CHECK(strpbrk(display, "\n\r\t\f\v") == NULL);

    /* Leading and trailing whitespace is dropped rather than collapsed to a space, which is what
     * lets a headword the producer padded still take the hero face. */
    char face_display[KANJI_FRONT_MAX];
    kanji_headword_display_text(face_display, " \t会\r\nう\v ");
    CHECK_STR(face_display, "会 う");
    CHECK(kanji_hero_is_large(face_display));

    /* And the reason the collapse runs before the face is chosen: raw whitespace counts as
     * characters, so a headword of three kanji separated by five whitespace bytes measures eight
     * and falls off the hero face for no reason a reader could see. */
    const char face_raw[] = "一\n\n二\t\r三";
    kanji_headword_display_text(face_display, face_raw);
    CHECK_STR(face_display, "一 二 三");
    CHECK(!kanji_hero_is_large(face_raw));
    CHECK(kanji_hero_is_large(face_display));

    CHECK_INT((int)kanji_headword_display_text(face_display, NULL), 0);
    CHECK_STR(face_display, "");
}

int main(void)
{
    test_every_rectangle_is_covered_by_a_table();
    test_every_rectangle_is_on_the_panel();
    test_nothing_drawn_together_overlaps();
    test_the_columns_never_cross_the_gutter();
    test_dock_bounds_are_byte_aligned();
    test_half_open_conversion_ignores_what_it_is_not_given();
    test_grade_cells_tile_the_dock_exactly();
    test_hero_face_is_chosen_where_the_slot_can_hold_it();
    test_center_x_never_leaves_its_box();
    test_display_headword_canonicalizes_maximum_whitespace_content();
    TH_REPORT("kanji_layout");
}
