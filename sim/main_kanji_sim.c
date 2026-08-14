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

static void check_public_dock_bounds(void)
{
    int x1 = -1, y1 = -1, x2 = -1, y2 = -1;
    ui_kanji_dock_area(&x1, &y1, &x2, &y2);
    if (x1 != 112 || y1 != 344 || x2 != 632 || y2 != 424) {
        FAILV("dock accessor returned (%d,%d,%d,%d), wanted (112,344,632,424)",
              x1, y1, x2, y2);
    } else {
        printf("  dock public half-open bounds: (%d,%d,%d,%d)\n", x1, y1, x2, y2);
    }
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

/* Everything in the snapshot is drawn from a body face, so every body face must
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
        for (int i = 0; i < c->sense_count; i++) cover(font, "sense", c->senses[i]);
        for (int i = 0; i < c->example_count; i++) {
            cover(font, "example", c->examples[i].text);
            cover(font, "example reading", c->examples[i].reading);
            cover(font, "example gloss", c->examples[i].gloss);
        }
        cover(font, "description", c->description);
        cover(font, "hook title", c->hook_title);
        cover(font, "hook body", c->hook_body);
        for (int i = 0; i < c->part_count; i++) {
            cover(font, "part glyph", c->parts[i].glyph);
            cover(font, "part meaning", c->parts[i].meaning);
            cover(font, "part reading", c->parts[i].reading);
        }
        for (int i = 0; i < c->comment_count; i++) {
            cover(font, "comment author", c->comments[i].author);
            cover(font, "comment body", c->comments[i].body);
        }
        cover(font, "fsrs state", c->fsrs.state_label);
        cover(font, "fsrs due", c->fsrs.due);
        for (int g = KANJI_GRADE_AGAIN; g <= KANJI_GRADE_EASY; g++) {
            cover(font, "preview span", kanji_preview_span(k, (kanji_grade_t)g));
            cover(font, "grade label", kanji_grade_label((kanji_grade_t)g));
        }

        /* The FSRS sheet's copy is the longest fixed text on the board and the
         * likeliest to contain a character no other literal does. */
        cover(font, "fsrs page 1", S_FSRS_P1_BODY);
        cover(font, "fsrs page 2", S_FSRS_P2_BODY);
        cover(font, "fsrs page 3", S_FSRS_P3_BODY);
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

/* The shapes the catalog's headwords actually take. Nine thousand of the
 * 9,956 rows route to the hero face on length alone, and 133 of them carry an
 * ASCII character the Japanese-only hero was not built with — which rendered as
 * a tofu box at 56 px, dead centre, until ui_hero_face() started asking the font
 * instead of only counting characters. These are drawn from that set. */
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

/* --- shots ---------------------------------------------------------------- */

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
    fwrite(header, 1, sizeof header, out);
    uint8_t *row = calloc(1, (size_t)row_size);
    if (!row) { fclose(out); FAIL("cannot allocate BMP row"); return; }
    for (int y = VER - 1; y >= 0; y--) {
        for (int x = 0; x < HOR; x++) {
            uint8_t value = is_black(x, y) ? 0 : 255;
            row[x * 3] = row[x * 3 + 1] = row[x * 3 + 2] = value;
        }
        fwrite(row, 1, (size_t)row_size, out);
    }
    free(row);
    fclose(out);
}

/* Render one nav state and leave a shot behind. */
static void shot(const kanji_t *k, const kanji_nav_t *nav, const char *name)
{
    ui_kanji_set_data(k);
    ui_kanji_set_nav(nav);
    refresh();
    write_bmp(name);
}

/* --- the chrome, on every screen ------------------------------------------ */

static void check_chrome(const char *screen, bool require_progress,
                         unsigned footer_mask)
{
    const kanji_chrome_t *c = kanji_chrome_layout();
    char label[128];

    snprintf(label, sizeof label, "%s: rail divider", screen);
    want_ink(label, c->rail_rule, c->rail_rule.h / 2);
    snprintf(label, sizeof label, "%s: rail identity", screen);
    want_ink(label, c->rail_identity, 40);
    if (require_progress) {
        snprintf(label, sizeof label, "%s: rail progress", screen);
        want_ink(label, c->rail_progress, 20);
    }
    snprintf(label, sizeof label, "%s: quiet masthead", screen);
    want_ink(label, c->masthead, 60);

    /* A footer slot is two authored objects. Require both when the physical
     * action is live, and zero ink in both when the state machine says the
     * action is unavailable. This catches both missing copy and dead controls. */
    for (int i = 0; i < 4; i++) {
        const bool visible = (footer_mask & (1u << i)) != 0;
        snprintf(label, sizeof label, "%s: footer keycap %d", screen, i);
        if (visible) want_ink(label, c->keycap[i], 4);
        else want_no_ink(label, c->keycap[i]);
        snprintf(label, sizeof label, "%s: footer action %d", screen, i);
        if (visible) want_ink(label, c->key_action[i], 20);
        else want_no_ink(label, c->key_action[i]);
    }
}

/* --- per-screen pixel checks ---------------------------------------------- */

static void check_question(const char *screen)
{
    const kanji_chrome_t *c = kanji_chrome_layout();
    const kanji_question_layout_t *q = kanji_question_layout();
    const kanji_rect_t whole = { 0, 0, HOR, VER };

    check_chrome(screen, true, 0x0f);
    want_ink("question: hero", q->hero, 500);
    want_ink("question: reveal prompt", q->prompt, 80);
    want_ink("question: remaining counts", q->counts, 80);
    want_mostly_paper("question: paper field", c->main, 30);
    report_ink_ceiling("question: whole screen", whole, 30);
}

static void check_answer(kanji_grade_t cursor)
{
    const kanji_answer_layout_t *a = kanji_answer_layout();

    check_chrome("answer", true, 0x0f);
    want_ink("answer: hero", a->hero, 500);
    want_ink("answer: reading", a->reading, 50);

    want_mostly_paper("answer: the meaning is on white", a->meaning, 30);
    want_ink("answer: meaning", a->meaning, 100);
    want_ink("answer: examples", a->examples, 100);
    want_ink("answer: rating prompt", a->prompt, 50);

    /* Exactly one cell is filled, and it is the cursor's. This is the check a
     * screenshot cannot make: a dock with two black cells and a dock with one
     * are equally plausible until the pixels are counted. */
    int filled = 0;
    for (int i = 0; i < KANJI_GRADE_COUNT; i++) {
        const bool is_cursor = (i + 1) == (int)cursor;
        const int ink = ink_rect(a->cell[i]);
        const int area = a->cell[i].w * a->cell[i].h;
        if (ink * 100 > area * 50) {
            filled++;
            if (!is_cursor) {
                FAILV("answer: cell %d is filled but the cursor is on %d",
                      i + 1, (int)cursor);
            }
            /* The label and the span must survive the inversion. */
            char label[96];
            snprintf(label, sizeof label, "answer: selected label %d", i + 1);
            want_paper(label, a->cell_label[i], 60);
            snprintf(label, sizeof label, "answer: selected span %d", i + 1);
            want_paper(label, a->cell_span[i], 40);
        } else if (is_cursor) {
            FAILV("answer: the cursor is on %d but that cell is not filled",
                  (int)cursor);
        } else {
            char label[96];
            snprintf(label, sizeof label, "answer: unselected label %d", i + 1);
            want_ink(label, a->cell_label[i], 60);
            snprintf(label, sizeof label, "answer: unselected span %d", i + 1);
            want_ink(label, a->cell_span[i], 40);
        }
    }
    if (filled != 1) FAILV("answer: %d dock cells are filled, wanted 1", filled);
}

static void check_sheet(const char *name, bool with_stats, unsigned footer_mask)
{
    const kanji_sheet_layout_t *l = kanji_sheet_layout(with_stats);
    char label[128];

    check_chrome(name, true, footer_mask);
    snprintf(label, sizeof label, "%s: sheet headword", name);
    want_ink(label, l->headword, 50);
    snprintf(label, sizeof label, "%s: sheet title", name);
    want_ink(label, l->title, 20);

    snprintf(label, sizeof label, "%s: the body is on white", name);
    want_mostly_paper(label, l->body, 35);
    snprintf(label, sizeof label, "%s: the body has text", name);
    want_ink(label, l->body, 80);

    if (with_stats) {
        snprintf(label, sizeof label, "%s: the card's own numbers", name);
        want_ink(label, l->stats, 600);
        for (int i = 0; i < KANJI_STAT_CELLS; i++) {
            snprintf(label, sizeof label, "%s: stat cell %d", name, i);
            want_ink(label, l->stat[i], 60);
        }
    }
}

/* --- the states worth a shot ---------------------------------------------- */

static kanji_nav_t nav_question(void)
{
    kanji_nav_t n;
    kanji_nav_reset(&n);
    return n;
}

static kanji_nav_t nav_answer(kanji_grade_t g)
{
    kanji_nav_t n = nav_question();
    n.revealed = true;
    n.grade = g;
    return n;
}

static kanji_nav_t nav_sheet(kanji_sheet_t sheet, int page, bool revealed)
{
    kanji_nav_t n = nav_question();
    n.revealed = revealed;
    n.sheet = sheet;
    n.sheet_page = page;
    return n;
}

/* A three-comment card, so the comments sheet has a second page to render. The
 * demo card carries two on purpose — it is the docs' specimen — so paging is
 * exercised from a variant rather than by making the specimen unrepresentative. */
static kanji_t with_three_comments(const kanji_t *base)
{
    kanji_t k = *base;
    k.card.comment_count = 3;
    kanji_str_copy(k.card.comments[2].author, KANJI_AUTHOR_MAX, "하루");
    kanji_str_copy(k.card.comments[2].body, KANJI_COMMENT_MAX,
                   "「会」는 모임이라는 뜻으로도 자주 쓰입니다. 会社, 会議 모두 "
                   "같은 글자예요.");
    k.card.comments[2].likes = 3;
    if (k.card.comment_total < 3) k.card.comment_total = 3;
    return k;
}

static char widest_body_ascii(uint16_t *advance)
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
    if (widest == '\0') {
        FAIL("ui_font_kr_16 has no drawable non-whitespace single-byte glyph");
    }
    if (advance) *advance = widest_advance;
    return widest;
}

static size_t fill_repeated(char *dst, size_t capacity, char glyph)
{
    if (!dst || capacity == 0) return 0;
    for (size_t i = 0; i + 1 < capacity; i++) dst[i] = glyph;
    dst[capacity - 1] = '\0';
    return capacity - 1;
}

static kanji_t with_max_description(const kanji_t *base, char glyph)
{
    kanji_t k = *base;
    char raw[KANJI_BODY_MAX];
    fill_repeated(raw, sizeof raw, glyph);
    const size_t shape_len = kanji_text_collapse_whitespace(
        k.card.description, sizeof k.card.description, raw);
    const size_t hook_len = kanji_text_collapse_whitespace(
        k.card.hook_body, sizeof k.card.hook_body, raw);
    if (shape_len != KANJI_BODY_MAX - 1 || hook_len != KANJI_BODY_MAX - 1) {
        FAILV("max prose normalization produced %zu/%zu bytes, wanted %d/%d",
              shape_len, hook_len, KANJI_BODY_MAX - 1, KANJI_BODY_MAX - 1);
    }

    k.card.part_count = KANJI_PARTS_MAX;
    for (int i = 0; i < KANJI_PARTS_MAX; i++) {
        fill_repeated(k.card.parts[i].glyph,
                      sizeof k.card.parts[i].glyph, glyph);
        fill_repeated(k.card.parts[i].meaning,
                      sizeof k.card.parts[i].meaning, glyph);
        fill_repeated(k.card.parts[i].reading,
                      sizeof k.card.parts[i].reading, glyph);
    }
    return k;
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

static void check_max_prose_measurement(const char *name, const char *text,
                                        char glyph, uint16_t advance)
{
    const kanji_sheet_layout_t *l = kanji_sheet_layout(false);
    lv_obj_t *label = find_visible_label_text(style_root, text);
    if (!label) {
        FAILV("%s: could not find the visible maximum-prose label", name);
        return;
    }

    const lv_font_t *font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    const int letter_space = lv_obj_get_style_text_letter_space(label, LV_PART_MAIN);
    const int line_space = lv_obj_get_style_text_line_space(label, LV_PART_MAIN);
    const int label_width = lv_obj_get_width(label);
    lv_point_t natural;
    lv_text_get_size(&natural, text, font, letter_space, line_space,
                     l->body.w, LV_TEXT_FLAG_NONE);

    printf("  font %-28s glyph='%c' advance=%u width=%d natural=%dx%d "
           "spacing=%d/%d\n",
           name, glyph, advance, l->body.w, natural.x, natural.y,
           letter_space, line_space);
    if (font != &ui_font_kr_16) FAILV("%s: maximum prose did not use ui_font_kr_16", name);
    if (l->body.w != 520 || label_width != 520) {
        FAILV("%s: measured width layout=%d label=%d, wanted 520/520",
              name, l->body.w, label_width);
    }
    if (natural.x > l->body.w) {
        FAILV("%s: natural line width %d exceeds the 520 px prose page",
              name, natural.x);
    }
    if (natural.y > 320) {
        FAILV("%s: natural height %d exceeds the 320 px prose page",
              name, natural.y);
    }
}

static void check_description_part_rows(const char *name, int rows, int minimum)
{
    const kanji_sheet_layout_t *l = kanji_sheet_layout(false);
    for (int i = 0; i < rows; i++) {
        char label[128];
        const kanji_rect_t row = {
            l->body.x, l->body.y + i * 72, l->body.w, 32,
        };
        snprintf(label, sizeof label, "%s: component row %d", name, i + 1);
        want_ink(label, row, minimum);
    }
}

int main(int argc, char **argv)
{
    if (argc > 1) snprintf(shot_dir, sizeof shot_dir, "%s", argv[1]);

    lv_init();
    lv_tick_set_cb(tick_cb);
    lv_display_t *display = lv_display_create(HOR, VER);
    lv_display_set_flush_cb(display, flush_cb);
    lv_display_set_buffers(display, fb, NULL, sizeof fb, LV_DISPLAY_RENDER_MODE_FULL);

    lv_obj_t *screen = lv_obj_create(NULL);
    lv_screen_load(screen);
    ui_kanji_create(screen);
    /* The simulator host screen carries LVGL's default theme. The product UI
     * is the stripped, opaque child created by ui_kanji_create(). */
    style_root = lv_obj_get_child(screen, 0);

    kanji_t card;
    const char *url = getenv("KANJI_URL");
    if (url && *url) {
        kanji_fetch_result_t result = kanji_service_fetch(url, &card);
        if (result != KANJI_FETCH_OK) {
            fprintf(stderr, "FAILED — KANJI_URL could not be rendered: %s\n",
                    kanji_fetch_result_name(result));
            return 2;
        }
        printf("fetched a card from %s\n", url);
    } else {
        kanji_mock(&card);
        printf("using the built-in demo card\n");
    }

    check_fonts(&card);
    check_hero_face_is_always_drawable();
    check_public_dock_bounds();

    const ui_status_t online = { true, false, true, 78 };
    ui_kanji_set_status(&online);

    /* The five screens, in the order a learner meets them. */
    kanji_nav_t nav = nav_question();
    shot(&card, &nav, "01-question");
    check_question("question");

    nav = nav_answer(KANJI_GRADE_GOOD);
    shot(&card, &nav, "02-answer");
    check_answer(KANJI_GRADE_GOOD);
    snapshot_threshold(grade_frame);

    /* KEY0 walks the cursor; the dock is the only thing that may change. */
    nav = nav_answer(KANJI_GRADE_EASY);
    shot(&card, &nav, "03-answer-easy");
    check_answer(KANJI_GRADE_EASY);
    check_dock_diff("Good -> Easy", grade_frame);
    snapshot_threshold(grade_frame);

    nav = nav_answer(KANJI_GRADE_AGAIN);
    shot(&card, &nav, "04-answer-again");
    check_answer(KANJI_GRADE_AGAIN);
    check_dock_diff("Easy -> Again", grade_frame);

    /* The full-height case. The demo card carries two examples, so a third row
     * colliding with the rating prompt below it renders invisibly on every
     * other shot — which is exactly how it got in. */
    {
        kanji_t full = card;
        full.card.example_count = 3;
        kanji_str_copy(full.card.examples[2].text, KANJI_FRONT_MAX, "会わせる");
        kanji_str_copy(full.card.examples[2].reading, KANJI_READING_MAX, "あわせる");
        kanji_str_copy(full.card.examples[2].gloss, KANJI_SENSE_MAX, "만나게 하다");

        nav = nav_answer(KANJI_GRADE_HARD);
        shot(&full, &nav, "04b-answer-three-examples");
        check_answer(KANJI_GRADE_HARD);
        check_fonts(&full);

        const kanji_answer_layout_t *a = kanji_answer_layout();
        kanji_rect_t third = { a->examples.x,
                               a->examples.y + 2 * a->example_step,
                               a->examples.w, a->example_step };
        want_ink("answer: the third example row", third, 150);
        want_ink("answer: the rating prompt", a->prompt, 150);
    }

    nav = nav_sheet(KANJI_SHEET_DESCRIPTION, 0, false);
    shot(&card, &nav, "05-description-shape");
    check_sheet("description shape", false, 0x0f);

    nav = nav_sheet(KANJI_SHEET_DESCRIPTION, 1, false);
    shot(&card, &nav, "05b-description-hook");
    check_sheet("description hook", false, 0x0f);

    nav = nav_sheet(KANJI_SHEET_DESCRIPTION, 2, false);
    shot(&card, &nav, "05c-description-parts");
    check_sheet("description parts", false, 0x0f);
    check_description_part_rows("description parts", card.card.part_count, 50);

    nav = nav_sheet(KANJI_SHEET_COMMENTS, 0, true);
    shot(&card, &nav, "06-comments");
    check_sheet("comments", false, 0x0e);

    kanji_t three = with_three_comments(&card);
    nav = nav_sheet(KANJI_SHEET_COMMENTS, 1, true);
    shot(&three, &nav, "07-comments-page2");
    check_sheet("comments page 2", false, 0x0f);

    for (int page = 0; page < 3; page++) {
        char name[32];
        snprintf(name, sizeof name, "%02d-fsrs-%d", 8 + page, page + 1);
        nav = nav_sheet(KANJI_SHEET_FSRS, page, true);
        shot(&card, &nav, name);
        check_sheet("fsrs", true, 0x0f);
    }

    /* The states a learner meets on a bad day, and the one they meet at the
     * end of a good one. */
    kanji_t done = card;
    done.demo = false;
    done.card.valid = false;
    done.session.complete = true;
    nav = nav_question();
    shot(&done, &nav, "11-session-complete");
    check_chrome("session complete", true, 0x0c);
    want_ink("session complete: message", kanji_question_layout()->prompt, 80);
    want_ink("session complete: recovery copy", kanji_question_layout()->secondary, 80);
    if (!find_visible_label_text(style_root, S_RAIL_COMPLETE)) {
        FAIL("session complete: rail does not identify the complete state");
    }
    {
        const kanji_rect_t whole = { 0, 0, HOR, VER };
        report_ink_ceiling("session complete: whole screen", whole, 30);
    }

    /* A ten-character headword: the longest the catalog holds, and the case
     * kanji_hero_is_large() exists for. It must still fit the hero box. */
    kanji_t longword = card;
    kanji_str_copy(longword.card.front, KANJI_FRONT_MAX, "取り替えるつもり");
    nav = nav_question();
    shot(&longword, &nav, "12-long-headword");
    {
        const kanji_question_layout_t *q = kanji_question_layout();
        want_ink("long headword: still drawn", q->hero, 500);

        /* The hero box is the boundary, and the layout test cannot check it:
         * it knows the box is 464 px but not how wide a string renders in a
         * given face. Ask LVGL, at both faces, for the two lengths that bound
         * the catalog — five characters is the most the 56 px face takes, ten
         * is the longest headword the catalog holds. */
        lv_point_t size;
        lv_text_get_size(&size, "取り替える", &ui_font_jp_56, 0, 0,
                         LV_COORD_MAX, LV_TEXT_FLAG_EXPAND);
        if (size.x > q->hero.w) {
            FAILV("a 5-character headword needs %d px, the hero box has %d",
                  size.x, q->hero.w);
        }
        lv_text_get_size(&size, "取り替えるつもりです", &ui_font_kr_28, 0, 0,
                         LV_COORD_MAX, LV_TEXT_FLAG_EXPAND);
        if (size.x > q->hero.w) {
            FAILV("a 10-character headword needs %d px, the hero box has %d",
                  size.x, q->hero.w);
        }
    }

    /* A headword the Japanese-only hero face cannot draw. 106 of the catalog's
     * 9,956 fronts carry a `~`, and until ui_hero_face() started asking the
     * font rather than only counting characters this rendered as a tofu box at
     * 56 px, dead centre. It must come out as readable text at the smaller
     * face, which is what the shot is here to show. */
    kanji_t tilde = card;
    kanji_str_copy(tilde.card.front, KANJI_FRONT_MAX, "~がたい");
    kanji_str_copy(tilde.card.reading, KANJI_READING_MAX, "がたい");
    nav = nav_question();
    shot(&tilde, &nav, "12b-ascii-headword");
    check_fonts(&tilde);
    want_ink("ascii headword: drawn, not tofu", kanji_question_layout()->hero, 500);

    const ui_status_t offline = { false, true, false, 0 };
    ui_kanji_set_status(&offline);
    nav = nav_question();
    shot(&card, &nav, "13-offline");
    check_chrome("offline", true, 0x0f);
    ui_kanji_set_status(&online);

    ui_kanji_set_overlay(S_WIFI_TITLE,
                         "\"Obsidian Board\" 에 접속한 뒤\n"
                         "브라우저에서 Wi-Fi 와 서버 주소를 입력하세요.");
    refresh();
    write_bmp("14-setup");
    {
        kanji_rect_t whole = { 0, 0, HOR, VER };
        want_ink("setup overlay: the message", whole, 3000);
        want_mostly_paper("setup overlay: is on white", whole, 25);
    }
    ui_kanji_set_overlay(NULL, NULL);
    refresh();

    /* No-data is a real canonical state, not the invalid-card shape used for
     * session completion. Passing NULL proves reveal and hint are absent while
     * refresh and the always-available FSRS sheet remain discoverable. */
    nav = nav_question();
    shot(NULL, &nav, "15-no-data");
    check_chrome("no data", false, 0x0c);
    want_no_ink("no data: no headword", kanji_question_layout()->hero);
    want_ink("no data: recovery message", kanji_question_layout()->prompt, 80);
    want_ink("no data: recovery detail", kanji_question_layout()->secondary, 80);
    want_no_ink("no data: no remaining counts", kanji_question_layout()->counts);

    const ui_status_t stale = { true, true, true, 78 };
    kanji_t stale_card = card;
    stale_card.demo = false;
    ui_kanji_set_status(&stale);
    nav = nav_question();
    shot(&stale_card, &nav, "16-stale");
    check_question("stale");
    if (!find_visible_label_text(style_root, S_BADGE_STALE)) {
        FAIL("stale: rail does not identify the stale last-card state");
    }
    ui_kanji_set_status(&online);

    /* Maximum-content proofs are review auxiliaries, not additional canonical
     * product states. Build them at the model limits and measure the exact
     * visible label with its computed font and spacing. */
    uint16_t widest_advance = 0;
    const char widest = widest_body_ascii(&widest_advance);
    kanji_t maximum = with_max_description(&card, widest);
    check_fonts(&maximum);

    nav = nav_sheet(KANJI_SHEET_DESCRIPTION, 0, false);
    shot(&maximum, &nav, "aux-description-shape-max");
    check_sheet("aux description shape max", false, 0x0f);
    check_max_prose_measurement("shape max", maximum.card.description,
                                widest, widest_advance);

    nav = nav_sheet(KANJI_SHEET_DESCRIPTION, 1, false);
    shot(&maximum, &nav, "aux-description-hook-max");
    check_sheet("aux description hook max", false, 0x0f);
    check_max_prose_measurement("hook max", maximum.card.hook_body,
                                widest, widest_advance);

    nav = nav_sheet(KANJI_SHEET_DESCRIPTION, 2, false);
    shot(&maximum, &nav, "aux-description-parts-max");
    check_sheet("aux description parts max", false, 0x0f);
    check_description_part_rows("aux description parts max",
                                KANJI_PARTS_MAX, 100);

    printf("%s — %d problem(s); shots in %s/\n",
           failures ? "FAILED" : "ok", failures, shot_dir);
    return failures ? 1 : 0;
}
