/*
 * ui_icons.c — see ui_icons.h. Each glyph is rendered in a LV_EVENT_DRAW_MAIN
 * callback using LVGL's vector draw API (arc / line / rect) in the object's
 * absolute coordinate space, so it scales to any `size` and stays crisp after
 * the panel's px<0x7FFF binarization.
 */
#include "ui_icons.h"
#include "ui_internal.h"

#include <stdbool.h>
#include <stdint.h>

/* One spec per icon; icons live for the app lifetime, so a static pool avoids
 * per-object heap churn and, more usefully, makes the count auditable.
 *
 * The UI builds exactly four: the header's battery/plug indicator, and the
 * question screen's three-item action rail. Eight is that with room for one
 * more rail, and small enough that the number is a claim about the UI rather
 * than a shrug. */
typedef struct { uint8_t type; int16_t pct; } icon_spec_t;
static icon_spec_t g_specs[8];
static int g_spec_n = 0;

/* Local shorthand over the shared primitives in ui_common.c. */
#define ring(L,cx,cy,r,w,a0,a1)     ui_draw_ring_abs((L),(cx),(cy),(r),(w),(a0),(a1))
#define seg(L,x1,y1,x2,y2,w)        ui_draw_line_abs((L),(x1),(y1),(x2),(y2),(w), false)
#define box(L,x1,y1,x2,y2,f,b)      ui_draw_rect_abs((L),(x1),(y1),(x2),(y2),(f),(b), false)

/* ---- per-icon drawing ---------------------------------------------------- */

static void draw_icon(lv_layer_t *L, const icon_spec_t *s, int x0, int y0, int sz)
{
    int st = sz / 10; if (st < 2) st = 2;          /* stroke weight */
    int cx = x0 + sz / 2, cy = y0 + sz / 2;
    #define FX(f) (x0 + (int)((f) * sz / 100))
    #define FY(f) (y0 + (int)((f) * sz / 100))

    switch (s->type) {
    case ICON_BATTERY: {
        /* Landscape cell with the nub on the right. Drawn as an outline with a
         * solid fill inset by a clear pixel, so the fill never merges with the
         * shell and turn into a plain black brick at low percentages. */
        int x1 = FX(4), x2 = FX(84), y1 = FY(26), y2 = FY(74);
        box(L, x1, y1, x2, y2, 0, st);
        box(L, FX(86), FY(40), FX(96), FY(60), 1, 0);        /* nub */

        int pct = s->pct < 0 ? 0 : (s->pct > 100 ? 100 : s->pct);
        int ix1 = x1 + st + 1, ix2 = x2 - st - 1;
        int fillw = (ix2 - ix1) * pct / 100;
        if (fillw > 0) box(L, ix1, y1 + st + 1, ix1 + fillw, y2 - st - 1, 1, 0);
        break;
    }
    case ICON_PLUG: {
        /* Two prongs over a body — reads as "mains" at 24 px where a lightning
         * bolt turns into a smudge. */
        box(L, FX(30), FY(6),  FX(38), FY(30), 1, 0);
        box(L, FX(60), FY(6),  FX(68), FY(30), 1, 0);
        box(L, FX(18), FY(30), FX(80), FY(62), 0, st);
        box(L, FX(44), FY(62), FX(54), FY(94), 1, 0);
        break;
    }
    case ICON_BOOK: {
        /* A page with three rules of text. An open book — two leaves and a
         * spine — was the first shape here and it collapsed into a black blob:
         * at 26 px each leaf is ten pixels wide and a two-pixel border eats
         * six of them. A single frame with room inside it survives. */
        box(L, FX(14), FY(6), FX(86), FY(94), 0, st);
        seg(L, FX(28), FY(30), FX(72), FY(30), st);
        seg(L, FX(28), FY(50), FX(72), FY(50), st);
        seg(L, FX(28), FY(70), FX(58), FY(70), st);
        break;
    }
    case ICON_COMMENT: {
        /* A speech bubble: a rounded-off box with a tail at the lower left.
         * The two rules inside are what make it read as a comment rather than
         * as an empty frame at this size. */
        box(L, FX(8), FY(12), FX(92), FY(70), 0, st);
        seg(L, FX(20), FY(70), FX(20), FY(92), st);
        seg(L, FX(20), FY(92), FX(44), FY(70), st);
        seg(L, FX(22), FY(32), FX(78), FY(32), st);
        seg(L, FX(22), FY(50), FX(60), FY(50), st);
        break;
    }
    case ICON_CLOCK:
        /* A clock face with the hands at roughly ten past two — asymmetric on
         * purpose, so it cannot be mistaken for the hollow dot. */
        ring(L, cx, cy, sz * 42 / 100, st, 0, 360);
        seg(L, cx, cy, cx, FY(22), st);
        seg(L, cx, cy, FX(76), cy, st);
        break;
    }
    #undef FX
    #undef FY
}

static void icon_draw_cb(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    lv_layer_t *L = lv_event_get_layer(e);
    const icon_spec_t *s = lv_obj_get_user_data(o);
    if (!s || !L) return;
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int sz = a.x2 - a.x1 + 1;
    draw_icon(L, s, a.x1, a.y1, sz);
}

lv_obj_t *ui_icon(lv_obj_t *parent, ui_icon_t type, int size, int pct)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, size, size);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);

    /* On pool exhaustion leave user_data NULL so the icon renders blank rather
     * than aliasing slot 0 and drawing the first icon's glyph in its place. */
    if (g_spec_n >= (int)(sizeof g_specs / sizeof g_specs[0])) return o;
    icon_spec_t *s = &g_specs[g_spec_n++];
    s->type = (uint8_t)type;
    s->pct  = (int16_t)pct;
    lv_obj_set_user_data(o, s);
    lv_obj_add_event_cb(o, icon_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    return o;
}

void ui_icon_set(lv_obj_t *icon, ui_icon_t type, int pct)
{
    if (!icon) return;
    icon_spec_t *s = lv_obj_get_user_data(icon);
    if (!s) return;
    if (s->type == (uint8_t)type && s->pct == (int16_t)pct) return;
    s->type = (uint8_t)type;
    s->pct  = (int16_t)pct;
    lv_obj_invalidate(icon);
}
