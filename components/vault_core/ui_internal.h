/*
 * ui_internal.h — the drawing shorthand every face shares.
 *
 * Private to vault_core: it is not in include/, and nothing outside the UI
 * files may include it. The public surface is ui_kanji.h.
 *
 * Why a shorthand at all: on a 1-bit panel every widget wants the same six
 * style calls (no theme, no radius, black on white, no padding, no scrolling),
 * and repeating them four hundred times is how a page ends up with a rounded
 * corner or a grey border that binarizes into a dashed line. One helper per
 * shape, used everywhere, means the panel's constraints are enforced once.
 *
 * There is no grid in this file. Every rectangle on the glass is a named field
 * of kanji_front_layout_t or kanji_back_layout_t in ui_kanji_layout.h, which is
 * pure integers and host-tested, and a screen pane is now the full 648x480 —
 * so a layout rectangle is used verbatim, with no translation. The rail-and-
 * main-column geometry this file used to carry (LOCAL_X/LOCAL_Y, UI_PAD,
 * UI_RULE) is gone with the rail: two copies of a coordinate is one copy too
 * many, and the copy in a header is the one nothing asserts on.
 */
#pragma once

#include <stdarg.h>
#include <stddef.h>

#include "kanji_model.h"
#include "kanji_nav.h"
#include "lvgl.h"
#include "ui_fonts.h"
#include "ui_kanji.h"
#include "ui_kanji_layout.h"
#include "ui_strings.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The two panel dimensions are repeated here because a screen file that has to
 * include a layout header to ask how wide the panel is reads worse than one
 * that already knows. Nothing else about the geometry lives in this file. */
#define UI_W            KANJI_SCREEN_W
#define UI_H            KANJI_SCREEN_H

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
 *   - an opaque screen root that covers the previous pane's framebuffer pixels
 *     instead of letting them show through.
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

/* ui_lab_w taking a layout rectangle, so a screen file never unpacks x/y/w by
 * hand. `r.h` is deliberately NOT used: the box is pinned to one line of `f`
 * for the reason ui_lab_w's body explains — a label with only a width set grows
 * downwards and wraps instead of ellipsizing, and the second line lands on
 * whatever the layout put below it. The layout's `h` is the slot the row is
 * allowed to occupy; the label's height is what decides wrap-vs-ellipsis, and
 * conflating the two is how a one-line row silently becomes two.
 *
 * Use ui_lab_wrap_r for a block the layout genuinely sized for several lines. */
lv_obj_t *ui_lab_r(lv_obj_t *par, kanji_rect_t r,
                   const lv_font_t *f, lv_text_align_t align, const char *txt);

/* A fixed w AND h block that wraps inside the rectangle instead of ellipsizing.
 * For the blocks the layout sized for more than one line — the senses at two
 * and the 성립 prose at three. Text that overruns `r.h` is clipped at the
 * rectangle rather than pushing the block below it down the page, which is the
 * behaviour the 831-byte description depends on: it ellipsizes into its slot by
 * design, and the simulator asserts it never overflows. */
lv_obj_t *ui_lab_wrap_r(lv_obj_t *par, kanji_rect_t r,
                        const lv_font_t *f, lv_text_align_t align, const char *txt);

/* Letter-spacing, in px.
 *
 * The rule this exists under: tracking is for CAPS and short cut labels, which
 * were drawn to be spaced and read as one long word without it. Korean or
 * Japanese PROSE must never take it — a CJK glyph is already full-width and its
 * own advance is the word spacing, so adding to it takes the sentence apart
 * into a column of unrelated characters. This helper is for eyebrows, and for
 * nothing else. */
void ui_track(lv_obj_t *label, int px);

/* An eyebrow: UI_F_BODY, left-aligned, filling `r`, with 2 px of tracking.
 *
 * The single most load-bearing device on the answer face. Every block opens
 * with one — 뜻 · いみ, 성립 · 成り立ち, 예문 · れいぶん, 읽기 · よみ — and it is
 * what makes a page this dense read as designed rather than dumped: the eye
 * finds the four openings before it reads a word, and a section becomes
 * eyebrow -> hairline -> content instead of an undifferentiated block of type.
 * It costs three lines, which is why there is no excuse for a block without
 * one. */
lv_obj_t *ui_eyebrow(lv_obj_t *par, kanji_rect_t r, const char *txt);

/* A fixed headword box that wraps without ellipsis. `h` is sized from the
 * fallback face's measured worst case, so every model-valid front remains
 * visible; short covered words still use the serif face. */
lv_obj_t *ui_lab_headword(lv_obj_t *par, int x, int y, int w, int h,
                          const lv_font_t *f, const char *txt);

/* Normalize a raw model front for display, select the face from that canonical
 * text, and copy it into `label`. The raw snapshot remains untouched. */
void ui_apply_headword(lv_obj_t *label, const char *front);

/* White-on-black text for selected surfaces and exceptional-state stamps. */
lv_obj_t *ui_lab_inv(lv_obj_t *par, int x, int y, int w,
                     const lv_font_t *f, lv_text_align_t align, const char *txt);

/* Let a fixed prose label wrap inside `height` px instead of ellipsizing.
 * The in-place form of ui_lab_wrap_r, for a label already built by something
 * else — the overlay body, and the provisioning states. */
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
 * and can be drawn by it, the title face otherwise. The two card faces call
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

/* --- the two faces --------------------------------------------------------
 * A card has a front and a back and nothing else. Each is one file and obeys
 * the same two-call contract: create() builds a pane the FULL SIZE OF THE PANEL
 * (0,0,648,480) and returns it, so every layout rectangle is used verbatim with
 * no translation; update() rewrites its widgets from a snapshot and touches
 * nothing else. A NULL snapshot means "blank yourself".
 *
 * Nothing in a face file talks to the panel, keeps state beyond its widgets, or
 * knows which face is on glass.
 *
 * Both faces take the board status as well as the card: 오프라인 / 오래됨 / DEMO
 * belongs on whichever face is up, and a frame that hides its own staleness on
 * one of the two is a frame that lies half the time.
 *
 * The back takes the nav rather than a grade cursor. There is no cursor: the
 * four buttons ARE the four grades, so the dock reads nav->committed to know
 * whether to ink a cell at all, and nav->grade to know which. */
lv_obj_t *ui_card_front_create(lv_obj_t *par);
void      ui_card_front_update(const kanji_t *k, const ui_status_t *st);

lv_obj_t *ui_card_back_create(lv_obj_t *par);
void      ui_card_back_update(const kanji_t *k, const kanji_nav_t *nav,
                              const ui_status_t *st);

/* The dock alone. The grade-cursor partial refresh it was written for is gone
 * with the cursor — committing is one press and one full redraw — but the dock
 * is still the one region that changes without the card changing, so it keeps
 * its own entry point rather than making the router redraw a whole face to ink
 * one cell. */
void      ui_card_back_dock(const kanji_t *k, const kanji_nav_t *nav);

#ifdef __cplusplus
}
#endif
