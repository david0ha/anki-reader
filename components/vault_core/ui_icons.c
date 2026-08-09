/*
 * ui_icons.c — see ui_icons.h. Each glyph is rendered in a LV_EVENT_DRAW_MAIN
 * callback using LVGL's vector draw API (arc / line / triangle / rect) in the
 * object's absolute coordinate space, so it scales to any `size` and stays crisp
 * after the panel's px<0x7FFF binarization.
 */
#include "ui_icons.h"

#include <stdbool.h>
#include <stdint.h>

/* one spec per icon; icons live for the app lifetime so a static pool avoids
 * any per-object heap churn. */
typedef struct { uint8_t type; int16_t pct; } icon_spec_t;
static icon_spec_t g_specs[32];
static int g_spec_n = 0;

static lv_color_t BLACK_(void) { return lv_color_make(0, 0, 0); }
static lv_color_t WHITE_(void) { return lv_color_make(0xff, 0xff, 0xff); }

/* ---- primitive helpers (absolute coords) -------------------------------- */

static void disc_c(lv_layer_t *L, int cx, int cy, int r, lv_color_t col) {
    lv_draw_arc_dsc_t d; lv_draw_arc_dsc_init(&d);
    d.color = col; d.width = r; d.radius = r;
    d.center.x = cx; d.center.y = cy;
    d.start_angle = 0; d.end_angle = 360; d.opa = LV_OPA_COVER;
    lv_draw_arc(L, &d);
}

static void ring(lv_layer_t *L, int cx, int cy, int r, int w) {
    lv_draw_arc_dsc_t d; lv_draw_arc_dsc_init(&d);
    d.color = BLACK_(); d.width = w; d.radius = r;
    d.center.x = cx; d.center.y = cy;
    d.start_angle = 0; d.end_angle = 360; d.opa = LV_OPA_COVER;
    lv_draw_arc(L, &d);
}

static void seg(lv_layer_t *L, int x1, int y1, int x2, int y2, int w) {
    lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d);
    d.color = BLACK_(); d.width = w; d.opa = LV_OPA_COVER;
    d.round_start = 1; d.round_end = 1;
    d.p1.x = x1; d.p1.y = y1; d.p2.x = x2; d.p2.y = y2;
    lv_draw_line(L, &d);
}

/* filled or outlined rounded rect (color = black) */
static void box_c(lv_layer_t *L, int x1, int y1, int x2, int y2,
                  int radius, int fill, int border, lv_color_t col) {
    lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
    d.radius = radius;
    d.bg_color = col;
    d.bg_opa = fill ? LV_OPA_COVER : LV_OPA_TRANSP;
    if (border > 0) {
        d.border_color = col;
        d.border_width = border;
        d.border_opa = LV_OPA_COVER;
    }
    lv_area_t a = { x1, y1, x2, y2 };
    lv_draw_rect(L, &d, &a);
}

static void box(lv_layer_t *L, int x1, int y1, int x2, int y2,
                int radius, int fill, int border) {
    box_c(L, x1, y1, x2, y2, radius, fill, border, BLACK_());
}

/* a puffy cloud silhouette of `col` inside [x0..x0+w] x [y0..y0+h] */
static void cloud_c(lv_layer_t *L, int x0, int y0, int w, int h,
                    int baseline, lv_color_t col) {
    int lr = h * 32 / 100, mr = h * 44 / 100, rr = h * 34 / 100;
    int by = y0 + baseline;                 /* flat bottom of the cloud */
    int lcx = x0 + w * 30 / 100, lcy = by - lr;
    int mcx = x0 + w * 52 / 100, mcy = by - mr;
    int rcx = x0 + w * 72 / 100, rcy = by - rr;
    disc_c(L, lcx, lcy, lr, col);
    disc_c(L, mcx, mcy, mr, col);
    disc_c(L, rcx, rcy, rr, col);
    box_c(L, lcx, by - lr, rcx, by, 0, 1, 0, col);  /* flat underside */
}

/* outline cloud: black silhouette with a white silhouette punched out,
 * leaving a ~`st` thick ring — crisp on the white panel. */
static void cloud_outline(lv_layer_t *L, int x0, int y0, int w, int h,
                          int baseline, int st) {
    cloud_c(L, x0, y0, w, h, baseline, BLACK_());
    cloud_c(L, x0 + st, y0 + st, w - 2 * st, h - 2 * st, baseline - st, WHITE_());
}

/* Solid cloud for the small sizes, filling exactly [x0..x0+w] x [y0..y0+h].
 *
 * cloud_c() insets its bumps well inside the box, which is right at 30 px and
 * fatal at 20: the three discs merge into one lump barely half the width of the
 * cell, and it reads as a dot. This spreads the bumps to the edges and runs the
 * flat underside the full width, so even at 8 px tall the silhouette still says
 * "cloud". */
static void cloud_small_c(lv_layer_t *L, int x0, int y0, int w, int h, lv_color_t col) {
    int by     = y0 + h;
    int r_side = h * 40 / 100; if (r_side < 2) r_side = 2;
    int r_mid  = h * 52 / 100; if (r_mid < 3)  r_mid  = 3;
    disc_c(L, x0 + w * 24 / 100, by - r_side, r_side, col);
    disc_c(L, x0 + w / 2,        by - r_mid,  r_mid,  col);
    disc_c(L, x0 + w * 76 / 100, by - r_side, r_side, col);
    box_c(L, x0, by - r_side, x0 + w, by, 0, 1, 0, col);
}

static void cloud_small(lv_layer_t *L, int x0, int y0, int w, int h) {
    cloud_small_c(L, x0, y0, w, h, BLACK_());
}

/* ---- per-icon drawing ---------------------------------------------------- */

static void draw_icon(lv_layer_t *L, const icon_spec_t *s,
                      int x0, int y0, int sz) {
    int st = sz / 12; if (st < 2) st = 2;          /* stroke weight */
    int cx = x0 + sz / 2, cy = y0 + sz / 2;
    #define FX(f) (x0 + (int)((f) * sz / 100))
    #define FY(f) (y0 + (int)((f) * sz / 100))

    /* 8 unit-circle directions (x100) for sun rays */
    static const int DX[8] = { 100, 71, 0, -71, -100, -71, 0, 71 };
    static const int DY[8] = { 0, 71, 100, 71, 0, -71, -100, -71 };

    /* Below this the outline treatment stops working: a 2 px ring around a 4 px
     * disc, or a punched-out cloud whose wall is one pixel, binarizes into a
     * smudge. The forecast strip lives at 20 px, so small sizes get a separate,
     * solid-silhouette geometry with fewer features. Same glyph vocabulary,
     * drawn to survive the threshold. */
    bool compact = sz < 26;

    switch (s->type) {
    case ICON_SUN: {
        if (compact) {
            disc_c(L, cx, cy, sz * 21 / 100, BLACK_());
            int ri = sz * 32 / 100, ro = sz * 52 / 100;
            for (int i = 0; i < 8; i += 2)            /* N/E/S/W only */
                seg(L, cx + DX[i] * ri / 100, cy + DY[i] * ri / 100,
                       cx + DX[i] * ro / 100, cy + DY[i] * ro / 100, st);
            break;
        }
        int r = sz * 22 / 100;
        ring(L, cx, cy, r, st);                       /* outline disc */
        int ri = r + sz * 9 / 100, ro = r + sz * 24 / 100;
        for (int i = 0; i < 8; i++)
            seg(L, cx + DX[i] * ri / 100, cy + DY[i] * ri / 100,
                   cx + DX[i] * ro / 100, cy + DY[i] * ro / 100, st);
        break;
    }
    case ICON_PARTLY: {
        if (compact) {
            /* Sun upper-right, cloud below-left — but offsetting them is not
             * enough on its own: two black masses that touch read as one
             * head-and-shoulders blob. Punching a 1 px white cloud first leaves
             * a visible gap between them, which is what makes it a sun *and* a
             * cloud. Rays are omitted; at 20 px they are noise. */
            disc_c(L, FX(62), FY(24), sz * 21 / 100, BLACK_());
            int cw = sz * 82 / 100, ch = sz * 40 / 100;
            cloud_small_c(L, FX(2) - 1, FY(48) - 1, cw + 2, ch + 2, WHITE_());
            cloud_small(L, FX(2), FY(48), cw, ch);
            break;
        }
        /* sun peeking from the upper-left; only the rays NOT behind the cloud
         * are drawn so it stays clean, then a solid cloud sits lower-right. */
        int sr = sz * 14 / 100, scx = FX(32), scy = FY(30);
        ring(L, scx, scy, sr, st);
        int ri = sr + sz * 7 / 100, ro = sr + sz * 19 / 100;
        static const int keep[5] = { 3, 4, 5, 6, 7 };   /* SW,W,NW,N,NE */
        for (int k = 0; k < 5; k++) {
            int i = keep[k];
            seg(L, scx + DX[i] * ri / 100, scy + DY[i] * ri / 100,
                   scx + DX[i] * ro / 100, scy + DY[i] * ro / 100, st);
        }
        cloud_outline(L, FX(30), FY(46), sz * 64 / 100, sz * 50 / 100, sz * 50 / 100, st);
        break;
    }
    case ICON_CLOUD:
        if (compact) {
            cloud_small(L, FX(2), FY(28), sz * 96 / 100, sz * 46 / 100);
            break;
        }
        cloud_outline(L, FX(6), FY(24), sz * 88 / 100, sz * 58 / 100, sz * 54 / 100, st);
        break;
    case ICON_RAIN: {
        if (compact) {
            cloud_small(L, FX(4), FY(14), sz * 92 / 100, sz * 40 / 100);
            int ry = FY(62), rh = sz * 28 / 100;
            for (int i = 0; i < 2; i++)               /* two drops, not three */
                seg(L, FX(40 + i * 24), ry, FX(32 + i * 24), ry + rh, st);
            break;
        }
        cloud_outline(L, FX(10), FY(10), sz * 80 / 100, sz * 52 / 100, sz * 48 / 100, st);
        int ry = FY(66), rh = sz * 24 / 100;
        for (int i = 0; i < 3; i++) {
            int rx = FX(32 + i * 18);
            seg(L, rx + st, ry, rx - st, ry + rh, st);
        }
        break;
    }
    case ICON_BATTERY: {
        int x1 = FX(10), x2 = FX(82), y1 = FY(32), y2 = FY(68);
        box(L, x1, y1, x2, y2, st, 0, st);                   /* shell  */
        box(L, FX(82), FY(42), FX(90), FY(58), 0, 1, 0);     /* nub    */
        int pct = s->pct < 0 ? 0 : (s->pct > 100 ? 100 : s->pct);
        int innerx1 = x1 + st + 1, innerx2 = x2 - st - 1;
        int fillw = (innerx2 - innerx1) * pct / 100;
        if (fillw > 0)
            box(L, innerx1, y1 + st + 1, innerx1 + fillw, y2 - st - 1, 0, 1, 0);
        break;
    }
    case ICON_DIAMOND: {
        /* A rotated square drawn as stacked 1px rows rather than an
         * arc/polygon fill: at sz=5 (its only use, on a rule) an
         * anti-aliased diagonal would binarize into a lopsided blob, while
         * row widths 1,3,..,sz,..,3,1 hit exact pixels every time. */
        for (int row = 0; row < sz; row++) {
            int dist = row < (sz - 1 - row) ? row : (sz - 1 - row);
            int w = 2 * dist + 1;
            int rx0 = x0 + (sz - w) / 2;
            box(L, rx0, y0 + row, rx0 + w - 1, y0 + row, 0, 1, 0);
        }
        break;
    }
    }
    #undef FX
    #undef FY
}

static void icon_draw_cb(lv_event_t *e) {
    lv_obj_t *o = lv_event_get_target(e);
    lv_layer_t *L = lv_event_get_layer(e);
    const icon_spec_t *s = lv_obj_get_user_data(o);
    if (!s || !L) return;
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int sz = a.x2 - a.x1 + 1;
    draw_icon(L, s, a.x1, a.y1, sz);
}

lv_obj_t *ui_icon(lv_obj_t *parent, ui_icon_t type, int size, int pct) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, size, size);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);

    /* On pool exhaustion leave user_data NULL so the icon renders blank rather
     * than aliasing slot 0 and hijacking the first icon's glyph. The UI
     * uses 9 of 32 slots, so this is a safety net, not a normal path. */
    if (g_spec_n >= (int)(sizeof g_specs / sizeof g_specs[0])) return o;
    icon_spec_t *s = &g_specs[g_spec_n++];
    s->type = (uint8_t)type;
    s->pct  = (int16_t)pct;
    lv_obj_set_user_data(o, s);
    lv_obj_add_event_cb(o, icon_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    return o;
}

void ui_icon_set(lv_obj_t *icon, ui_icon_t type, int pct) {
    if (!icon) return;
    icon_spec_t *s = lv_obj_get_user_data(icon);
    if (!s) return;
    s->type = (uint8_t)type;
    s->pct  = (int16_t)pct;
    lv_obj_invalidate(icon);
}
