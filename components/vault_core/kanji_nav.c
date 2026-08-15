/* The button state machine. Pure: no LVGL, no panel, no network, and no card content beyond
 * "is there a card". Everything it decides is host-tested in test_kanji_nav.c. */
#include "kanji_nav.h"

#include "ui_strings.h"

static bool has_card(const kanji_t *k)
{
    return k && k->valid && k->card.valid;
}

void kanji_nav_reset(kanji_nav_t *nav)
{
    if (!nav) return;
    nav->revealed  = false;
    nav->committed = false;
    nav->grade     = KANJI_GRADE_GOOD;
}

kanji_screen_t kanji_nav_screen(const kanji_nav_t *nav)
{
    if (!nav) return KANJI_SCREEN_QUESTION;
    return nav->revealed ? KANJI_SCREEN_ANSWER : KANJI_SCREEN_QUESTION;
}

const char *kanji_screen_title(kanji_screen_t s)
{
    switch (s) {
    case KANJI_SCREEN_ANSWER:   return S_SCREEN_ANSWER;
    case KANJI_SCREEN_QUESTION: return S_SCREEN_QUESTION;
    default:                    return S_SCREEN_QUESTION;
    }
}

/* The one table that maps a physical button to a rating. The dock draws its four cells from
 * this in button order, so what the glass says and what a press does have a single source. */
kanji_grade_t kanji_button_grade(kanji_button_t button)
{
    switch (button) {
    case KANJI_BTN_KEY0: return KANJI_GRADE_AGAIN;
    case KANJI_BTN_KEY1: return KANJI_GRADE_HARD;
    case KANJI_BTN_KEY2: return KANJI_GRADE_GOOD;
    case KANJI_BTN_BOOT: return KANJI_GRADE_EASY;
    default:             return (kanji_grade_t)0;
    }
}

/* Clamp anything a caller or a corrupted restore could have put in the struct. Run before and
 * after every press so no press can ever observe or leave an impossible state. */
static void clamp(kanji_nav_t *nav)
{
    if (nav->grade < KANJI_GRADE_AGAIN || nav->grade > KANJI_GRADE_EASY) {
        nav->grade = KANJI_GRADE_GOOD;
    }
    if (!nav->revealed) nav->committed = false;   /* nothing is in flight on the front */
}

static kanji_nav_result_t decide(kanji_nav_t *nav, kanji_button_t btn, const kanji_t *k)
{
    kanji_nav_result_t r = { KANJI_ACT_NONE, false };

    if (!nav->revealed) {
        /* 문제. KEY2 re-polls; everything else turns the card over, but only when there is a
         * card to turn over — on the completion screen a reveal would show an empty answer. */
        if (btn == KANJI_BTN_KEY2) {
            r.action = KANJI_ACT_REFRESH;
            return r;
        }
        if (!has_card(k)) return r;
        nav->revealed = true;
        r.action = KANJI_ACT_DRAW_FULL;
        return r;
    }

    /* 정답. A grade is already in flight: refuse every rating rather than counting a second
     * one, because the proxy answers a repeat of the id it just graded with the same payload
     * and a DIFFERENT rating on the same card is not a retry, it is a mis-grade. KEY2 keeps
     * working as a re-poll so a learner whose laptop was asleep is not stranded. */
    if (nav->committed) {
        if (btn == KANJI_BTN_KEY2) r.action = KANJI_ACT_REFRESH;
        return r;
    }

    const kanji_grade_t g = kanji_button_grade(btn);
    if (g == 0 || !has_card(k)) return r;

    nav->grade     = g;
    nav->committed = true;
    r.action = KANJI_ACT_SUBMIT;
    return r;
}

kanji_nav_result_t kanji_nav_press(kanji_nav_t *nav, kanji_button_t btn, const kanji_t *k)
{
    kanji_nav_result_t r = { KANJI_ACT_NONE, false };
    if (!nav || btn < 0 || btn >= KANJI_BTN_COUNT) return r;

    clamp(nav);
    const kanji_nav_t before = *nav;

    r = decide(nav, btn, k);
    clamp(nav);

    /* Field by field, never memcmp: the struct has padding, and comparing padding makes the
     * panel refresh at random on a device whose whole refresh policy is "change nothing that
     * did not change". */
    r.changed = before.revealed  != nav->revealed
             || before.committed != nav->committed
             || before.grade     != nav->grade;
    return r;
}

bool kanji_nav_can_press(const kanji_nav_t *nav, kanji_button_t button, const kanji_t *k)
{
    if (!nav || button < 0 || button >= KANJI_BTN_COUNT) return false;
    kanji_nav_t probe = *nav;
    return kanji_nav_press(&probe, button, k).action != KANJI_ACT_NONE;
}

bool kanji_nav_is_dock_only_transition(const kanji_nav_t *before, const kanji_nav_t *after)
{
    if (!before || !after) return false;
    return before->revealed && after->revealed
        && !before->committed && after->committed
        && after->grade >= KANJI_GRADE_AGAIN && after->grade <= KANJI_GRADE_EASY;
}

bool kanji_nav_set_screen(kanji_nav_t *nav, kanji_screen_t screen, const kanji_t *k)
{
    if (!nav || screen < 0 || screen >= KANJI_SCREEN_COUNT) return false;

    if (screen == KANJI_SCREEN_ANSWER && !has_card(k)) return false;

    nav->revealed  = (screen == KANJI_SCREEN_ANSWER);
    nav->committed = false;
    clamp(nav);
    return true;
}

/* The legend. It is derived from the same state the buttons act on, because a fixed legend on
 * a board whose KEY0 means 정답 on one face and 다시 on the next is a lie printed in 16 px. */
static const char *hint(const kanji_nav_t *nav, kanji_button_t btn)
{
    const bool answer = nav && nav->revealed;
    if (!answer) {
        return btn == KANJI_BTN_KEY2 ? S_KEY_REFRESH : S_HINT_REVEAL;
    }
    if (nav->committed) {
        return btn == KANJI_BTN_KEY2 ? S_KEY_REFRESH : S_HINT_WAIT;
    }
    return kanji_grade_label(kanji_button_grade(btn));
}

const char *kanji_nav_hint_key0(const kanji_nav_t *nav) { return hint(nav, KANJI_BTN_KEY0); }
const char *kanji_nav_hint_key1(const kanji_nav_t *nav) { return hint(nav, KANJI_BTN_KEY1); }
const char *kanji_nav_hint_key2(const kanji_nav_t *nav) { return hint(nav, KANJI_BTN_KEY2); }
const char *kanji_nav_hint_boot(const kanji_nav_t *nav) { return hint(nav, KANJI_BTN_BOOT); }
