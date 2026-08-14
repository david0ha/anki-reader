/*
 * The button state machine: every button from every screen.
 *
 * This is the file that decides whether the board is usable. Three buttons have
 * to reach five screens and a four-way rating without a touch panel, and every
 * screen change costs a multi-second full-panel flash — so the tests here are
 * as much about how OFTEN the panel refreshes as about where a press lands.
 */
#include "th.h"

#include "kanji_nav.h"

/* --- fixtures ------------------------------------------------------------- */

/* A card with enough content that every sheet has something to page through. */
static kanji_t rich_card(void)
{
    kanji_t k = {0};
    k.valid = true;
    k.card.valid = true;
    kanji_str_copy(k.card.front, KANJI_FRONT_MAX, "会う");
    kanji_str_copy(k.card.description, KANJI_BODY_MAX,
                   "会는 사람들이 모여 서로 말하고 교류하는 모습을 바탕으로 한 글자입니다.");
    kanji_str_copy(k.card.hook_body, KANJI_BODY_MAX,
                   "위의 구성은 모임을, 아래의 모양은 말함을 나타냅니다.");
    k.card.part_count = 1;
    k.card.comment_count = 3;
    k.card.comment_total = 12;
    return k;
}

/* The state the board is in before the first poll answers, and whenever the
 * session runs dry. */
static kanji_t no_card(void)
{
    kanji_t k = {0};
    k.valid = true;
    k.session.complete = true;
    return k;
}

static kanji_nav_t at_question(void)
{
    kanji_nav_t n;
    kanji_nav_reset(&n);
    return n;
}

static kanji_nav_t at_answer(void)
{
    kanji_nav_t n = at_question();
    kanji_t k = rich_card();
    kanji_nav_press(&n, KANJI_BTN_KEY0, &k);
    return n;
}

/* --- the starting state --------------------------------------------------- */

static void test_reset_is_question_side_up_with_the_cursor_on_good(void)
{
    kanji_nav_t n;
    kanji_nav_reset(&n);
    CHECK(!n.revealed);
    CHECK_INT(n.sheet, KANJI_SHEET_NONE);
    CHECK_INT(n.sheet_page, 0);
    CHECK_INT(n.grade, KANJI_GRADE_GOOD);
    CHECK_INT(kanji_nav_screen(&n), KANJI_SCREEN_QUESTION);
}

static void test_screen_is_derived_and_the_sheet_wins_over_the_card(void)
{
    kanji_nav_t n = at_question();
    CHECK_INT(kanji_nav_screen(&n), KANJI_SCREEN_QUESTION);

    n.revealed = true;
    CHECK_INT(kanji_nav_screen(&n), KANJI_SCREEN_ANSWER);

    /* A sheet is on top of whichever side of the card is showing, so both
     * "revealed" values must name the same screen. */
    n.sheet = KANJI_SHEET_COMMENTS;
    CHECK_INT(kanji_nav_screen(&n), KANJI_SCREEN_COMMENTS);
    n.revealed = false;
    CHECK_INT(kanji_nav_screen(&n), KANJI_SCREEN_COMMENTS);

    n.sheet = KANJI_SHEET_DESCRIPTION;
    CHECK_INT(kanji_nav_screen(&n), KANJI_SCREEN_DESCRIPTION);
    n.sheet = KANJI_SHEET_FSRS;
    CHECK_INT(kanji_nav_screen(&n), KANJI_SCREEN_FSRS);

    CHECK(kanji_nav_screen(NULL) == KANJI_SCREEN_QUESTION);
}

/* --- the question screen -------------------------------------------------- */

static void test_key0_reveals_the_answer(void)
{
    kanji_t k = rich_card();
    kanji_nav_t n = at_question();

    kanji_nav_result_t r = kanji_nav_press(&n, KANJI_BTN_KEY0, &k);
    CHECK_INT(r.action, KANJI_ACT_DRAW_FULL);
    CHECK(r.changed);
    CHECK(n.revealed);
    CHECK_INT(kanji_nav_screen(&n), KANJI_SCREEN_ANSWER);
    /* Revealing must not pre-arm a rating other than the default. */
    CHECK_INT(n.grade, KANJI_GRADE_GOOD);
}

static void test_key0_does_nothing_when_there_is_no_card_to_reveal(void)
{
    kanji_t k = no_card();
    kanji_nav_t n = at_question();

    kanji_nav_result_t r = kanji_nav_press(&n, KANJI_BTN_KEY0, &k);
    CHECK_INT(r.action, KANJI_ACT_NONE);
    CHECK(!r.changed);
    CHECK(!n.revealed);

    /* And a NULL snapshot behaves like an empty one rather than crashing. */
    r = kanji_nav_press(&n, KANJI_BTN_KEY0, NULL);
    CHECK_INT(r.action, KANJI_ACT_NONE);
}

static void test_key1_opens_the_description_and_boot_opens_fsrs(void)
{
    kanji_t k = rich_card();

    kanji_nav_t n = at_question();
    kanji_nav_result_t r = kanji_nav_press(&n, KANJI_BTN_KEY1, &k);
    CHECK_INT(r.action, KANJI_ACT_DRAW_FULL);
    CHECK_INT(n.sheet, KANJI_SHEET_DESCRIPTION);
    CHECK_INT(n.sheet_page, 0);

    n = at_question();
    r = kanji_nav_press(&n, KANJI_BTN_BOOT, &k);
    CHECK_INT(r.action, KANJI_ACT_DRAW_FULL);
    CHECK_INT(n.sheet, KANJI_SHEET_FSRS);
}

/* FSRS is the one sheet that is readable with no card at all — it explains the
 * scheduler, which is exactly what a learner staring at an empty session wants
 * to know. The description and comments sheets have nothing to say. */
static void test_fsrs_opens_without_a_card_but_the_others_do_not(void)
{
    kanji_t k = no_card();

    kanji_nav_t n = at_question();
    CHECK_INT(kanji_nav_press(&n, KANJI_BTN_BOOT, &k).action, KANJI_ACT_DRAW_FULL);
    CHECK_INT(n.sheet, KANJI_SHEET_FSRS);

    n = at_question();
    CHECK_INT(kanji_nav_press(&n, KANJI_BTN_KEY1, &k).action, KANJI_ACT_NONE);
    CHECK_INT(n.sheet, KANJI_SHEET_NONE);
}

/* --- the answer screen: the grade dock ------------------------------------ */

static void test_key0_walks_the_grade_cursor_and_only_repaints_the_dock(void)
{
    kanji_t k = rich_card();
    kanji_nav_t n = at_answer();
    CHECK_INT(n.grade, KANJI_GRADE_GOOD);

    /* The cursor moves 보통 -> 쉬움 -> 다시 -> 어려움 -> 보통: it wraps, so
     * every rating is reachable in at most three presses. */
    const kanji_grade_t want[] = {
        KANJI_GRADE_EASY, KANJI_GRADE_AGAIN, KANJI_GRADE_HARD, KANJI_GRADE_GOOD,
    };
    for (int i = 0; i < 4; i++) {
        kanji_nav_result_t r = kanji_nav_press(&n, KANJI_BTN_KEY0, &k);
        /* A cursor move is the ONLY press on this board that does not flash
         * the whole panel. If this ever becomes DRAW_FULL, choosing a rating
         * costs four full refreshes. */
        CHECK_INT(r.action, KANJI_ACT_DRAW_DOCK);
        CHECK(r.changed);
        CHECK_INT(n.grade, want[i]);
    }
}

static void test_key1_submits_the_cursor_grade(void)
{
    kanji_t k = rich_card();
    kanji_nav_t n = at_answer();
    kanji_nav_press(&n, KANJI_BTN_KEY0, &k);         /* -> 쉬움 */
    CHECK_INT(n.grade, KANJI_GRADE_EASY);

    kanji_nav_result_t r = kanji_nav_press(&n, KANJI_BTN_KEY1, &k);
    CHECK_INT(r.action, KANJI_ACT_SUBMIT);
    /* Submitting does not itself move the nav: the caller resets it when the
     * next card lands, so a failed POST leaves the learner where they were
     * rather than on a blank question screen for a card they already graded. */
    CHECK(!r.changed);
    CHECK_INT(n.grade, KANJI_GRADE_EASY);
    CHECK(n.revealed);
}

static void test_boot_opens_the_description_from_the_answer(void)
{
    kanji_t k = rich_card();
    kanji_nav_t n = at_answer();

    kanji_nav_result_t r = kanji_nav_press(&n, KANJI_BTN_BOOT, &k);
    CHECK_INT(r.action, KANJI_ACT_DRAW_FULL);
    CHECK_INT(n.sheet, KANJI_SHEET_DESCRIPTION);
    CHECK(n.revealed);        /* the answer is still underneath */
}

/* --- the sheets ----------------------------------------------------------- */

static void test_key1_closes_a_sheet_back_to_the_side_it_opened_from(void)
{
    kanji_t k = rich_card();

    /* Opened from the question -> closes to the question. */
    kanji_nav_t n = at_question();
    kanji_nav_press(&n, KANJI_BTN_KEY1, &k);
    kanji_nav_result_t r = kanji_nav_press(&n, KANJI_BTN_KEY1, &k);
    CHECK_INT(r.action, KANJI_ACT_DRAW_FULL);
    CHECK_INT(n.sheet, KANJI_SHEET_NONE);
    CHECK_INT(kanji_nav_screen(&n), KANJI_SCREEN_QUESTION);

    /* Opened from the answer -> closes to the answer, NOT back to the question.
     * Re-hiding an answer the learner already read is the one thing a "close"
     * must never do: they would have to reveal it again to grade it. */
    n = at_answer();
    kanji_nav_press(&n, KANJI_BTN_BOOT, &k);
    r = kanji_nav_press(&n, KANJI_BTN_KEY1, &k);
    CHECK_INT(r.action, KANJI_ACT_DRAW_FULL);
    CHECK_INT(n.sheet, KANJI_SHEET_NONE);
    CHECK_INT(kanji_nav_screen(&n), KANJI_SCREEN_ANSWER);
}

static void test_boot_cycles_the_sheets_and_the_last_one_closes(void)
{
    kanji_t k = rich_card();
    kanji_nav_t n = at_question();

    kanji_nav_press(&n, KANJI_BTN_KEY1, &k);
    CHECK_INT(n.sheet, KANJI_SHEET_DESCRIPTION);
    kanji_nav_press(&n, KANJI_BTN_BOOT, &k);
    CHECK_INT(n.sheet, KANJI_SHEET_COMMENTS);
    kanji_nav_press(&n, KANJI_BTN_BOOT, &k);
    CHECK_INT(n.sheet, KANJI_SHEET_FSRS);
    kanji_nav_press(&n, KANJI_BTN_BOOT, &k);
    CHECK_INT(n.sheet, KANJI_SHEET_NONE);   /* round trip, back to the card */
}

static void test_switching_sheets_rewinds_the_page(void)
{
    kanji_t k = rich_card();
    kanji_nav_t n = at_question();

    kanji_nav_press(&n, KANJI_BTN_KEY1, &k);       /* description */
    n.sheet_page = 1;
    kanji_nav_press(&n, KANJI_BTN_BOOT, &k);       /* -> comments */
    CHECK_INT(n.sheet_page, 0);
}

static void test_key0_pages_within_a_sheet_and_wraps(void)
{
    kanji_t k = rich_card();
    kanji_nav_t n = at_question();
    kanji_nav_press(&n, KANJI_BTN_BOOT, &k);       /* FSRS: the long one */

    const int pages = kanji_sheet_pages(&k, KANJI_SHEET_FSRS);
    CHECK(pages >= 2);                              /* it explains a scheduler */

    for (int i = 1; i < pages; i++) {
        kanji_nav_result_t r = kanji_nav_press(&n, KANJI_BTN_KEY0, &k);
        CHECK_INT(r.action, KANJI_ACT_DRAW_FULL);
        CHECK_INT(n.sheet_page, i);
    }
    kanji_nav_press(&n, KANJI_BTN_KEY0, &k);
    CHECK_INT(n.sheet_page, 0);                     /* wraps */
}

static void test_a_single_page_sheet_does_not_flash_the_panel_to_stay_put(void)
{
    kanji_t k = rich_card();
    k.card.comment_count = 1;
    k.card.comments[0].likes = 3;
    kanji_str_copy(k.card.comments[0].body, KANJI_COMMENT_MAX, "짧은 한 줄");

    kanji_nav_t n = at_question();
    kanji_nav_press(&n, KANJI_BTN_KEY1, &k);
    kanji_nav_press(&n, KANJI_BTN_BOOT, &k);       /* comments */
    CHECK_INT(kanji_sheet_pages(&k, KANJI_SHEET_COMMENTS), 1);

    kanji_nav_result_t r = kanji_nav_press(&n, KANJI_BTN_KEY0, &k);
    CHECK_INT(r.action, KANJI_ACT_NONE);
    CHECK(!r.changed);
}

static void test_sheet_pages_is_never_zero(void)
{
    kanji_t empty = no_card();
    for (int s = 0; s < KANJI_SHEET_COUNT; s++) {
        CHECK(kanji_sheet_pages(&empty, (kanji_sheet_t)s) >= 1);
        CHECK(kanji_sheet_pages(NULL, (kanji_sheet_t)s) >= 1);
    }
}

/* --- paging agrees with what the sheets actually draw ----------------------
 * kanji_sheet_pages() decides how far KEY0 walks; the sheet files decide how
 * much each of those pages holds. Nothing links the two at runtime, so if they
 * disagree the failure is silent and in opposite directions: too few pages and
 * the last comment is unreachable, too many and the pager says 4/4 over a page
 * the sheet clamped back to the first. Both numbers therefore come from
 * kanji_nav.h, and these tests pin the arithmetic that reads them. */

static void test_every_comment_is_reachable_by_paging(void)
{
    for (int n = 0; n <= KANJI_COMMENTS_MAX; n++) {
        kanji_t k = rich_card();
        k.card.comment_count = n;

        const int pages = kanji_sheet_pages(&k, KANJI_SHEET_COMMENTS);
        CHECK(pages >= 1);

        /* The last page reaches the last comment. */
        CHECK(pages * KANJI_COMMENTS_PER_PAGE >= n);

        /* And no page is empty: a page KEY0 walks to that draws nothing is a
         * blank screen the learner asked for on purpose. */
        if (n > 0) {
            CHECK((pages - 1) * KANJI_COMMENTS_PER_PAGE < n);
        }
    }
}

static void test_the_fsrs_sheet_has_one_page_per_page_of_copy(void)
{
    /* The card's own numbers ride along the bottom of every page, so the count
     * is the copy's and never the card's — including with no card at all, which
     * is exactly when a learner wants to read what the scheduler does. */
    kanji_t k = rich_card();
    CHECK_INT(kanji_sheet_pages(&k, KANJI_SHEET_FSRS), KANJI_FSRS_PAGES);

    kanji_t empty = no_card();
    CHECK_INT(kanji_sheet_pages(&empty, KANJI_SHEET_FSRS), KANJI_FSRS_PAGES);
    CHECK_INT(kanji_sheet_pages(NULL, KANJI_SHEET_FSRS), KANJI_FSRS_PAGES);
}

/* --- the companion app uses the same door as the buttons ------------------ */

/* The phone can put the board on any screen — but only the ones a button could
 * also reach. Anything else and the two controls disagree about what the board
 * can show: the app parks it on an empty 설명 sheet, the learner picks it up,
 * and no press they know produces that screen. */
static void test_setting_a_screen_obeys_the_same_rules_as_a_button(void)
{
    kanji_t k = rich_card();
    kanji_nav_t nav;

    /* With a card, every screen is reachable and the nav lands on it. */
    for (int s = 0; s < KANJI_SCREEN_COUNT; s++) {
        kanji_nav_reset(&nav);
        CHECK(kanji_nav_set_screen(&nav, (kanji_screen_t)s, &k));
        CHECK_INT(kanji_nav_screen(&nav), s);
    }

    /* With no card, only the question side and FSRS are. There is no answer to
     * reveal — KEY0 refuses for the same reason — and 설명/댓글 are about a
     * card. FSRS survives because it explains the scheduler, which is exactly
     * what a learner staring at an empty session wants to read. */
    kanji_t empty = no_card();
    const struct { kanji_screen_t screen; bool reachable; } WANT[] = {
        { KANJI_SCREEN_QUESTION,    true  },
        { KANJI_SCREEN_ANSWER,      false },
        { KANJI_SCREEN_DESCRIPTION, false },
        { KANJI_SCREEN_COMMENTS,    false },
        { KANJI_SCREEN_FSRS,        true  },
    };
    for (size_t i = 0; i < sizeof WANT / sizeof WANT[0]; i++) {
        kanji_nav_reset(&nav);
        const kanji_nav_t before = nav;
        const bool moved = kanji_nav_set_screen(&nav, WANT[i].screen, &empty);
        CHECK_INT(moved, WANT[i].reachable);
        if (!moved) {
            /* A refused screen leaves the nav exactly as it was — the board
             * must not half-move and then repaint. */
            CHECK_INT(nav.sheet, before.sheet);
            CHECK_INT(nav.revealed, before.revealed);
            CHECK_INT(nav.sheet_page, before.sheet_page);
        }
    }

    /* A card with no shape story, no hook and no parts has nothing to put on
     * 설명 — the same test open_sheet() applies to BOOT. */
    kanji_t bare = rich_card();
    bare.card.description[0] = '\0';
    bare.card.hook_body[0] = '\0';
    bare.card.part_count = 0;
    kanji_nav_reset(&nav);
    CHECK(!kanji_nav_set_screen(&nav, KANJI_SCREEN_DESCRIPTION, &bare));
    CHECK_INT(kanji_nav_screen(&nav), KANJI_SCREEN_QUESTION);

    /* Out of range is refused rather than clamped: a companion app sending 9
     * has a bug, and silently showing it screen 0 hides that. */
    kanji_nav_reset(&nav);
    CHECK(!kanji_nav_set_screen(&nav, (kanji_screen_t)-1, &k));
    CHECK(!kanji_nav_set_screen(&nav, (kanji_screen_t)KANJI_SCREEN_COUNT, &k));
    CHECK(!kanji_nav_set_screen(NULL, KANJI_SCREEN_FSRS, &k));
}

/* Whatever the app leaves on the glass, one press gets back to the card.
 *
 * A sheet the phone opened has to close the same way a sheet BOOT opened does —
 * a learner who picks the board up has no idea which of the two put it there. */
static void test_no_screen_the_app_can_set_is_a_dead_end(void)
{
    kanji_t cards[2] = { rich_card(), no_card() };
    for (int c = 0; c < 2; c++) {
        for (int s = 0; s < KANJI_SCREEN_COUNT; s++) {
            kanji_nav_t nav;
            kanji_nav_reset(&nav);
            if (!kanji_nav_set_screen(&nav, (kanji_screen_t)s, &cards[c])) {
                continue;
            }
            /* A card side is already home; only a sheet needs closing. */
            if (nav.sheet == KANJI_SHEET_NONE) {
                const kanji_screen_t now = kanji_nav_screen(&nav);
                CHECK(now == KANJI_SCREEN_QUESTION || now == KANJI_SCREEN_ANSWER);
                continue;
            }
            kanji_nav_press(&nav, KANJI_BTN_KEY1, &cards[c]);
            const kanji_screen_t after = kanji_nav_screen(&nav);
            CHECK(after == KANJI_SCREEN_QUESTION || after == KANJI_SCREEN_ANSWER);
        }
    }
}

/* --- KEY2, from anywhere -------------------------------------------------- */

static void test_key2_always_refreshes_and_never_moves_the_nav(void)
{
    kanji_t k = rich_card();
    kanji_nav_t states[3] = { at_question(), at_answer(), at_question() };
    kanji_nav_press(&states[2], KANJI_BTN_KEY1, &k);   /* a sheet */

    for (int i = 0; i < 3; i++) {
        const kanji_nav_t before = states[i];
        kanji_nav_result_t r = kanji_nav_press(&states[i], KANJI_BTN_KEY2, &k);
        CHECK_INT(r.action, KANJI_ACT_REFRESH);
        CHECK(!r.changed);
        CHECK_INT(states[i].revealed, before.revealed);
        CHECK_INT(states[i].sheet, before.sheet);
        CHECK_INT(states[i].grade, before.grade);
    }
}

/* --- the footer legend ---------------------------------------------------- */

static void test_the_legend_says_what_the_buttons_currently_do(void)
{
    kanji_t k = rich_card();

    kanji_nav_t n = at_question();
    CHECK_STR(kanji_nav_hint_key0(&n), "정답");
    CHECK_STR(kanji_nav_hint_key1(&n), "설명");
    CHECK_STR(kanji_nav_hint_boot(&n), "FSRS");

    n = at_answer();
    CHECK_STR(kanji_nav_hint_key0(&n), "등급");
    CHECK_STR(kanji_nav_hint_key1(&n), "확정");
    CHECK_STR(kanji_nav_hint_boot(&n), "설명");

    kanji_nav_press(&n, KANJI_BTN_BOOT, &k);
    CHECK_STR(kanji_nav_hint_key0(&n), "다음 쪽");
    CHECK_STR(kanji_nav_hint_key1(&n), "닫기");
    CHECK_STR(kanji_nav_hint_boot(&n), "다음 탭");

    /* Never NULL, even for a state nothing produced. */
    CHECK(kanji_nav_hint_key0(NULL) != NULL);
    CHECK(kanji_nav_hint_key1(NULL) != NULL);
    CHECK(kanji_nav_hint_boot(NULL) != NULL);
}

static void test_every_screen_has_a_title(void)
{
    for (int s = 0; s < KANJI_SCREEN_COUNT; s++) {
        const char *t = kanji_screen_title((kanji_screen_t)s);
        CHECK(t != NULL);
        CHECK(t[0] != '\0');
    }
    CHECK(kanji_screen_title((kanji_screen_t)99) != NULL);
}

/* --- the whole surface ---------------------------------------------------- */

/* Nothing may wedge the board: from any reachable state, any button leaves a
 * state that is still reachable and still has a screen. This is the test that
 * catches an enum added without a case. */
static void test_no_button_from_no_state_produces_an_impossible_state(void)
{
    kanji_t k = rich_card();

    for (int rev = 0; rev < 2; rev++) {
        for (int sh = 0; sh < KANJI_SHEET_COUNT; sh++) {
            for (int pg = 0; pg < 3; pg++) {
                for (int g = KANJI_GRADE_AGAIN; g <= KANJI_GRADE_EASY; g++) {
                    for (int b = 0; b < KANJI_BTN_COUNT; b++) {
                        kanji_nav_t n = {
                            .revealed = rev != 0,
                            .sheet = (kanji_sheet_t)sh,
                            .sheet_page = pg,
                            .grade = (kanji_grade_t)g,
                        };
                        kanji_nav_press(&n, (kanji_button_t)b, &k);

                        CHECK(n.sheet >= KANJI_SHEET_NONE && n.sheet < KANJI_SHEET_COUNT);
                        CHECK(n.grade >= KANJI_GRADE_AGAIN && n.grade <= KANJI_GRADE_EASY);
                        CHECK(n.sheet_page >= 0);
                        CHECK(n.sheet_page < kanji_sheet_pages(&k, n.sheet));
                        const kanji_screen_t s = kanji_nav_screen(&n);
                        CHECK(s >= KANJI_SCREEN_QUESTION && s < KANJI_SCREEN_COUNT);
                    }
                }
            }
        }
    }
}

int main(void)
{
    test_reset_is_question_side_up_with_the_cursor_on_good();
    test_screen_is_derived_and_the_sheet_wins_over_the_card();
    test_key0_reveals_the_answer();
    test_key0_does_nothing_when_there_is_no_card_to_reveal();
    test_key1_opens_the_description_and_boot_opens_fsrs();
    test_fsrs_opens_without_a_card_but_the_others_do_not();
    test_key0_walks_the_grade_cursor_and_only_repaints_the_dock();
    test_key1_submits_the_cursor_grade();
    test_boot_opens_the_description_from_the_answer();
    test_key1_closes_a_sheet_back_to_the_side_it_opened_from();
    test_boot_cycles_the_sheets_and_the_last_one_closes();
    test_switching_sheets_rewinds_the_page();
    test_key0_pages_within_a_sheet_and_wraps();
    test_a_single_page_sheet_does_not_flash_the_panel_to_stay_put();
    test_sheet_pages_is_never_zero();
    test_every_comment_is_reachable_by_paging();
    test_the_fsrs_sheet_has_one_page_per_page_of_copy();
    test_setting_a_screen_obeys_the_same_rules_as_a_button();
    test_no_screen_the_app_can_set_is_a_dead_end();
    test_key2_always_refreshes_and_never_moves_the_nav();
    test_the_legend_says_what_the_buttons_currently_do();
    test_every_screen_has_a_title();
    test_no_button_from_no_state_produces_an_impossible_state();
    TH_REPORT("kanji_nav");
}
