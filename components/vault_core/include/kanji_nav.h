/*
 * kanji_nav.h — the only interaction state on the board.
 *
 * Four buttons, and FSRS has four grades. That coincidence is the whole design: the answer
 * face maps each physical button directly onto a rating, so committing one costs exactly one
 * press. The five-screen UI this replaced spent up to three presses walking a cursor around a
 * dock and a full panel refresh on each of them — nine seconds of strobing before the learner
 * had told the board anything.
 *
 *   button   문제                                    정답
 *   KEY0     뜻 보기                                 다시   (again)
 *   KEY1     뜻 보기                                 어려움 (hard)
 *   KEY2     새로고침                                보통   (good)
 *   BOOT     뜻 보기                                 쉬움   (easy)
 *
 * Left to right across the physical row that is Anki's canonical again -> hard -> good -> easy,
 * and each dock cell prints its own button glyph, so the legend documents itself rather than
 * being a fixed strip of text that has to be kept true by hand.
 *
 * KEY2 held for five seconds still reboots into the Wi-Fi portal. That is handled in the button
 * driver, not here — this file only ever sees a short press — but it is the reason KEY2's short
 * press stays useful on both faces rather than being folded into the reveal.
 *
 * ## What the state is
 *
 * Two booleans. `revealed` says which face is on the glass, `committed` says a grade has been
 * handed to the network task and the board is waiting for the next card. There are no sheets,
 * no page indices and no grade cursor, because there is nothing left to page through: 유래,
 * 구성요소 and the FSRS numbers are all on the answer face.
 */
#pragma once

#include <stdbool.h>

#include "kanji_model.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KANJI_BTN_KEY0 = 0,
    KANJI_BTN_KEY1,
    KANJI_BTN_KEY2,
    KANJI_BTN_BOOT,
    KANJI_BTN_COUNT,
} kanji_button_t;

typedef enum {
    KANJI_ACT_NONE = 0,
    KANJI_ACT_DRAW_FULL,   /* redraw everything and refresh the whole panel */
    KANJI_ACT_DRAW_DOCK,   /* redraw the dock alone and refresh only its rectangle */
    KANJI_ACT_SUBMIT,      /* hand nav->grade to the network task, then DRAW_DOCK */
    KANJI_ACT_REFRESH,     /* re-poll; nav is untouched */
} kanji_action_t;

typedef struct {
    bool          revealed;  /* the answer has been shown for THIS card */
    bool          committed; /* a grade is in flight; the dock shows which */
    kanji_grade_t grade;     /* meaningful only while `committed` */
} kanji_nav_t;

typedef struct {
    kanji_action_t action;
    bool           changed;
} kanji_nav_result_t;

typedef enum {
    KANJI_SCREEN_QUESTION = 0,
    KANJI_SCREEN_ANSWER,
    KANJI_SCREEN_COUNT,
} kanji_screen_t;

void           kanji_nav_reset(kanji_nav_t *nav);
kanji_screen_t kanji_nav_screen(const kanji_nav_t *nav);
const char    *kanji_screen_title(kanji_screen_t s);

/* The grade a button commits on the answer face, or 0 for a button that does not grade.
 * The dock reads its labels through this, so the legend on the glass and the behaviour of the
 * state machine cannot drift apart. */
kanji_grade_t  kanji_button_grade(kanji_button_t button);

kanji_nav_result_t kanji_nav_press(kanji_nav_t *nav, kanji_button_t btn, const kanji_t *k);

/* Whether `button` would do anything at all right now. The footer and the dock hide dead
 * controls with this; a control that is drawn and does nothing teaches the learner to distrust
 * the legend. */
bool kanji_nav_can_press(const kanji_nav_t *nav, kanji_button_t button, const kanji_t *k);

/* Whether the only difference between two states is which grade is committed — the gate for
 * the one partial refresh left on the board. */
bool kanji_nav_is_dock_only_transition(const kanji_nav_t *before, const kanji_nav_t *after);

/* The companion app's screen setter. Refuses an unreachable screen rather than clamping. */
bool kanji_nav_set_screen(kanji_nav_t *nav, kanji_screen_t screen, const kanji_t *k);

const char *kanji_nav_hint_key0(const kanji_nav_t *nav);
const char *kanji_nav_hint_key1(const kanji_nav_t *nav);
const char *kanji_nav_hint_key2(const kanji_nav_t *nav);
const char *kanji_nav_hint_boot(const kanji_nav_t *nav);

#ifdef __cplusplus
}
#endif
