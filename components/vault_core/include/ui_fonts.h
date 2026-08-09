/*
 * ui_fonts.h — the subset CJK faces used by the fortune UI.
 *
 * Source: Noto Serif KR, Regular and Bold (SIL Open Font License 1.1 —
 * fonts/OFL.txt). A serif face on purpose: the 만세력 mockup is set in a
 * brush-derived serif, and on a 1-bit panel the thick/thin contrast survives
 * binarization better than a uniform-stroke sans at these sizes. Bold is used
 * exactly where the mockup asks for weight 700: the grade, the seal, and the
 * fortune-table headers.
 *
 * All faces are 1-bpp and carry only the glyphs that actually appear on
 * screen — a couple hundred of Korean's ~11k — which is what keeps them small
 * enough to live in flash as C source.
 *
 * Do NOT hand-edit these files, and do not hand-maintain the glyph list. Run
 *
 *     python3 tools/gen_fonts.py --download
 *
 * which derives the symbol set from omikuji_messages.h and saju.c and calls
 * lv_font_conv. Editing a message without regenerating is how you get a tofu
 * box on the glass; the generator exists so that cannot happen. Characters
 * that only exist in runtime-composed strings (digits and punctuation of the
 * date line, the space in "<hanja> <hangul>") are added explicitly there.
 *
 * ASCII on page 1 (clock, temperature, battery) comes from LVGL's built-in
 * Montserrat faces — see sdkconfig.defaults.
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The grade (大吉 … 大凶), Bold. One size for every rank, like the mockup —
 * one- and two-character grades share an optical line by centering. */
extern const lv_font_t ui_font_kr_hanja_34;

/* The seal's 吉/凶, Bold. */
extern const lv_font_t ui_font_kr_hanja_16;

/* The fortune-table headers 財運/事業/對人/健康, Bold, at table-column width. */
extern const lv_font_t ui_font_kr_hanja_12;

/* The 만세력 page's body: date line, side pillars, the vertical verse and its
 * tags, table values, foot line. Hangul + ASCII only. */
extern const lv_font_t ui_font_kr_12;

/* Head band, page-1 labels, the composed 일진 line, and the overlay. */
extern const lv_font_t ui_font_kr_16;

#ifdef __cplusplus
}
#endif
