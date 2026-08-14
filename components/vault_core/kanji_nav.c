/*
 * kanji_nav.c — the button state machine declared in kanji_nav.h.
 *
 * No LVGL, no ESP-IDF, no allocation, no card content beyond "is there a card"
 * and "how long is the open sheet". Everything here is exercised directly by
 * test_kanji_nav.c, which drives every button from every reachable state.
 */
#include "kanji_nav.h"

#include "ui_strings.h"

void kanji_nav_reset(kanji_nav_t *nav)
{
    if (!nav) return;
    nav->revealed   = false;
    nav->sheet      = KANJI_SHEET_NONE;
    nav->sheet_page = 0;
    nav->grade      = KANJI_GRADE_GOOD;
}

/* The one place the two vocabularies meet. A sheet and the screen it puts on
 * the glass are the same fact, and it is needed in both directions — sheet ->
 * screen for the router and the footer, screen -> sheet for the companion app.
 * Written as two switches they drift, and the drift is invisible from either
 * side alone: the app asks for 댓글, the board opens 설명, and /api/state then
 * honestly reports 설명 to an app that believes it asked for something else. */
static const struct {
    kanji_sheet_t  sheet;
    kanji_screen_t screen;
} SHEET_SCREEN[] = {
    { KANJI_SHEET_DESCRIPTION, KANJI_SCREEN_DESCRIPTION },
    { KANJI_SHEET_COMMENTS,    KANJI_SCREEN_COMMENTS    },
    { KANJI_SHEET_FSRS,        KANJI_SCREEN_FSRS        },
};

/* Every sheet but NONE is in the table. A new sheet with no row would silently
 * render as the question side. */
_Static_assert(sizeof SHEET_SCREEN / sizeof SHEET_SCREEN[0] ==
                   KANJI_SHEET_COUNT - 1,
               "every kanji_sheet_t except NONE needs a row in SHEET_SCREEN");

kanji_screen_t kanji_nav_screen(const kanji_nav_t *nav)
{
    if (!nav) return KANJI_SCREEN_QUESTION;
    for (size_t i = 0; i < sizeof SHEET_SCREEN / sizeof SHEET_SCREEN[0]; i++) {
        if (SHEET_SCREEN[i].sheet == nav->sheet) return SHEET_SCREEN[i].screen;
    }
    return nav->revealed ? KANJI_SCREEN_ANSWER : KANJI_SCREEN_QUESTION;
}

const char *kanji_screen_title(kanji_screen_t s)
{
    switch (s) {
    case KANJI_SCREEN_ANSWER:      return S_SCREEN_ANSWER;
    case KANJI_SCREEN_DESCRIPTION: return S_SCREEN_DESC;
    case KANJI_SCREEN_COMMENTS:    return S_SCREEN_COMMENTS;
    case KANJI_SCREEN_FSRS:        return S_SCREEN_FSRS;
    case KANJI_SCREEN_QUESTION:    return S_SCREEN_QUESTION;
    default:                       return S_SCREEN_QUESTION;
    }
}

/* --- what a sheet is worth ------------------------------------------------ */

static bool has_card(const kanji_t *k)
{
    return k != NULL && k->card.valid;
}

/* Whether a sheet has anything to say about this card. FSRS always does — it
 * explains the scheduler, which is exactly what a learner staring at an empty
 * session wants to read. The other two are about a card, so with no card there
 * is nothing to open. */
static bool sheet_available(const kanji_t *k, kanji_sheet_t sheet)
{
    switch (sheet) {
    case KANJI_SHEET_FSRS:
        return true;
    case KANJI_SHEET_DESCRIPTION:
        return has_card(k) && (k->card.description[0] != '\0' ||
                               k->card.hook_body[0] != '\0' ||
                               k->card.part_count > 0);
    case KANJI_SHEET_COMMENTS:
        return has_card(k);
    default:
        return false;
    }
}

int kanji_sheet_pages(const kanji_t *k, kanji_sheet_t sheet)
{
    switch (sheet) {
    case KANJI_SHEET_FSRS:
        return KANJI_FSRS_PAGES;
    case KANJI_SHEET_COMMENTS: {
        const int n = has_card(k) ? k->card.comment_count : 0;
        if (n <= KANJI_COMMENTS_PER_PAGE) return 1;
        return (n + KANJI_COMMENTS_PER_PAGE - 1) / KANJI_COMMENTS_PER_PAGE;
    }
    case KANJI_SHEET_DESCRIPTION:
        /* One screen: the explanation, the memory hook and up to three parts
         * are each bounded by the model's own byte limits and were sized to
         * fit the content area together. */
        return 1;
    default:
        return 1;
    }
}

/* --- the legend ----------------------------------------------------------- */

const char *kanji_nav_hint_key0(const kanji_nav_t *nav)
{
    if (!nav) return S_HINT_REVEAL;
    if (nav->sheet != KANJI_SHEET_NONE) return S_HINT_PAGE;
    return nav->revealed ? S_HINT_GRADE : S_HINT_REVEAL;
}

const char *kanji_nav_hint_key1(const kanji_nav_t *nav)
{
    if (!nav) return S_HINT_DESC;
    if (nav->sheet != KANJI_SHEET_NONE) return S_HINT_CLOSE;
    return nav->revealed ? S_HINT_COMMIT : S_HINT_DESC;
}

const char *kanji_nav_hint_boot(const kanji_nav_t *nav)
{
    if (!nav) return S_HINT_FSRS;
    if (nav->sheet != KANJI_SHEET_NONE) return S_HINT_TAB;
    return nav->revealed ? S_HINT_DESC : S_HINT_FSRS;
}

/* --- the machine ---------------------------------------------------------- */

/* Bring a nav struct back inside its own invariants. Called before every press
 * so that a state nothing here produced — a stale page after the card shrank,
 * a grade read off a corrupted NVS blob — cannot survive one button. */
static void clamp(kanji_nav_t *nav, const kanji_t *k)
{
    if (nav->sheet < KANJI_SHEET_NONE || nav->sheet >= KANJI_SHEET_COUNT) {
        nav->sheet = KANJI_SHEET_NONE;
    }
    if (nav->grade < KANJI_GRADE_AGAIN || nav->grade > KANJI_GRADE_EASY) {
        nav->grade = KANJI_GRADE_GOOD;
    }
    const int pages = kanji_sheet_pages(k, nav->sheet);
    if (nav->sheet_page < 0 || nav->sheet_page >= pages) nav->sheet_page = 0;
}

/* The next sheet BOOT should land on, skipping any that has nothing to show.
 * Walks at most KANJI_SHEET_COUNT steps, so it terminates even if every sheet
 * is unavailable (it cannot be — FSRS always is). */
static kanji_sheet_t next_sheet(const kanji_t *k, kanji_sheet_t from)
{
    kanji_sheet_t s = from;
    for (int i = 0; i < KANJI_SHEET_COUNT; i++) {
        s = (kanji_sheet_t)((s + 1) % KANJI_SHEET_COUNT);
        if (s == KANJI_SHEET_NONE) return KANJI_SHEET_NONE;  /* round trip */
        if (sheet_available(k, s)) return s;
    }
    return KANJI_SHEET_NONE;
}

/* Open `sheet` if it has anything to say. Returns whether it opened. */
static bool open_sheet(kanji_nav_t *nav, const kanji_t *k, kanji_sheet_t sheet)
{
    if (!sheet_available(k, sheet)) return false;
    nav->sheet = sheet;
    nav->sheet_page = 0;
    return true;
}

static kanji_action_t press_in_sheet(kanji_nav_t *nav, kanji_button_t btn,
                                     const kanji_t *k)
{
    switch (btn) {
    case KANJI_BTN_KEY0: {
        const int pages = kanji_sheet_pages(k, nav->sheet);
        if (pages <= 1) return KANJI_ACT_NONE;   /* nothing to page to */
        nav->sheet_page = (nav->sheet_page + 1) % pages;
        return KANJI_ACT_DRAW_FULL;
    }
    case KANJI_BTN_KEY1:
        nav->sheet = KANJI_SHEET_NONE;
        nav->sheet_page = 0;
        return KANJI_ACT_DRAW_FULL;
    case KANJI_BTN_BOOT:
        nav->sheet = next_sheet(k, nav->sheet);
        nav->sheet_page = 0;
        return KANJI_ACT_DRAW_FULL;
    default:
        return KANJI_ACT_NONE;
    }
}

static kanji_action_t press_on_card(kanji_nav_t *nav, kanji_button_t btn,
                                    const kanji_t *k)
{
    if (!nav->revealed) {
        switch (btn) {
        case KANJI_BTN_KEY0:
            if (!has_card(k)) return KANJI_ACT_NONE;
            nav->revealed = true;
            return KANJI_ACT_DRAW_FULL;
        case KANJI_BTN_KEY1:
            /* 설명 doubles as the hint before the answer: the shape story is
             * what the web app's hint sheet shows, and reading it and still
             * failing is a more honest AGAIN than not reading it. */
            return open_sheet(nav, k, KANJI_SHEET_DESCRIPTION)
                       ? KANJI_ACT_DRAW_FULL : KANJI_ACT_NONE;
        case KANJI_BTN_BOOT:
            return open_sheet(nav, k, KANJI_SHEET_FSRS)
                       ? KANJI_ACT_DRAW_FULL : KANJI_ACT_NONE;
        default:
            return KANJI_ACT_NONE;
        }
    }

    switch (btn) {
    case KANJI_BTN_KEY0:
        /* 보통 -> 쉬움 -> 다시 -> 어려움 -> 보통. Every rating is at most
         * three presses away, and the dock is the one thing on this board that
         * repaints without flashing the whole panel. */
        nav->grade = (kanji_grade_t)(nav->grade % KANJI_GRADE_COUNT + 1);
        return KANJI_ACT_DRAW_DOCK;
    case KANJI_BTN_KEY1:
        /* The caller resets the nav when the next card lands, so a POST that
         * fails leaves the learner on the answer they already read rather than
         * on a blank question for a card they have graded. */
        return has_card(k) ? KANJI_ACT_SUBMIT : KANJI_ACT_NONE;
    case KANJI_BTN_BOOT:
        return open_sheet(nav, k, KANJI_SHEET_DESCRIPTION)
                   ? KANJI_ACT_DRAW_FULL : KANJI_ACT_NONE;
    default:
        return KANJI_ACT_NONE;
    }
}

bool kanji_nav_set_screen(kanji_nav_t *nav, kanji_screen_t screen,
                          const kanji_t *k)
{
    if (!nav || screen < KANJI_SCREEN_QUESTION || screen >= KANJI_SCREEN_COUNT) {
        return false;
    }

    /* Decided on a copy, committed only if it holds. A screen that turns out to
     * be unreachable must leave the nav exactly as it was, not half-moved. */
    kanji_nav_t next = *nav;

    switch (screen) {
    case KANJI_SCREEN_QUESTION:
        next.sheet = KANJI_SHEET_NONE;
        next.revealed = false;
        break;
    case KANJI_SCREEN_ANSWER:
        /* There is no answer to show without a card, and the question side
         * already says so. */
        if (!has_card(k)) return false;
        next.sheet = KANJI_SHEET_NONE;
        next.revealed = true;
        break;
    default: {
        /* A sheet, named by the same table kanji_nav_screen() reads, and opened
         * through the same gate BOOT goes through. */
        const kanji_sheet_t *sheet = NULL;
        for (size_t i = 0; i < sizeof SHEET_SCREEN / sizeof SHEET_SCREEN[0]; i++) {
            if (SHEET_SCREEN[i].screen == screen) {
                sheet = &SHEET_SCREEN[i].sheet;
                break;
            }
        }
        if (!sheet || !open_sheet(&next, k, *sheet)) return false;
        break;
    }
    }

    next.sheet_page = 0;
    clamp(&next, k);
    *nav = next;
    return true;
}

kanji_nav_result_t kanji_nav_press(kanji_nav_t *nav, kanji_button_t btn,
                                   const kanji_t *k)
{
    kanji_nav_result_t r = { KANJI_ACT_NONE, false };
    if (!nav) return r;

    clamp(nav, k);
    const kanji_nav_t before = *nav;

    if (btn == KANJI_BTN_KEY2) {
        /* Refresh, from anywhere, without moving. KEY2's five-second hold is
         * the Wi-Fi escape hatch and is caught before it reaches here. */
        r.action = KANJI_ACT_REFRESH;
        return r;
    }

    r.action = (nav->sheet != KANJI_SHEET_NONE)
                   ? press_in_sheet(nav, btn, k)
                   : press_on_card(nav, btn, k);

    clamp(nav, k);
    /* Field by field, not memcmp: the struct has padding that a struct
     * assignment is not required to copy, and a fingerprint that depends on
     * uninitialised bytes would flash the panel at random. */
    r.changed = before.revealed   != nav->revealed ||
                before.sheet      != nav->sheet    ||
                before.sheet_page != nav->sheet_page ||
                before.grade      != nav->grade;
    return r;
}
