/*
 * ui_kanji.h — the whole on-glass UI, for the 648x480 e-Paper panel.
 *
 * A card has two faces and nothing else:
 *
 *   문제   an art print — the headword alone, a Japanese example set as a
 *          pull-quote, and the learner's own history with this card
 *   정답   a dictionary spread — reading, senses, 성립, 구성, 예문, the FSRS
 *          figures, and the four ratings, all at once
 *
 * There is nothing behind a button. The five-screen UI this replaced paged 유래,
 * 구성요소 and the FSRS numbers through their own sheets, so seeing what a
 * character is made of cost three presses and three full refreshes — nine
 * seconds of a panel strobing to read two lines that fit beside the senses all
 * along. Every field those sheets showed was already in kanji_t.
 *
 * Neither face has chrome around it: each is a full-panel page that draws its
 * own header, its own status badges and its own footer. This header's remaining
 * job is the router and the setup overlay.
 *
 * Every setter only mutates widgets. Nothing here talks to the panel: on
 * e-Paper the caller decides when a refresh is worth several seconds of
 * flashing, so the sequence is always
 *
 *     ui_kanji_set_*(...);  Lvgl_RenderNow();  epd_refresh_*();
 *
 * Portable: LVGL only, no ESP-IDF. The desktop simulator builds these files
 * verbatim, which is how the layout gets checked against a real 648x480 bitmap
 * before it ever reaches hardware.
 */
#pragma once

#include <stdbool.h>

#include "kanji_model.h"
#include "kanji_nav.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Everything the glass reports that is about the board rather than the study
 * session. Passed as a struct so adding an indicator does not change three
 * signatures, and passed down to BOTH faces: 오프라인 / 오래됨 / DEMO belongs on
 * whichever one is up, and a frame that admits its staleness on the answer and
 * hides it on the question is a frame that lies half the time. */
typedef struct {
    bool online;          /* Wi-Fi associated and the last poll succeeded */
    bool stale;           /* showing a card older than one poll interval  */
    bool battery_present; /* a cell is fitted (false = USB power)         */
    int  battery_pct;     /* 0..100, meaningless unless battery_present   */
} ui_status_t;

/* Build the UI under `parent` (a full-screen 648x480 container). */
void ui_kanji_create(lv_obj_t *parent);

/* Push a snapshot into the face that is up. The struct is copied, so a
 * stack-local is fine. Pass NULL to blank the content and show the "no data"
 * state. */
void ui_kanji_set_data(const kanji_t *k);

/* Show the face named by nav. An answer-to-answer transition that only commits
 * a grade mutates the dock exclusively; every other change turns a face over
 * and redraws it. */
void ui_kanji_set_nav(const kanji_nav_t *nav);

/* The board's own indicators — the badges and the battery warning both faces
 * draw for themselves. */
void ui_kanji_set_status(const ui_status_t *st);

/* The dock's rectangle, in panel coordinates. `x2` and `y2` are exclusive, so
 * the returned window is [x1, x2) × [y1, y2), matching
 * epd_refresh_partial_area().
 *
 * This is the ONLY partial refresh on the board. It no longer moves a cursor —
 * the four buttons ARE the four grades, so committing one is a single press —
 * but the dock is still the one region that changes while the card behind it
 * does not: the cell that was pressed inverts to acknowledge it, and the answer
 * the learner is reading stays on the glass until the next card arrives.
 * Redrawing the whole spread to ink one cell would be seconds of strobing over
 * text that did not change. The dock's x bounds are byte-aligned
 * (ui_kanji_layout.c), so the window the driver refreshes is exactly the window
 * that was drawn. */
void ui_kanji_dock_area(int *x1, int *y1, int *x2, int *y2);

/* Full-screen message, for provisioning status and fatal states. Pass NULL to
 * dismiss it and return to the screens. */
void ui_kanji_set_overlay(const char *title, const char *body);

#ifdef __cplusplus
}
#endif
