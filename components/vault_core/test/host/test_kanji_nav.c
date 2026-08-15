/*
 * The button state machine, exhaustively.
 *
 * The whole of the board's interaction state is two booleans and a rating:
 * `revealed` says which of the two faces is on the glass, `committed` says a
 * grade has been handed to the network task, `grade` says which one. Sixteen
 * combinations, four buttons, a handful of card conditions — small enough to
 * ENUMERATE rather than sample, so that is what this file does. Every legal
 * kanji_nav_t is crossed with every button and every card condition, and each
 * cell is checked against an oracle written out separately below.
 *
 * The machine this replaced had five screens, three paged sheets and a grade
 * cursor, and its test could only afford to walk a spanning tree of that state
 * space. Enumerating is strictly stronger: it is the difference between "no
 * path I thought of wedges the board" and "no state exists that a button can
 * wedge". It is also the only way to prove the two assertions this device
 * actually depends on — that a rating is never recorded twice for one card,
 * and that the footer legend never advertises a control that does nothing.
 */
#include "th.h"

#include "kanji_nav.h"
#include "ui_strings.h"

/* --- fixtures -------------------------------------------------------------
 *
 * The machine reads nothing from a snapshot but `valid` and `card.valid`. The
 * fixtures carry real content anyway, so that an edit which starts sniffing
 * the card's fields — "grade only when there are senses to have read" — has
 * something to be caught sniffing rather than passing on empty strings. */

static kanji_t rich_card(void)
{
    kanji_t k = {0};
    k.valid = true;
    k.card.valid = true;
    kanji_str_copy(k.card.id, KANJI_ID_MAX, "card-0001");
    kanji_str_copy(k.card.front, KANJI_FRONT_MAX, "会う");
    kanji_str_copy(k.card.reading, KANJI_READING_MAX, "あう");
    kanji_str_copy(k.card.senses[0], KANJI_SENSE_MAX, "만나다");
    k.card.sense_count = 1;
    kanji_str_copy(k.card.description, KANJI_BODY_MAX,
                   "会는 사람들이 모여 서로 말하고 교류하는 모습입니다.");
    k.card.part_count = 2;
    return k;
}

/* The completion screen. The session answered and had nothing left to serve —
 * a real state a learner reaches every day, not an error. */
static kanji_t session_complete(void)
{
    kanji_t k = {0};
    k.valid = true;
    k.session.complete = true;
    return k;
}

/* A snapshot that was never published, carrying a card struct anyway. The two
 * validity flags are an AND, and a half-written restore is exactly the shape
 * that sets one without the other; checking only `card.valid` would put the
 * board on an answer face built from a struct nobody filled in. */
static kanji_t unpublished(void)
{
    kanji_t k = rich_card();
    k.valid = false;
    return k;
}

/* --- the oracle -----------------------------------------------------------
 *
 * A second, independent statement of the mapping, transcribed from the design
 * record rather than read out of kanji_button_grade(). That is the entire
 * point: an edit to the table in kanji_nav.c has to be made here too, so an
 * off-by-one is a test failure instead of every card in the deck being graded
 * one rating too hard for as long as nobody notices. */
static const kanji_grade_t ORACLE_GRADE[KANJI_BTN_COUNT] = {
    [KANJI_BTN_KEY0] = KANJI_GRADE_AGAIN,
    [KANJI_BTN_KEY1] = KANJI_GRADE_HARD,
    [KANJI_BTN_KEY2] = KANJI_GRADE_GOOD,
    [KANJI_BTN_BOOT] = KANJI_GRADE_EASY,
};

static const char *const BTN_NAME[KANJI_BTN_COUNT] = {
    "KEY0", "KEY1", "KEY2", "BOOT",
};

typedef struct {
    kanji_action_t action;
    kanji_nav_t    after;
} expect_t;

/* What a press must do, from the design record's two rows:
 *
 *   문제  KEY0/KEY1/BOOT = 뜻 보기          KEY2 = 새로고침
 *   정답  KEY0/KEY1/KEY2/BOOT = the rating  (KEY2 falls back to 새로고침 once
 *                                            a rating is already in flight)
 *
 * `card` is whether the snapshot holds a card at all; the machine may look at
 * nothing else about it. */
static expect_t oracle(kanji_nav_t nav, kanji_button_t btn, bool card)
{
    expect_t e = { KANJI_ACT_NONE, nav };

    if (!nav.revealed) {
        /* The front. KEY2 re-polls whether or not there is a card — a learner
         * looking at the completion screen is precisely the one who wants to
         * ask again. Everything else turns the card over, and there has to be
         * one: revealing nothing shows an empty answer face. */
        if (btn == KANJI_BTN_KEY2) e.action = KANJI_ACT_REFRESH;
        else if (card) { e.after.revealed = true; e.action = KANJI_ACT_DRAW_FULL; }
        return e;
    }

    if (nav.committed) {
        /* A rating is already in flight. Every button that grades is refused —
         * a different rating on the same card is not a retry, it is a
         * mis-grade — and KEY2 reverts to the re-poll so that a learner whose
         * laptop was asleep is not stranded on a card they have already
         * answered. */
        if (btn == KANJI_BTN_KEY2) e.action = KANJI_ACT_REFRESH;
        return e;
    }

    if (!card) return e;
    e.after.grade     = ORACLE_GRADE[btn];
    e.after.committed = true;
    e.action = KANJI_ACT_SUBMIT;
    return e;
}

static const char *act_name(kanji_action_t a)
{
    switch (a) {
    case KANJI_ACT_NONE:      return "NONE";
    case KANJI_ACT_DRAW_FULL: return "DRAW_FULL";
    case KANJI_ACT_DRAW_DOCK: return "DRAW_DOCK";
    case KANJI_ACT_SUBMIT:    return "SUBMIT";
    case KANJI_ACT_REFRESH:   return "REFRESH";
    }
    return "?";
}

static bool same_nav(const kanji_nav_t *a, const kanji_nav_t *b)
{
    return a->revealed == b->revealed && a->committed == b->committed &&
           a->grade == b->grade;
}

/* CHECK prints a file and a line, which inside a sweep of two hundred cells
 * names the assertion but not the cell that broke it — and the cell IS the bug
 * report. Print it whenever a cell's checks added a failure. */
static void cell_report(int mark, const kanji_nav_t *start, kanji_button_t btn,
                        const char *card, kanji_action_t want, kanji_action_t got)
{
    if (g_fail == mark) return;
    printf("    ^ cell: revealed=%d committed=%d grade=%d  %s  card=%s"
           "  want %s got %s\n",
           (int)start->revealed, (int)start->committed, (int)start->grade,
           BTN_NAME[btn], card, act_name(want), act_name(got));
}

/* --- the starting state --------------------------------------------------- */

static void test_reset_is_the_question_face_with_nothing_in_flight(void)
{
    kanji_nav_t n;
    kanji_nav_reset(&n);
    CHECK(!n.revealed);
    CHECK(!n.committed);
    /* GOOD, not zero: `grade` is read whenever `committed` is set, and a nav
     * restored from a reset that left it 0 would submit a rating py-fsrs has
     * no name for. */
    CHECK_INT(n.grade, KANJI_GRADE_GOOD);
    CHECK_INT(kanji_nav_screen(&n), KANJI_SCREEN_QUESTION);

    kanji_nav_reset(NULL);   /* must not crash */
}

static void test_the_screen_is_derived_from_revealed_alone(void)
{
    kanji_nav_t n;
    kanji_nav_reset(&n);
    CHECK_INT(kanji_nav_screen(&n), KANJI_SCREEN_QUESTION);

    n.revealed = true;
    CHECK_INT(kanji_nav_screen(&n), KANJI_SCREEN_ANSWER);

    /* Whether a grade is in flight changes what the dock draws, never which
     * face is on the glass: the answer the learner just graded stays up until
     * the next card actually lands. */
    for (int com = 0; com < 2; com++) {
        for (int g = KANJI_GRADE_AGAIN; g <= KANJI_GRADE_EASY; g++) {
            n.committed = com != 0;
            n.grade     = (kanji_grade_t)g;
            n.revealed  = false;
            CHECK_INT(kanji_nav_screen(&n), KANJI_SCREEN_QUESTION);
            n.revealed  = true;
            CHECK_INT(kanji_nav_screen(&n), KANJI_SCREEN_ANSWER);
        }
    }

    CHECK_INT(kanji_nav_screen(NULL), KANJI_SCREEN_QUESTION);
}

static void test_every_screen_has_a_title(void)
{
    for (int s = 0; s < KANJI_SCREEN_COUNT; s++) {
        const char *t = kanji_screen_title((kanji_screen_t)s);
        CHECK(t != NULL);
        CHECK(t != NULL && t[0] != '\0');
    }
    CHECK_STR(kanji_screen_title(KANJI_SCREEN_QUESTION), S_SCREEN_QUESTION);
    CHECK_STR(kanji_screen_title(KANJI_SCREEN_ANSWER),   S_SCREEN_ANSWER);

    /* The two titles are what the header prints; sharing one would make the
     * masthead unable to say which face is up. */
    CHECK(strcmp(kanji_screen_title(KANJI_SCREEN_QUESTION),
                 kanji_screen_title(KANJI_SCREEN_ANSWER)) != 0);

    CHECK(kanji_screen_title((kanji_screen_t)99) != NULL);
    CHECK(kanji_screen_title((kanji_screen_t)-1) != NULL);
}

/* --- the four buttons are the four grades --------------------------------- */

/* The single most consequential table on the board. Every other mistake here
 * shows on the glass; this one does not — a board that grades 다시 when the
 * learner pressed 쉬움 looks exactly like a board that works, and the damage
 * lands in a review history the panel never displays. */
static void test_the_four_buttons_are_the_four_grades(void)
{
    CHECK_INT(kanji_button_grade(KANJI_BTN_KEY0), KANJI_GRADE_AGAIN);
    CHECK_INT(kanji_button_grade(KANJI_BTN_KEY1), KANJI_GRADE_HARD);
    CHECK_INT(kanji_button_grade(KANJI_BTN_KEY2), KANJI_GRADE_GOOD);
    CHECK_INT(kanji_button_grade(KANJI_BTN_BOOT), KANJI_GRADE_EASY);

    /* Left to right across the physical row, that is Anki's canonical order.
     * Asserting the order as well as the values catches a table rewritten with
     * the right four grades in the wrong four places. */
    CHECK(kanji_button_grade(KANJI_BTN_KEY0) < kanji_button_grade(KANJI_BTN_KEY1));
    CHECK(kanji_button_grade(KANJI_BTN_KEY1) < kanji_button_grade(KANJI_BTN_KEY2));
    CHECK(kanji_button_grade(KANJI_BTN_KEY2) < kanji_button_grade(KANJI_BTN_BOOT));

    /* Distinct, and between them they cover all four grades: a duplicate would
     * leave one rating unreachable from any button on the board. */
    int seen[KANJI_GRADE_COUNT] = {0};
    for (int b = 0; b < KANJI_BTN_COUNT; b++) {
        const kanji_grade_t g = kanji_button_grade((kanji_button_t)b);
        CHECK(g >= KANJI_GRADE_AGAIN && g <= KANJI_GRADE_EASY);
        if (g >= KANJI_GRADE_AGAIN && g <= KANJI_GRADE_EASY) seen[g - 1]++;
    }
    for (int i = 0; i < KANJI_GRADE_COUNT; i++) CHECK_INT(seen[i], 1);

    /* A button that is not a button grades nothing rather than grading AGAIN,
     * which is what a zero-initialised `switch` default would do. */
    CHECK_INT(kanji_button_grade((kanji_button_t)KANJI_BTN_COUNT), 0);
    CHECK_INT(kanji_button_grade((kanji_button_t)-1), 0);
}

/* --- the front ------------------------------------------------------------ */

static void test_a_reveal_costs_one_press_and_pre_arms_nothing(void)
{
    const kanji_t k = rich_card();

    for (int b = 0; b < KANJI_BTN_COUNT; b++) {
        if (b == KANJI_BTN_KEY2) continue;      /* KEY2 re-polls, see below */
        kanji_nav_t n;
        kanji_nav_reset(&n);
        const kanji_nav_result_t r = kanji_nav_press(&n, (kanji_button_t)b, &k);
        CHECK_INT(r.action, KANJI_ACT_DRAW_FULL);
        CHECK(r.changed);
        CHECK(n.revealed);
        CHECK_INT(kanji_nav_screen(&n), KANJI_SCREEN_ANSWER);
        /* Turning the card over must not arm a rating. If it did, the button
         * the learner used to reveal would be the one whose grade is already
         * loaded, and a second press of anything would look like a choice. */
        CHECK(!n.committed);
        CHECK_INT(n.grade, KANJI_GRADE_GOOD);
    }
}

static void test_reveal_is_refused_when_there_is_no_card(void)
{
    const kanji_t empty = session_complete();
    const kanji_t half  = unpublished();
    const kanji_t *const NO_CARD[] = { NULL, &empty, &half };

    for (size_t c = 0; c < sizeof NO_CARD / sizeof NO_CARD[0]; c++) {
        for (int b = 0; b < KANJI_BTN_COUNT; b++) {
            if (b == KANJI_BTN_KEY2) continue;
            kanji_nav_t n;
            kanji_nav_reset(&n);
            const kanji_nav_result_t r =
                kanji_nav_press(&n, (kanji_button_t)b, NO_CARD[c]);
            /* An empty answer face is worse than the completion screen the
             * learner is already looking at: it says the board lost the card
             * rather than that the session is done. */
            CHECK_INT(r.action, KANJI_ACT_NONE);
            CHECK(!r.changed);
            CHECK(!n.revealed);
            CHECK(!kanji_nav_can_press(&n, (kanji_button_t)b, NO_CARD[c]));
        }
    }
}

/* The claim the whole redesign rests on: a card is two presses. The machine it
 * replaced spent one press to reveal, up to three to walk a cursor and one to
 * commit — five presses and four full panel refreshes for one rating. */
static void test_one_press_reveals_and_one_press_grades(void)
{
    const kanji_t k = rich_card();

    for (int b = 0; b < KANJI_BTN_COUNT; b++) {
        kanji_nav_t n;
        kanji_nav_reset(&n);

        CHECK_INT(kanji_nav_press(&n, KANJI_BTN_KEY0, &k).action,
                  KANJI_ACT_DRAW_FULL);

        const kanji_nav_result_t r = kanji_nav_press(&n, (kanji_button_t)b, &k);
        CHECK_INT(r.action, KANJI_ACT_SUBMIT);
        CHECK(r.changed);
        CHECK(n.committed);
        CHECK_INT(n.grade, ORACLE_GRADE[b]);
        /* Still the answer face. The card the learner just graded stays on the
         * glass until the next one arrives — drawing anything sooner would be
         * drawing a guess at what the proxy will say. */
        CHECK(n.revealed);
        CHECK_INT(kanji_nav_screen(&n), KANJI_SCREEN_ANSWER);
    }
}

/* --- KEY2 ----------------------------------------------------------------- */

static void test_key2_refreshes_from_the_front_and_from_a_committed_back(void)
{
    const kanji_t k     = rich_card();
    const kanji_t empty = session_complete();

    /* The front, with a card and without one. */
    const kanji_t *const FRONTS[] = { &k, &empty, NULL };
    for (size_t i = 0; i < sizeof FRONTS / sizeof FRONTS[0]; i++) {
        kanji_nav_t n;
        kanji_nav_reset(&n);
        const kanji_nav_t before = n;
        const kanji_nav_result_t r = kanji_nav_press(&n, KANJI_BTN_KEY2, FRONTS[i]);
        CHECK_INT(r.action, KANJI_ACT_REFRESH);
        CHECK(!r.changed);
        CHECK(same_nav(&n, &before));
    }

    /* The back, with a rating already in flight. This is the escape hatch: the
     * learner graded, the laptop was asleep, and the only way back to a live
     * card is to ask again. Without it the board is wedged on an answer whose
     * grade will never be acknowledged. */
    kanji_nav_t n;
    kanji_nav_reset(&n);
    kanji_nav_press(&n, KANJI_BTN_KEY0, &k);      /* reveal */
    kanji_nav_press(&n, KANJI_BTN_KEY1, &k);      /* grade 어려움 */
    CHECK(n.committed);
    const kanji_nav_t committed = n;

    const kanji_nav_result_t r = kanji_nav_press(&n, KANJI_BTN_KEY2, &k);
    CHECK_INT(r.action, KANJI_ACT_REFRESH);
    CHECK(!r.changed);
    /* And it does NOT withdraw the rating: the grade is already on its way to
     * the network task, so clearing `committed` here would re-arm four buttons
     * against a card that has been answered. */
    CHECK(same_nav(&n, &committed));
    CHECK_INT(n.grade, KANJI_GRADE_HARD);

    /* On the back with no rating in flight, KEY2 is 보통 — the refresh is not
     * available there, because a button that grades cannot also re-poll. */
    kanji_nav_t fresh;
    kanji_nav_reset(&fresh);
    kanji_nav_press(&fresh, KANJI_BTN_KEY0, &k);
    CHECK_INT(kanji_nav_press(&fresh, KANJI_BTN_KEY2, &k).action, KANJI_ACT_SUBMIT);
    CHECK_INT(fresh.grade, KANJI_GRADE_GOOD);
}

/* --- one card, one rating ------------------------------------------------- */

/* The assertion that stops a card being graded twice.
 *
 * The proxy answers a repeat of the id it just graded with the same payload,
 * so a second press on the same answer face is not a retry — it is a different
 * rating recorded against a card the learner has already answered, in a review
 * history nothing on this board would ever show them. */
static void test_a_second_grade_is_refused(void)
{
    const kanji_t k     = rich_card();
    const kanji_t empty = session_complete();
    const kanji_t *const CARDS[] = { &k, &empty, NULL };

    for (int first = 0; first < KANJI_BTN_COUNT; first++) {
        kanji_nav_t n;
        kanji_nav_reset(&n);
        kanji_nav_press(&n, KANJI_BTN_KEY0, &k);
        kanji_nav_press(&n, (kanji_button_t)first, &k);
        CHECK(n.committed);
        const kanji_nav_t committed = n;

        for (size_t c = 0; c < sizeof CARDS / sizeof CARDS[0]; c++) {
            for (int b = 0; b < KANJI_BTN_COUNT; b++) {
                kanji_nav_t again = committed;
                const kanji_nav_result_t r =
                    kanji_nav_press(&again, (kanji_button_t)b, CARDS[c]);

                /* Whatever else happens, no second rating is recorded and the
                 * committed one is not overwritten. */
                CHECK(r.action != KANJI_ACT_SUBMIT);
                CHECK(!r.changed);
                CHECK(same_nav(&again, &committed));

                /* KEY2 keeps the re-poll; the three grading buttons are inert,
                 * and can_press() reports them inert so the footer greys them
                 * rather than advertising a control that does nothing. */
                if (b == KANJI_BTN_KEY2) {
                    CHECK_INT(r.action, KANJI_ACT_REFRESH);
                    CHECK(kanji_nav_can_press(&committed, (kanji_button_t)b, CARDS[c]));
                } else {
                    CHECK_INT(r.action, KANJI_ACT_NONE);
                    CHECK(!kanji_nav_can_press(&committed, (kanji_button_t)b, CARDS[c]));
                }
            }
        }
    }
}

/* --- clamping ------------------------------------------------------------- */

/* Nothing on the board writes an impossible nav, but the struct is restored
 * from a state record and handed in by a companion app, and a press that
 * OBSERVES an impossible state decides from it. Clamp runs on the way in as
 * well as on the way out, so no press can ever read one. */
static void test_clamp_repairs_an_impossible_restore(void)
{
    const kanji_t k = rich_card();

    /* An out-of-range grade is repaired to GOOD — the default FSRS and Anki
     * both start from — rather than to AGAIN, which would quietly bury a card
     * whose state record was merely torn. */
    static const int BAD_GRADE[] = { 0, -1, 5, 99, KANJI_GRADE_COUNT + 1 };
    for (size_t i = 0; i < sizeof BAD_GRADE / sizeof BAD_GRADE[0]; i++) {
        kanji_nav_t n = { true, true, (kanji_grade_t)BAD_GRADE[i] };
        const kanji_nav_result_t r = kanji_nav_press(&n, KANJI_BTN_KEY2, &k);
        CHECK_INT(r.action, KANJI_ACT_REFRESH);
        CHECK_INT(n.grade, KANJI_GRADE_GOOD);
        CHECK(n.revealed);
        CHECK(n.committed);
    }

    /* A grade cannot be in flight on the front: the front has no dock, so
     * `committed` there would hide the reveal behind a rating nobody chose. */
    for (int b = 0; b < KANJI_BTN_COUNT; b++) {
        kanji_nav_t n = { false, true, KANJI_GRADE_EASY };
        kanji_nav_press(&n, (kanji_button_t)b, &k);
        CHECK(!n.committed || n.revealed);
        if (b == KANJI_BTN_KEY2) {
            /* The repair happens on the way in, so `before` is already the
             * repaired state and the press reports no change. Nothing on the
             * front draws `committed`, so there is nothing to repaint — but
             * the flag is gone, which is what the next press needs. */
            CHECK(!n.committed);
        }
    }

    /* A press through the front reveal repairs the flag and reveals in one
     * step, leaving a state the answer face can actually be graded from. */
    kanji_nav_t n = { false, true, (kanji_grade_t)77 };
    const kanji_nav_result_t r = kanji_nav_press(&n, KANJI_BTN_KEY0, &k);
    CHECK_INT(r.action, KANJI_ACT_DRAW_FULL);
    CHECK(n.revealed);
    CHECK(!n.committed);
    CHECK_INT(n.grade, KANJI_GRADE_GOOD);

    /* A NULL nav and a button that is not a button are refused rather than
     * dereferenced or indexed. */
    CHECK_INT(kanji_nav_press(NULL, KANJI_BTN_KEY0, &k).action, KANJI_ACT_NONE);
    CHECK(!kanji_nav_can_press(NULL, KANJI_BTN_KEY0, &k));
    kanji_nav_reset(&n);
    const kanji_nav_t before = n;
    CHECK_INT(kanji_nav_press(&n, (kanji_button_t)KANJI_BTN_COUNT, &k).action,
              KANJI_ACT_NONE);
    CHECK_INT(kanji_nav_press(&n, (kanji_button_t)-1, &k).action, KANJI_ACT_NONE);
    CHECK(same_nav(&n, &before));
    CHECK(!kanji_nav_can_press(&n, (kanji_button_t)KANJI_BTN_COUNT, &k));
    CHECK(!kanji_nav_can_press(&n, (kanji_button_t)-1, &k));
}

/* --- the complete oracle -------------------------------------------------- */

/* Every legal state x every button x every card condition. Twelve states —
 * sixteen combinations less the four clamp() forbids — four buttons and four
 * card conditions is 192 cells, and each is checked for the action it returns,
 * the state it leaves field by field, whether it reports having changed
 * anything, whether the state it left is legal, and whether can_press() agrees
 * without touching the caller's copy.
 *
 * This is the oracle the older test could only approximate by walking. */
static void test_every_button_from_every_state_against_the_full_oracle(void)
{
    const kanji_t rich  = rich_card();
    const kanji_t empty = session_complete();
    const kanji_t half  = unpublished();

    const struct { const char *what; const kanji_t *k; bool card; } CARDS[] = {
        { "none",        NULL,   false },   /* no snapshot has ever landed   */
        { "complete",    &empty, false },   /* the session ran dry           */
        { "unpublished", &half,  false },   /* half-restored: !valid         */
        { "card",        &rich,  true  },   /* a card to read and to grade   */
    };

    int legal_states = 0;
    for (int rev = 0; rev < 2; rev++) {
        for (int com = 0; com < 2; com++) {
            for (int g = KANJI_GRADE_AGAIN; g <= KANJI_GRADE_EASY; g++) {
                const kanji_nav_t start = {
                    .revealed  = rev != 0,
                    .committed = com != 0,
                    .grade     = (kanji_grade_t)g,
                };
                /* clamp() forbids a rating in flight while the answer is not
                 * showing, so those four combinations are not states at all
                 * and the oracle would be describing fiction. */
                if (!start.revealed && start.committed) continue;
                legal_states++;

                for (size_t c = 0; c < sizeof CARDS / sizeof CARDS[0]; c++) {
                    for (int b = 0; b < KANJI_BTN_COUNT; b++) {
                        const kanji_button_t btn = (kanji_button_t)b;
                        const expect_t want = oracle(start, btn, CARDS[c].card);
                        const int mark = g_fail;

                        kanji_nav_t n = start;
                        const kanji_nav_result_t r =
                            kanji_nav_press(&n, btn, CARDS[c].k);

                        CHECK_INT(r.action, want.action);
                        CHECK_INT(n.revealed,  want.after.revealed);
                        CHECK_INT(n.committed, want.after.committed);
                        CHECK_INT(n.grade,     want.after.grade);

                        /* `changed` is what stops the poller flashing a panel
                         * that shows the same thing, so it is checked against
                         * the fields and never against the action. */
                        const bool moved = start.revealed  != n.revealed
                                        || start.committed != n.committed
                                        || start.grade     != n.grade;
                        CHECK_INT(r.changed, moved);

                        /* The two actions that move the machine are the two
                         * that draw. A press that reports a change without one
                         * leaves the glass disagreeing with the state. */
                        CHECK_INT(r.changed, r.action == KANJI_ACT_DRAW_FULL ||
                                             r.action == KANJI_ACT_SUBMIT);

                        /* There is no cursor left to walk, so nothing produces
                         * a dock-only repaint any more; the dock is redrawn on
                         * the back of the SUBMIT that filled it. */
                        CHECK(r.action != KANJI_ACT_DRAW_DOCK);

                        /* The state left behind is legal, whatever went in. */
                        CHECK(n.grade >= KANJI_GRADE_AGAIN &&
                              n.grade <= KANJI_GRADE_EASY);
                        CHECK(!n.committed || n.revealed);
                        CHECK_INT(kanji_nav_screen(&n),
                                  n.revealed ? KANJI_SCREEN_ANSWER
                                             : KANJI_SCREEN_QUESTION);

                        /* The partial-refresh gate is exactly the commit step
                         * and never anything else — tied to the machine here
                         * rather than restated, so the two cannot drift. */
                        CHECK_INT(kanji_nav_is_dock_only_transition(&start, &n),
                                  r.action == KANJI_ACT_SUBMIT);

                        /* Availability has no state machine of its own: it is
                         * the press, tried on a copy, and it must leave the
                         * caller's nav exactly as it found it. */
                        kanji_nav_t probe = start;
                        CHECK_INT(kanji_nav_can_press(&probe, btn, CARDS[c].k),
                                  want.action != KANJI_ACT_NONE);
                        CHECK(same_nav(&probe, &start));

                        /* The legend for this cell exists and says something. */
                        const char *hints[KANJI_BTN_COUNT] = {
                            kanji_nav_hint_key0(&start),
                            kanji_nav_hint_key1(&start),
                            kanji_nav_hint_key2(&start),
                            kanji_nav_hint_boot(&start),
                        };
                        CHECK(hints[b] != NULL);
                        CHECK(hints[b] != NULL && hints[b][0] != '\0');

                        cell_report(mark, &start, btn, CARDS[c].what,
                                    want.action, r.action);
                    }
                }
            }
        }
    }

    /* Sixteen combinations, four of them impossible. If this number moves, a
     * field was added to kanji_nav_t and the sweep above is no longer the
     * whole state space it claims to be. */
    CHECK_INT(legal_states, 12);
}

/* --- the partial-refresh gate --------------------------------------------- */

/* `kanji_nav_is_dock_only_transition` decides whether the panel is repainted
 * whole or through one window. Say yes too often and the board shows a stale
 * answer under a fresh dock; say no too often and every rating costs a
 * multi-second full flash. Both are silent, so the predicate is enumerated
 * over every pair of states rather than spot-checked. */
static void test_only_the_commit_step_is_a_dock_only_transition(void)
{
    kanji_nav_t all[16];
    size_t count = 0;
    for (int rev = 0; rev < 2; rev++) {
        for (int com = 0; com < 2; com++) {
            for (int g = KANJI_GRADE_AGAIN; g <= KANJI_GRADE_EASY; g++) {
                all[count].revealed  = rev != 0;
                all[count].committed = com != 0;
                all[count].grade     = (kanji_grade_t)g;
                count++;
            }
        }
    }
    CHECK_INT(count, 16);

    for (size_t a = 0; a < count; a++) {
        for (size_t b = 0; b < count; b++) {
            /* The one step in the machine that leaves everything on the glass
             * where it was and fills one dock cell. */
            const bool want = all[a].revealed && all[b].revealed
                           && !all[a].committed && all[b].committed;
            CHECK_INT(kanji_nav_is_dock_only_transition(&all[a], &all[b]), want);
        }
    }

    /* A grade outside the four is not a dock cell, so it is not a dock-only
     * repaint either — the window would be refreshed over a cell nothing drew. */
    kanji_nav_t before = { true, false, KANJI_GRADE_GOOD };
    static const int BAD[] = { 0, -1, 5, KANJI_GRADE_COUNT + 1 };
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        kanji_nav_t after = { true, true, (kanji_grade_t)BAD[i] };
        CHECK(!kanji_nav_is_dock_only_transition(&before, &after));
    }

    /* A missing state is not a transition. The caller holds the previous nav
     * in a variable that is NULL before the first frame. */
    kanji_nav_t after = { true, true, KANJI_GRADE_EASY };
    CHECK(!kanji_nav_is_dock_only_transition(NULL, &after));
    CHECK(!kanji_nav_is_dock_only_transition(&before, NULL));
    CHECK(!kanji_nav_is_dock_only_transition(NULL, NULL));
}

/* --- the companion app uses the same door as the buttons ------------------ */

static void test_setting_a_screen_obeys_the_same_rules_as_a_button(void)
{
    const kanji_t k     = rich_card();
    const kanji_t empty = session_complete();
    const kanji_t half  = unpublished();
    kanji_nav_t nav;

    /* With a card, both faces are reachable and each round-trips: what the app
     * asked for is what /api/state then reports back to it. */
    for (int s = 0; s < KANJI_SCREEN_COUNT; s++) {
        kanji_nav_reset(&nav);
        CHECK(kanji_nav_set_screen(&nav, (kanji_screen_t)s, &k));
        CHECK_INT(kanji_nav_screen(&nav), s);
        CHECK(!nav.committed || nav.revealed);
        CHECK(nav.grade >= KANJI_GRADE_AGAIN && nav.grade <= KANJI_GRADE_EASY);
    }

    /* Without one, the answer face is refused for the same reason KEY0 refuses
     * to reveal: there is nothing to show. A phone that could park the board on
     * an empty answer would leave a learner holding a screen no press of theirs
     * produces or clears. */
    const kanji_t *const NO_CARD[] = { NULL, &empty, &half };
    for (size_t c = 0; c < sizeof NO_CARD / sizeof NO_CARD[0]; c++) {
        kanji_nav_reset(&nav);
        CHECK(kanji_nav_set_screen(&nav, KANJI_SCREEN_QUESTION, NO_CARD[c]));
        CHECK_INT(kanji_nav_screen(&nav), KANJI_SCREEN_QUESTION);

        const kanji_nav_t before = nav;
        CHECK(!kanji_nav_set_screen(&nav, KANJI_SCREEN_ANSWER, NO_CARD[c]));
        /* A refused screen leaves the nav exactly as it was — the board must
         * not half-move and then repaint. */
        CHECK(same_nav(&nav, &before));
    }

    /* Out of range is refused, not clamped. An app sending 9 has a bug, and
     * silently showing it the question face hides that bug from whoever has to
     * find it. */
    kanji_nav_reset(&nav);
    const kanji_nav_t before = nav;
    CHECK(!kanji_nav_set_screen(&nav, (kanji_screen_t)-1, &k));
    CHECK(!kanji_nav_set_screen(&nav, (kanji_screen_t)KANJI_SCREEN_COUNT, &k));
    CHECK(!kanji_nav_set_screen(&nav, (kanji_screen_t)99, &k));
    CHECK(same_nav(&nav, &before));
    CHECK(!kanji_nav_set_screen(NULL, KANJI_SCREEN_QUESTION, &k));

    /* Neither face the app can set is a dead end: from either, one press of a
     * button the learner knows moves the board on. */
    for (int s = 0; s < KANJI_SCREEN_COUNT; s++) {
        kanji_nav_reset(&nav);
        CHECK(kanji_nav_set_screen(&nav, (kanji_screen_t)s, &k));
        CHECK(kanji_nav_can_press(&nav, KANJI_BTN_KEY0, &k));
        CHECK(kanji_nav_can_press(&nav, KANJI_BTN_KEY2, &k));
    }

    /* Setting a screen clears any rating in flight. Pinned rather than assumed:
     * it means the app can re-arm the four rating buttons on a card the learner
     * has already graded, and if the first grade has left the pending slot by
     * then a second rating reaches the proxy for the same card. The guard that
     * currently stops that lives in user_app, not here. */
    kanji_nav_reset(&nav);
    kanji_nav_press(&nav, KANJI_BTN_KEY0, &k);
    kanji_nav_press(&nav, KANJI_BTN_BOOT, &k);
    CHECK(nav.committed);
    CHECK(kanji_nav_set_screen(&nav, KANJI_SCREEN_ANSWER, &k));
    CHECK(!nav.committed);
}

/* --- the footer legend ---------------------------------------------------- */

/* The legend is derived from the same state the buttons act on, because a
 * fixed strip of text on a board whose KEY0 means 정답 보기 on one face and
 * 다시 on the next is a lie printed in 16 px. */
static void test_the_legend_says_what_the_buttons_currently_do(void)
{
    kanji_nav_t n;

    /* The front: three buttons turn the card over, KEY2 re-polls. */
    kanji_nav_reset(&n);
    CHECK_STR(kanji_nav_hint_key0(&n), S_HINT_REVEAL);
    CHECK_STR(kanji_nav_hint_key1(&n), S_HINT_REVEAL);
    CHECK_STR(kanji_nav_hint_key2(&n), S_KEY_REFRESH);
    CHECK_STR(kanji_nav_hint_boot(&n), S_HINT_REVEAL);

    /* The back: the four grade labels, in button order. Written out as the
     * strings rather than read back through kanji_button_grade(), so that a
     * swapped pair in the table is a failure here and not a tautology. */
    n.revealed = true;
    CHECK_STR(kanji_nav_hint_key0(&n), S_GRADE_AGAIN);
    CHECK_STR(kanji_nav_hint_key1(&n), S_GRADE_HARD);
    CHECK_STR(kanji_nav_hint_key2(&n), S_GRADE_GOOD);
    CHECK_STR(kanji_nav_hint_boot(&n), S_GRADE_EASY);

    /* Four distinct labels: two cells reading the same word is a dock the
     * learner cannot choose from. */
    const char *dock[KANJI_BTN_COUNT] = {
        kanji_nav_hint_key0(&n), kanji_nav_hint_key1(&n),
        kanji_nav_hint_key2(&n), kanji_nav_hint_boot(&n),
    };
    for (int a = 0; a < KANJI_BTN_COUNT; a++) {
        for (int b = a + 1; b < KANJI_BTN_COUNT; b++) {
            CHECK(strcmp(dock[a], dock[b]) != 0);
        }
    }

    /* Waiting on the proxy: the three grading buttons say so rather than
     * keeping their rating up, which would read as an offer to press again. */
    n.committed = true;
    for (int g = KANJI_GRADE_AGAIN; g <= KANJI_GRADE_EASY; g++) {
        n.grade = (kanji_grade_t)g;
        CHECK_STR(kanji_nav_hint_key0(&n), S_HINT_WAIT);
        CHECK_STR(kanji_nav_hint_key1(&n), S_HINT_WAIT);
        CHECK_STR(kanji_nav_hint_boot(&n), S_HINT_WAIT);
        /* KEY2 is still the escape hatch, and still says so. */
        CHECK_STR(kanji_nav_hint_key2(&n), S_KEY_REFRESH);
    }

    /* Every legal state, every button: never NULL and never empty. A blank
     * footer cell is indistinguishable from a control that is not there. */
    for (int rev = 0; rev < 2; rev++) {
        for (int com = 0; com < 2; com++) {
            for (int g = KANJI_GRADE_AGAIN; g <= KANJI_GRADE_EASY; g++) {
                const kanji_nav_t s = {
                    .revealed = rev != 0, .committed = com != 0,
                    .grade = (kanji_grade_t)g,
                };
                if (!s.revealed && s.committed) continue;
                const char *h[KANJI_BTN_COUNT] = {
                    kanji_nav_hint_key0(&s), kanji_nav_hint_key1(&s),
                    kanji_nav_hint_key2(&s), kanji_nav_hint_boot(&s),
                };
                for (int b = 0; b < KANJI_BTN_COUNT; b++) {
                    CHECK(h[b] != NULL);
                    CHECK(h[b] != NULL && h[b][0] != '\0');
                }
            }
        }
    }

    /* A NULL nav is the front, because that is what the board shows before the
     * first snapshot lands — not a crash and not four blank cells. */
    CHECK_STR(kanji_nav_hint_key0(NULL), S_HINT_REVEAL);
    CHECK_STR(kanji_nav_hint_key1(NULL), S_HINT_REVEAL);
    CHECK_STR(kanji_nav_hint_key2(NULL), S_KEY_REFRESH);
    CHECK_STR(kanji_nav_hint_boot(NULL), S_HINT_REVEAL);
}

int main(void)
{
    test_reset_is_the_question_face_with_nothing_in_flight();
    test_the_screen_is_derived_from_revealed_alone();
    test_every_screen_has_a_title();
    test_the_four_buttons_are_the_four_grades();
    test_a_reveal_costs_one_press_and_pre_arms_nothing();
    test_reveal_is_refused_when_there_is_no_card();
    test_one_press_reveals_and_one_press_grades();
    test_key2_refreshes_from_the_front_and_from_a_committed_back();
    test_a_second_grade_is_refused();
    test_clamp_repairs_an_impossible_restore();
    test_every_button_from_every_state_against_the_full_oracle();
    test_only_the_commit_step_is_a_dock_only_transition();
    test_setting_a_screen_obeys_the_same_rules_as_a_button();
    test_the_legend_says_what_the_buttons_currently_do();
    TH_REPORT("kanji_nav");
}
