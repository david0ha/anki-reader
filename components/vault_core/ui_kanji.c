/* The router, and the only thing on the board that is neither of the two faces: the Wi-Fi
 * setup overlay.
 *
 * This file used to build a rail, a masthead and a footer legend as well, and route five
 * screens between them. All three are gone. The rail was 80 px of the panel's 648 spent on a
 * badge and the string 35/60; the masthead and the legend were chrome drawn around a card
 * rather than part of one. The front now carries its own header and footer rectangles and the
 * back's dock IS its legend — each cell prints the button that commits it — so both faces are
 * whole pages and this file has nothing left to draw around them.
 *
 * What is left is the part that genuinely does not belong to either face: which of the two is
 * on the glass, and the sheet that covers both while the board is being put on a network. */
#include "ui_kanji.h"

#include "ui_internal.h"

typedef struct {
    lv_obj_t   *screen[KANJI_SCREEN_COUNT];
    lv_obj_t   *overlay;
    lv_obj_t   *overlay_title;
    lv_obj_t   *overlay_body;
    kanji_t     data;
    bool        has_data;
    kanji_nav_t nav;
    ui_status_t status;
} kanji_ui_t;

static kanji_ui_t s;

/* --- the setup overlay -----------------------------------------------------------------------
 * A full-panel white sheet over whichever face is up, for provisioning and fatal states.
 *
 * It is not a card, so ui_kanji_layout.h has no rectangles of its own for it — and inventing
 * coordinates here is exactly what the pure-integer layout exists to keep out of a screen file.
 * So it borrows the front's masthead rhythm instead: the same hairline at the same y, and the
 * title where the headword goes. A learner who looks up mid-setup then sees the board they
 * already know with something else written on it, rather than a second design.
 *
 * The body is the paper between the bottom of the hero slot and the footer rule, composed from
 * those two named edges. Every number in this function is a field of kanji_front_layout_t;
 * there is no constant here that can drift away from one. */
static void build_overlay(lv_obj_t *par)
{
    const kanji_front_layout_t *l = kanji_front_layout();
    const kanji_rect_t body = {
        .x = l->hero.x,
        .y = l->hero.y + l->hero.h,
        .w = l->hero.w,
        .h = l->foot_rule.y - (l->hero.y + l->hero.h),
    };

    s.overlay = ui_fill_white(par, 0, 0, UI_W, UI_H);
    ui_rule(s.overlay, l->head_rule.x, l->head_rule.y, l->head_rule.w, l->head_rule.h);
    s.overlay_title = ui_lab_r(s.overlay, l->hero, UI_F_TITLE, LV_TEXT_ALIGN_LEFT, "");
    /* The portal instructions are four short lines with the SSID interpolated into them, so
     * this block wraps rather than ellipsizing: a network name cut off at "..." is a name
     * nobody can type into a phone. */
    s.overlay_body = ui_lab_wrap_r(s.overlay, body, UI_F_HEAD, LV_TEXT_ALIGN_LEFT, "");
    ui_show(s.overlay, false);
}

void ui_kanji_set_overlay(const char *title, const char *body)
{
    if (!s.overlay) return;
    if (title) {
        ui_set(s.overlay_title, title);
        ui_set(s.overlay_body, body ? body : "");
    }
    ui_show(s.overlay, title != NULL);
}

/* --- the router ----------------------------------------------------------------------------- */

static void show_only(kanji_screen_t which)
{
    for (int i = 0; i < KANJI_SCREEN_COUNT; i++) {
        ui_show(s.screen[i], i == (int)which);
    }
}

/* Only the face that is up is rewritten. The other one is hidden, and its widgets are rewritten
 * the moment the router turns it over — redrawing both would double the LVGL work on every poll
 * for pixels nobody can see.
 *
 * Both faces get the board status as well as the card, because 오프라인 / 오래됨 / DEMO belongs
 * on whichever one is showing: a frame that admits its staleness on the answer and hides it on
 * the question is a frame that lies half the time. */
static void refresh_current(void)
{
    const kanji_t *k = s.has_data ? &s.data : NULL;

    if (kanji_nav_screen(&s.nav) == KANJI_SCREEN_ANSWER) {
        ui_card_back_update(k, &s.nav, &s.status);
    } else {
        ui_card_front_update(k, &s.status);
    }
}

void ui_kanji_create(lv_obj_t *parent)
{
    /* Opaque white under both faces. Each face's own root is opaque too, but this one is what
     * the overlay is lifted off and what covers the panel before the first snapshot arrives. */
    lv_obj_t *root = ui_fill_white(parent, 0, 0, UI_W, UI_H);

    /* Neither create() is positioned here. A face builds a pane the full size of the panel and
     * places every widget at the layout's own panel coordinates, so a lv_obj_set_pos() in this
     * file would be a second, silent origin for a grid that has exactly one. */
    s.screen[KANJI_SCREEN_QUESTION] = ui_card_front_create(root);
    s.screen[KANJI_SCREEN_ANSWER]   = ui_card_back_create(root);

    /* Last, so it is the top child and covers whichever face is under it. */
    build_overlay(root);

    kanji_nav_reset(&s.nav);
    s.status.online = true;
    show_only(KANJI_SCREEN_QUESTION);
    refresh_current();
}

void ui_kanji_set_data(const kanji_t *k)
{
    if (k) {
        s.data = *k;
        s.has_data = true;
    } else {
        s.has_data = false;
    }
    refresh_current();
}

void ui_kanji_set_nav(const kanji_nav_t *nav)
{
    if (!nav) return;

    /* A grade being committed changes exactly one thing on the glass: one dock cell inverts to
     * acknowledge the press. Everything else on the spread is the answer the learner is still
     * reading, and it stays there until the next card actually arrives — so this transition
     * redraws the dock alone and the caller refreshes only its rectangle. */
    const bool dock_only = kanji_nav_is_dock_only_transition(&s.nav, nav);
    s.nav = *nav;
    if (dock_only) {
        ui_card_back_dock(s.has_data ? &s.data : NULL, &s.nav);
        return;
    }

    show_only(kanji_nav_screen(&s.nav));
    refresh_current();
}

void ui_kanji_set_status(const ui_status_t *st)
{
    if (!st) return;
    s.status = *st;
    /* The badges moved onto the faces with the rail, so a status change is a face redraw. It is
     * still cheap: nothing here touches the panel, and KanjiTask's fingerprint decides whether a
     * refresh happens at all. */
    refresh_current();
}

void ui_kanji_dock_area(int *x1, int *y1, int *x2, int *y2)
{
    kanji_rect_to_half_open(&kanji_back_layout()->dock, x1, y1, x2, y2);
}
