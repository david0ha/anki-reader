/*
 * ui_vault.h — the whole on-glass UI, for the 648x480 e-Paper panel.
 *
 * Four pages under a shared header and footer:
 *
 *   0 볼트 통계   the four headline counters, a 7-day activity chart, top tags
 *                 and a vault-health block
 *   1 링크 그래프 the highest-degree notes and the links between them
 *   2 에이전트    one row per agent: state, last run, counts, progress, note
 *   3 최근 노트   recently modified notes, and the inbox queue beside them
 *
 * Every setter only mutates widgets. Nothing here talks to the panel: on
 * e-Paper the caller decides when a refresh is worth several seconds of
 * flashing, so the sequence is always
 *
 *     ui_vault_set_*(...);  Lvgl_RenderNow();  epd_refresh_*();
 *
 * Portable: LVGL only, no ESP-IDF. The desktop simulator builds these files
 * verbatim, which is how the layout gets checked against a real 648x480 bitmap
 * before it ever reaches hardware.
 */
#pragma once

#include <stdbool.h>

#include "lvgl.h"
#include "vault_model.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_PAGE_STATS = 0,
    UI_PAGE_GRAPH,
    UI_PAGE_AGENTS,
    UI_PAGE_NOTES,
    UI_PAGE_COUNT,
} ui_page_t;

/* Everything the header reports that is about the board rather than the vault.
 * Passed as a struct so adding an indicator does not change three signatures. */
typedef struct {
    bool online;          /* Wi-Fi associated and the last poll succeeded */
    bool stale;           /* showing a snapshot older than one poll interval */
    bool battery_present; /* a cell is fitted (false = USB power)          */
    int  battery_pct;     /* 0..100, meaningless unless battery_present    */
} ui_status_t;

/* Build the UI under `parent` (a full-screen 648x480 container). */
void ui_vault_create(lv_obj_t *parent);

void      ui_vault_show_page(ui_page_t page);
ui_page_t ui_vault_page(void);

/* The page's own name, for the footer and the companion-app JSON. */
const char *ui_vault_page_title(ui_page_t page);

/* Push a snapshot into all four pages. The struct is copied, so a stack-local
 * is fine. Pass NULL to blank the content and show the "no data" state. */
void ui_vault_set_data(const vault_t *v);

/* Header indicators. */
void ui_vault_set_status(const ui_status_t *st);

/* Repaint the header clock and date from the system time (already local via
 * TZ). This is the only thing on the panel that changes without new data, and
 * therefore the only candidate for a partial refresh. */
void ui_vault_tick(void);

/* The rectangle ui_vault_tick() can change, in panel coordinates, so the caller
 * can refresh just that instead of the whole 648x480. Inclusive of the border
 * the clock font needs; still cheap. */
void ui_vault_clock_area(int *x1, int *y1, int *x2, int *y2);

/* Full-screen message, for provisioning status and fatal states. Pass NULL to
 * dismiss it and return to the pages. */
void ui_vault_set_overlay(const char *title, const char *body);

#ifdef __cplusplus
}
#endif
