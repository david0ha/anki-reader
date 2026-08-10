/*
 * ui_page_notes.c — page 3, 최근 노트.
 *
 *   최근 수정                  |  수신함 (11)
 *   -------------------------  |  ---------------------------
 *   21:02  주간 회고      12 ↔ |  [] todo: 스펙 정리      3일
 *   20:41  UC8179 정리     4 ↔ |  [] 회의록 미정리        2일
 *
 * Two independent lists side by side. Both carry titles that came off the
 * network, so both draw from the full 완성형 face and both ellipsize — this is
 * the page where a subset font would have failed on somebody's note title.
 */
#include "ui_internal.h"

#define NT_HEAD_Y       2
#define NT_RULE_Y       30
#define NT_ROW_Y        40
#define NT_ROW_H        42

#define NT_SPLIT_X      332

/* left: recent */
#define RC_X            UI_PAD
#define RC_W            (NT_SPLIT_X - UI_PAD - 10)          /* 308 */
#define RC_TIME_W       50
#define RC_TITLE_X      (RC_X + RC_TIME_W + 8)              /* 72  */
#define RC_LINKS_W      52
#define RC_LINKS_X      (RC_X + RC_W - RC_LINKS_W)          /* 270 */
#define RC_TITLE_W      (RC_LINKS_X - RC_TITLE_X - 8)       /* 190 */

/* right: inbox */
#define IB_X            (NT_SPLIT_X + 14)                   /* 346 */
#define IB_W            (UI_W - UI_PAD - IB_X)              /* 288 */
#define IB_BOX_SZ       12
#define IB_TITLE_X      (IB_X + IB_BOX_SZ + 10)             /* 368 */
#define IB_AGE_W        56
#define IB_AGE_X        (IB_X + IB_W - IB_AGE_W)            /* 578 */
#define IB_TITLE_W      (IB_AGE_X - IB_TITLE_X - 8)         /* 202 */

static lv_obj_t *s_root;
static lv_obj_t *s_inbox_head;

static lv_obj_t *s_rc_time[VAULT_RECENT_MAX];
static lv_obj_t *s_rc_title[VAULT_RECENT_MAX];
static lv_obj_t *s_rc_links[VAULT_RECENT_MAX];
static lv_obj_t *s_rc_empty;

static lv_obj_t *s_ib_box[VAULT_INBOX_MAX];
static lv_obj_t *s_ib_title[VAULT_INBOX_MAX];
static lv_obj_t *s_ib_age[VAULT_INBOX_MAX];
static lv_obj_t *s_ib_empty;

lv_obj_t *ui_page_notes_create(lv_obj_t *par)
{
    s_root = ui_pane(par, 0, 0, UI_W, UI_CONTENT_H);

    ui_lab(s_root, RC_X, NT_HEAD_Y, UI_F_HEAD, S_RECENT);
    ui_fill(s_root, RC_X, NT_RULE_Y, RC_W, 1);

    s_inbox_head = ui_lab_w(s_root, IB_X, NT_HEAD_Y, IB_W, UI_F_HEAD,
                            LV_TEXT_ALIGN_LEFT, S_INBOX);
    ui_fill(s_root, IB_X, NT_RULE_Y, IB_W, 1);

    /* The divider runs the full height of the content area, not just the rows:
     * two ragged-bottomed lists with no rule between them read as one list. */
    ui_fill(s_root, NT_SPLIT_X, NT_HEAD_Y, 1, UI_CONTENT_H - NT_HEAD_Y - 4);

    s_rc_empty = ui_lab_w(s_root, RC_X, 150, RC_W, UI_F_BODY,
                          LV_TEXT_ALIGN_CENTER, S_EMPTY_RECENT);
    s_ib_empty = ui_lab_w(s_root, IB_X, 150, IB_W, UI_F_BODY,
                          LV_TEXT_ALIGN_CENTER, S_EMPTY_INBOX);
    ui_show(s_rc_empty, false);
    ui_show(s_ib_empty, false);

    for (int i = 0; i < VAULT_RECENT_MAX; i++) {
        int y = NT_ROW_Y + i * NT_ROW_H;
        s_rc_time[i]  = ui_lab_w(s_root, RC_X, y, RC_TIME_W, UI_F_BODY,
                                 LV_TEXT_ALIGN_LEFT, "");
        s_rc_title[i] = ui_lab_w(s_root, RC_TITLE_X, y, RC_TITLE_W, UI_F_BODY,
                                 LV_TEXT_ALIGN_LEFT, "");
        s_rc_links[i] = ui_lab_w(s_root, RC_LINKS_X, y, RC_LINKS_W, UI_F_BODY,
                                 LV_TEXT_ALIGN_RIGHT, "");
    }

    for (int i = 0; i < VAULT_INBOX_MAX; i++) {
        int y = NT_ROW_Y + i * NT_ROW_H;
        s_ib_box[i]   = ui_frame(s_root, IB_X, y + 4, IB_BOX_SZ, IB_BOX_SZ, 2);
        s_ib_title[i] = ui_lab_w(s_root, IB_TITLE_X, y, IB_TITLE_W, UI_F_BODY,
                                 LV_TEXT_ALIGN_LEFT, "");
        s_ib_age[i]   = ui_lab_w(s_root, IB_AGE_X, y, IB_AGE_W, UI_F_BODY,
                                 LV_TEXT_ALIGN_RIGHT, "");
    }

    return s_root;
}

void ui_page_notes_update(const vault_t *v)
{
    if (!s_root) return;

    int nr = (v && v->valid) ? v->recent_count : 0;
    int ni = (v && v->valid) ? v->inbox_count : 0;
    if (nr > VAULT_RECENT_MAX) nr = VAULT_RECENT_MAX;
    if (ni > VAULT_INBOX_MAX)  ni = VAULT_INBOX_MAX;

    ui_show(s_rc_empty, nr == 0);
    ui_show(s_ib_empty, ni == 0);

    for (int i = 0; i < VAULT_RECENT_MAX; i++) {
        bool on = i < nr;
        ui_show(s_rc_time[i], on);
        ui_show(s_rc_title[i], on);
        ui_show(s_rc_links[i], on);
        if (!on) continue;
        const vault_note_t *n = &v->recent[i];
        ui_set(s_rc_time[i], n->time);
        ui_set(s_rc_title[i], n->title);
        ui_setf(s_rc_links[i], "%d ↔", n->links);
    }

    /* The header carries the real queue length; the list shows what fits. A
     * count that silently equalled the number of visible rows would turn a
     * backlog of forty into a comfortable eight. */
    if (v && v->valid && v->inbox_total > 0) {
        ui_setf(s_inbox_head, "%s (%d)", S_INBOX, v->inbox_total);
    } else {
        ui_set(s_inbox_head, S_INBOX);
    }

    for (int i = 0; i < VAULT_INBOX_MAX; i++) {
        bool on = i < ni;
        ui_show(s_ib_box[i], on);
        ui_show(s_ib_title[i], on);
        ui_show(s_ib_age[i], on);
        if (!on) continue;
        const vault_inbox_t *n = &v->inbox[i];
        ui_set(s_ib_title[i], n->title);
        ui_setf(s_ib_age[i], "%d%s", n->age_days, S_DAYS_SUFFIX);
    }
}
