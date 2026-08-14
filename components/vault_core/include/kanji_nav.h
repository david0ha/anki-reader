/*
 * kanji_nav.h — what each button does, as a pure state machine.
 *
 * Three side buttons and no touch have to drive five screens and a four-way
 * rating. That is a real interaction design, and on this board getting it wrong
 * is expensive in a way it is not on a phone: every screen change is a
 * multi-second full-panel flash, so a mapping that needs two presses where one
 * would do is not a minor annoyance, it is four extra seconds of the panel
 * strobing at the learner.
 *
 * So the mapping lives here, in twenty lines of integer logic with no LVGL, no
 * panel and no card data — and the host test drives every button from every
 * state. What the firmware does with the answer (render, refresh, POST a grade)
 * is the firmware's problem; this file only decides.
 *
 * The shape is: one press in, one `kanji_nav_result_t` out. The nav struct is
 * the ONLY interaction state on the device — there is no "which sheet was I in"
 * hidden inside a widget, because a widget cannot be host-tested.
 */
#pragma once

#include <stdbool.h>

#include "kanji_model.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The four physical buttons, in the order components/buttons declares them.
 * KEY2's five-second hold (Wi-Fi setup) is caught before it ever reaches here:
 * it reboots the board, so it is not a navigation event. */
typedef enum {
    KANJI_BTN_KEY0 = 0,
    KANJI_BTN_KEY1,
    KANJI_BTN_KEY2,
    KANJI_BTN_BOOT,
    KANJI_BTN_COUNT,
} kanji_button_t;

/* The rising sheets, in the order BOOT cycles them. KANJI_SHEET_NONE is the
 * player itself — the card, question or answer. */
typedef enum {
    KANJI_SHEET_NONE = 0,
    KANJI_SHEET_DESCRIPTION,   /* 설명: the shape explanation + memory hook */
    KANJI_SHEET_COMMENTS,      /* 댓글 */
    KANJI_SHEET_FSRS,          /* FSRS가 뭔가요 + this card's own state */
    KANJI_SHEET_COUNT,
} kanji_sheet_t;

/* What the caller should do about the press. Exactly one of these; the refresh
 * kind is the point of the enum, because on e-Paper it is the difference
 * between a silent update and a two-second flash of the whole panel. */
typedef enum {
    KANJI_ACT_NONE = 0,     /* nothing changed — do not touch the panel */
    KANJI_ACT_DRAW_FULL,    /* re-render, full refresh */
    KANJI_ACT_DRAW_DOCK,    /* re-render, partial refresh of the grade dock */
    KANJI_ACT_SUBMIT,       /* commit nav.grade, then fetch the next card */
    KANJI_ACT_REFRESH,      /* re-poll the current card */
} kanji_action_t;

/* The whole of the device's interaction state. Copyable, comparable, and small
 * enough to log in one line. */
typedef struct {
    bool          revealed;    /* the answer has been shown for THIS card */
    kanji_sheet_t sheet;
    int           sheet_page;  /* 0-based page within the open sheet */
    kanji_grade_t grade;       /* the dock cursor */
} kanji_nav_t;

typedef struct {
    kanji_action_t action;
    bool           changed;    /* the nav struct differs from before the press */
} kanji_nav_result_t;

/* The five things that can be on the glass. Derived from the nav state rather
 * than stored, so "sheet open but not revealed" cannot describe two screens. */
typedef enum {
    KANJI_SCREEN_QUESTION = 0,
    KANJI_SCREEN_ANSWER,
    KANJI_SCREEN_DESCRIPTION,
    KANJI_SCREEN_COMMENTS,
    KANJI_SCREEN_FSRS,
    KANJI_SCREEN_COUNT,
} kanji_screen_t;

/* The starting state, and the state every new card resets to: question side up,
 * no sheet, cursor parked on 보통 — the rating a learner who got it right but
 * had to think picks, and the one Anki and FSRS both treat as the default. */
void kanji_nav_reset(kanji_nav_t *nav);

/* Which of the five screens this state puts on the glass. Derived, never
 * stored: an open sheet wins over the card beneath it, and with no sheet open
 * `revealed` picks the side. A NULL nav reads as the question side, which is
 * what the board shows before anything has been loaded. */
kanji_screen_t kanji_nav_screen(const kanji_nav_t *nav);

/* The screen's own name, for the footer and the companion-app JSON. */
const char *kanji_screen_title(kanji_screen_t s);

/* --- how much a page holds ------------------------------------------------
 * These live here, next to kanji_sheet_pages(), because the paging arithmetic
 * and the sheets that draw those pages must not be able to disagree. They did
 * once, in two files, and the failure was silent in both directions: too few
 * pages and KEY0 could never reach the last comment, too many and the pager
 * printed 4/4 over a page the sheet had quietly clamped back to the first.
 *
 * The sheet files static_assert against these, so a drift is a compile error
 * rather than a screen nobody can reach. */

/* Two comments to a page. Three would fit only if all three were one line, and
 * a comment that silently loses its last line reads as a rendering bug. */
#define KANJI_COMMENTS_PER_PAGE 2

/* The FSRS sheet's copy is three fixed pages (S_FSRS_P1..P3 in ui_strings.h).
 * The card's own numbers ride along the bottom of every one of them, so the
 * count is the copy's and never the card's. */
#define KANJI_FSRS_PAGES 3

/* How many pages `sheet` needs for `k`. A sheet whose content fits shows one
 * page; KEY0 wraps through the rest. Always >= 1, so the caller never divides
 * by zero and an empty comments list still has a page to say so on. */
int kanji_sheet_pages(const kanji_t *k, kanji_sheet_t sheet);

/* Apply one press. `k` is read for nothing but "is there a card at all" and the
 * open sheet's page count, so a NULL snapshot behaves like an empty one. */
kanji_nav_result_t kanji_nav_press(kanji_nav_t *nav, kanji_button_t btn,
                                   const kanji_t *k);

/* Jump straight to `screen`, for the companion app. Returns whether the screen
 * was ACCEPTED, not whether anything moved — a refused screen leaves *nav
 * exactly as it was, and the caller compares before and after to decide whether
 * a repaint is owed.
 *
 * This exists so the phone and the buttons go through the SAME gate. A screen
 * is refused for exactly the reasons BOOT would skip it — no card, or a 설명
 * sheet with no shape story, hook or parts to show — because a board the app
 * can park on a screen no press produces is a board whose two controls disagree
 * about what it can display, and the learner holding it has only one of them.
 * An out-of-range screen is refused rather than clamped: a companion app asking
 * for screen 9 has a bug, and quietly showing it screen 0 hides that. */
bool kanji_nav_set_screen(kanji_nav_t *nav, kanji_screen_t screen,
                          const kanji_t *k);

/* The footer legend for the current state: what KEY0 / KEY1 / BOOT do right
 * now. Never NULL. The legend changes with the screen because the buttons do —
 * a fixed legend on a board whose KEY0 means "정답" on one screen and "등급"
 * on the next is a lie printed in 16 px. */
const char *kanji_nav_hint_key0(const kanji_nav_t *nav);
const char *kanji_nav_hint_key1(const kanji_nav_t *nav);
const char *kanji_nav_hint_boot(const kanji_nav_t *nav);

#ifdef __cplusplus
}
#endif
