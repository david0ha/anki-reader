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

/* Five glyphs, and no more: everything else this board has to say it says in
 * words, because Korean words at 16 px are unambiguous where a 20 px pictogram
 * is a guess. The connection state in particular is a text badge (S_BADGE_*),
 * not a Wi-Fi fan — "오프라인" cannot be misread, and a struck-through fan at
 * this size can. */
typedef enum {
    ICON_BATTERY,      /* outline shell with a fill proportional to `pct` */
    ICON_PLUG,         /* mains/USB power, shown instead of a battery       */

    /* The action rail, in the order the rail stacks them. These are drawn in
     * BLACK inside a white chip, not white on the filled player: a white glyph
     * on black loses its thin strokes to the panel's binarization at 26 px,
     * and the chip is also what makes the rail read as three buttons rather
     * than three decorations. */
    ICON_BOOK,         /* 설명 — the shape story and the memory hook        */
    ICON_COMMENT,      /* 댓글                                              */
    ICON_CLOCK,        /* FSRS — when this card comes back                  */
} ui_icon_t;

/* Create a square icon of side `size` px under `parent`. Returns the lv_obj so
 * the caller can position it. For ICON_BATTERY, `pct` (0..100) sets the fill
 * level; every other icon ignores it. */
lv_obj_t *ui_icon(lv_obj_t *parent, ui_icon_t type, int size, int pct);

/* Re-skin an existing icon in place, with no object churn. The one live user is
 * the header's power indicator, which swaps between ICON_PLUG and ICON_BATTERY
 * as a cell is fitted or removed and re-fills as the battery drains. A no-op
 * when neither the type nor `pct` changed, so an idle wake that re-reads the
 * same battery level does not invalidate anything. */
void ui_icon_set(lv_obj_t *icon, ui_icon_t type, int pct);

#ifdef __cplusplus
}
#endif
