/*
 * Where everything goes. Pure integers, no LVGL.
 *
 * Two classes of bug live here. The cheap one is a rectangle off the panel or
 * on top of another. The expensive one is the grade dock: its rectangle is
 * handed verbatim to epd_refresh_partial_area(), so if it does not contain
 * everything the dock draws, choosing a rating updates nothing visible and the
 * board silently shows the previous one.
 */
#include "th.h"

#include "ui_kanji_layout.h"

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

/* --- the shared chrome ---------------------------------------------------- */

static void test_chrome_covers_the_panel_exactly_once(void)
{
    const kanji_chrome_t *c = kanji_chrome_layout();
    CHECK(on_screen(c->header));
    CHECK(on_screen(c->content));
    CHECK(on_screen(c->footer));

    /* The header starts at the top edge and the footer ends at the bottom one:
     * a one-pixel white margin around a full-bleed band is the kind of thing
     * that only shows up on the glass. */
    CHECK_INT(c->header.x, 0);
    CHECK_INT(c->header.y, 0);
    CHECK_INT(c->header.w, KANJI_SCREEN_W);
    CHECK_INT(c->footer.x, 0);
    CHECK_INT(c->footer.w, KANJI_SCREEN_W);
    CHECK_INT(c->footer.y + c->footer.h, KANJI_SCREEN_H);

    /* Header, content and footer tile the panel top to bottom with the two
     * rules between them and nothing left over. */
    CHECK(!overlaps(c->header, c->content));
    CHECK(!overlaps(c->content, c->footer));
    CHECK(c->rule_top >= c->header.y + c->header.h);
    CHECK(c->rule_top < c->content.y);
    CHECK(c->rule_bottom >= c->content.y + c->content.h);
    CHECK(c->rule_bottom < c->footer.y);

    /* The content area is the whole point of the panel: at least 80% of it. */
    CHECK(c->content.h * 100 >= KANJI_SCREEN_H * 80);
}

static void test_the_header_furniture_stays_inside_the_header(void)
{
    const kanji_chrome_t *c = kanji_chrome_layout();
    CHECK(contains(c->header, c->brand));
    CHECK(contains(c->header, c->chips));
    CHECK(contains(c->header, c->track));

    /* Brand left, chips, then the track pill hard right — the order the web
     * app's header has, so the two read the same way. */
    CHECK(c->brand.x < c->chips.x);
    CHECK(c->chips.x + c->chips.w <= c->track.x);
    CHECK_INT(c->track.x + c->track.w + 14, KANJI_SCREEN_W);

    CHECK(!overlaps(c->brand, c->chips));
    CHECK(!overlaps(c->chips, c->track));
}

static void test_the_footer_holds_four_key_hints_side_by_side(void)
{
    const kanji_chrome_t *c = kanji_chrome_layout();
    for (int i = 0; i < 4; i++) {
        CHECK(on_screen(c->key[i]));
        CHECK(contains(c->footer, c->key[i]));
        CHECK(c->key[i].w >= 120);      /* "KEY1 다음 쪽" needs the room */
        if (i > 0) {
            CHECK(c->key[i].x >= c->key[i - 1].x + c->key[i - 1].w);
            CHECK(!overlaps(c->key[i], c->key[i - 1]));
        }
    }
}

/* --- the question screen -------------------------------------------------- */

static void test_the_player_fills_the_content_area(void)
{
    const kanji_chrome_t *c = kanji_chrome_layout();
    const kanji_question_layout_t *q = kanji_question_layout();

    CHECK(on_screen(q->player));
    /* The immersive player IS the content area — the web app insets it inside
     * a phone frame, but a 648x480 panel has no frame to inset from and every
     * pixel spent on a margin is a pixel not spent on the headword. */
    CHECK_INT(q->player.x, c->content.x);
    CHECK_INT(q->player.y, c->content.y);
    CHECK_INT(q->player.w, c->content.w);
    CHECK_INT(q->player.h, c->content.h);
}

static void test_the_headword_is_the_hero_and_is_centred(void)
{
    const kanji_question_layout_t *q = kanji_question_layout();

    CHECK(contains(q->player, q->hero));
    /* Centred horizontally in the player, to the pixel. The action rail is an
     * overlay drawn on top and must NOT push the headword off centre — that
     * is the one thing the web app is explicit about in its card slot. */
    const int left  = q->hero.x - q->player.x;
    const int right = (q->player.x + q->player.w) - (q->hero.x + q->hero.w);
    CHECK_INT(left, right);

    /* Tall enough for a 56 px face with its ascender and descender. */
    CHECK(q->hero.h >= 72);
    /* And sitting in the upper half, so the caption and rail have room. */
    CHECK(q->hero.y + q->hero.h < q->player.y + q->player.h * 2 / 3);
}

static void test_the_question_furniture_does_not_collide(void)
{
    const kanji_question_layout_t *q = kanji_question_layout();
    const kanji_rect_t parts[] = {
        q->hero, q->prompt, q->caption, q->queue, q->rail, q->scrubber,
    };
    const int n = (int)(sizeof parts / sizeof parts[0]);

    for (int i = 0; i < n; i++) {
        CHECK(on_screen(parts[i]));
        CHECK(contains(q->player, parts[i]));
        for (int j = i + 1; j < n; j++) {
            if (overlaps(parts[i], parts[j])) {
                printf("  FAIL question rects %d and %d overlap\n", i, j);
                g_fail++;
            }
            g_total++;
        }
    }

    /* The caption sits bottom-left and the rail hard right, the way a Short
     * lays out — the caption must not run under the rail. */
    CHECK(q->caption.x + q->caption.w <= q->rail.x);
    CHECK(q->caption.y > q->hero.y);
    CHECK(q->rail.x + q->rail.w < q->player.x + q->player.w);

    /* The rail is a column: N items, evenly stepped, all inside it. */
    CHECK(q->rail_items >= 3);
    CHECK(q->rail_step > 0);
    CHECK(q->rail_items * q->rail_step <= q->rail.h);

    /* The scrubber is a line along the player's foot, full width. */
    CHECK(q->scrubber.h <= 6);
    CHECK_INT(q->scrubber.x, q->player.x);
    CHECK_INT(q->scrubber.w, q->player.w);
    CHECK_INT(q->scrubber.y + q->scrubber.h, q->player.y + q->player.h);
}

/* --- the answer screen ---------------------------------------------------- */

static void test_the_answer_band_carries_the_word_and_the_prose_is_below_it(void)
{
    const kanji_chrome_t *c = kanji_chrome_layout();
    const kanji_answer_layout_t *a = kanji_answer_layout();

    CHECK(on_screen(a->band));
    CHECK_INT(a->band.x, c->content.x);
    CHECK_INT(a->band.y, c->content.y);
    CHECK_INT(a->band.w, c->content.w);

    CHECK(contains(a->band, a->hero));
    CHECK(contains(a->band, a->reading));
    CHECK(contains(a->band, a->level));
    CHECK(!overlaps(a->hero, a->level));
    CHECK(!overlaps(a->reading, a->level));
    /* The reading sits under the headword, not beside it. */
    CHECK(a->reading.y >= a->hero.y + a->hero.h);
    /* The JLPT chip is right-aligned. */
    CHECK(a->level.x > a->hero.x + a->hero.w);

    /* Everything readable as prose is BELOW the inverted band, on white. */
    CHECK(a->meaning.y >= a->band.y + a->band.h);
    CHECK(a->examples.y >= a->meaning.y + a->meaning.h);
    CHECK(contains(c->content, a->meaning));
    CHECK(contains(c->content, a->examples));
    CHECK(!overlaps(a->meaning, a->examples));

    /* Three rows, because KANJI_EXAMPLES_MAX is three and a card whose third
     * example silently does not render is a card that lies about the catalog. */
    CHECK_INT(a->example_rows, KANJI_EXAMPLES_MAX);
    CHECK(a->example_step > 0);
    CHECK(a->example_rows * a->example_step <= a->examples.h);

    /* The line that asks for a rating sits between the last example row and the
     * dock. It is in the layout struct rather than placed by eye in the screen
     * file precisely because the budget below the band has no slack: with the
     * examples block at its full three rows there are 26 px between it and the
     * dock, and a prompt placed relative to the dock instead of to the gap
     * overlapped the third example — invisibly, because the demo card has two. */
    CHECK(on_screen(a->prompt));
    CHECK(contains(c->content, a->prompt));
    CHECK(!overlaps(a->prompt, a->examples));
    CHECK(!overlaps(a->prompt, a->dock));
    CHECK(a->prompt.y >= a->examples.y + a->examples.h);
    CHECK(a->prompt.y + a->prompt.h <= a->dock.y);
}

/* The dock's rectangle goes straight to epd_refresh_partial_area(). Everything
 * the dock draws must be inside it, or choosing a rating updates a strip that
 * does not contain the thing that changed. */
static void test_the_grade_dock_contains_everything_it_draws(void)
{
    const kanji_chrome_t *c = kanji_chrome_layout();
    const kanji_answer_layout_t *a = kanji_answer_layout();

    CHECK(on_screen(a->dock));
    CHECK(contains(c->content, a->dock));
    CHECK(!overlaps(a->dock, a->examples));
    CHECK(!overlaps(a->dock, a->meaning));
    CHECK(!overlaps(a->dock, a->band));

    for (int i = 0; i < KANJI_GRADE_COUNT; i++) {
        CHECK(contains(a->dock, a->cell[i]));
        CHECK(contains(a->cell[i], a->cell_label[i]));
        CHECK(contains(a->cell[i], a->cell_span[i]));
        CHECK(!overlaps(a->cell_label[i], a->cell_span[i]));
        /* The span reads under its label, the way the swipe panel words it. */
        CHECK(a->cell_span[i].y >= a->cell_label[i].y + a->cell_label[i].h);
        if (i > 0) {
            CHECK(!overlaps(a->cell[i], a->cell[i - 1]));
            CHECK(a->cell[i].x >= a->cell[i - 1].x + a->cell[i - 1].w);
            /* Equal cells: an unequal dock reads as one rating being the
             * recommended one, which is a claim FSRS does not make. */
            CHECK_INT(a->cell[i].w, a->cell[i - 1].w);
        }
    }
}

/* A partial-refresh window whose x bounds are byte-aligned refreshes exactly
 * the strip that was drawn. The driver snaps outward if it is not, so this is
 * an efficiency and predictability check rather than a correctness one — but
 * it is free to hold, and a dock that drifts off alignment is a silent
 * regression nobody would look for. */
static void test_the_dock_is_byte_aligned(void)
{
    const kanji_answer_layout_t *a = kanji_answer_layout();
    CHECK_INT(a->dock.x % KANJI_BYTE_ALIGN, 0);
    CHECK_INT(a->dock.w % KANJI_BYTE_ALIGN, 0);
    CHECK(a->dock.w >= 4 * 100);      /* four labels plus their spans */
    CHECK(a->dock.h >= 56);
}

/* --- the sheets ----------------------------------------------------------- */

static void test_a_sheet_is_a_strip_and_a_page(void)
{
    const kanji_chrome_t *c = kanji_chrome_layout();
    const kanji_sheet_layout_t *s = kanji_sheet_layout(false);

    CHECK(on_screen(s->band));
    CHECK(on_screen(s->body));
    CHECK(contains(c->content, s->band));
    CHECK(contains(c->content, s->body));
    CHECK(!overlaps(s->band, s->body));
    CHECK(s->body.y >= s->band.y + s->band.h);

    CHECK(contains(s->band, s->band_word));
    CHECK(contains(s->band, s->band_title));
    CHECK(!overlaps(s->band_word, s->band_title));

    CHECK(contains(s->body, s->pager));

    /* The band is a strip, not a hero: the sheet is for reading, and a card
     * whose headword eats a third of the page is a card you cannot read about. */
    CHECK(s->band.h <= 64);
    CHECK(s->body.h >= 280);
}

static void test_the_fsrs_sheet_gives_up_page_height_for_the_card_numbers(void)
{
    const kanji_sheet_layout_t *plain = kanji_sheet_layout(false);
    const kanji_sheet_layout_t *stats = kanji_sheet_layout(true);
    const kanji_chrome_t *c = kanji_chrome_layout();

    CHECK(on_screen(stats->stats));
    CHECK(contains(c->content, stats->stats));
    CHECK(!overlaps(stats->stats, stats->body));
    CHECK(stats->stats.y >= stats->body.y + stats->body.h);

    /* The body is shorter by exactly what the strip took, and the band is
     * unchanged — a sheet whose header moved between pages would jump. */
    CHECK(stats->body.h < plain->body.h);
    CHECK_INT(stats->band.y, plain->band.y);
    CHECK_INT(stats->band.h, plain->band.h);
    CHECK(stats->body.h >= 200);

    for (int i = 0; i < KANJI_STAT_CELLS; i++) {
        CHECK(contains(stats->stats, stats->stat[i]));
        CHECK(stats->stat[i].w >= 60);
        if (i > 0) {
            CHECK(!overlaps(stats->stat[i], stats->stat[i - 1]));
            CHECK(stats->stat[i].x >= stats->stat[i - 1].x + stats->stat[i - 1].w);
            CHECK_INT(stats->stat[i].w, stats->stat[i - 1].w);
        }
    }

    /* The plain sheet has no strip, and says so rather than leaving a stale
     * rectangle a caller might draw into. */
    CHECK_INT(plain->stats.h, 0);
}

/* --- content-dependent choices -------------------------------------------- */

static void test_the_hero_face_shrinks_before_it_truncates(void)
{
    /* The catalog is 90% vocabulary: the modal headword is two characters and
     * the longest is ten. Everything up to five gets the 56 px face. */
    CHECK(kanji_hero_is_large("会"));
    CHECK(kanji_hero_is_large("会う"));
    CHECK(kanji_hero_is_large("出会う"));
    CHECK(kanji_hero_is_large("取り替え"));
    CHECK(kanji_hero_is_large("取り替える"));         /* 5 */
    CHECK(!kanji_hero_is_large("取り替えるの"));      /* 6 */
    CHECK(!kanji_hero_is_large("あいうえおかきくけこ"));

    /* Nothing to draw is not a reason to pick the small face and leave a gap
     * where the hero was. */
    CHECK(kanji_hero_is_large(""));
    CHECK(kanji_hero_is_large(NULL));
}

static void test_centering_is_symmetric_and_never_negative(void)
{
    const kanji_rect_t outer = { 100, 0, 400, 10 };
    CHECK_INT(kanji_center_x(&outer, 200), 200);
    CHECK_INT(kanji_center_x(&outer, 400), 100);
    /* Wider than its container clamps to the left edge rather than starting
     * off-panel, which would clip the head of the word instead of its tail. */
    CHECK_INT(kanji_center_x(&outer, 600), 100);
    CHECK_INT(kanji_center_x(NULL, 10), 0);
}

int main(void)
{
    test_chrome_covers_the_panel_exactly_once();
    test_the_header_furniture_stays_inside_the_header();
    test_the_footer_holds_four_key_hints_side_by_side();
    test_the_player_fills_the_content_area();
    test_the_headword_is_the_hero_and_is_centred();
    test_the_question_furniture_does_not_collide();
    test_the_answer_band_carries_the_word_and_the_prose_is_below_it();
    test_the_grade_dock_contains_everything_it_draws();
    test_the_dock_is_byte_aligned();
    test_a_sheet_is_a_strip_and_a_page();
    test_the_fsrs_sheet_gives_up_page_height_for_the_card_numbers();
    test_the_hero_face_shrinks_before_it_truncates();
    test_centering_is_symmetric_and_never_negative();
    TH_REPORT("kanji_layout");
}
