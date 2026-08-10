/*
 * ui_page_agents.c — page 2, 에이전트.
 *
 *   에이전트 · 2 / 5 실행 중                          마지막 동기화 21:04
 *   ------------------------------------------------------------------
 *   * indexer      [실행 중]  20:55   처리 1,428   대기 3   [#######   ]
 *     새 노트 6건 임베딩 중
 *   ------------------------------------------------------------------
 *   o summarizer   [대기]     18:30   처리 412     대기 0
 *
 * Two lines per agent: the machine-readable row, and what it is actually doing.
 * The second line is the one a human reads, so it gets the full width and is
 * ellipsized rather than truncated silently.
 */
#include "ui_internal.h"
#include "ui_icons.h"

#define AG_HEAD_Y      2
#define AG_RULE_Y      30
#define AG_ROW_Y       38
#define AG_ROW_H       60

#define AG_BULLET_X    UI_PAD
#define AG_BULLET_SZ   14
#define AG_NAME_X      38
#define AG_NAME_W      152
#define AG_CHIP_X      194
#define AG_CHIP_W      78
#define AG_CHIP_H      22
#define AG_RUN_X       282
#define AG_RUN_W       70
#define AG_DONE_X      360
#define AG_DONE_W      112
#define AG_QUEUE_X     478
#define AG_QUEUE_W     80
#define AG_BAR_X       566
#define AG_BAR_W       68
#define AG_BAR_H       14
#define AG_NOTE_X      AG_NAME_X
#define AG_NOTE_W      (UI_W - UI_PAD - AG_NOTE_X)
#define AG_NOTE_DY     30

typedef struct {
    lv_obj_t *bullet;
    lv_obj_t *name;
    lv_obj_t *chip;
    lv_obj_t *chip_txt;
    lv_obj_t *run;
    lv_obj_t *done;
    lv_obj_t *queued;
    lv_obj_t *bar_frame;
    lv_obj_t *bar_fill;
    lv_obj_t *note;
    lv_obj_t *rule;
} agent_row_t;

static lv_obj_t   *s_root;
static lv_obj_t   *s_head;
static lv_obj_t   *s_sync;
static lv_obj_t   *s_empty;
static agent_row_t s_rows[VAULT_AGENTS_MAX];

/* The wire's state word is English because it is an identifier; what goes on
 * the glass is Korean because it is prose. Keeping the two apart is why
 * vault_agent_state_name() (for /api) and this (for the panel) are separate. */
static const char *state_label(agent_state_t s)
{
    switch (s) {
    case AGENT_RUNNING: return S_STATE_RUNNING;
    case AGENT_ERROR:   return S_STATE_ERROR;
    case AGENT_DONE:    return S_STATE_DONE;
    case AGENT_IDLE:    return S_STATE_IDLE;
    default:            return S_STATE_IDLE;
    }
}

static ui_icon_t state_icon(agent_state_t s)
{
    switch (s) {
    case AGENT_RUNNING: return ICON_DOT_FULL;
    case AGENT_ERROR:   return ICON_CROSS;
    case AGENT_DONE:    return ICON_CHECK;
    default:            return ICON_DOT_HOLLOW;
    }
}

lv_obj_t *ui_page_agents_create(lv_obj_t *par)
{
    s_root = ui_pane(par, 0, 0, UI_W, UI_CONTENT_H);

    s_head = ui_lab_w(s_root, UI_PAD, AG_HEAD_Y, 380, UI_F_HEAD,
                      LV_TEXT_ALIGN_LEFT, S_PAGE_AGENTS);
    s_sync = ui_lab_w(s_root, 400, AG_HEAD_Y + 4, UI_W - UI_PAD - 400, UI_F_BODY,
                      LV_TEXT_ALIGN_RIGHT, "");
    ui_fill(s_root, UI_PAD, AG_RULE_Y, UI_W - 2 * UI_PAD, 1);

    s_empty = ui_lab_w(s_root, UI_PAD, 160, UI_W - 2 * UI_PAD, UI_F_HEAD,
                       LV_TEXT_ALIGN_CENTER, S_NO_DATA);
    ui_show(s_empty, false);

    for (int i = 0; i < VAULT_AGENTS_MAX; i++) {
        agent_row_t *r = &s_rows[i];
        int y = AG_ROW_Y + i * AG_ROW_H;

        r->bullet = ui_icon(s_root, ICON_DOT_HOLLOW, AG_BULLET_SZ, 0);
        lv_obj_set_pos(r->bullet, AG_BULLET_X, y + 6);

        r->name = ui_lab_w(s_root, AG_NAME_X, y, AG_NAME_W, UI_F_HEAD,
                           LV_TEXT_ALIGN_LEFT, "");

        r->chip     = ui_fill(s_root, AG_CHIP_X, y + 2, AG_CHIP_W, AG_CHIP_H);
        r->chip_txt = ui_lab_inv(s_root, AG_CHIP_X, y + 5, AG_CHIP_W, UI_F_BODY,
                                 LV_TEXT_ALIGN_CENTER, "");

        r->run    = ui_lab_w(s_root, AG_RUN_X, y + 4, AG_RUN_W, UI_F_BODY,
                             LV_TEXT_ALIGN_RIGHT, "");
        r->done   = ui_lab_w(s_root, AG_DONE_X, y + 4, AG_DONE_W, UI_F_BODY,
                             LV_TEXT_ALIGN_RIGHT, "");
        r->queued = ui_lab_w(s_root, AG_QUEUE_X, y + 4, AG_QUEUE_W, UI_F_BODY,
                             LV_TEXT_ALIGN_RIGHT, "");

        r->bar_frame = ui_frame(s_root, AG_BAR_X, y + 5, AG_BAR_W, AG_BAR_H, 2);
        r->bar_fill  = ui_fill(s_root, AG_BAR_X + 3, y + 8, 1, AG_BAR_H - 6);

        r->note = ui_lab_w(s_root, AG_NOTE_X, y + AG_NOTE_DY, AG_NOTE_W, UI_F_BODY,
                           LV_TEXT_ALIGN_LEFT, "");

        /* Separator below every row but the last one drawn — set per update, so
         * a vault with two agents does not get four empty rules. */
        r->rule = ui_fill(s_root, UI_PAD, y + AG_ROW_H - 4, UI_W - 2 * UI_PAD, 1);
    }

    return s_root;
}

static void show_row(agent_row_t *r, bool on)
{
    ui_show(r->bullet, on);
    ui_show(r->name, on);
    ui_show(r->chip, on);
    ui_show(r->chip_txt, on);
    ui_show(r->run, on);
    ui_show(r->done, on);
    ui_show(r->queued, on);
    ui_show(r->note, on);
    if (!on) {
        ui_show(r->bar_frame, false);
        ui_show(r->bar_fill, false);
        ui_show(r->rule, false);
    }
}

void ui_page_agents_update(const vault_t *v)
{
    if (!s_root) return;

    int n = (v && v->valid) ? v->agent_count : 0;
    if (n > VAULT_AGENTS_MAX) n = VAULT_AGENTS_MAX;

    ui_show(s_empty, n == 0);
    if (n == 0) {
        ui_set(s_head, S_PAGE_AGENTS);
        ui_set(s_sync, "");
        for (int i = 0; i < VAULT_AGENTS_MAX; i++) show_row(&s_rows[i], false);
        return;
    }

    ui_setf(s_head, "%s · %d / %d %s",
            S_PAGE_AGENTS, vault_running_agents(v), n, S_AGENTS_RUNNING);
    ui_setf(s_sync, "%s %s", S_LAST_SYNC,
            v->generated_at[0] ? v->generated_at : "—");

    char buf[32];
    for (int i = 0; i < VAULT_AGENTS_MAX; i++) {
        agent_row_t *r = &s_rows[i];
        if (i >= n) { show_row(r, false); continue; }

        const vault_agent_t *a = &v->agents[i];
        show_row(r, true);

        ui_icon_set(r->bullet, state_icon(a->state), 0);
        ui_set(r->name, a->name);
        ui_set(r->chip_txt, state_label(a->state));
        ui_set(r->run, a->last_run[0] ? a->last_run : "—");

        ui_group_int(buf, sizeof(buf), a->processed);
        ui_setf(r->done, "%s %s", S_AGENT_DONE_N, buf);
        ui_setf(r->queued, "%s %d", S_AGENT_QUEUED, a->queued);

        /* progress < 0 means the agent has no meaningful completion figure. An
         * empty bar there would read as "stuck at 0%", which is a different and
         * much more alarming thing than "not applicable". */
        bool has_bar = a->progress >= 0;
        ui_show(r->bar_frame, has_bar);
        ui_show(r->bar_fill, has_bar);
        if (has_bar) {
            int inner = AG_BAR_W - 6;
            int w = inner * a->progress / 100;
            lv_obj_set_width(r->bar_fill, w < 1 ? 1 : w);
        }

        ui_set(r->note, a->note);
        ui_show(r->rule, i < n - 1);
    }
}
