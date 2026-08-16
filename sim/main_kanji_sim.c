/*
 * Pixel acceptance renderer for the kanji study device.
 *
 * This calls the same ui_kanji.c, the same nav state machine, the same model,
 * the same parser and the same bitmap fonts as the ESP32 firmware. The only
 * substitution is the display flush: instead of transferring the one-bit result
 * to the UC8179, it writes a monochrome BMP.
 *
 * It is not a preview, it is a test. It fails the build on a glyph the font
 * cannot draw, on a screen that rendered nothing where the layout says
 * something belongs, and on a grade dock that has anything other than exactly
 * one cell filled — which is the one defect a screenshot would not reveal,
 * because a dock with two cursors and a dock with one look equally plausible
 * until you count the black pixels.
 *
 * Every state it renders is also left behind as a PNG, so sim/shots/ is the
 * board's gallery: one image per screen, generated from the code that ships.
 */
#include "lvgl.h"

#include "kanji_mock.h"
#include "kanji_model.h"
#include "kanji_nav.h"
#include "kanji_service.h"
#include "ui_fonts.h"
#include "ui_kanji.h"
#include "ui_internal.h"
#include "ui_kanji_layout.h"
#include "ui_strings.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HOR KANJI_SCREEN_W
#define VER KANJI_SCREEN_H

static uint8_t  fb[HOR * VER * 2];
static uint16_t capture[HOR * VER];
static uint8_t  grade_frame[HOR * VER];
static uint32_t tick_now;
static int      failures;
static char     shot_dir[512] = "shots";
static lv_obj_t *style_root;

static lv_obj_t *find_visible_label_text(lv_obj_t *obj, const char *text);

#define FAILV(fmt, ...) do { failures++; printf("  FAIL " fmt "\n", __VA_ARGS__); } while (0)
#define FAIL(msg)       do { failures++; printf("  FAIL %s\n", (msg)); } while (0)

static uint32_t tick_cb(void) { return tick_now; }

static void flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    int width = area->x2 - area->x1 + 1;
    int height = area->y2 - area->y1 + 1;
    if (width == HOR && height == VER) {
        memcpy(capture, pixels, sizeof(capture));
    } else {
        for (int y = 0; y < height; y++) {
            int dst_y = area->y1 + y;
            if (dst_y < 0 || dst_y >= VER) continue;
            int dst_x = area->x1 < 0 ? 0 : area->x1;
            int src_x = dst_x - area->x1;
            int copy_w = width - src_x;
            if (dst_x + copy_w > HOR) copy_w = HOR - dst_x;
            if (copy_w > 0) {
                memcpy(&capture[dst_y * HOR + dst_x],
                       &((uint16_t *)pixels)[dst_y * HOR + dst_x],
                       (size_t)copy_w * sizeof(uint16_t));
            }
        }
    }
    lv_display_flush_ready(display);
}

static void refresh(void)
{
    for (int i = 0; i < 12; i++) {
        tick_now += 16;
        lv_timer_handler();
    }
}

/* --- reading the framebuffer ---------------------------------------------- */

static int is_black(int x, int y)
{
    if (x < 0 || y < 0 || x >= HOR || y >= VER) return 0;
    return capture[y * HOR + x] < 0x7fff;
}

static int ink_count(int x0, int y0, int x1, int y1)
{
    int count = 0;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > HOR) x1 = HOR;
    if (y1 > VER) y1 = VER;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) count += is_black(x, y);
    return count;
}

static int ink_rect(kanji_rect_t r)
{
    return ink_count(r.x, r.y, r.x + r.w, r.y + r.h);
}

static void want_ink(const char *name, kanji_rect_t r, int minimum)
{
    int found = ink_rect(r);
    if (found < minimum) {
        FAILV("%s: only %d black pixels in x[%d..%d) y[%d..%d)",
              name, found, r.x, r.x + r.w, r.y, r.y + r.h);
    }
}

/* An inverted area is mostly black, so "did anything render here" is asked the
 * other way round: count the WHITE pixels the text punched out of the fill. */
static int paper_count(kanji_rect_t r)
{
    return r.w * r.h - ink_rect(r);
}

static void want_paper(const char *name, kanji_rect_t r, int minimum)
{
    int found = paper_count(r);
    if (found < minimum) {
        FAILV("%s: only %d white pixels inside the filled area x[%d..%d) y[%d..%d)",
              name, found, r.x, r.x + r.w, r.y, r.y + r.h);
    }
}

static void want_mostly_paper(const char *name, kanji_rect_t r, int max_pct)
{
    int found = ink_rect(r);
    if (found * 100 > r.w * r.h * max_pct) {
        FAILV("%s: %d%% inked, wanted at most %d%%",
              name, found * 100 / (r.w * r.h), max_pct);
    }
}

static void want_no_ink(const char *name, kanji_rect_t r)
{
    const int found = ink_rect(r);
    if (found != 0) FAILV("%s: found %d black pixels, wanted none", name, found);
}

static void report_ink_ceiling(const char *name, kanji_rect_t r, int max_pct)
{
    const int ink = ink_rect(r);
    const int area = r.w * r.h;
    printf("  ink %-31s %d/%d (%.2f%%, limit %d%%)\n",
           name, ink, area, area ? 100.0 * ink / area : 0.0, max_pct);
    want_mostly_paper(name, r, max_pct);
}

/* RGB565 is deliberately inspected before write_bmp() thresholds it. Generated
 * 1 bpp faces contribute only the endpoints; LVGL's built-in Montserrat may
 * contribute neutral edge coverage. Neither is permission for chroma. */
static void check_raw_neutral_ramp(const char *shot_name)
{
    bool seen[32][64] = {{ false }};
    int levels = 0;
    int chromatic = 0;
    int first_x = -1, first_y = -1;
    uint16_t first_pixel = 0;

    for (int y = 0; y < VER; y++) {
        for (int x = 0; x < HOR; x++) {
            const uint16_t p = capture[y * HOR + x];
            const int r5 = (p >> 11) & 0x1f;
            const int g6 = (p >> 5) & 0x3f;
            const int b5 = p & 0x1f;
            const int neutral_g6 = (r5 * 63 + 15) / 31;
            if (r5 != b5 || abs(g6 - neutral_g6) > 1) {
                if (chromatic == 0) {
                    first_x = x;
                    first_y = y;
                    first_pixel = p;
                }
                chromatic++;
            }
            if (!seen[r5][g6]) {
                seen[r5][g6] = true;
                levels++;
            }
        }
    }

    if (chromatic != 0) {
        FAILV("%s: %d chromatic raw RGB565 pixels; first 0x%04x at (%d,%d)",
              shot_name, chromatic, first_pixel, first_x, first_y);
    } else {
        printf("  palette %-27s %d neutral RGB565 pixels, %d level(s)\n",
               shot_name, HOR * VER, levels);
    }
}

typedef struct {
    int visible_objects;
    int violations;
    const char *first_property;
    uint32_t first_value;
    lv_area_t first_area;
} style_check_t;

static bool token_color(lv_color_t color)
{
    return lv_color_eq(color, lv_color_black()) ||
           lv_color_eq(color, lv_color_white());
}

static bool binary_opa(lv_opa_t opa)
{
    return opa == LV_OPA_TRANSP || opa == LV_OPA_COVER;
}

static void style_violation(style_check_t *check, lv_obj_t *obj,
                            const char *property, uint32_t value)
{
    if (check->violations == 0) {
        check->first_property = property;
        check->first_value = value;
        lv_obj_get_coords(obj, &check->first_area);
    }
    check->violations++;
}

static void check_authored_color(style_check_t *check, lv_obj_t *obj,
                                 lv_style_prop_t property,
                                 const char *property_name)
{
    lv_style_value_t value;
    if (lv_obj_get_local_style_prop(obj, property, &value, LV_PART_MAIN) ==
            LV_STYLE_RES_FOUND &&
        !token_color(value.color)) {
        style_violation(check, obj, property_name, lv_color_to_u32(value.color));
    }
}

static void check_authored_opa(style_check_t *check, lv_obj_t *obj,
                               lv_style_prop_t property,
                               const char *property_name)
{
    lv_style_value_t value;
    if (lv_obj_get_local_style_prop(obj, property, &value, LV_PART_MAIN) ==
            LV_STYLE_RES_FOUND &&
        !binary_opa((lv_opa_t)value.num)) {
        style_violation(check, obj, property_name, (uint32_t)value.num);
    }
}

static void check_visible_style_node(lv_obj_t *obj, style_check_t *check)
{
    if (!lv_obj_is_visible(obj)) return;
    check->visible_objects++;

    const lv_color_t text = lv_obj_get_style_text_color(obj, LV_PART_MAIN);
    const lv_color_t bg = lv_obj_get_style_bg_color(obj, LV_PART_MAIN);
    const lv_color_t border = lv_obj_get_style_border_color(obj, LV_PART_MAIN);
    const lv_color_t line = lv_obj_get_style_line_color(obj, LV_PART_MAIN);
    const lv_opa_t text_opa = lv_obj_get_style_text_opa(obj, LV_PART_MAIN);
    const lv_opa_t bg_opa = lv_obj_get_style_bg_opa(obj, LV_PART_MAIN);
    const lv_opa_t border_opa = lv_obj_get_style_border_opa(obj, LV_PART_MAIN);
    const lv_opa_t line_opa = lv_obj_get_style_line_opa(obj, LV_PART_MAIN);
    const lv_opa_t object_opa = lv_obj_get_style_opa(obj, LV_PART_MAIN);
    const lv_opa_t recursive_opa = lv_obj_get_style_opa_recursive(obj, LV_PART_MAIN);
    const lv_opa_t layered_opa = lv_obj_get_style_opa_layered(obj, LV_PART_MAIN);

    const bool is_label = lv_obj_check_type(obj, &lv_label_class);
    const bool has_bg = bg_opa != LV_OPA_TRANSP;
    const bool has_border = lv_obj_get_style_border_width(obj, LV_PART_MAIN) > 0 &&
                            border_opa != LV_OPA_TRANSP;
    const bool has_line = lv_obj_get_style_line_width(obj, LV_PART_MAIN) > 0 &&
                          line_opa != LV_OPA_TRANSP;

    /* Local values are the authored token declarations. Check them even when
     * their current opacity/width makes them non-drawing; then separately check
     * every effective property that can contribute a visible pixel. */
    check_authored_color(check, obj, LV_STYLE_TEXT_COLOR,
                         "authored text color");
    check_authored_color(check, obj, LV_STYLE_BG_COLOR,
                         "authored background color");
    check_authored_color(check, obj, LV_STYLE_BORDER_COLOR,
                         "authored border color");
    check_authored_color(check, obj, LV_STYLE_LINE_COLOR,
                         "authored line color");
    check_authored_opa(check, obj, LV_STYLE_TEXT_OPA,
                       "authored text opacity");
    check_authored_opa(check, obj, LV_STYLE_BG_OPA,
                       "authored background opacity");
    check_authored_opa(check, obj, LV_STYLE_BORDER_OPA,
                       "authored border opacity");
    check_authored_opa(check, obj, LV_STYLE_LINE_OPA,
                       "authored line opacity");
    check_authored_opa(check, obj, LV_STYLE_OPA,
                       "authored object opacity");
    check_authored_opa(check, obj, LV_STYLE_OPA_LAYERED,
                       "authored layered opacity");

    if (is_label && !token_color(text))
        style_violation(check, obj, "text color", lv_color_to_u32(text));
    if (has_bg && !token_color(bg))
        style_violation(check, obj, "background color", lv_color_to_u32(bg));
    if (has_border && !token_color(border))
        style_violation(check, obj, "border color", lv_color_to_u32(border));
    if (has_line && !token_color(line))
        style_violation(check, obj, "line color", lv_color_to_u32(line));
    if (!binary_opa(text_opa))
        style_violation(check, obj, "text opacity", text_opa);
    if (!binary_opa(bg_opa))
        style_violation(check, obj, "background opacity", bg_opa);
    if (!binary_opa(border_opa))
        style_violation(check, obj, "border opacity", border_opa);
    if (!binary_opa(line_opa))
        style_violation(check, obj, "line opacity", line_opa);
    if (!binary_opa(object_opa))
        style_violation(check, obj, "object opacity", object_opa);
    if (!binary_opa(recursive_opa))
        style_violation(check, obj, "recursive object opacity", recursive_opa);
    if (!binary_opa(layered_opa))
        style_violation(check, obj, "layered opacity", layered_opa);
    if (is_label && bg_opa != LV_OPA_TRANSP)
        style_violation(check, obj, "label background opacity", bg_opa);

    const uint32_t children = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < children; i++) {
        check_visible_style_node(lv_obj_get_child(obj, (int32_t)i), check);
    }
}

static void check_visible_styles(const char *shot_name)
{
    style_check_t check = {0};
    check_visible_style_node(style_root, &check);
    if (check.violations != 0) {
        FAILV("%s: %d authored style violation(s); first %s=0x%x at "
              "x[%d..%d] y[%d..%d]",
              shot_name, check.violations, check.first_property,
              (unsigned)check.first_value,
              check.first_area.x1, check.first_area.x2,
              check.first_area.y1, check.first_area.y2);
    } else {
        printf("  styles  %-27s %d visible objects, black/white + binary opacity\n",
               shot_name, check.visible_objects);
    }
}

static void snapshot_threshold(uint8_t dst[HOR * VER])
{
    for (int y = 0; y < VER; y++) {
        for (int x = 0; x < HOR; x++) dst[y * HOR + x] = (uint8_t)is_black(x, y);
    }
}

static void check_dock_diff(const char *transition,
                            const uint8_t before[HOR * VER])
{
    int x1 = -1, y1 = -1, x2 = -1, y2 = -1;
    ui_kanji_dock_area(&x1, &y1, &x2, &y2);
    int inside = 0, outside = 0;
    int min_x = HOR, min_y = VER, max_x = -1, max_y = -1;

    for (int y = 0; y < VER; y++) {
        for (int x = 0; x < HOR; x++) {
            if (before[y * HOR + x] == (uint8_t)is_black(x, y)) continue;
            const bool in = x >= x1 && x < x2 && y >= y1 && y < y2;
            if (in) inside++;
            else outside++;
            if (x < min_x) min_x = x;
            if (x > max_x) max_x = x;
            if (y < min_y) min_y = y;
            if (y > max_y) max_y = y;
        }
    }

    printf("  xor %-30s inside=%d outside=%d bounds=x[%d..%d) y[%d..%d)\n",
           transition, inside, outside, min_x, max_x + 1, min_y, max_y + 1);
    if (inside == 0) FAILV("%s: no thresholded pixels changed inside the dock", transition);
    if (outside != 0) FAILV("%s: %d thresholded pixels changed outside the dock",
                            transition, outside);
}

/* --- glyph coverage ------------------------------------------------------- */

static uint32_t utf8_next(const char *text, int *index)
{
    unsigned char c = (unsigned char)text[*index];
    int extra = c < 0x80 ? 0 : (c < 0xe0 ? 1 : (c < 0xf0 ? 2 : 3));
    uint32_t cp = c < 0x80 ? c : (c & (0x3fU >> extra));
    for (int i = 0; i < extra && text[*index + 1 + i]; i++)
        cp = (cp << 6) | ((unsigned char)text[*index + 1 + i] & 0x3fU);
    *index += extra + 1;
    return cp;
}

static void cover(const lv_font_t *font, const char *field, const char *text)
{
    int at = 0;
    while (text && text[at]) {
        uint32_t cp = utf8_next(text, &at);
        lv_font_glyph_dsc_t glyph;
        if (cp != '\n' && cp != '\r' && cp != ' ' &&
            !lv_font_get_glyph_dsc(font, &glyph, cp, 0)) {
            FAILV("%s: U+%04X is missing from the font", field, cp);
        }
    }
}

/* --- font coverage ---------------------------------------------------------
 * Everything in the snapshot is drawn from a body face, so every body face must
 * carry it. The hero face is checked separately and only against the headword,
 * because that is the only string that ever reaches it. */

static void check_fonts(const kanji_t *k)
{
    static const lv_font_t *BODY[] = {
        &ui_font_kr_16, &ui_font_kr_20, &ui_font_kr_28,
    };
    const kanji_card_t *c = &k->card;

    for (size_t f = 0; f < sizeof BODY / sizeof BODY[0]; f++) {
        const lv_font_t *font = BODY[f];
        cover(font, "deck", k->session.deck);
        cover(font, "level", k->session.level);
        cover(font, "front", c->front);
        cover(font, "reading", c->reading);
        cover(font, "on reading", c->on_reading);
        cover(font, "kun reading", c->kun_reading);
        for (int i = 0; i < c->sense_count; i++) cover(font, "sense", c->senses[i]);
        for (int i = 0; i < c->example_count; i++) {
            cover(font, "example", c->examples[i].text);
            cover(font, "example reading", c->examples[i].reading);
            cover(font, "example gloss", c->examples[i].gloss);
        }
        cover(font, "description", c->description);
        cover(font, "hook title", c->hook_title);
        cover(font, "hook body", c->hook_body);
        cover(font, "composition", c->composition);
        for (int i = 0; i < c->part_count; i++) {
            cover(font, "part glyph", c->parts[i].glyph);
            cover(font, "part meaning", c->parts[i].meaning);
            cover(font, "part reading", c->parts[i].reading);
        }
        cover(font, "fsrs state", c->fsrs.state_label);
        cover(font, "fsrs due", c->fsrs.due);
        for (int g = KANJI_GRADE_AGAIN; g <= KANJI_GRADE_EASY; g++) {
            cover(font, "preview span", kanji_preview_span(k, (kanji_grade_t)g));
            cover(font, "grade label", kanji_grade_label((kanji_grade_t)g));
        }

        /* The eyebrows are the only fixed copy left on the answer face, and
         * they are the likeliest strings on the board to carry a character no
         * other literal does: each one pairs Korean with a Japanese word, and
         * 成り立ち's 成 and 立 reach the face from nowhere else. */
        cover(font, "eyebrow meaning", S_EB_MEANING);
        cover(font, "eyebrow build",   S_EB_BUILD);
        cover(font, "eyebrow example", S_EB_EXAMPLE);
        cover(font, "eyebrow reading", S_EB_READING);
        cover(font, "eyebrow parts",   S_EB_PARTS);
        cover(font, "eyebrow memory",  S_EB_MEMORY);
        cover(font, "on label",  S_ON_READING);
        cover(font, "kun label", S_KUN_READING);
        cover(font, "stat reps",   S_STAT_REPS);
        cover(font, "stat lapses", S_STAT_LAPSES);
        cover(font, "stat difficulty", S_STAT_DIFFICULTY);
        cover(font, "plate state",     S_PLATE_STATE);
        cover(font, "plate reps",      S_PLATE_REPS);
        cover(font, "plate stability", S_PLATE_STABILITY);
        cover(font, "plate lapses",    S_PLATE_LAPSES);
        cover(font, "hint wait", S_HINT_WAIT);
        cover(font, "composed", S_COMPOSED_CHARS);
        cover(font, "data punctuation", S_DATA_PUNCT);
        cover(font, "reveal prompt", S_TAP_TO_REVEAL);
        cover(font, "session done", S_SESSION_DONE);
    }

    /* Whatever face the headword ends up in must be able to draw it. This is
     * the invariant, not "the hero has every glyph": the hero is deliberately
     * Japanese-only, so the guarantee has to come from ui_hero_face() choosing
     * the fallback rather than from the hero being complete. */
    cover(ui_hero_face(c->front), "hero headword", c->front);
}

/* The shapes the catalog's headwords actually take. */
static void check_hero_face_is_always_drawable(void)
{
    static const char *HEADWORDS[] = {
        "会", "会う", "出会う", "取り替え", "取り替える",
        "~がたい", "~がる", "~ごと", "~さん", "~すぎ", "~ちゃん",
        "(する)", "お(ご)",
        "取り替えるつもり", "あいうえおかきくけこ",
        "", "N5",
    };
    for (size_t i = 0; i < sizeof HEADWORDS / sizeof HEADWORDS[0]; i++) {
        const lv_font_t *f = ui_hero_face(HEADWORDS[i]);
        if (!ui_font_can_draw(f, HEADWORDS[i])) {
            FAILV("hero face cannot draw the headword it was chosen for: %s",
                  HEADWORDS[i]);
        }
    }
}

static void check_printable_ascii_hero(void)
{
    for (int cp = 0x20; cp <= 0x7e; cp++) {
        lv_font_glyph_dsc_t glyph;
        if (!lv_font_get_glyph_dsc(&ui_font_jp_56, &glyph, (uint32_t)cp, 0) ||
            glyph.is_placeholder) {
            FAILV("the hero face cannot draw printable ASCII 0x%02x", cp);
        }
    }
    if (ui_hero_face("~がたい") != &ui_font_jp_56) {
        FAIL("a four-character ASCII+kana headword must reach the hero face");
    }
}

/* --- shots ---------------------------------------------------------------- */

static bool write_exact(FILE *out, const void *bytes, size_t size)
{
    return fwrite(bytes, 1, size, out) == size;
}

static void write_bmp(const char *name)
{
    check_raw_neutral_ramp(name);
    check_visible_styles(name);

    char path[640];
    snprintf(path, sizeof path, "%s/%s.bmp", shot_dir, name);

    int row_size = (HOR * 3 + 3) & ~3;
    int data_size = row_size * VER;
    int file_size = 54 + data_size;
    uint8_t header[54] = {0};
    header[0] = 'B'; header[1] = 'M';
    header[2] = file_size; header[3] = file_size >> 8;
    header[4] = file_size >> 16; header[5] = file_size >> 24;
    header[10] = 54; header[14] = 40;
    header[18] = (uint8_t)HOR; header[19] = (uint8_t)(HOR >> 8);
    header[22] = (uint8_t)VER; header[23] = (uint8_t)(VER >> 8);
    header[24] = VER >> 16; header[25] = VER >> 24;
    header[26] = 1; header[28] = 24;
    header[34] = data_size; header[35] = data_size >> 8;
    header[36] = data_size >> 16; header[37] = data_size >> 24;

    FILE *out = fopen(path, "wb");
    if (!out) { FAILV("cannot open output %s", path); return; }
    bool write_ok = write_exact(out, header, sizeof header);
    uint8_t *row = calloc(1, (size_t)row_size);
    if (!row) {
        FAIL("cannot allocate BMP row");
        if (fclose(out) != 0) FAILV("cannot close output %s", path);
        return;
    }
    for (int y = VER - 1; y >= 0 && write_ok; y--) {
        for (int x = 0; x < HOR; x++) {
            uint8_t value = is_black(x, y) ? 0 : 255;
            row[x * 3] = row[x * 3 + 1] = row[x * 3 + 2] = value;
        }
        write_ok = write_exact(out, row, (size_t)row_size);
    }
    free(row);
    if (!write_ok || ferror(out)) FAILV("cannot write complete output %s", path);
    if (fclose(out) != 0) FAILV("cannot close output %s", path);
}

static lv_obj_t *find_visible_label_text(lv_obj_t *obj, const char *text)
{
    if (!lv_obj_is_visible(obj)) return NULL;
    if (lv_obj_check_type(obj, &lv_label_class) &&
        strcmp(lv_label_get_text(obj), text) == 0) {
        return obj;
    }
    const uint32_t children = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < children; i++) {
        lv_obj_t *found = find_visible_label_text(
            lv_obj_get_child(obj, (int32_t)i), text);
        if (found) return found;
    }
    return NULL;
}

static void want_visible_text(const char *name, const char *text)
{
    if (!find_visible_label_text(style_root, text)) {
        FAILV("%s: exact visible label is missing: %s", name, text);
    }
}

/* Some labels are composed — the reveal prompt is the hint plus an arrow — so an exact match
 * would be asserting the composition rather than that the word reached the glass. */
static bool label_contains(lv_obj_t *obj, const char *needle)
{
    if (!lv_obj_is_visible(obj)) return false;
    if (lv_obj_check_type(obj, &lv_label_class) &&
        strstr(lv_label_get_text(obj), needle)) {
        return true;
    }
    const uint32_t children = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < children; i++) {
        if (label_contains(lv_obj_get_child(obj, (int32_t)i), needle)) return true;
    }
    return false;
}

static void want_text_somewhere(const char *name, const char *needle)
{
    if (!label_contains(style_root, needle)) {
        FAILV("%s: no visible label contains: %s", name, needle);
    }
}

/* --- the assertions this redesign exists for -------------------------------
 *
 * The five-screen UI this replaced passed every assertion it had while inking
 * 2.55% of the panel. That is the failure these checks are for: a page can be
 * correct in every rectangle it draws and still be mostly empty paper, and no
 * per-widget assertion notices. */

static int whole_screen_ink(void)
{
    return ink_count(0, 0, HOR, VER);
}

/* OCCUPANCY, not ink mass.
 *
 * The obvious metric — what fraction of the panel is black — is the wrong one,
 * and measuring it was itself a mistake worth recording. 1-bit CJK type at
 * 16 px is thin: a page that is completely full of text still only blackens
 * five or six percent of its pixels, and the shipped five-screen design inked
 * 2.55% while looking half empty. Ink mass cannot tell those apart, so a floor
 * set on it either passes everything or fails everything.
 *
 * What "the page does not use the space" actually means is that large regions
 * of it contain NOTHING. So divide the content area into a grid of 16 px cells
 * — one line of body type — and count the cells that contain at least one black
 * pixel. That is a direct measure of how much of the page is doing work, it is
 * insensitive to how heavy a face is, and it is the number that separates this
 * design from the one it replaced. */
#define OCC_CELL 16

static int occupancy_pct(void)
{
    const int x0 = KANJI_CONTENT_X, x1 = KANJI_CONTENT_R;
    const int y0 = 8, y1 = VER - 8;
    int cells = 0, used = 0;
    for (int y = y0; y + OCC_CELL <= y1; y += OCC_CELL) {
        for (int x = x0; x + OCC_CELL <= x1; x += OCC_CELL) {
            cells++;
            if (ink_count(x, y, x + OCC_CELL, y + OCC_CELL) > 0) used++;
        }
    }
    return cells ? (used * 100) / cells : 0;
}

/* CALIBRATING THE FLOOR.
 *
 * A floor picked to sit just under whatever the code happens to score is not a test, it is a
 * thermometer. These two are picked instead from what they have to CATCH — the loss of a whole
 * block, which is the regression this redesign is guarding against, because it is exactly what
 * the five-screen design did by putting three blocks behind buttons.
 *
 *   answer face, measured 56%. Its six blocks are the senses, 성립, 예문, 읽기, 구성 and 기억.
 *   The right rail is 184 of the 600 content columns and is the densest third of the page;
 *   deleting it drops occupancy to roughly 38%. A floor of 45% therefore fails if ANY of the
 *   three rail blocks stops rendering, and fails if either prose block does.
 *
 *   question face, measured 23% (24% on the demo card, 36% on the worst case). It is
 *   deliberately quiet — the brief for it was an art print, not a dashboard — and quieter still
 *   since the pull-quote came off it, so the only block left for this floor to catch is the
 *   plate. Suppressing the plate's rows in a rendered shot and re-running this grid scores 17%
 *   on the kanji card and 19% on the demo card, so 21% is the only floor that both fails when
 *   the plate goes and passes on every front shot. That is a two-point margin either side, which
 *   is thin — thin enough that it is NOT the real guard any more. check_front_blocks() below
 *   asserts ink in each of the front's rectangles by name, which is the specific test this
 *   percentage used to stand in for; the floor is kept as the coarse net underneath it.
 *
 * Both are recorded here rather than in a constant so that raising one later requires reading
 * the argument for the current value first. */
static void check_page_budget(const char *name, int min_occ, int max_ink_pct)
{
    const int total = HOR * VER;
    const int ink = whole_screen_ink();
    const int ink10 = (ink * 1000) / total;
    const int occ = occupancy_pct();
    printf("  page %-25s occupancy %3d%% (floor %d%%)   ink %d.%d%%\n",
           name, occ, min_occ, ink10 / 10, ink10 % 10);
    if (occ < min_occ) {
        FAILV("%s: only %d%% of the content grid carries anything, below the "
              "%d%% floor — the page is not using the panel", name, occ, min_occ);
    }
    if (ink * 100 > total * max_ink_pct) {
        FAILV("%s: inks %d.%d%% of the panel, above the %d%% ceiling",
              name, ink10 / 10, ink10 % 10, max_ink_pct);
    }
}

/* Every visible, non-blank label on the screen, with its real coordinates. */
#define MAX_LABELS 128
typedef struct {
    lv_obj_t   *obj;
    lv_area_t   area;
    const char *text;
} label_box_t;

static void collect_labels(lv_obj_t *obj, label_box_t *out, int *n)
{
    if (!lv_obj_is_visible(obj)) return;
    if (lv_obj_check_type(obj, &lv_label_class)) {
        const char *t = lv_label_get_text(obj);
        if (t && t[0] && *n < MAX_LABELS) {
            lv_area_t a;
            lv_obj_get_coords(obj, &a);
            out[*n].obj = obj;
            out[*n].area = a;
            out[*n].text = t;
            (*n)++;
        }
    }
    const uint32_t children = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < children; i++) {
        collect_labels(lv_obj_get_child(obj, (int32_t)i), out, n);
    }
}

static bool areas_overlap(const lv_area_t *a, const lv_area_t *b)
{
    return a->x1 <= b->x2 && b->x1 <= a->x2 && a->y1 <= b->y2 && b->y1 <= a->y2;
}

/* No two labels may share paper.
 *
 * Pixels cannot catch this. Black text drawn over black text is still black,
 * so a screenshot of two overlapping labels looks like one slightly bold
 * label — which is exactly what a fourteen-block two-column page produces the
 * first time one block grows a line. */
static void check_no_label_overlap(const char *name)
{
    label_box_t boxes[MAX_LABELS];
    int n = 0;
    collect_labels(style_root, boxes, &n);
    if (n >= MAX_LABELS) {
        FAILV("%s: more than %d visible labels; the overlap walk is truncated "
              "and is no longer proving anything", name, MAX_LABELS);
    }
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (areas_overlap(&boxes[i].area, &boxes[j].area)) {
                FAILV("%s: labels overlap: \"%s\" (%d,%d..%d,%d) and \"%s\" "
                      "(%d,%d..%d,%d)", name,
                      boxes[i].text, boxes[i].area.x1, boxes[i].area.y1,
                      boxes[i].area.x2, boxes[i].area.y2,
                      boxes[j].text, boxes[j].area.x1, boxes[j].area.y1,
                      boxes[j].area.x2, boxes[j].area.y2);
            }
        }
    }
}

/* Nothing in the left column may cross the gutter rule, and nothing in the
 * right rail may cross the margin. Asserted against the widgets' OWN
 * coordinates rather than against the layout constants, because the fault
 * being hunted is a renderer that ignored the rectangle it was handed. */
static void check_columns_contained(const char *name)
{
    label_box_t boxes[MAX_LABELS];
    int n = 0;
    collect_labels(style_root, boxes, &n);
    const kanji_back_layout_t *b = kanji_back_layout();
    const int top = b->col_rule.y;
    const int bot = b->col_rule.y + b->col_rule.h;

    for (int i = 0; i < n; i++) {
        const lv_area_t *a = &boxes[i].area;
        if (a->y1 < top || a->y2 >= bot) continue;      /* masthead or dock */
        const bool left = a->x1 < KANJI_COL_RULE_X;
        if (left && a->x2 >= KANJI_COL_RULE_X) {
            FAILV("%s: \"%s\" crosses the gutter rule at x=%d (%d..%d)",
                  name, boxes[i].text, KANJI_COL_RULE_X, a->x1, a->x2);
        }
        if (!left && a->x2 >= KANJI_CONTENT_R) {
            FAILV("%s: \"%s\" crosses the right margin at x=%d (%d..%d)",
                  name, boxes[i].text, KANJI_CONTENT_R, a->x1, a->x2);
        }
        if (a->x1 < KANJI_CONTENT_X) {
            FAILV("%s: \"%s\" crosses the left margin at x=%d (x1=%d)",
                  name, boxes[i].text, KANJI_CONTENT_X, a->x1);
        }
    }
}

/* THE SPOILER ASSERTION.
 *
 * The question face may print only Japanese and the learner's own history.
 * senses[], parts[].meaning and examples[].gloss are all Korean and all of them
 * ARE the answer. examples[].gloss is the one that gets shipped by accident: it
 * reads as harmless context right up until it prints 우연히 만나다 under 会う,
 * at which point the card is worthless and nothing in the build says so. */
static void check_front_hides_the_answer(const kanji_t *k)
{
    label_box_t boxes[MAX_LABELS];
    int n = 0;
    collect_labels(style_root, boxes, &n);

    const char *secrets[KANJI_SENSES_MAX + KANJI_PARTS_MAX + KANJI_EXAMPLES_MAX];
    const char *kinds[KANJI_SENSES_MAX + KANJI_PARTS_MAX + KANJI_EXAMPLES_MAX];
    int s = 0;
    for (int i = 0; i < k->card.sense_count; i++) {
        if (k->card.senses[i][0]) { secrets[s] = k->card.senses[i]; kinds[s++] = "sense"; }
    }
    for (int i = 0; i < k->card.part_count; i++) {
        if (k->card.parts[i].meaning[0]) {
            secrets[s] = k->card.parts[i].meaning; kinds[s++] = "part meaning";
        }
    }
    for (int i = 0; i < k->card.example_count; i++) {
        if (k->card.examples[i].gloss[0]) {
            secrets[s] = k->card.examples[i].gloss; kinds[s++] = "example gloss";
        }
    }
    if (s == 0) {
        FAIL("the spoiler check ran against a card with nothing to spoil; it "
             "would pass for any renderer and is proving nothing");
        return;
    }
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < s; j++) {
            if (strstr(boxes[i].text, secrets[j])) {
                FAILV("문제 leaks the answer: label \"%s\" contains the %s "
                      "\"%s\"", boxes[i].text, kinds[j], secrets[j]);
            }
        }
    }
}

/* Every block the question face is supposed to be carrying, asserted by name.
 *
 * occupancy_pct() answers "is this page using the panel" as one number, and one number cannot say
 * WHICH block went missing — nor notice at all when the block that went missing is small. That
 * was tolerable when the face had a pull-quote to lose as well; with the face down to a headword,
 * an ornament and a plate, the percentage's whole margin is the plate, and it is two points wide.
 * So each rectangle is checked for ink directly. The minimums are floors on "did this render",
 * not measurements: the hero is 56 px of kanji, the ornament's diamond is exactly 41 px by
 * construction, and a hairline inks its own length.
 *
 * Only valid for a card WITH history — 07-front-new-card collapses the four rows into 새 카드,
 * and 14/18 have no card at all, so those states are covered by check_no_label_overlap() and by
 * their shots instead. */
static void check_front_blocks(const char *name)
{
    const kanji_front_layout_t *f = kanji_front_layout();
    char label[96];

    snprintf(label, sizeof label, "%s: the headword", name);
    want_ink(label, f->hero, 200);

    snprintf(label, sizeof label, "%s: the ornament's left rule", name);
    want_ink(label, f->orn_left, 100);
    snprintf(label, sizeof label, "%s: the ornament's mark", name);
    want_ink(label, f->orn_mark, 30);
    snprintf(label, sizeof label, "%s: the ornament's right rule", name);
    want_ink(label, f->orn_right, 100);

    for (int i = 0; i < KANJI_PLATE_ROWS; i++) {
        snprintf(label, sizeof label, "%s: plate row %d's label", name, i);
        want_ink(label, f->plate_label[i], 30);
        snprintf(label, sizeof label, "%s: plate row %d's value", name, i);
        want_ink(label, f->plate_value[i], 30);
    }
    snprintf(label, sizeof label, "%s: the plate's hairline", name);
    want_ink(label, f->plate_rule, 100);

    snprintf(label, sizeof label, "%s: the queue counters", name);
    want_ink(label, f->queue, 40);
    snprintf(label, sizeof label, "%s: the reveal prompt", name);
    want_ink(label, f->prompt, 40);
}

/* --- the dock -------------------------------------------------------------- */

static void check_dock(const kanji_t *k, const kanji_nav_t *nav)
{
    const kanji_back_layout_t *b = kanji_back_layout();
    int filled = 0;

    for (int i = 0; i < KANJI_GRADE_COUNT; i++) {
        const kanji_grade_t g = kanji_button_grade((kanji_button_t)i);
        const bool want_filled = nav->committed && nav->grade == g;
        const kanji_rect_t cell = b->cell[i];
        const int ink = ink_rect(cell);
        const int area = cell.w * cell.h;
        const bool is_filled = ink * 100 > area * 50;

        if (is_filled) filled++;
        if (is_filled != want_filled) {
            FAILV("dock cell %d (grade %d) is %s but should be %s",
                  i, (int)g, is_filled ? "filled" : "plain",
                  want_filled ? "filled" : "plain");
        }

        char nm[96];
        snprintf(nm, sizeof nm, "dock cell %d name", i);
        if (want_filled) {
            /* White on black: the labels have to survive the inversion, which
             * is measured as PAPER inside a cell that is otherwise solid. */
            want_paper(nm, b->cell_name[i], 40);
        } else {
            want_ink(nm, b->cell_name[i], 40);
        }
        want_visible_text(nm, kanji_grade_label(g));
    }

    if (filled != (nav->committed ? 1 : 0)) {
        FAILV("%d dock cells are filled, wanted %d",
              filled, nav->committed ? 1 : 0);
    }

    /* The four cells must tile the dock exactly. A one-pixel crack between two
     * cells is a white stripe down a ruled row. */
    for (int i = 1; i < KANJI_GRADE_COUNT; i++) {
        if (b->cell[i].x != b->cell[i - 1].x + b->cell[i - 1].w) {
            FAILV("dock cells %d and %d do not abut (%d+%d != %d)",
                  i - 1, i, b->cell[i - 1].x, b->cell[i - 1].w, b->cell[i].x);
        }
    }
    (void)k;
}

static void check_public_dock_bounds(void)
{
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    ui_kanji_dock_area(&x1, &y1, &x2, &y2);
    const kanji_rect_t d = kanji_back_layout()->dock;
    if (x1 != d.x || y1 != d.y || x2 != d.x + d.w || y2 != d.y + d.h) {
        FAILV("dock accessor returned (%d,%d,%d,%d), wanted (%d,%d,%d,%d)",
              x1, y1, x2, y2, d.x, d.y, d.x + d.w, d.y + d.h);
    }
    if (x1 % KANJI_BYTE_ALIGN || x2 % KANJI_BYTE_ALIGN) {
        FAILV("the dock's x bounds (%d,%d) are not whole framebuffer bytes; a "
              "partial refresh would miss the thing that changed", x1, x2);
    }
}

/* --- fixtures -------------------------------------------------------------- */

static void set_str(char *dst, size_t cap, const char *src)
{
    kanji_str_copy(dst, cap, src);
}

/* A real kanji card, taken from the backend row for 語 (JLPT N3 Kanji):
 * 형성 principle, three components — exactly the number the rail holds — and an
 * 85-character build story, which is longer than the three-line slot and so
 * proves the ellipsis rather than assuming it never happens. The demo card is a
 * VOCAB card and is honest in having no on-yomi, no principle and one
 * component, but it therefore exercises almost none of the answer face. */
static kanji_t kanji_card_fixture(void)
{
    kanji_t k;
    memset(&k, 0, sizeof k);
    k.valid = true;
    k.source = KANJI_SOURCE_CATALOG;

    set_str(k.session.deck, sizeof k.session.deck, "JLPT N3 Kanji");
    set_str(k.session.level, sizeof k.session.level, "N3");
    k.session.streak = 23;
    k.session.reviewed_today = 41;
    k.session.left_new = 5;
    k.session.left_review = 22;
    k.session.retry = 1;
    k.session.track = 42;
    k.session.track_total = 68;

    kanji_card_t *c = &k.card;
    c->valid = true;
    set_str(c->id, sizeof c->id, "sim-kanji-go");
    set_str(c->front, sizeof c->front, "語");
    set_str(c->reading, sizeof c->reading, "ゴ");
    set_str(c->on_reading, sizeof c->on_reading, "ゴ");
    set_str(c->kun_reading, sizeof c->kun_reading, "かたる · かたらう");
    set_str(c->level, sizeof c->level, "N3");
    set_str(c->senses[0], sizeof c->senses[0], "단어");
    set_str(c->senses[1], sizeof c->senses[1], "언어");
    c->sense_count = 2;

    set_str(c->examples[0].text, sizeof c->examples[0].text, "国語");
    set_str(c->examples[0].reading, sizeof c->examples[0].reading, "こくご");
    set_str(c->examples[0].gloss, sizeof c->examples[0].gloss, "국어");
    set_str(c->examples[1].text, sizeof c->examples[1].text, "物語");
    set_str(c->examples[1].reading, sizeof c->examples[1].reading, "ものがたり");
    set_str(c->examples[1].gloss, sizeof c->examples[1].gloss, "이야기");
    set_str(c->examples[2].text, sizeof c->examples[2].text, "語る");
    set_str(c->examples[2].reading, sizeof c->examples[2].reading, "かたる");
    set_str(c->examples[2].gloss, sizeof c->examples[2].gloss, "말하다");
    c->example_count = 3;

    set_str(c->description, sizeof c->description,
            "語는 말을 뜻하는 言과 소리를 나타내는 吾가 합쳐진 형성자입니다.");
    set_str(c->hook_title, sizeof c->hook_title, "형성");
    set_str(c->hook_body, sizeof c->hook_body,
            "「言」(말)과 「口」(입)가 함께 '입으로 말하다'라는 뜻을 이루고, "
            "「五」는 여기서 소리부(ゴ)로 발음을 제공해 語의 음과 뜻을 결합한다.");

    set_str(c->parts[0].glyph, sizeof c->parts[0].glyph, "言");
    set_str(c->parts[0].meaning, sizeof c->parts[0].meaning, "말");
    set_str(c->parts[1].glyph, sizeof c->parts[1].glyph, "口");
    set_str(c->parts[1].meaning, sizeof c->parts[1].meaning, "입");
    set_str(c->parts[2].glyph, sizeof c->parts[2].glyph, "五");
    set_str(c->parts[2].meaning, sizeof c->parts[2].meaning, "다섯");
    c->part_count = 3;

    set_str(c->fsrs.state, sizeof c->fsrs.state, "review");
    set_str(c->fsrs.state_label, sizeof c->fsrs.state_label, "복습");
    set_str(c->fsrs.due, sizeof c->fsrs.due, "12일 뒤");
    c->fsrs.reps = 7;
    c->fsrs.lapses = 2;
    c->fsrs.stability_days = 12;
    c->fsrs.difficulty_pct = 38;
    set_str(c->preview.span[0], sizeof c->preview.span[0], "10분 뒤");
    set_str(c->preview.span[1], sizeof c->preview.span[1], "5일 뒤");
    set_str(c->preview.span[2], sizeof c->preview.span[2], "12일 뒤");
    set_str(c->preview.span[3], sizeof c->preview.span[3], "28일 뒤");
    return k;
}

static size_t fill_repeated(char *dst, size_t capacity, char glyph)
{
    if (!dst || capacity == 0) return 0;
    for (size_t i = 0; i + 1 < capacity; i++) dst[i] = glyph;
    dst[capacity - 1] = '\0';
    return capacity - 1;
}

static char widest_body_ascii(void)
{
    char widest = '\0';
    uint16_t widest_advance = 0;
    for (int cp = 1; cp <= 0x7f; cp++) {
        if (isspace((unsigned char)cp)) continue;
        lv_font_glyph_dsc_t glyph;
        if (!lv_font_get_glyph_dsc(&ui_font_kr_16, &glyph, (uint32_t)cp, 0) ||
            glyph.is_placeholder) {
            continue;
        }
        if (glyph.adv_w > widest_advance) {
            widest = (char)cp;
            widest_advance = glyph.adv_w;
        }
    }
    if (widest == '\0') FAIL("no drawable single-byte glyph in ui_font_kr_16");
    return widest;
}

/* Every field at its model maximum, in the widest glyph the body face has.
 * The point is not that all of it is readable — the design says outright that
 * an 819-byte explanation cannot be — but that none of it escapes its
 * rectangle or crosses a column. */
static kanji_t worst_case_fixture(const kanji_t *base)
{
    kanji_t k = *base;
    const char g = widest_body_ascii();
    char raw[KANJI_BODY_MAX];
    fill_repeated(raw, sizeof raw, g);

    kanji_text_collapse_whitespace(k.card.description,
                                   sizeof k.card.description, raw);
    kanji_text_collapse_whitespace(k.card.hook_body,
                                   sizeof k.card.hook_body, raw);
    fill_repeated(k.card.front, sizeof k.card.front, g);
    fill_repeated(k.card.reading, sizeof k.card.reading, g);
    fill_repeated(k.card.on_reading, sizeof k.card.on_reading, g);
    fill_repeated(k.card.kun_reading, sizeof k.card.kun_reading, g);

    k.card.sense_count = KANJI_SENSES_MAX;
    for (int i = 0; i < KANJI_SENSES_MAX; i++) {
        fill_repeated(k.card.senses[i], sizeof k.card.senses[i], g);
    }
    k.card.part_count = KANJI_PARTS_MAX;
    for (int i = 0; i < KANJI_PARTS_MAX; i++) {
        fill_repeated(k.card.parts[i].glyph, sizeof k.card.parts[i].glyph, g);
        fill_repeated(k.card.parts[i].meaning, sizeof k.card.parts[i].meaning, g);
        fill_repeated(k.card.parts[i].reading, sizeof k.card.parts[i].reading, g);
    }
    k.card.example_count = KANJI_EXAMPLES_MAX;
    for (int i = 0; i < KANJI_EXAMPLES_MAX; i++) {
        fill_repeated(k.card.examples[i].text, sizeof k.card.examples[i].text, g);
        fill_repeated(k.card.examples[i].reading,
                      sizeof k.card.examples[i].reading, g);
        fill_repeated(k.card.examples[i].gloss,
                      sizeof k.card.examples[i].gloss, g);
    }
    fill_repeated(k.card.hook_title, sizeof k.card.hook_title, g);
    return k;
}

static kanji_nav_t nav_front(void)
{
    kanji_nav_t n;
    kanji_nav_reset(&n);
    return n;
}

static kanji_nav_t nav_back(void)
{
    kanji_nav_t n;
    kanji_nav_reset(&n);
    n.revealed = true;
    return n;
}

/* Committed states are reached through a real press so the shot proves the
 * production path rather than a struct the test filled in itself. */
static kanji_nav_t nav_committed(kanji_button_t btn, const kanji_t *k)
{
    kanji_nav_t n = nav_back();
    kanji_nav_press(&n, btn, k);
    return n;
}

/* --- shots ----------------------------------------------------------------- */

static const ui_status_t ONLINE = { true, false, false, 0 };

static void shot(const kanji_t *k, const kanji_nav_t *nav,
                 const ui_status_t *st, const char *name)
{
    ui_kanji_set_data(k);
    ui_kanji_set_nav(nav);
    ui_kanji_set_status(st ? st : &ONLINE);
    refresh();
    write_bmp(name);
}

/* 문제 MUST NOT READ examples[].
 *
 * This is the machine-checkable half of the spoiler rule, and it is stricter than
 * check_front_hides_the_answer() above, which only catches Korean reaching the glass. The bug it
 * exists for was entirely Japanese and therefore invisible to that check: the face used to set
 * examples[0].text as a pull-quote with examples[0].reading beneath it, and because the catalog's
 * examples are the word list under the kanji's reading entries rather than sentences using the
 * headword, 破れる printed 破る / やぶる — the reading of a different word, one kana off the
 * answer, in the slot a learner reads as the answer.
 *
 * A rectangle-based check cannot catch the regression, because reintroducing the quote would come
 * with its own rectangle and every "does this rectangle have ink" assertion would pass. So this
 * renders the same card twice — once with its examples, once with example_count zeroed — and
 * demands the two frames be identical to the pixel. Any path from examples[] to the front, at any
 * size, in any slot, differs somewhere and fails here. It is also the 1,525-of-9,956 case running
 * on every build: those cards have no example, and this face must be the same face for them. */
static uint16_t twin_frame[HOR * VER];

static void check_front_ignores_examples(const char *name, const kanji_t *rich,
                                         const kanji_t *bare, const kanji_nav_t *nav)
{
    if (rich->card.example_count == 0) {
        FAILV("%s: the reference card carries no examples, so this compares a card "
              "with none against a card with none and proves nothing", name);
        return;
    }

    ui_kanji_set_nav(nav);
    ui_kanji_set_status(&ONLINE);

    ui_kanji_set_data(rich);
    refresh();
    memcpy(twin_frame, capture, sizeof twin_frame);

    /* bare LAST, so the caller's shot and the framebuffer still agree afterwards. */
    ui_kanji_set_data(bare);
    refresh();

    for (int y = 0; y < VER; y++) {
        for (int x = 0; x < HOR; x++) {
            if (twin_frame[y * HOR + x] == capture[y * HOR + x]) continue;
            FAILV("%s: 문제 draws pixel (%d,%d) differently for a card with %d "
                  "examples than for the same card with none — the question face "
                  "is reading examples[] again, which is how 破る / やぶる got "
                  "printed under 破れる", name, x, y, rich->card.example_count);
            return;
        }
    }
}

/* --- main ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    if (argc > 1) snprintf(shot_dir, sizeof shot_dir, "%s", argv[1]);

    lv_init();
    lv_tick_set_cb(tick_cb);
    lv_display_t *display = lv_display_create(HOR, VER);
    lv_display_set_buffers(display, fb, NULL, sizeof fb,
                           LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, flush_cb);

    lv_obj_t *screen = lv_display_get_screen_active(display);
    ui_kanji_create(screen);
    style_root = lv_obj_get_child(screen, 0);

    kanji_t demo;
    kanji_mock(&demo);
    const kanji_t go = kanji_card_fixture();

    check_hero_face_is_always_drawable();
    check_printable_ascii_hero();
    check_public_dock_bounds();
    check_fonts(&demo);
    check_fonts(&go);

    const kanji_nav_t front = nav_front();
    const kanji_nav_t back  = nav_back();

    /* 01 — the demo card's front, which is what a board with no URL shows. */
    shot(&demo, &front, NULL, "01-front");
    check_page_budget("01-front", 21, 40);
    check_no_label_overlap("01-front");
    check_front_blocks("01-front");
    check_front_hides_the_answer(&demo);
    want_text_somewhere("01-front", S_HINT_REVEAL);

    /* 02 — the demo card's back. */
    shot(&demo, &back, NULL, "02-back");
    check_page_budget("02-back", 45, 40);
    check_no_label_overlap("02-back");
    check_columns_contained("02-back");
    check_dock(&demo, &back);
    want_visible_text("02-back", S_EB_MEANING);
    want_visible_text("02-back", S_EB_BUILD);
    want_visible_text("02-back", S_EB_EXAMPLE);
    want_visible_text("02-back", S_EB_READING);
    want_visible_text("02-back", S_EB_PARTS);
    want_visible_text("02-back", S_EB_MEMORY);

    /* 03/04 — a real kanji card, which is where 성립 and 구성 do their work. */
    shot(&go, &front, NULL, "03-front-kanji");
    check_page_budget("03-front-kanji", 21, 40);
    check_no_label_overlap("03-front-kanji");
    check_front_blocks("03-front-kanji");
    check_front_hides_the_answer(&go);

    shot(&go, &back, NULL, "04-back-kanji");
    check_page_budget("04-back-kanji", 45, 40);
    check_no_label_overlap("04-back-kanji");
    check_columns_contained("04-back-kanji");
    check_dock(&go, &back);
    want_visible_text("04-back-kanji", "형성");

    /* 05/06 — a grade committed. Exactly one cell inverts, and the transition
     * from 04 touches only the dock. */
    {
        uint8_t before[HOR * VER];
        shot(&go, &back, NULL, "04-back-kanji");
        snapshot_threshold(before);

        const kanji_nav_t again = nav_committed(KANJI_BTN_KEY0, &go);
        if (!kanji_nav_is_dock_only_transition(&back, &again)) {
            FAIL("uncommitted -> committed is not a dock-only transition");
        }
        ui_kanji_set_nav(&again);          /* set_nav ONLY: the production path */
        refresh();
        write_bmp("05-back-again");
        check_dock_diff("uncommitted -> 다시", before);
        check_dock(&go, &again);
        check_no_label_overlap("05-back-again");

        const kanji_nav_t easy = nav_committed(KANJI_BTN_BOOT, &go);
        shot(&go, &easy, NULL, "06-back-easy");
        check_dock(&go, &easy);
    }

    /* 07 — a new card: no history, so the plate prints one row instead of four
     * rows of zeroes. */
    {
        kanji_t fresh = go;
        fresh.card.fsrs.state_label[0] = '\0';
        fresh.card.fsrs.due[0] = '\0';
        fresh.card.fsrs.reps = 0;
        fresh.card.fsrs.lapses = 0;
        fresh.card.fsrs.stability_days = -1;
        fresh.card.fsrs.difficulty_pct = -1;
        for (int i = 0; i < KANJI_GRADE_COUNT; i++) fresh.card.preview.span[i][0] = '\0';
        shot(&fresh, &front, NULL, "07-front-new-card");
        check_no_label_overlap("07-front-new-card");
        shot(&fresh, &back, NULL, "08-back-new-card");
        check_no_label_overlap("08-back-new-card");
        check_columns_contained("08-back-new-card");
    }

    /* 09 — no examples at all. The front prints none of them either way, so this shot is now
     * proving the OPPOSITE of what it used to: 1,525 of the 9,956 shipped cards have no example,
     * and this face must be pixel-identical for them and for a card with three. A front that
     * still reserved paper for an example would show up here as a shot that differs from 03. */
    {
        kanji_t bare = go;
        bare.card.example_count = 0;
        shot(&bare, &front, NULL, "09-front-no-examples");
        check_no_label_overlap("09-front-no-examples");
        check_front_ignores_examples("09-front-no-examples", &go, &bare, &front);
    }

    /* 10 — the worst case the model permits, on the denser face. */
    {
        const kanji_t worst = worst_case_fixture(&go);
        shot(&worst, &back, NULL, "10-back-worst-case");
        check_no_label_overlap("10-back-worst-case");
        check_columns_contained("10-back-worst-case");
        check_page_budget("10-back-worst-case", 45, 60);
        shot(&worst, &front, NULL, "11-front-worst-case");
        check_no_label_overlap("11-front-worst-case");
    }

    /* 12/13 — the badges. */
    {
        const ui_status_t offline = { false, false, false, 0 };
        shot(&go, &front, &offline, "12-front-offline");
        want_visible_text("12-front-offline", S_BADGE_OFFLINE);

        const ui_status_t stale = { true, true, false, 0 };
        shot(&go, &back, &stale, "13-back-stale");
        want_visible_text("13-back-stale", S_BADGE_STALE);
    }

    /* 14 — the session is over. */
    {
        kanji_t done = go;
        done.card.valid = false;
        done.session.complete = true;
        shot(&done, &front, NULL, "14-session-complete");
        check_no_label_overlap("14-session-complete");
    }

    /* 15 — a long headword, which falls back from the hero face by length. */
    {
        kanji_t longw = go;
        set_str(longw.card.front, sizeof longw.card.front, "取り替えるつもり");
        shot(&longw, &front, NULL, "15-front-long-headword");
        check_no_label_overlap("15-front-long-headword");
        shot(&longw, &back, NULL, "16-back-long-headword");
        check_no_label_overlap("16-back-long-headword");
        check_columns_contained("16-back-long-headword");
    }

    /* 17 — the Wi-Fi overlay, and 18 — no snapshot at all. */
    ui_kanji_set_overlay(S_WIFI_TITLE, S_WIFI_CONNECTING);
    refresh();
    write_bmp("17-setup");
    ui_kanji_set_overlay(NULL, NULL);

    shot(NULL, &front, NULL, "18-no-data");

    printf(failures ? "FAILED — %d problem(s); shots in %s\n"
                    : "OK — %d problem(s); shots in %s\n",
           failures, shot_dir);
    return failures ? 1 : 0;
}
