/*
 * ui_fonts.h — the faces this board draws with.
 *
 * Source: Noto Sans KR and Noto Sans JP, Regular and Medium, plus Noto Serif JP
 * SemiBold for the hero (SIL Open Font License 1.1 — fonts/OFL.txt). The body
 * families are cuts of the same Source Han Sans design, so Korean and Japanese
 * text share a baseline, a stroke weight and a colour. Body text stays Sans
 * because a serif's thin strokes drop out at 16 px after binarization; the
 * Serif face is reserved for the 56 px display-sized headword. Every generated
 * face remains 1 bpp because anti-aliasing would cost four times the flash for
 * pixels the panel thresholds straight back to black and white.
 *
 * ## The body faces carry both scripts whole, on purpose
 *
 * The fortune board could subset its fonts down to seventy glyphs because every
 * string it drew was a literal in its own source. This board draws a headword,
 * its かな reading, example sentences, Korean senses and comment bodies that
 * arrive from kanjis.ai at runtime. There is no symbol list that can be derived
 * ahead of time, and the failure mode of guessing is a tofu box in the middle of
 * somebody's card.
 *
 * So each of the three body faces carries 9,242 glyphs: the 2350 KS X 1001
 * 완성형 syllables, ASCII, every kana, all 6355 kanji of JIS X 0208 (level 1
 * and level 2), the 94-character JIS punctuation row, the typography the UI
 * composes at runtime, and the 158 component forms and marks of
 * S_DATA_RADICALS. Neither Noto family can do that alone — Noto Sans KR is
 * missing 289 of the 2965 level 1 kanji (the 新字体 with no hanja counterpart)
 * and Noto Sans JP has no Hangul at all — so tools/gen_fonts.py converts each
 * face from both, splitting the symbol set between them.
 *
 * S_DATA_RADICALS is there because the 설명 sheet's body text is decompositions
 * — 別 = 另 + 刂 — and a radical's combining form is a codepoint outside the JIS
 * levels. Without it 3,217 of the catalog's 9,956 cards had a substitute
 * character in their explanation; with it, 263 do.
 *
 * What is still not covered is a character outside those tables: a syllable
 * outside 완성형, a JIS X 0212 or Unicode-only kanji, an emoji, anything above
 * U+FFFF (LVGL's sparse cmap stores uint16 offsets, so the astral planes cannot
 * be represented at all). Those draw as tofu. It is not a silent risk —
 * tools/kanji_server.py checks every string it is about to send against
 * gen_fonts.symbol_set(), which IS this set, and says so on a laptop rather
 * than on the glass.
 *
 * ## The hero needs LV_FONT_FMT_TXT_LARGE
 *
 * LVGL packs a glyph's bitmap offset into 20 bits unless LV_FONT_FMT_TXT_LARGE
 * is set, which caps one face at 1 MB of bitmap. ui_font_jp_56 has over 2 MiB
 * of bitmap data, so the build needs
 *
 *     CONFIG_LV_FONT_FMT_TXT_LARGE=y     (sdkconfig.defaults, for the firmware)
 *     #define LV_FONT_FMT_TXT_LARGE 1    (sim/lv_conf.h, for the simulator)
 *
 * It is not a silent requirement — without it the compiler stops on the #error
 * lv_font_conv wrote into ui_font_jp_56.c. It widens every glyph descriptor in
 * the build from 8 bytes to 16, which costs the three body faces 213 KB between
 * them; that is the price of a hero big enough to read across a room, and the
 * only alternative that fits in 20 bits is a 39 px "hero" that the 28 px face
 * already does better.
 *
 * ## Do not hand-edit
 *
 *     python3 tools/gen_fonts.py --download
 *
 * The generated .c files are committed so a normal build never needs node.
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Body text: the Korean senses, the example rows and their readings, comment
 * bodies, the FSRS sheet's prose, the queue counters and the footer legend. */
extern const lv_font_t ui_font_kr_16;

/* The headings around that text: the header band, the section titles (뜻, 예문),
 * the sheet titles, the grade dock's four labels, the JLPT chip. */
extern const lv_font_t ui_font_kr_20;

/* The headword when it is too long for the hero — kanji_hero_is_large() decides
 * — and the compact word in each sheet's band. Full coverage, and measurably
 * so: all 9,956 headwords in the catalog are drawable by this face, which is
 * what makes falling back to it a real answer rather than a smaller tofu. */
extern const lv_font_t ui_font_kr_28;

/* The hero: the Noto Serif JP SemiBold headword on the question and answer
 * screens, at the size that makes a card readable from across a room.
 *
 * Japanese script — kana, both kanji levels, the JIS punctuation row — plus the
 * whole printable ASCII range. ASCII because the catalog writes its bound forms
 * with a tilde (~がる) and its optional okurigana with parens (表(わ)す): with
 * only the ten digits this face once had, that was a tofu box at 56 px dead
 * centre on 133 cards.
 *
 * Not Hangul: the 완성형 set at 56 px is roughly a megabyte of flash for glyphs
 * a Japanese headword cannot contain.
 *
 * kanji_hero_is_large() chooses this face by LENGTH ALONE, so a headword outside
 * gen_fonts.hero_set() still reaches it — 2 of the catalog's 9,956 do, both
 * kanji outside JIS X 0208. That, and not the length, is what a coverage check
 * has to catch. */
extern const lv_font_t ui_font_jp_56;

#ifdef __cplusplus
}
#endif
