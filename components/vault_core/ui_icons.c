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
 * per-object heap churn and, more usefully, makes the count auditable. */
typedef struct { uint8_t type; int16_t pct; } icon_spec_t;
static icon_spec_t g_specs[48];
static int g_spec_n = 0;

/* Local shorthand over the shared primitives in ui_common.c. */
#define disc(L,cx,cy,r)             ui_draw_disc_abs((L),(cx),(cy),(r), false)
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
    case ICON_WIFI:
    case ICON_WIFI_OFF: {
        /* Three arcs plus a dot, drawn from the bottom centre. LVGL measures
         * arc angles clockwise from 3 o'clock, so 215..325 is the upper fan. */
        int bx = cx, by = FY(80);
        disc(L, bx, by, st);
        ring(L, bx, by, sz * 26 / 100, st, 215, 325);
        ring(L, bx, by, sz * 44 / 100, st, 215, 325);
        ring(L, bx, by, sz * 62 / 100, st, 215, 325);
        if (s->type == ICON_WIFI_OFF) {
            /* A white slash under the black one keeps the bar visible where it
             * crosses an arc — without it the two blacks merge and the "off"
             * reading is lost. */
            ui_draw_line_abs(L, FX(14), FY(14), FX(86), FY(86), st + 4, true);
            seg(L, FX(16), FY(16), FX(84), FY(84), st);
        }
        break;
    }
    case ICON_DOT_FULL:
        disc(L, cx, cy, sz * 40 / 100);
        break;
    case ICON_DOT_HOLLOW:
        ring(L, cx, cy, sz * 38 / 100, st, 0, 360);
        break;
    case ICON_CROSS:
        seg(L, FX(18), FY(18), FX(82), FY(82), st);
        seg(L, FX(82), FY(18), FX(18), FY(82), st);
        break;
    case ICON_CHECK:
        seg(L, FX(14), FY(52), FX(40), FY(78), st);
        seg(L, FX(40), FY(78), FX(88), FY(20), st);
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
