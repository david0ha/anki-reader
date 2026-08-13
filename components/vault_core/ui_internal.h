/*
 * ui_internal.h — the layout grid and the drawing shorthand every page shares.
 *
 * Private to vault_core: it is not in include/, and nothing outside the UI
 * files may include it. The public surface is ui_vault.h.
 *
 * Why a shorthand at all: on a 1-bit panel every widget wants the same six
 * style calls (no theme, no radius, black on white, no padding, no scrolling),
 * and repeating them four hundred times is how a page ends up with a rounded
 * corner or a grey border that binarizes into a dashed line. One helper per
 * shape, used everywhere, means the panel's constraints are enforced once.
 */
#pragma once

#include <stdarg.h>
#include <stddef.h>

#include "lvgl.h"
#include "ui_fonts.h"
#include "ui_strings.h"
#include "vault_model.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- the grid -------------------------------------------------------------
 * Everything on the glass is placed against these. They are the only magic
 * numbers the pages are allowed to share; a page's own internals stay in that
 * page's file. */
#define UI_W            648
#define UI_H            480

#define UI_HEADER_H      44
#define UI_FOOTER_H      34
#define UI_RULE           2     /* the black hairline under/over the chrome */

#define UI_CONTENT_Y    (UI_HEADER_H + UI_RULE)                       /* 46  */
#define UI_CONTENT_H    (UI_H - UI_CONTENT_Y - UI_FOOTER_H - UI_RULE) /* 398 */

#define UI_PAD           14     /* content inset from the panel edge */

/* --- fonts ----------------------------------------------------------------
 * The two Korean faces are FULL 완성형 (see ui_fonts.h), so either can draw any
 * string the network sends. Montserrat is used only where a run of digits
 * wants a proper numeral face. */
#define UI_F_BODY       (&ui_font_kr_16)
#define UI_F_HEAD       (&ui_font_kr_20)
#define UI_F_NUM_SM     (&lv_font_montserrat_14)
#define UI_F_NUM        (&lv_font_montserrat_20)
#define UI_F_NUM_LG     (&lv_font_montserrat_28)
#define UI_F_NUM_XL     (&lv_font_montserrat_44)
#define UI_F_ART_HEAD   (&ui_font_kr_28)

/* --- shapes ---------------------------------------------------------------
 * All coordinates are relative to `par`. Every one of these returns an object
 * that is non-scrollable, non-clickable, square-cornered and un-themed. */

/* An invisible container. Use it to group a section so the whole thing can be
 * shown or hidden in one call. */
lv_obj_t *ui_pane(lv_obj_t *par, int x, int y, int w, int h);

/* A solid black rectangle — rules, bars, filled chips, the header band. */
lv_obj_t *ui_fill(lv_obj_t *par, int x, int y, int w, int h);

/* A white rectangle with a black border of `bw` px. */
lv_obj_t *ui_frame(lv_obj_t *par, int x, int y, int w, int h, int bw);

/* A left-aligned label that sizes itself to its text. */
lv_obj_t *ui_lab(lv_obj_t *par, int x, int y, const lv_font_t *f, const char *txt);

/* A label with a fixed width and an alignment. Text longer than `w` is
 * ellipsized rather than wrapped or clipped — a dashboard row that silently
 * grows a second line pushes everything below it off the panel. */
lv_obj_t *ui_lab_w(lv_obj_t *par, int x, int y, int w,
                   const lv_font_t *f, lv_text_align_t align, const char *txt);

/* White-on-black text, for the header band and state chips. */
lv_obj_t *ui_lab_inv(lv_obj_t *par, int x, int y, int w,
                     const lv_font_t *f, lv_text_align_t align, const char *txt);

/* Let a label wrap inside `height` px instead of ellipsizing. The exception,
 * not the rule: only the provisioning overlay's body wants this, because an
 * ellipsis in the middle of an AP name and its instructions is useless. */
void ui_lab_wrap(lv_obj_t *label, int height);

/* Paint white behind a label's box. For text that sits on top of something
 * already drawn — the graph's node titles over its edges — where transparency
 * would leave a line running through the middle of a word. */
void ui_lab_opaque(lv_obj_t *label);

void ui_set(lv_obj_t *label, const char *txt);
void ui_setf(lv_obj_t *label, const char *fmt, ...) LV_FORMAT_ATTRIBUTE(2, 3);
void ui_show(lv_obj_t *obj, bool visible);

/* --- immediate-mode drawing ----------------------------------------------
 * For the two things LVGL widgets cannot express on this panel: the icon
 * glyphs and the link graph's edges. Called only from a LV_EVENT_DRAW_MAIN
 * handler, in ABSOLUTE screen coordinates (add lv_obj_get_coords()'s origin).
 *
 * `white` draws in white — used to punch a hole in something already drawn,
 * which is how a node circle stays readable with six edges running under it. */
void ui_draw_line_abs(lv_layer_t *L, int x1, int y1, int x2, int y2, int w, bool white);
void ui_draw_disc_abs(lv_layer_t *L, int cx, int cy, int r, bool white);
void ui_draw_ring_abs(lv_layer_t *L, int cx, int cy, int r, int w, int a0, int a1);
void ui_draw_rect_abs(lv_layer_t *L, int x1, int y1, int x2, int y2,
                      bool fill, int border, bool white);

/* --- text ----------------------------------------------------------------- */

/* 1428 -> "1,428". Grouping matters here: the four headline counters are the
 * first thing read from across a room, and an ungrouped five-digit number is
 * genuinely slower to parse. */
void ui_group_int(char *out, size_t n, int v);

/* --- the pages ------------------------------------------------------------
 * Each page is one file and obeys the same two-call contract: create() builds
 * a pane the size of the content area and returns it (the router positions and
 * shows/hides it), update() rewrites its widgets from a snapshot and touches
 * nothing else. A NULL snapshot means "blank yourself".
 *
 * Nothing in a page file talks to the panel, keeps state beyond its widgets, or
 * knows which page is on screen. */
lv_obj_t *ui_page_stats_create(lv_obj_t *par);
void      ui_page_stats_update(const vault_t *v);

lv_obj_t *ui_page_graph_create(lv_obj_t *par);
void      ui_page_graph_update(const vault_t *v);

lv_obj_t *ui_page_agents_create(lv_obj_t *par);
void      ui_page_agents_update(const vault_t *v);

lv_obj_t *ui_page_notes_create(lv_obj_t *par);
void      ui_page_notes_update(const vault_t *v);

#ifdef __cplusplus
}
#endif
