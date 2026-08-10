/*
 * ui_vault.c — the header, the footer, the overlay, and which page is on top.
 *
 * The pages themselves are four separate files; this one owns only the chrome
 * they share and the routing between them. Keeping it that way is what lets a
 * page be understood, changed and asserted on without reading the rest of the
 * UI — and it is why each page file stays under a couple hundred lines.
 *
 * Nothing here refreshes the panel. See ui_vault.h.
 */
#include "ui_vault.h"
#include "ui_internal.h"
#include "ui_icons.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* --- header geometry ------------------------------------------------------
 * Fixed slots rather than a flex row, for two reasons: the clock is the one
 * thing that gets a targeted partial refresh and a targeted refresh needs a
 * rectangle that does not move when the vault name gets longer; and every slot
 * here has a neighbour it must not touch, which a layout engine would let it do
 * silently.
 *
 * The slots, left to right, and the gaps between them:
 *
 *   14  brand    OBSIDIAN               104     ->118   gap 4
 *   122 vault    <name>, ellipsized     116     ->238   gap 6
 *   244 badge    DEMO / 오래됨 / 오프라인  88     ->332   gap 4
 *   336 date     2026-08-10 (월)        134     ->470   gap 8
 *   478 clock    11:20                   92     ->570   gap 8
 *   578 wifi                             22     ->600   gap 6
 *   606 battery                          28     ->634 = UI_W - UI_PAD
 */
#define H_BRAND_X     UI_PAD
#define H_BRAND_W     104

#define H_VAULT_X     122
#define H_VAULT_W     116

#define H_BADGE_X     244
#define H_BADGE_W     88
#define H_BADGE_Y     11
#define H_BADGE_H     22

#define H_DATE_X      336
#define H_DATE_W      134
#define H_DATE_Y      14

#define H_CLOCK_X     478
#define H_CLOCK_W     92
#define H_CLOCK_Y     6

#define H_NET_SZ      22
#define H_NET_X       578
#define H_NET_Y       11

#define H_ICON_SZ     28
#define H_BATT_X      (UI_W - UI_PAD - H_ICON_SZ)       /* 606 */
#define H_BATT_Y      8

/* --- footer geometry ------------------------------------------------------ */
#define F_Y           (UI_H - UI_FOOTER_H)              /* 446 */
#define F_TEXT_Y      (F_Y + 8)
#define F_TITLE_X     UI_PAD
#define F_TITLE_W     158
#define F_DOT_X       (F_TITLE_X + F_TITLE_W + 6)       /* 178 */
#define F_DOT_SZ      10
#define F_DOT_GAP     16
#define F_LEGEND_X    (F_DOT_X + UI_PAGE_COUNT * F_DOT_GAP + 12)
#define F_LEGEND_W    (UI_W - UI_PAD - F_LEGEND_X)

static lv_obj_t *s_root;
static lv_obj_t *s_pages[UI_PAGE_COUNT];
static ui_page_t s_page;

static lv_obj_t *s_brand;
static lv_obj_t *s_vault;
static lv_obj_t *s_badge_box;
static lv_obj_t *s_badge_txt;
static lv_obj_t *s_date;
static lv_obj_t *s_clock;
static lv_obj_t *s_batt;
static lv_obj_t *s_net;

static lv_obj_t *s_f_title;
static lv_obj_t *s_f_dots[UI_PAGE_COUNT];

static lv_obj_t *s_overlay;
static lv_obj_t *s_ov_title;
static lv_obj_t *s_ov_body;

/* The header text depends on both the snapshot and the status, and the two
 * arrive from different places at different times, so both are remembered. */
static vault_t     s_data;
static bool        s_have_data;
static ui_status_t s_status;

static const char *PAGE_TITLES[UI_PAGE_COUNT] = {
    S_PAGE_STATS, S_PAGE_GRAPH, S_PAGE_AGENTS, S_PAGE_NOTES,
};

const char *ui_vault_page_title(ui_page_t page)
{
    if (page < 0 || page >= UI_PAGE_COUNT) return "";
    return PAGE_TITLES[page];
}

/* --- header --------------------------------------------------------------- */

/* One badge slot, and a priority order for it. Three indicators competing for
 * the same 96 px would either overlap or need a layout pass; ranking them means
 * the header always shows the most important thing wrong, which is all a glance
 * from across a room can carry anyway. */
static void refresh_badge(void)
{
    const char *text = NULL;
    if (s_have_data && s_data.demo) text = S_BADGE_DEMO;
    else if (!s_status.online)      text = S_BADGE_OFFLINE;
    else if (s_status.stale)        text = S_BADGE_STALE;

    ui_show(s_badge_box, text != NULL);
    ui_show(s_badge_txt, text != NULL);
    if (text) ui_set(s_badge_txt, text);
}

/* The vault name is the header's only variable-width text, so it gets its own
 * slot and its own ellipsis rather than being concatenated onto the brand — a
 * combined "OBSIDIAN · <name>" label ellipsizes the brand away first, which
 * reads as a device that has forgotten what it is. */
static void refresh_brand(void)
{
    ui_set(s_vault, (s_have_data && s_data.vault[0]) ? s_data.vault : "");
}

static void build_header(lv_obj_t *par)
{
    s_brand = ui_lab_w(par, H_BRAND_X, 12, H_BRAND_W, UI_F_HEAD,
                       LV_TEXT_ALIGN_LEFT, S_BRAND);
    s_vault = ui_lab_w(par, H_VAULT_X, 14, H_VAULT_W, UI_F_BODY,
                       LV_TEXT_ALIGN_LEFT, "");

    s_badge_box = ui_fill(par, H_BADGE_X, H_BADGE_Y, H_BADGE_W, H_BADGE_H);
    s_badge_txt = ui_lab_inv(par, H_BADGE_X, H_BADGE_Y + 3, H_BADGE_W, UI_F_BODY,
                             LV_TEXT_ALIGN_CENTER, S_BADGE_DEMO);
    ui_show(s_badge_box, false);
    ui_show(s_badge_txt, false);

    s_date  = ui_lab_w(par, H_DATE_X, H_DATE_Y, H_DATE_W, UI_F_BODY,
                       LV_TEXT_ALIGN_RIGHT, "");
    s_clock = ui_lab_w(par, H_CLOCK_X, H_CLOCK_Y, H_CLOCK_W, UI_F_NUM_LG,
                       LV_TEXT_ALIGN_RIGHT, "--:--");

    s_net = ui_icon(par, ICON_WIFI, H_NET_SZ, 0);
    lv_obj_set_pos(s_net, H_NET_X, H_NET_Y);

    s_batt = ui_icon(par, ICON_PLUG, H_ICON_SZ, 0);
    lv_obj_set_pos(s_batt, H_BATT_X, H_BATT_Y);

    ui_fill(par, 0, UI_HEADER_H, UI_W, UI_RULE);
}

/* --- footer --------------------------------------------------------------- */

static void build_footer(lv_obj_t *par)
{
    ui_fill(par, 0, F_Y - UI_RULE, UI_W, UI_RULE);

    s_f_title = ui_lab_w(par, F_TITLE_X, F_TEXT_Y, F_TITLE_W, UI_F_BODY,
                         LV_TEXT_ALIGN_LEFT, "");

    for (int i = 0; i < UI_PAGE_COUNT; i++) {
        s_f_dots[i] = ui_icon(par, ICON_DOT_HOLLOW, F_DOT_SZ, 0);
        lv_obj_set_pos(s_f_dots[i], F_DOT_X + i * F_DOT_GAP, F_TEXT_Y + 5);
    }

    ui_lab_w(par, F_LEGEND_X, F_TEXT_Y, F_LEGEND_W, UI_F_BODY,
             LV_TEXT_ALIGN_RIGHT,
             S_KEY_PAGE " · " S_KEY_REFRESH " · " S_KEY_WIFI);
}

static void refresh_footer(void)
{
    ui_setf(s_f_title, "%d/%d  %s", (int)s_page + 1, (int)UI_PAGE_COUNT,
            ui_vault_page_title(s_page));
    for (int i = 0; i < UI_PAGE_COUNT; i++) {
        ui_icon_set(s_f_dots[i], i == (int)s_page ? ICON_DOT_FULL : ICON_DOT_HOLLOW, 0);
    }
}

/* --- overlay -------------------------------------------------------------- */

static void build_overlay(lv_obj_t *par)
{
    /* Opaque and full-bleed: it has to hide the pages underneath, because on
     * e-Paper "hidden" only means "not drawn this frame" and the previous frame
     * is still physically on the glass until something covers it. */
    s_overlay = ui_frame(par, 0, 0, UI_W, UI_H, 0);
    ui_frame(s_overlay, 40, 120, UI_W - 80, 240, 3);

    s_ov_title = ui_lab_w(s_overlay, 60, 158, UI_W - 120, UI_F_HEAD,
                          LV_TEXT_ALIGN_CENTER, "");
    s_ov_body  = ui_lab_w(s_overlay, 60, 200, UI_W - 120, UI_F_BODY,
                          LV_TEXT_ALIGN_CENTER, "");
    /* The body is the one label on the board allowed to wrap: it carries an
     * AP name and instructions, and an ellipsis there would be useless. */
    ui_lab_wrap(s_ov_body, 140);

    ui_show(s_overlay, false);
}

void ui_vault_set_overlay(const char *title, const char *body)
{
    if (!s_overlay) return;
    if (!title && !body) {
        ui_show(s_overlay, false);
        return;
    }
    ui_set(s_ov_title, title);
    ui_set(s_ov_body, body);
    ui_show(s_overlay, true);
    lv_obj_move_foreground(s_overlay);
}

/* --- public --------------------------------------------------------------- */

void ui_vault_create(lv_obj_t *parent)
{
    s_root = parent;
    lv_obj_remove_style_all(parent);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(parent, UI_W, UI_H);
    lv_obj_set_style_bg_color(parent, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    memset(&s_status, 0, sizeof(s_status));
    s_status.online = true;

    build_header(parent);

    s_pages[UI_PAGE_STATS]  = ui_page_stats_create(parent);
    s_pages[UI_PAGE_GRAPH]  = ui_page_graph_create(parent);
    s_pages[UI_PAGE_AGENTS] = ui_page_agents_create(parent);
    s_pages[UI_PAGE_NOTES]  = ui_page_notes_create(parent);
    for (int i = 0; i < UI_PAGE_COUNT; i++) {
        lv_obj_set_pos(s_pages[i], 0, UI_CONTENT_Y);
        ui_show(s_pages[i], i == 0);
    }

    build_footer(parent);
    build_overlay(parent);

    s_page = UI_PAGE_STATS;
    refresh_footer();
    ui_vault_tick();
}

void ui_vault_show_page(ui_page_t page)
{
    if (page < 0 || page >= UI_PAGE_COUNT) return;
    s_page = page;
    for (int i = 0; i < UI_PAGE_COUNT; i++) {
        ui_show(s_pages[i], i == (int)page);
    }
    refresh_footer();
}

ui_page_t ui_vault_page(void)
{
    return s_page;
}

void ui_vault_set_data(const vault_t *v)
{
    if (v) {
        s_data = *v;
        s_have_data = true;
    } else {
        memset(&s_data, 0, sizeof(s_data));
        s_have_data = false;
    }

    refresh_brand();
    refresh_badge();

    const vault_t *arg = s_have_data ? &s_data : NULL;
    ui_page_stats_update(arg);
    ui_page_graph_update(arg);
    ui_page_agents_update(arg);
    ui_page_notes_update(arg);
}

void ui_vault_set_status(const ui_status_t *st)
{
    if (!st) return;
    s_status = *st;

    ui_icon_set(s_net, st->online ? ICON_WIFI : ICON_WIFI_OFF, 0);
    /* No cell fitted is not 0% — showing an empty battery on a board running
     * from USB is a false alarm somebody will chase. */
    if (st->battery_present) ui_icon_set(s_batt, ICON_BATTERY, st->battery_pct);
    else                     ui_icon_set(s_batt, ICON_PLUG, 0);

    refresh_badge();
}

void ui_vault_tick(void)
{
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);

    static const char *WD[7] = { "일", "월", "화", "수", "목", "금", "토" };
    int wd = lt.tm_wday;
    if (wd < 0 || wd > 6) wd = 0;

    ui_setf(s_date, "%04d-%02d-%02d (%s)",
            lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, WD[wd]);
    ui_setf(s_clock, "%02d:%02d", lt.tm_hour, lt.tm_min);
}

void ui_vault_clock_area(int *x1, int *y1, int *x2, int *y2)
{
    /* The date and the clock share the rectangle: both change on the same tick,
     * and refreshing two windows costs two panel updates. */
    if (x1) *x1 = H_DATE_X - 4;
    if (y1) *y1 = 0;
    if (x2) *x2 = H_CLOCK_X + H_CLOCK_W + 4;
    if (y2) *y2 = UI_HEADER_H;
}
