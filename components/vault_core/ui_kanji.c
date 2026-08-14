/* Shared rail, masthead, footer, router, and setup overlay. */
#include "ui_kanji.h"

#include <stdio.h>

#include "ui_internal.h"

typedef struct {
    lv_obj_t *identity_fill;
    lv_obj_t *identity;
    lv_obj_t *progress;
    lv_obj_t *session;
    lv_obj_t *battery;
    lv_obj_t *keycap[4];
    lv_obj_t *key_action[4];
    lv_obj_t *screen[KANJI_SCREEN_COUNT];
    lv_obj_t *overlay;
    lv_obj_t *overlay_title;
    lv_obj_t *overlay_body;
    kanji_t data;
    bool has_data;
    kanji_nav_t nav;
    ui_status_t status;
} kanji_ui_t;

static kanji_ui_t s;

static void build_chrome(lv_obj_t *par)
{
    const kanji_chrome_t *c = kanji_chrome_layout();

    ui_rule(par, c->rail_rule.x, c->rail_rule.y,
            c->rail_rule.w, c->rail_rule.h);

    s.identity_fill = ui_fill(par, c->rail_identity.x,
                              c->rail_identity.y + 16,
                              c->rail_identity.w, 40);
    s.identity = ui_lab_w(par, c->rail_identity.x,
                          c->rail_identity.y + 24, c->rail_identity.w,
                          UI_F_HEAD, LV_TEXT_ALIGN_CENTER, "");
    s.progress = ui_lab_w(par, c->rail_progress.x,
                          c->rail_progress.y + 28, c->rail_progress.w,
                          UI_F_UTILITY, LV_TEXT_ALIGN_CENTER, "");

    ui_lab_w(par, c->brand.x, c->brand.y, c->brand.w,
             UI_F_UTILITY, LV_TEXT_ALIGN_LEFT, S_BRAND);
    s.session = ui_lab_w(par, c->session.x, c->session.y, c->session.w,
                         UI_F_HEAD, LV_TEXT_ALIGN_LEFT, "");
    s.battery = ui_lab_w(par, c->battery.x, c->battery.y, c->battery.w,
                         UI_F_BODY, LV_TEXT_ALIGN_RIGHT, "");

    for (int i = 0; i < 4; i++) {
        s.keycap[i] = ui_lab_w(par, c->keycap[i].x, c->keycap[i].y,
                               c->keycap[i].w, UI_F_UTILITY,
                               LV_TEXT_ALIGN_LEFT, "");
        s.key_action[i] = ui_lab_w(par, c->key_action[i].x,
                                   c->key_action[i].y, c->key_action[i].w,
                                   UI_F_HEAD, LV_TEXT_ALIGN_LEFT, "");
    }
}

static void update_rail(void)
{
    const kanji_t *k = s.has_data ? &s.data : NULL;
    const bool exceptional = !s.status.online ||
                             (s.has_data && s.data.demo) || s.status.stale;
    const char *identity = "";

    if (!s.status.online) identity = S_BADGE_OFFLINE;
    else if (s.has_data && s.data.demo) identity = S_BADGE_DEMO;
    else if (s.status.stale) identity = S_BADGE_STALE;
    else if (s.nav.sheet != KANJI_SHEET_NONE) {
        identity = kanji_screen_title(kanji_nav_screen(&s.nav));
    } else if (s.has_data && s.data.session.complete) {
        identity = S_RAIL_COMPLETE;
    } else if (!s.has_data || !s.data.card.valid) {
        identity = S_RAIL_EMPTY;
    } else if (s.has_data && s.data.session.level[0]) {
        identity = s.data.session.level;
    } else {
        identity = S_RAIL_EMPTY;
    }

    ui_set(s.identity, identity);
    ui_show(s.identity_fill, exceptional);
    lv_obj_set_style_text_color(s.identity,
                                exceptional ? lv_color_white() : lv_color_black(), 0);

    if (s.nav.sheet != KANJI_SHEET_NONE) {
        const int pages = kanji_sheet_pages(k, s.nav.sheet);
        int page = s.nav.sheet_page;
        if (page < 0 || page >= pages) page = 0;
        ui_setf(s.progress, "%d/%d", page + 1, pages);
    } else if (s.has_data && s.data.session.track_total > 0) {
        ui_setf(s.progress, "%d/%d", s.data.session.track,
                s.data.session.track_total);
    } else {
        ui_set(s.progress, "");
    }
}

static void update_masthead(void)
{
    if (s.has_data) {
        ui_setf(s.session, "%s %d · %s %d", S_STREAK, s.data.session.streak,
                S_REVIEWED_TODAY, s.data.session.reviewed_today);
    } else {
        ui_set(s.session, "");
    }

    if (s.status.battery_present && s.status.battery_pct >= 0 &&
        s.status.battery_pct <= 20) {
        ui_setf(s.battery, "%s %d%%", S_BATTERY, s.status.battery_pct);
    } else {
        ui_set(s.battery, "");
    }
}

static void update_footer(void)
{
    static const char *KEYS[4] = { S_KEY0, S_KEY1, S_KEY2, S_BOOT };
    const kanji_button_t buttons[4] = {
        KANJI_BTN_KEY0, KANJI_BTN_KEY1, KANJI_BTN_KEY2, KANJI_BTN_BOOT,
    };
    const char *actions[4] = {
        kanji_nav_hint_key0(&s.nav), kanji_nav_hint_key1(&s.nav),
        S_KEY_REFRESH, kanji_nav_hint_boot(&s.nav),
    };
    const kanji_t *k = s.has_data ? &s.data : NULL;

    for (int i = 0; i < 4; i++) {
        const bool on = kanji_nav_can_press(&s.nav, buttons[i], k);
        ui_set(s.keycap[i], KEYS[i]);
        ui_set(s.key_action[i], actions[i]);
        ui_show(s.keycap[i], on);
        ui_show(s.key_action[i], on);
    }
}

static void update_chrome(void)
{
    update_rail();
    update_masthead();
    update_footer();
}

void ui_sheet_band_create(lv_obj_t *par, ui_sheet_band_t *out, const char *title)
{
    const kanji_sheet_layout_t *l = kanji_sheet_layout(false);
    out->word = ui_lab_w(par, LOCAL_X(l->headword.x), LOCAL_Y(l->headword.y),
                         l->headword.w, UI_F_TITLE, LV_TEXT_ALIGN_LEFT, "");
    out->title = ui_lab_w(par, LOCAL_X(l->title.x), LOCAL_Y(l->title.y),
                          l->title.w, UI_F_HEAD, LV_TEXT_ALIGN_RIGHT, title);
}

void ui_sheet_band_update(const ui_sheet_band_t *band, const kanji_t *k)
{
    if (!band) return;
    const bool have = k && k->card.valid;
    ui_setf(band->word, "%s%s%s", have ? k->card.front : "",
            have && k->card.reading[0] ? "  " : "",
            have ? k->card.reading : "");
}

void ui_pager_set(lv_obj_t *pager, int page, int pages)
{
    if (!pager) return;
    if (pages <= 1) ui_set(pager, "");
    else ui_setf(pager, "%d/%d", page + 1, pages);
}

static void build_overlay(lv_obj_t *par)
{
    s.overlay = ui_fill_white(par, 0, 0, UI_W, UI_H);
    ui_fill(s.overlay, UI_PAD, 64, UI_W - 2 * UI_PAD, 2);
    s.overlay_title = ui_lab_w(s.overlay, UI_PAD, 88, UI_W - 2 * UI_PAD,
                               UI_F_TITLE, LV_TEXT_ALIGN_LEFT, "");
    s.overlay_body = ui_lab_w(s.overlay, UI_PAD, 144, UI_W - 2 * UI_PAD,
                              UI_F_HEAD, LV_TEXT_ALIGN_LEFT, "");
    ui_lab_wrap(s.overlay_body, 240);
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

static void show_only(kanji_screen_t which)
{
    for (int i = 0; i < KANJI_SCREEN_COUNT; i++) {
        ui_show(s.screen[i], i == (int)which);
    }
}

static void refresh_current(void)
{
    const kanji_t *k = s.has_data ? &s.data : NULL;
    switch (kanji_nav_screen(&s.nav)) {
    case KANJI_SCREEN_ANSWER:
        ui_card_answer_update(k, s.nav.grade);
        break;
    case KANJI_SCREEN_DESCRIPTION:
        ui_sheet_desc_update(k, s.nav.sheet_page);
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
    lv_obj_t *root = ui_fill_white(parent, 0, 0, UI_W, UI_H);
    const kanji_chrome_t *c = kanji_chrome_layout();
    build_chrome(root);
    s.screen[KANJI_SCREEN_QUESTION] = ui_card_question_create(root);
    s.screen[KANJI_SCREEN_ANSWER] = ui_card_answer_create(root);
    s.screen[KANJI_SCREEN_DESCRIPTION] = ui_sheet_desc_create(root);
    s.screen[KANJI_SCREEN_COMMENTS] = ui_sheet_comments_create(root);
    s.screen[KANJI_SCREEN_FSRS] = ui_sheet_fsrs_create(root);
    for (int i = 0; i < KANJI_SCREEN_COUNT; i++) {
        lv_obj_set_pos(s.screen[i], c->main.x, c->main.y);
    }
    build_overlay(root);
    kanji_nav_reset(&s.nav);
    s.status.online = true;
    show_only(KANJI_SCREEN_QUESTION);
    update_chrome();
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
    update_chrome();
    refresh_current();
}

void ui_kanji_set_nav(const kanji_nav_t *nav)
{
    if (!nav) return;
    const bool dock_only = kanji_nav_is_grade_only_transition(&s.nav, nav);
    s.nav = *nav;
    if (dock_only) {
        ui_card_answer_dock(s.has_data ? &s.data : NULL, s.nav.grade);
        return;
    }
    show_only(kanji_nav_screen(&s.nav));
    update_chrome();
    refresh_current();
}

void ui_kanji_set_status(const ui_status_t *st)
{
    if (!st) return;
    s.status = *st;
    update_chrome();
}

void ui_kanji_dock_area(int *x1, int *y1, int *x2, int *y2)
{
    kanji_rect_to_half_open(&kanji_answer_layout()->dock, x1, y1, x2, y2);
}
