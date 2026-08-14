/*
 * ui_internal.h — the layout grid and the drawing shorthand every page shares.
 *
 * Private to vault_core: it is not in include/, and nothing outside the UI
 * files may include it. The public surface is ui_kanji.h.
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

#include "kanji_model.h"
#include "kanji_nav.h"
#include "lvgl.h"
#include "ui_fonts.h"
#include "ui_kanji_layout.h"
#include "ui_strings.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- the grid -------------------------------------------------------------
 * Every rectangle on the glass comes from ui_kanji_layout.h, which is pure
 * integers and is host-tested. Only the two panel dimensions are repeated here,
 * because a screen file that has to include a layout header to ask how wide the
 * panel is reads worse than one that already knows. */
#define UI_W            KANJI_SCREEN_W
#define UI_H            KANJI_SCREEN_H
#define UI_PAD          16      /* approved panel edge */
#define UI_RULE          1      /* ordinary separation rule */

/* Screen panes begin at the main-column origin. Layout rectangles are panel
 * coordinates, so both axes must be converted before creating pane children. */
#define LOCAL_X(v) ((v) - kanji_chrome_layout()->main.x)
#define LOCAL_Y(v) ((v) - kanji_chrome_layout()->main.y)

/* --- fonts ----------------------------------------------------------------
 * The body, heading, and title faces carry the complete multilingual set (see
 * ui_fonts.h), so they can draw mixed Korean/Japanese network strings.
 *
 * The hero face is Japanese-only — 56 px of Hangul is flash this board does not
 * have to spend — so it is never selected by size alone; see ui_hero_face().
 *
 * Montserrat 18 is the one Latin face kept, for utility labels and counters
 * where a run of digits wants a proper numeral face. The other Montserrats
 * this UI inherited are gone: nothing draws them, and each costs flash. */
#define UI_F_BODY       (&ui_font_kr_16)
#define UI_F_HEAD       (&ui_font_kr_20)
#define UI_F_TITLE      (&ui_font_kr_28)
#define UI_F_HERO       (&ui_font_jp_56)
#define UI_F_UTILITY    (&lv_font_montserrat_18)

/* --- shapes ---------------------------------------------------------------
 * All coordinates are relative to `par`. Every one of these returns an object
 * that is non-scrollable, non-clickable, square-cornered and un-themed. */

/* An invisible container. Use it to group a section so the whole thing can be
 * shown or hidden in one call. */
lv_obj_t *ui_pane(lv_obj_t *par, int x, int y, int w, int h);

/* A solid black rectangle — rules, bars, filled chips, the header band. */
lv_obj_t *ui_fill(lv_obj_t *par, int x, int y, int w, int h);

/* A one-pixel black separator. One dimension must be 1. */
lv_obj_t *ui_rule(lv_obj_t *par, int x, int y, int w, int h);

/* A solid WHITE rectangle. Two uses, and both need the opacity rather than
 * transparency:
 *
 *   - a white mark on top of a selected black surface, where a transparent
 *     object would show the black through;
 *   - the root of a screen that is paper rather than player, so the sheet
 *     covers whatever the previous screen left in the framebuffer instead of
 *     letting it show through.
 */
lv_obj_t *ui_fill_white(lv_obj_t *par, int x, int y, int w, int h);

/* A white rectangle with a black border of `bw` px. */
lv_obj_t *ui_frame(lv_obj_t *par, int x, int y, int w, int h, int bw);

/* A left-aligned label that sizes itself to its text. */
lv_obj_t *ui_lab(lv_obj_t *par, int x, int y, const lv_font_t *f, const char *txt);

/* A label with a fixed width and an alignment. Text longer than `w` is
 * ellipsized rather than wrapped or clipped — a dashboard row that silently
 * grows a second line pushes everything below it off the panel. */
lv_obj_t *ui_lab_w(lv_obj_t *par, int x, int y, int w,
                   const lv_font_t *f, lv_text_align_t align, const char *txt);

/* A fixed headword box that wraps without ellipsis. `h` is sized from the
 * fallback face's measured worst case, so every model-valid front remains
 * visible; short covered words still use the serif face. */
lv_obj_t *ui_lab_headword(lv_obj_t *par, int x, int y, int w, int h,
                          const lv_font_t *f, const char *txt);

/* White-on-black text for selected surfaces and exceptional-state stamps. */
lv_obj_t *ui_lab_inv(lv_obj_t *par, int x, int y, int w,
                     const lv_font_t *f, lv_text_align_t align, const char *txt);

/* Let a fixed prose label wrap inside `height` px instead of ellipsizing.
 * Provisioning, description, comments, and FSRS use this for bounded bodies. */
void ui_lab_wrap(lv_obj_t *label, int height);

void ui_set(lv_obj_t *label, const char *txt);
void ui_setf(lv_obj_t *label, const char *fmt, ...) LV_FORMAT_ATTRIBUTE(2, 3);
void ui_show(lv_obj_t *obj, bool visible);

/* Whether `f` has a glyph for every character of `s`. Empty and NULL are true.
 *
 * This exists for exactly one caller, and for a defect it already caught: the
 * hero face is Japanese-only to keep 56 px worth of Hangul out of flash, and
 * kanji_hero_is_large() picks it by LENGTH — so a short headword containing a
 * character the hero happens not to carry rendered as a tofu box, at 56 px,
 * dead centre of the card. Length is a layout constraint and coverage is a font
 * constraint; both have to hold, and only one of them can be decided without
 * LVGL. */
bool ui_font_can_draw(const lv_font_t *f, const char *s);

/* The face the headword should be drawn in: the hero when the word both fits it
 * and can be drawn by it, the title face otherwise. The two card sides call
 * this rather than deciding separately, because a headword that changed size
 * between the question and the answer would read as two different words. */
const lv_font_t *ui_hero_face(const char *front);

/* --- immediate-mode drawing ----------------------------------------------
 * For the things LVGL widgets cannot express on this panel: the icon glyphs
 * and the wordmark's play badge. Called only from a LV_EVENT_DRAW_MAIN
 * handler, in ABSOLUTE screen coordinates (add lv_obj_get_coords()'s origin).
 *
 * `white` draws in white — used to punch a hole in something already drawn,
 * which is how the wordmark's play triangle is cut out of its badge and how the
 * Wi-Fi-off glyph gets its slash. */
void ui_draw_line_abs(lv_layer_t *L, int x1, int y1, int x2, int y2, int w, bool white);
void ui_draw_ring_abs(lv_layer_t *L, int cx, int cy, int r, int w, int a0, int a1);
void ui_draw_rect_abs(lv_layer_t *L, int x1, int y1, int x2, int y2,
                      bool fill, int border, bool white);

/* --- the screens ----------------------------------------------------------
 * Each screen is one file and obeys the same two-call contract: create() builds
 * a pane the size of the content area and returns it (the router positions and
 * shows/hides it), update() rewrites its widgets from a snapshot and touches
 * nothing else. A NULL snapshot means "blank yourself".
 *
 * Nothing in a screen file talks to the panel, keeps state beyond its widgets,
 * or knows which screen is on glass.
 *
 * The answer takes a grade cursor as well as the card. Reading sheets take a
 * semantic page index selected by the first physical key. */
lv_obj_t *ui_card_question_create(lv_obj_t *par);
void      ui_card_question_update(const kanji_t *k);

lv_obj_t *ui_card_answer_create(lv_obj_t *par);
void      ui_card_answer_update(const kanji_t *k, kanji_grade_t cursor);

/* The dock alone, for the partial refresh that moves the cursor without
 * flashing the whole panel. */
void      ui_card_answer_dock(const kanji_t *k, kanji_grade_t cursor);

lv_obj_t *ui_sheet_desc_create(lv_obj_t *par);
void      ui_sheet_desc_update(const kanji_t *k, int page);

lv_obj_t *ui_sheet_comments_create(lv_obj_t *par);
void      ui_sheet_comments_update(const kanji_t *k, int page);

lv_obj_t *ui_sheet_fsrs_create(lv_obj_t *par);
void      ui_sheet_fsrs_update(const kanji_t *k, int page);

/* The paper title row every sheet shares: headword at left, page title at
 * right. The rail carries the sheet identity and page position. */
typedef struct {
    lv_obj_t *word;
    lv_obj_t *title;
} ui_sheet_band_t;

void ui_sheet_band_create(lv_obj_t *par, ui_sheet_band_t *out, const char *title);
void ui_sheet_band_update(const ui_sheet_band_t *band, const kanji_t *k);

/* "1/3" in the body's bottom-right corner. Hidden when there is only one page:
 * a pager that always says 1/1 trains the eye to ignore it. */
void      ui_pager_set(lv_obj_t *pager, int page, int pages);

#ifdef __cplusplus
}
#endif
