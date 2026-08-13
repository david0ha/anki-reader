/*
 * ui_fonts.h — the Korean faces this board draws with.
 *
 * Source: Noto Sans KR Regular/Medium (SIL Open Font License 1.1 —
 * fonts/OFL.txt). Uniform strokes survive 1-bit binarization reliably; the
 * 28px tarot headline adds hierarchy without sacrificing those fine strokes.
 *
 * ## All sizes carry the whole 완성형 set, on purpose
 *
 * The fortune board could subset its fonts down to seventy glyphs because every
 * string it drew was a literal in its own source. This board draws note titles,
 * tag names and inbox items that arrive from the network at runtime. There is
 * no symbol list that can be derived ahead of time, and the failure mode of
 * guessing is a tofu box on somebody's note title.
 *
 * So all three generated sizes contain the 2350 KS X 1001 완성형
 * syllables plus ASCII and punctuation — about 100 KB each at 1 bpp, against an
 * 8 MB app partition. 1 bpp because the panel binarizes anyway: anti-aliasing
 * would cost four times the flash to produce pixels that are then thresholded
 * back to black and white.
 *
 * The one thing still not covered is a syllable outside 완성형 (old Hangul, or
 * a rare modern combination). Those are vanishingly unlikely in a note title
 * and would show as tofu; if it ever matters, the fix is a different symbol
 * range in tools/gen_fonts.py, not a code change.
 *
 * ## Do not hand-edit
 *
 * Run
 *
 *     python3 tools/gen_fonts.py --download
 *
 * The generated .c files are committed so a normal build never needs node.
 *
 * Latin digits at display sizes come from LVGL's built-in Montserrat faces —
 * see sdkconfig.defaults and UI_F_NUM* in ui_internal.h.
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Body text: note titles, inbox items, agent notes, table values. */
extern const lv_font_t ui_font_kr_16;

/* Headings, the header line, section titles and the stat captions. */
extern const lv_font_t ui_font_kr_20;

/* Tarot headline: full network-safe Noto Sans KR Medium at display scale. */
extern const lv_font_t ui_font_kr_28;

#ifdef __cplusplus
}
#endif
