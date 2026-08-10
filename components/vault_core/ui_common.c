/*
 * ui_common.c — the shapes in ui_internal.h.
 *
 * Every object is stripped of the theme before anything else. LVGL's default
 * theme is built for a colour LCD: it rounds corners, adds a two-pixel padding,
 * makes borders a mid-grey and puts scrollbars on containers. On a 1-bit panel
 * a mid-grey border binarizes into a dashed line, a rounded corner into a
 * notch, and a scrollbar into a stray tick at the edge of a table. So:
 * remove_style_all first, then add back exactly what the shape needs.
 */
#include "ui_internal.h"

#include <stdio.h>

static void strip(lv_obj_t *o)
{
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
}

lv_obj_t *ui_pane(lv_obj_t *par, int x, int y, int w, int h)
{
    lv_obj_t *o = lv_obj_create(par);
    strip(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    return o;
}

lv_obj_t *ui_fill(lv_obj_t *par, int x, int y, int w, int h)
{
    lv_obj_t *o = ui_pane(par, x, y, w, h);
    lv_obj_set_style_bg_color(o, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}

lv_obj_t *ui_frame(lv_obj_t *par, int x, int y, int w, int h, int bw)
{
    lv_obj_t *o = ui_pane(par, x, y, w, h);
    lv_obj_set_style_bg_color(o, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(o, lv_color_black(), 0);
    lv_obj_set_style_border_width(o, bw, 0);
    lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
    return o;
}

/* Labels are transparent so they can sit on a filled band without punching a
 * white hole in it; the caller picks the text colour via ui_lab vs ui_lab_inv. */
static lv_obj_t *label_base(lv_obj_t *par, int x, int y, const lv_font_t *f)
{
    lv_obj_t *l = lv_label_create(par);
    strip(l);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_TRANSP, 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

lv_obj_t *ui_lab(lv_obj_t *par, int x, int y, const lv_font_t *f, const char *txt)
{
    lv_obj_t *l = label_base(par, x, y, f);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_CLIP);
    lv_label_set_text(l, txt ? txt : "");
    return l;
}

lv_obj_t *ui_lab_w(lv_obj_t *par, int x, int y, int w,
                   const lv_font_t *f, lv_text_align_t align, const char *txt)
{
    lv_obj_t *l = label_base(par, x, y, f);

    /* The HEIGHT is what makes DOTS work, and leaving it to auto-size is the
     * bug this call exists to prevent: with only a width set, LVGL grows the
     * label downwards and wraps instead of ellipsizing, and the second line
     * lands on top of whatever is below it. Pinning the box to exactly one line
     * is what turns "too long" into an ellipsis rather than into a collision. */
    lv_obj_set_size(l, w, lv_font_get_line_height(f));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(l, align, 0);
    lv_label_set_text(l, txt ? txt : "");
    return l;
}

void ui_lab_wrap(lv_obj_t *label, int height)
{
    if (!label) return;
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_height(label, height);
}

void ui_lab_opaque(lv_obj_t *label)
{
    if (!label) return;
    lv_obj_set_style_bg_color(label, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);
}

lv_obj_t *ui_lab_inv(lv_obj_t *par, int x, int y, int w,
                     const lv_font_t *f, lv_text_align_t align, const char *txt)
{
    lv_obj_t *l = ui_lab_w(par, x, y, w, f, align, txt);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    return l;
}

void ui_set(lv_obj_t *label, const char *txt)
{
    if (label) lv_label_set_text(label, txt ? txt : "");
}

void ui_setf(lv_obj_t *label, const char *fmt, ...)
{
    if (!label) return;
    va_list ap;
    va_start(ap, fmt);
    lv_label_set_text_vfmt(label, fmt, ap);
    va_end(ap);
}

void ui_show(lv_obj_t *obj, bool visible)
{
    if (!obj) return;
    if (visible) lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

/* --- immediate-mode drawing ---------------------------------------------- */

static lv_color_t ink(bool white)
{
    return white ? lv_color_white() : lv_color_black();
}

void ui_draw_line_abs(lv_layer_t *L, int x1, int y1, int x2, int y2, int w, bool white)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = ink(white);
    d.width = w;
    d.opa   = LV_OPA_COVER;
    d.round_start = 1;
    d.round_end   = 1;
    d.p1.x = x1; d.p1.y = y1;
    d.p2.x = x2; d.p2.y = y2;
    lv_draw_line(L, &d);
}

/* A disc, not a circle: LVGL draws an arc of width `w` at radius `r`, so a
 * width equal to the radius fills all the way to the centre. */
void ui_draw_disc_abs(lv_layer_t *L, int cx, int cy, int r, bool white)
{
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.color = ink(white);
    d.width = r;
    d.radius = r;
    d.center.x = cx; d.center.y = cy;
    d.start_angle = 0; d.end_angle = 360;
    d.opa = LV_OPA_COVER;
    lv_draw_arc(L, &d);
}

void ui_draw_ring_abs(lv_layer_t *L, int cx, int cy, int r, int w, int a0, int a1)
{
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.color = lv_color_black();
    d.width = w;
    d.radius = r;
    d.center.x = cx; d.center.y = cy;
    d.start_angle = a0; d.end_angle = a1;
    d.opa = LV_OPA_COVER;
    lv_draw_arc(L, &d);
}

void ui_draw_rect_abs(lv_layer_t *L, int x1, int y1, int x2, int y2,
                      bool fill, int border, bool white)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius  = 0;
    d.bg_color = ink(white);
    d.bg_opa   = fill ? LV_OPA_COVER : LV_OPA_TRANSP;
    if (border > 0) {
        d.border_color = ink(white);
        d.border_width = border;
        d.border_opa   = LV_OPA_COVER;
    }
    lv_area_t a = { x1, y1, x2, y2 };
    lv_draw_rect(L, &d, &a);
}

void ui_group_int(char *out, size_t n, int v)
{
    if (!out || n == 0) return;

    char digits[16];
    bool neg = v < 0;
    /* Negate through long: -INT_MIN does not fit in an int, and a counter that
     * arrives as INT_MIN is exactly the sort of thing a broken producer sends. */
    long a = neg ? -(long)v : (long)v;
    int  nd = snprintf(digits, sizeof(digits), "%ld", a);
    if (nd < 0) { out[0] = '\0'; return; }
    if (nd > (int)sizeof(digits) - 1) nd = (int)sizeof(digits) - 1;

    size_t o = 0;
    if (neg && o + 1 < n) out[o++] = '-';
    for (int i = 0; i < nd; i++) {
        if (i > 0 && (nd - i) % 3 == 0) {
            if (o + 1 >= n) break;
            out[o++] = ',';
        }
        if (o + 1 >= n) break;
        out[o++] = digits[i];
    }
    out[o] = '\0';
}
