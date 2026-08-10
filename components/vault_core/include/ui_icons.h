/*
 * ui_icons.h — the handful of vector glyphs this board draws.
 *
 * Each icon is a transparent, non-interactive lv_obj with a DRAW_MAIN callback
 * — no image assets, no canvas buffers — so it composites and binarizes
 * identically in the simulator and on the device, and scales to any size from
 * unit fractions.
 *
 * On a binarizing panel a hairline outline shimmers, so shapes are either solid
 * silhouettes or outlines at least two pixels thick. Nothing here uses grey.
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ICON_BATTERY,      /* outline shell with a fill proportional to `pct` */
    ICON_PLUG,         /* mains/USB power, shown instead of a battery       */
    ICON_WIFI,         /* connected                                         */
    ICON_WIFI_OFF,     /* connected glyph with a slash through it           */
    ICON_DOT_FULL,     /* agent running / current page                      */
    ICON_DOT_HOLLOW,   /* agent idle / another page                         */
    ICON_CROSS,        /* agent error                                       */
    ICON_CHECK,        /* agent done                                        */
} ui_icon_t;

/* Create a square icon of side `size` px under `parent`. Returns the lv_obj so
 * the caller can position it. For ICON_BATTERY, `pct` (0..100) sets the fill
 * level; every other icon ignores it. */
lv_obj_t *ui_icon(lv_obj_t *parent, ui_icon_t type, int size, int pct);

/* Re-skin an existing icon in place (no object churn) — for live updates such
 * as a changing battery level or an agent that has just failed. */
void ui_icon_set(lv_obj_t *icon, ui_icon_t type, int pct);

#ifdef __cplusplus
}
#endif
