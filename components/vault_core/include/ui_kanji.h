/*
 * ui_kanji.h — the whole on-glass UI, for the 648x480 e-Paper panel.
 *
 * Five screens under a shared header and footer, redrawn from the kanjis.ai
 * Shorts player:
 *
 *   문제   the headword alone, filled black — the "video"
 *   정답   the headword, its reading, the Korean senses, an example, and the
 *          four-rating FSRS dock
 *   설명   the shape story, the memory hook and the headword's components
 *   댓글   what people said under this card
 *   FSRS   what the scheduler is, in three pages, plus this card's own numbers
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

/* Everything the header reports that is about the board rather than the study
 * session. Passed as a struct so adding an indicator does not change three
 * signatures. */
typedef struct {
    bool online;          /* Wi-Fi associated and the last poll succeeded */
    bool stale;           /* showing a card older than one poll interval  */
    bool battery_present; /* a cell is fitted (false = USB power)         */
    int  battery_pct;     /* 0..100, meaningless unless battery_present   */
} ui_status_t;

/* Build the UI under `parent` (a full-screen 648x480 container). */
void ui_kanji_create(lv_obj_t *parent);

/* Push a snapshot into every screen. The struct is copied, so a stack-local is
 * fine. Pass NULL to blank the content and show the "no data" state. */
void ui_kanji_set_data(const kanji_t *k);

/* Show the screen the nav state names, and draw the grade cursor and the sheet
 * page it carries. Cheap: it hides and shows panes, it does not rebuild them. */
void ui_kanji_set_nav(const kanji_nav_t *nav);

/* Header indicators. */
void ui_kanji_set_status(const ui_status_t *st);

/* The rectangle the grade cursor lives in, in panel coordinates.
 *
 * This is the ONLY partial refresh on the board, and it exists for one reason:
 * choosing among four ratings takes up to three presses, and three full
 * refreshes is nine seconds of the panel strobing before the learner has told
 * it anything. The dock's x bounds are byte-aligned (ui_kanji_layout.c), so the
 * window the driver refreshes is exactly the window that was drawn. */
void ui_kanji_dock_area(int *x1, int *y1, int *x2, int *y2);

/* Full-screen message, for provisioning status and fatal states. Pass NULL to
 * dismiss it and return to the screens. */
void ui_kanji_set_overlay(const char *title, const char *body);

#ifdef __cplusplus
}
#endif
