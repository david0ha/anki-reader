/*
 * ui_kanji.c — the chrome, the router and the overlay.
 *
 * The chrome is the inverted band at the top and the key legend at the bottom;
 * everything between them belongs to whichever screen the nav state names. The
 * router's whole job is to keep exactly one of those panes visible and to hand
 * it the snapshot — it never draws content itself, because a router that knows
 * what a card looks like is a router that has to change when a card does.
 *
 * The footer legend is rebuilt on every nav change rather than fixed at boot.
 * That is deliberate: KEY0 means 정답 on one screen and 등급 on the next, and a
 * legend that does not follow is a lie printed in 16 px.
 */
#include "ui_kanji.h"

#include <stdio.h>

#include "ui_icons.h"
#include "ui_internal.h"

typedef struct {
    /* chrome */
    lv_obj_t *header;
    lv_obj_t *chips;
    lv_obj_t *track;
    lv_obj_t *badge;          /* DEMO / 오래됨 / 오프라인, or hidden */
    lv_obj_t *battery;
    lv_obj_t *key[4];

    /* the screens, indexed by kanji_screen_t */
    lv_obj_t *screen[KANJI_SCREEN_COUNT];

    /* overlay */
    lv_obj_t *overlay;
    lv_obj_t *overlay_title;
    lv_obj_t *overlay_body;

    /* the last thing pushed in, so a nav change can redraw the screen it
     * switches to without the caller having to re-send the card */
    kanji_t       data;
    bool          has_data;
    kanji_nav_t   nav;
    ui_status_t   status;
} kanji_ui_t;

static kanji_ui_t s;

/* --- the header ----------------------------------------------------------- */

/* The wordmark is drawn, not written: "Kanjis.ai" in the web app puts a red
 * play badge before the word, and red is the one thing this panel cannot say.
 * A filled triangle in a filled square carries the same shape at 1 bit. */
static void brand_draw_cb(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    lv_layer_t *L = lv_event_get_layer(e);
    if (!L) return;

    lv_area_t a;
    lv_obj_get_coords(o, &a);
    const int x = a.x1, y = a.y1;
    const int h = a.y2 - a.y1 + 1;

    /* A white rounded-off badge on the inverted header, with a black play
     * triangle punched out of it. */
    ui_draw_rect_abs(L, x, y + 2, x + 20, y + h - 3, true, 0, true);
    for (int i = 0; i < 8; i++) {
        ui_draw_line_abs(L, x + 6 + i, y + 6 + i, x + 6 + i, y + h - 7 - i, 1, false);
    }
}

static void build_header(lv_obj_t *par)
{
    const kanji_chrome_t *c = kanji_chrome_layout();

    s.header = ui_fill(par, c->header.x, c->header.y, c->header.w, c->header.h);
    ui_fill(par, 0, c->rule_top, UI_W, UI_RULE);

    lv_obj_t *mark = ui_pane(s.header, 0, 0, 22, c->header.h);
    lv_obj_add_event_cb(mark, brand_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_set_pos(mark, c->brand.x, 0);

    ui_lab_inv(s.header, c->brand.x + 26, c->brand.y + 2, c->brand.w - 26,
               UI_F_HEAD, LV_TEXT_ALIGN_LEFT, S_BRAND);

    /* The two stat chips are one label, not two: 연속 and 오늘 always appear
     * together and always in that order, and one label cannot half-update. */
    s.chips = ui_lab_inv(s.header, c->chips.x, c->chips.y, c->chips.w,
                         UI_F_BODY, LV_TEXT_ALIGN_RIGHT, "");

    s.track = ui_lab_inv(s.header, c->track.x, c->track.y, c->track.w,
                         UI_F_NUM_SM, LV_TEXT_ALIGN_RIGHT, "");

    /* The badge and the battery share the header's left half with the brand,
     * below it — there is no room beside it and a 44 px band has two rows. */
    s.badge = ui_lab_inv(s.header, c->brand.x + 26 + 96, c->brand.y + 5, 96,
                         UI_F_BODY, LV_TEXT_ALIGN_LEFT, "");
    s.battery = ui_icon(s.header, ICON_PLUG, 18, 0);
    lv_obj_set_pos(s.battery, c->chips.x - 30, c->chips.y);
}

/* --- the footer ----------------------------------------------------------- */

static void build_footer(lv_obj_t *par)
{
    const kanji_chrome_t *c = kanji_chrome_layout();

    ui_fill(par, 0, c->rule_bottom, UI_W, UI_RULE);
    for (int i = 0; i < 4; i++) {
        s.key[i] = ui_lab_w(par, c->key[i].x, c->key[i].y, c->key[i].w,
                            UI_F_BODY, LV_TEXT_ALIGN_LEFT, "");
    }
}

static void update_footer(void)
{
    /* KEY2 is the only button whose meaning never changes, so it is the only
     * one written from a constant. */
    ui_setf(s.key[0], "%s %s", S_KEY0, kanji_nav_hint_key0(&s.nav));
    ui_setf(s.key[1], "%s %s", S_KEY1, kanji_nav_hint_key1(&s.nav));
    ui_setf(s.key[2], "%s %s", S_KEY2, S_KEY_REFRESH);
    ui_setf(s.key[3], "%s %s", S_BOOT, kanji_nav_hint_boot(&s.nav));
}

/* --- the header's live values --------------------------------------------- */

static void update_header(void)
{
    const kanji_session_t *ss = &s.data.session;

    if (s.has_data) {
        ui_setf(s.chips, "%s %d  ·  %s %d",
                S_STREAK, ss->streak, S_REVIEWED_TODAY, ss->reviewed_today);
        if (ss->track_total > 0) {
            ui_setf(s.track, "%s %d/%d", S_TRACK, ss->track, ss->track_total);
        } else {
            ui_set(s.track, "");
        }
    } else {
        ui_set(s.chips, "");
        ui_set(s.track, "");
    }

    /* One badge, in priority order: a board that is offline is offline whatever
     * else is also true of it. */
    if (!s.status.online)      ui_set(s.badge, S_BADGE_OFFLINE);
    else if (s.data.demo)      ui_set(s.badge, S_BADGE_DEMO);
    else if (s.status.stale)   ui_set(s.badge, S_BADGE_STALE);
    else                       ui_set(s.badge, "");

    ui_icon_set(s.battery,
                s.status.battery_present ? ICON_BATTERY : ICON_PLUG,
                s.status.battery_pct);
}

/* --- the sheets' shared strip --------------------------------------------- */

void ui_sheet_band_create(lv_obj_t *par, ui_sheet_band_t *out, const char *title)
{
    const kanji_sheet_layout_t *l = kanji_sheet_layout(false);
    const int y = LOCAL_Y(l->band.y);                             /* pane-local */

    ui_fill(par, l->band.x, y, l->band.w, l->band.h);
    out->word = ui_lab_inv(par, l->band_word.x,
                           y + (l->band_word.y - l->band.y), l->band_word.w,
                           UI_F_TITLE, LV_TEXT_ALIGN_LEFT, "");
    out->title = ui_lab_inv(par, l->band_title.x,
                            y + (l->band_title.y - l->band.y), l->band_title.w,
                            UI_F_HEAD, LV_TEXT_ALIGN_RIGHT, title);
}

void ui_sheet_band_update(const ui_sheet_band_t *band, const kanji_t *k)
{
    if (!band) return;
    const bool have = k && k->card.valid;
    ui_setf(band->word, "%s%s%s",
            have ? k->card.front : "",
            have && k->card.reading[0] ? "  " : "",
            have ? k->card.reading : "");
}

void ui_pager_set(lv_obj_t *pager, int page, int pages)
{
    if (!pager) return;
    /* A pager that always says 1/1 trains the eye to ignore it, and then the
     * one time it says 2/3 nobody sees it. */
    if (pages <= 1) {
        ui_set(pager, "");
        return;
    }
    ui_setf(pager, "%d/%d", page + 1, pages);
}

/* --- the overlay ---------------------------------------------------------- */

static void build_overlay(lv_obj_t *par)
{
    s.overlay = ui_pane(par, 0, 0, UI_W, UI_H);
    lv_obj_set_style_bg_color(s.overlay, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s.overlay, LV_OPA_COVER, 0);

    ui_fill(s.overlay, 0, 0, UI_W, 6);
    s.overlay_title = ui_lab_w(s.overlay, UI_PAD, 120, UI_W - 2 * UI_PAD,
                               UI_F_TITLE, LV_TEXT_ALIGN_CENTER, "");
    s.overlay_body = ui_lab_w(s.overlay, UI_PAD, 176, UI_W - 2 * UI_PAD,
                              UI_F_HEAD, LV_TEXT_ALIGN_CENTER, "");
    /* The overlay's body is the one place on this board that wraps rather than
     * ellipsizes: it carries an AP name and the instructions for joining it,
     * and an ellipsis in the middle of either is useless. */
    ui_lab_wrap(s.overlay_body, 180);
    ui_show(s.overlay, false);
}

void ui_kanji_set_overlay(const char *title, const char *body)
{
    if (!s.overlay) return;
    const bool on = title != NULL;
    if (on) {
        ui_set(s.overlay_title, title);
        ui_set(s.overlay_body, body ? body : "");
    }
    ui_show(s.overlay, on);
}

/* --- the router ----------------------------------------------------------- */

static void show_only(kanji_screen_t which)
{
    for (int i = 0; i < KANJI_SCREEN_COUNT; i++) {
        ui_show(s.screen[i], i == (int)which);
    }
}

/* Repaint whichever screen is on glass. Called on both a data change and a nav
 * change, because a sheet that was built for the previous card and merely
 * un-hidden would show it. */
static void refresh_current(void)
{
    const kanji_t *k = s.has_data ? &s.data : NULL;

    switch (kanji_nav_screen(&s.nav)) {
    case KANJI_SCREEN_ANSWER:
        ui_card_answer_update(k, s.nav.grade);
        break;
    case KANJI_SCREEN_DESCRIPTION:
        ui_sheet_desc_update(k);
        break;
    case KANJI_SCREEN_COMMENTS:
        ui_sheet_comments_update(k, s.nav.sheet_page);
        break;
    case KANJI_SCREEN_FSRS:
        ui_sheet_fsrs_update(k, s.nav.sheet_page);
        break;
    case KANJI_SCREEN_QUESTION:
    default:
        ui_card_question_update(k);
        break;
    }
}

void ui_kanji_create(lv_obj_t *parent)
{
    lv_obj_t *root = ui_pane(parent, 0, 0, UI_W, UI_H);
    lv_obj_set_style_bg_color(root, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    build_header(root);
    build_footer(root);

    const kanji_chrome_t *c = kanji_chrome_layout();
    s.screen[KANJI_SCREEN_QUESTION]    = ui_card_question_create(root);
    s.screen[KANJI_SCREEN_ANSWER]      = ui_card_answer_create(root);
    s.screen[KANJI_SCREEN_DESCRIPTION] = ui_sheet_desc_create(root);
    s.screen[KANJI_SCREEN_COMMENTS]    = ui_sheet_comments_create(root);
    s.screen[KANJI_SCREEN_FSRS]        = ui_sheet_fsrs_create(root);
    for (int i = 0; i < KANJI_SCREEN_COUNT; i++) {
        lv_obj_set_pos(s.screen[i], c->content.x, c->content.y);
    }

    build_overlay(root);

    kanji_nav_reset(&s.nav);
    s.status.online = true;
    show_only(KANJI_SCREEN_QUESTION);
    update_footer();
    update_header();
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
    update_header();
    refresh_current();
}

void ui_kanji_set_nav(const kanji_nav_t *nav)
{
    if (!nav) return;
    s.nav = *nav;
    show_only(kanji_nav_screen(&s.nav));
    update_footer();
    refresh_current();
}

void ui_kanji_set_status(const ui_status_t *st)
{
    if (!st) return;
    s.status = *st;
    update_header();
}

void ui_kanji_dock_area(int *x1, int *y1, int *x2, int *y2)
{
    const kanji_answer_layout_t *a = kanji_answer_layout();
    if (x1) *x1 = a->dock.x;
    if (y1) *y1 = a->dock.y;
    if (x2) *x2 = a->dock.x + a->dock.w - 1;
    if (y2) *y2 = a->dock.y + a->dock.h - 1;
}
