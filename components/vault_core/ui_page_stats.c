/*
 * ui_page_stats.c — page 0, 볼트 통계.
 *
 *   +------------------------------------------------------------------+
 *   |   1,428    |   3,910    |     37     |      212                   |  counters
 *   |    노트    |    링크    |    고아    |      태그                  |
 *   +------------------------------------------------------------------+
 *   | 최근 7일 활동                     오늘 +6 · 이번 주 +41           |
 *   |    _   #   #   _   .   #   #                                      |  activity
 *   +------------------------------------------------------------------+
 *   | 상위 태그              | 볼트 상태                                |
 *   | 프로젝트 ####### 186   | 링크 밀도        2.74 개 / 노트          |
 *   | 데일리   #####   141   | 고아 비율        2.6 %                   |
 *   +------------------------------------------------------------------+
 *
 * The grid is fixed and absolute. Nothing here reflows: a dashboard whose rows
 * move when a number gains a digit is unreadable at a glance, and on e-Paper a
 * reflow means the whole panel changed and has to be fully refreshed.
 */
#include "ui_internal.h"

#include <stdio.h>

/* --- grid ----------------------------------------------------------------- */

#define CW              (UI_W - 2 * UI_PAD)            /* 620 content width */

#define CELL_W          (CW / 4)                       /* 155 */
#define CELL_NUM_Y      12
#define CELL_CAP_Y      72
#define ROW_A_H         112

#define ACT_Y           118
#define ACT_HEAD_H      26
#define ACT_PLOT_Y      (ACT_Y + ACT_HEAD_H + 4)       /* 148 */
#define ACT_PLOT_H      86
#define ACT_BASE_Y      (ACT_PLOT_Y + ACT_PLOT_H)      /* 234 */
#define ACT_SLOT_W      (CW / VAULT_DAILY_DAYS)        /* 88  */
#define ACT_BAR_W       (ACT_SLOT_W - 14)              /* 74  */
#define ACT_VAL_H       18

#define ROW_C_Y         248
#define ROW_C_ROW_H     20
#define ROW_C_FIRST     (ROW_C_Y + 28)                 /* 276 */

#define TAG_X           UI_PAD
#define TAG_NAME_W      92
#define TAG_BAR_X       (TAG_X + TAG_NAME_W + 8)       /* 114 */
#define TAG_BAR_W       138
#define TAG_CNT_X       (TAG_BAR_X + TAG_BAR_W + 6)    /* 258 */
#define TAG_CNT_W       46

#define SPLIT_X         324
#define HEALTH_X        (SPLIT_X + 20)                 /* 344 */
#define HEALTH_W        (UI_W - UI_PAD - HEALTH_X)     /* 290 */
#define HEALTH_LAB_W    130
#define HEALTH_VAL_X    (HEALTH_X + HEALTH_LAB_W)
#define HEALTH_VAL_W    (HEALTH_W - HEALTH_LAB_W)

#define HEALTH_ROWS     4

static const char *CAPTIONS[4] = { S_STAT_NOTES, S_STAT_LINKS, S_STAT_ORPHANS, S_STAT_TAGS };

static lv_obj_t *s_root;
static lv_obj_t *s_num[4];
static lv_obj_t *s_added;

static lv_obj_t *s_bar[VAULT_DAILY_DAYS];
static lv_obj_t *s_bar_val[VAULT_DAILY_DAYS];

static lv_obj_t *s_tag_name[VAULT_TAGS_MAX];
static lv_obj_t *s_tag_bar[VAULT_TAGS_MAX];
static lv_obj_t *s_tag_cnt[VAULT_TAGS_MAX];

static lv_obj_t *s_health_val[HEALTH_ROWS];

lv_obj_t *ui_page_stats_create(lv_obj_t *par)
{
    s_root = ui_pane(par, 0, 0, UI_W, UI_CONTENT_H);

    /* --- the four counters ------------------------------------------------ */
    for (int i = 0; i < 4; i++) {
        int x = UI_PAD + i * CELL_W;
        s_num[i] = ui_lab_w(s_root, x, CELL_NUM_Y, CELL_W, UI_F_NUM_XL,
                            LV_TEXT_ALIGN_CENTER, "0");
        ui_lab_w(s_root, x, CELL_CAP_Y, CELL_W, UI_F_HEAD,
                 LV_TEXT_ALIGN_CENTER, CAPTIONS[i]);
        /* Hairline separators between cells, not around them: a full box grid
         * at this size reads as a table of unrelated numbers. */
        if (i > 0) ui_fill(s_root, x - 1, CELL_NUM_Y + 8, 1, 76);
    }
    ui_fill(s_root, UI_PAD, ROW_A_H, CW, 1);

    /* --- activity --------------------------------------------------------- */
    ui_lab(s_root, UI_PAD, ACT_Y, UI_F_HEAD, S_ACTIVITY);
    s_added = ui_lab_w(s_root, 300, ACT_Y + 5, CW - 300 + UI_PAD, UI_F_BODY,
                       LV_TEXT_ALIGN_RIGHT, "");

    /* The baseline is drawn even when every bar is zero, so an empty week reads
     * as "nothing happened" rather than as a page that failed to render. */
    ui_fill(s_root, UI_PAD, ACT_BASE_Y, CW, 1);

    for (int i = 0; i < VAULT_DAILY_DAYS; i++) {
        int x = UI_PAD + i * ACT_SLOT_W + (ACT_SLOT_W - ACT_BAR_W) / 2;
        s_bar[i]     = ui_fill(s_root, x, ACT_BASE_Y - 2, ACT_BAR_W, 2);
        s_bar_val[i] = ui_lab_w(s_root, x - 6, ACT_BASE_Y - 20, ACT_BAR_W + 12,
                                UI_F_NUM_SM, LV_TEXT_ALIGN_CENTER, "");
    }
    ui_fill(s_root, UI_PAD, ACT_BASE_Y + 8, CW, 1);

    /* --- top tags --------------------------------------------------------- */
    ui_lab(s_root, TAG_X, ROW_C_Y, UI_F_HEAD, S_TOP_TAGS);
    for (int i = 0; i < VAULT_TAGS_MAX; i++) {
        int y = ROW_C_FIRST + i * ROW_C_ROW_H;
        s_tag_name[i] = ui_lab_w(s_root, TAG_X, y, TAG_NAME_W, UI_F_BODY,
                                 LV_TEXT_ALIGN_LEFT, "");
        s_tag_bar[i]  = ui_fill(s_root, TAG_BAR_X, y + 5, 1, 10);
        s_tag_cnt[i]  = ui_lab_w(s_root, TAG_CNT_X, y, TAG_CNT_W, UI_F_NUM_SM,
                                 LV_TEXT_ALIGN_RIGHT, "");
    }

    /* --- vault health ----------------------------------------------------- */
    ui_fill(s_root, SPLIT_X, ROW_C_Y, 1, UI_CONTENT_H - ROW_C_Y - 6);
    ui_lab(s_root, HEALTH_X, ROW_C_Y, UI_F_HEAD, S_HEALTH);

    static const char *LABELS[HEALTH_ROWS] = {
        S_LINK_DENSITY, S_ORPHAN_RATE, S_PAGE_AGENTS, S_LAST_SYNC,
    };
    for (int i = 0; i < HEALTH_ROWS; i++) {
        int y = ROW_C_FIRST + i * ROW_C_ROW_H;
        ui_lab_w(s_root, HEALTH_X, y, HEALTH_LAB_W, UI_F_BODY,
                 LV_TEXT_ALIGN_LEFT, LABELS[i]);
        s_health_val[i] = ui_lab_w(s_root, HEALTH_VAL_X, y, HEALTH_VAL_W, UI_F_BODY,
                                   LV_TEXT_ALIGN_RIGHT, "");
    }

    return s_root;
}

static void blank(void)
{
    for (int i = 0; i < 4; i++) ui_set(s_num[i], "—");
    ui_set(s_added, "");
    for (int i = 0; i < VAULT_DAILY_DAYS; i++) {
        lv_obj_set_height(s_bar[i], 2);
        lv_obj_set_y(s_bar[i], ACT_BASE_Y - 2);
        ui_set(s_bar_val[i], "");
    }
    for (int i = 0; i < VAULT_TAGS_MAX; i++) {
        ui_set(s_tag_name[i], "");
        ui_set(s_tag_cnt[i], "");
        lv_obj_set_width(s_tag_bar[i], 1);
    }
    for (int i = 0; i < HEALTH_ROWS; i++) ui_set(s_health_val[i], "");
    ui_set(s_health_val[0], S_NO_DATA);
}

void ui_page_stats_update(const vault_t *v)
{
    if (!s_root) return;
    if (!v || !v->valid) { blank(); return; }

    char buf[32];
    const int counters[4] = {
        v->stats.notes, v->stats.links, v->stats.orphans, v->stats.tags,
    };
    for (int i = 0; i < 4; i++) {
        ui_group_int(buf, sizeof(buf), counters[i]);
        ui_set(s_num[i], buf);
    }

    ui_setf(s_added, "%s +%d · %s +%d",
            S_ADDED_TODAY, v->stats.added_today, S_ADDED_WEEK, v->stats.added_7d);

    /* Bars scale against the week's own peak, not against a fixed maximum: a
     * quiet week should still show its shape rather than seven stubs. */
    int peak = vault_daily_peak(v);
    for (int i = 0; i < VAULT_DAILY_DAYS; i++) {
        int val = v->stats.daily[i];
        int h   = (val * (ACT_PLOT_H - ACT_VAL_H)) / peak;
        if (h < 2) h = 2;                    /* a zero day keeps its column */
        lv_obj_set_height(s_bar[i], h);
        lv_obj_set_y(s_bar[i], ACT_BASE_Y - h);
        lv_obj_set_y(s_bar_val[i], ACT_BASE_Y - h - ACT_VAL_H);
        ui_setf(s_bar_val[i], "%d", val);
    }

    int tag_peak = 1;
    for (int i = 0; i < v->tag_count; i++) {
        if (v->tags[i].count > tag_peak) tag_peak = v->tags[i].count;
    }
    for (int i = 0; i < VAULT_TAGS_MAX; i++) {
        if (i < v->tag_count) {
            ui_set(s_tag_name[i], v->tags[i].name);
            ui_setf(s_tag_cnt[i], "%d", v->tags[i].count);
            int w = (v->tags[i].count * TAG_BAR_W) / tag_peak;
            lv_obj_set_width(s_tag_bar[i], w < 2 ? 2 : w);
            ui_show(s_tag_bar[i], true);
        } else {
            ui_set(s_tag_name[i], "");
            ui_set(s_tag_cnt[i], "");
            ui_show(s_tag_bar[i], false);
        }
    }

    int density = vault_link_density_x100(v);
    ui_setf(s_health_val[0], "%d.%02d %s", density / 100, density % 100, S_PER_NOTE);

    int orphan = vault_orphan_rate_x10(v);
    ui_setf(s_health_val[1], "%d.%d %%", orphan / 10, orphan % 10);

    ui_setf(s_health_val[2], "%d / %d %s",
            vault_running_agents(v), v->agent_count, S_AGENTS_RUNNING);

    ui_set(s_health_val[3], v->generated_at[0] ? v->generated_at : "—");
}
