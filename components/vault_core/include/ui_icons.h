/*
 * ui_icons.h — tiny vector glyphs drawn straight onto the LVGL canvas for the
 * 122x250 1-bit e-Paper panel.
 *
 * On a binarizing panel hairline outlines shimmer, so these are drawn as crisp
 * solid silhouettes (sun, cloud, rain) plus an outline battery, where an
 * outline reads better. Each icon is a transparent, non-interactive lv_obj with
 * a DRAW_MAIN callback — no image assets, no canvas buffers — and it composites
 * and binarizes identically in the simulator and on the device.
 *
 * Everything is drawn from unit fractions of `size`, so the same four weather
 * glyphs serve both the 30 px current-conditions icon and the 16 px forecast
 * strip.
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ICON_SUN,        /* clear        */
    ICON_PARTLY,     /* sun + cloud  */
    ICON_CLOUD,      /* overcast     */
    ICON_RAIN,       /* cloud + rain */
    ICON_BATTERY,    /* battery (uses pct fill) */
    ICON_DIAMOND,    /* solid square-on-point rule ornament */
} ui_icon_t;

/* Create a square icon of side `size` px under `parent`. Returns the lv_obj so
 * the caller can position it with lv_obj_align/set_pos. For ICON_BATTERY, `pct`
 * (0..100) sets the fill level; ignored by other icons. */
lv_obj_t *ui_icon(lv_obj_t *parent, ui_icon_t type, int size, int pct);

/* Re-skin an existing icon in place (no object churn) — for live updates such
 * as a changing weather glyph or battery level. */
void ui_icon_set(lv_obj_t *icon, ui_icon_t type, int pct);

#ifdef __cplusplus
}
#endif
