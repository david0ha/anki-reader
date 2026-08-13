/*
 * Pixel acceptance renderer for the native landscape artwork screen.
 *
 * This calls the same ui_artwork.c, model, parser and full Korean bitmap fonts
 * as the ESP32 firmware. The only substitution is the display flush: instead
 * of transferring the one-bit result to UC8179, it writes a monochrome BMP.
 */
#include "lvgl.h"

#include "ui_artwork.h"
#include "ui_artwork_layout.h"
#include "ui_fonts.h"
#include "tarot_cards.h"
#include "vault_mock.h"
#include "vault_service.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HOR ARTWORK_SCREEN_W
#define VER ARTWORK_SCREEN_H

static uint8_t fb[HOR * VER * 2];
static uint16_t capture[HOR * VER];
static uint32_t tick_now;
static int failures;

#define FAILV(fmt, ...) do { failures++; printf("  FAIL " fmt "\n", __VA_ARGS__); } while (0)
#define FAIL(msg) do { failures++; printf("  FAIL %s\n", (msg)); } while (0)

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

static void want_ink(const char *name, int x0, int y0, int x1, int y1, int minimum)
{
    int found = ink_count(x0, y0, x1, y1);
    if (found < minimum) {
        FAILV("%s: only %d black pixels in x[%d..%d) y[%d..%d)",
              name, found, x0, x1, y0, y1);
    }
}

static void want_blank(const char *name, int x0, int y0, int x1, int y1)
{
    int found = ink_count(x0, y0, x1, y1);
    if (found) FAILV("%s: found %d unexpected black pixels", name, found);
}

static void want_rule(const char *name, int x0, int y0, int x1, int y1, int minimum)
{
    int found = ink_count(x0, y0, x1, y1);
    if (found < minimum) FAILV("%s: only %d rule pixels", name, found);
}

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
        if (cp != '\n' && cp != '\r' && !lv_font_get_glyph_dsc(font, &glyph, cp, 0))
            FAILV("%s: U+%04X is missing from the font", field, cp);
    }
}

static void check_fonts(const vault_t *vault)
{
    const daily_tarot_t *tarot = &vault->daily_tarot;
    const tarot_card_metadata_t *meta = tarot_card_metadata(tarot->card_id);
    for (int i = 0; i < tarot->headline.line_count; i++)
        cover(&ui_font_kr_28, "headline", tarot->headline.lines[i]);
    for (int i = 0; i < tarot->flow.line_count; i++)
        cover(&ui_font_kr_16, "flow", tarot->flow.lines[i]);
    for (int i = 0; i < tarot->caution.line_count; i++)
        cover(&ui_font_kr_16, "caution", tarot->caution.lines[i]);
    for (int i = 0; i < tarot->action.line_count; i++)
        cover(&ui_font_kr_16, "action", tarot->action.lines[i]);
    if (meta) {
        cover(&ui_font_kr_20, "card name", meta->name_ko);
        cover(&ui_font_kr_16, "card name english", meta->name_en);
    }
}

static void check_copy_contract_width(void)
{
    /* The producer's visual-cell limits are a promise that Korean copy reaches
     * the glass verbatim. Exercise the widest full-width rows at the exact
     * fonts and pixel boxes used by ui_artwork.c. */
    static const char HEADLINE_LIMIT[] = "가가가가가가가가가가가"; /* 22 cells */
    static const char HEADLINE_LATIN_LIMIT[] = "WWWWWWWWWWW";       /* 22 cells */
    static const char BODY_LIMIT[] =
        "가가가가가가가가가가가가가가가가";                 /* 32 cells */
    static const char BODY_LATIN_LIMIT[] = "WWWWWWWWWWWWWWWW";    /* 32 cells */
    lv_point_t size;
    lv_text_get_size(&size, HEADLINE_LIMIT, &ui_font_kr_28, 0, 0,
                     LV_COORD_MAX, LV_TEXT_FLAG_EXPAND);
    if (size.x > 310) FAILV("headline contract row needs %d px, label has 310", size.x);
    lv_text_get_size(&size, HEADLINE_LATIN_LIMIT, &ui_font_kr_28, 0, 0,
                     LV_COORD_MAX, LV_TEXT_FLAG_EXPAND);
    if (size.x > 310) FAILV("Latin headline contract row needs %d px, label has 310", size.x);
    lv_text_get_size(&size, BODY_LIMIT, &ui_font_kr_16, 0, 0,
                     LV_COORD_MAX, LV_TEXT_FLAG_EXPAND);
    if (size.x > 263) FAILV("body contract row needs %d px, label has 263", size.x);
    lv_text_get_size(&size, BODY_LATIN_LIMIT, &ui_font_kr_16, 0, 0,
                     LV_COORD_MAX, LV_TEXT_FLAG_EXPAND);
    if (size.x > 263) FAILV("Latin body contract row needs %d px, label has 263", size.x);
}

static void write_bmp(const char *path)
{
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
    fwrite(header, 1, sizeof(header), out);
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

static void check_pixels(void)
{
    const artwork_layout_t *l = artwork_tarot_layout();
    want_blank("top paper margin", 0, 0, 280, 6);
    want_blank("left paper margin", 0, 0, 4, VER);
    want_rule("native card top edge", l->card.x - 1, l->card.y - 1,
              l->card.x + l->card.w + 1, l->card.y + 2, 250);
    want_rule("native card left edge", l->card.x - 1, l->card.y - 1,
              l->card.x + 2, l->card.y + l->card.h + 1, 440);
    want_ink("full-height tarot engraving", l->card.x, l->card.y,
             l->card.x + l->card.w, l->card.y + l->card.h, 15000);
    want_ink("ornamented deck spine", l->deck_spine.x, l->deck_spine.y,
             l->deck_spine.x + l->deck_spine.w,
             l->deck_spine.y + l->deck_spine.h, 350);
    want_rule("cut-corner reading frame top", l->reading_frame.x + 6,
              l->reading_frame.y, l->reading_frame.x + l->reading_frame.w - 6,
              l->reading_frame.y + 6, 500);
    want_rule("double reading frame right", l->reading_frame.x + l->reading_frame.w - 6,
              l->reading_frame.y + 8, l->reading_frame.x + l->reading_frame.w,
              l->reading_frame.y + l->reading_frame.h - 8, 800);
    want_ink("date and deck reference", 319, 18, 630, 40, 100);
    want_ink("reading headline", 319, 47, 630, 112, 450);
    want_ink("card identity", 319, 116, 630, 146, 140);
    want_rule("flow divider", 319, l->rule_y[0] - 1, 630,
              l->rule_y[0] + 2, 260);
    want_ink("flow reading", 319, 178, 630, 230, 180);
    want_rule("caution divider", 319, l->rule_y[1] - 1, 630,
              l->rule_y[1] + 2, 260);
    want_ink("caution reading", 319, 280, 630, 332, 180);
    want_rule("action divider", 319, l->rule_y[2] - 1, 630,
              l->rule_y[2] + 2, 260);
    want_ink("action reading", 319, 377, 630, 429, 180);
    want_ink("deck footer and seal", 319, 436, 630, 459, 90);

    int total = ink_count(0, 0, HOR, VER);
    double percentage = 100.0 * total / (HOR * VER);
    if (percentage < 9.0 || percentage > 48.0)
        FAILV("overall ink %.2f%% is outside the e-paper artwork envelope", percentage);
    else
        printf("  ink envelope       %.2f%%\n", percentage);
}

static void check_whole_deck_renders(vault_t *vault)
{
    static const char *SUITS[] = { "cups", "pentacles", "swords", "wands" };
    char original[TAROT_CARD_ID_MAX];
    snprintf(original, sizeof(original), "%s", vault->daily_tarot.card_id);
    int rendered = 0;
    int least_ink = HOR * VER;
    int most_ink = 0;

    for (int group = -1; group < 4; group++) {
        int first = group < 0 ? 0 : 1;
        int last = group < 0 ? 21 : 14;
        for (int number = first; number <= last; number++) {
            char id[TAROT_CARD_ID_MAX];
            if (group < 0)
                snprintf(id, sizeof(id), "major-%02d", number);
            else
                snprintf(id, sizeof(id), "%s-%02d", SUITS[group], number);

            const lv_image_dsc_t *image = tarot_card_image(id);
            const tarot_card_metadata_t *meta = tarot_card_metadata(id);
            if (!image || !meta) {
                FAILV("deck catalog cannot resolve %s", id);
                continue;
            }
            if (image->header.cf != LV_COLOR_FORMAT_I1 ||
                image->header.w != TAROT_CARD_WIDTH ||
                image->header.h != TAROT_CARD_HEIGHT ||
                image->header.stride != TAROT_CARD_STRIDE ||
                image->data_size != TAROT_CARD_DATA_SIZE) {
                FAILV("%s descriptor is not native 272x464 I1", id);
                continue;
            }
            cover(&ui_font_kr_20, "deck Korean name", meta->name_ko);
            cover(&lv_font_montserrat_14, "deck English name", meta->name_en);

            snprintf(vault->daily_tarot.card_id,
                     sizeof(vault->daily_tarot.card_id), "%s", id);
            ui_artwork_set_data(vault);
            refresh();
            const artwork_layout_t *l = artwork_tarot_layout();
            int ink = ink_count(l->card.x, l->card.y,
                                l->card.x + l->card.w, l->card.y + l->card.h);
            if (ink < 2000 || ink > 115000)
                FAILV("%s renders with implausible card ink: %d", id, ink);
            if (ink < least_ink) least_ink = ink;
            if (ink > most_ink) most_ink = ink;
            rendered++;
        }
    }
    if (rendered != (int)TAROT_CARD_COUNT)
        FAILV("deck rendered %d cards, expected %u", rendered, TAROT_CARD_COUNT);
    else
        printf("  deck render         %d cards, ink %d..%d px\n",
               rendered, least_ink, most_ink);

    snprintf(vault->daily_tarot.card_id,
             sizeof(vault->daily_tarot.card_id), "%s", original);
    ui_artwork_set_data(vault);
    refresh();
}

static void check_partial_full_buffer_flush(lv_display_t *display)
{
    uint16_t source[HOR * VER];
    uint16_t before[HOR * VER];
    for (int i = 0; i < HOR * VER; i++) source[i] = 0xffff;
    for (int y = 250; y < 258; y++)
        for (int x = 210; x < 222; x++) source[y * HOR + x] = 0;
    memcpy(before, capture, sizeof(before));
    lv_area_t area = {210, 250, 221, 257};
    flush_cb(display, &area, (uint8_t *)source);
    if (memcmp(&capture[250 * HOR + 210], &source[250 * HOR + 210],
               12 * sizeof(uint16_t)) != 0)
        FAIL("partial flush did not index the full framebuffer at area origin");
    if (capture[0] != before[0])
        FAIL("partial flush overwrote pixels outside its invalidated area");
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "artwork.bmp";

    lv_init();
    lv_tick_set_cb(tick_cb);
    lv_display_t *display = lv_display_create(HOR, VER);
    lv_display_set_flush_cb(display, flush_cb);
    lv_display_set_buffers(display, fb, NULL, sizeof(fb), LV_DISPLAY_RENDER_MODE_FULL);

    lv_obj_t *screen = lv_obj_create(NULL);
    lv_screen_load(screen);
    ui_artwork_create(screen);

    vault_t vault;
    const char *url = getenv("VAULT_URL");
    if (url && *url) {
        vault_fetch_result_t result = vault_service_fetch(url, &vault);
        if (result != VAULT_FETCH_OK) {
            fprintf(stderr, "FAILED — VAULT_URL could not be rendered: %s\n",
                    vault_fetch_result_name(result));
            return 2;
        }
        printf("fetched normalized artwork from %s\n", url);
    } else {
        vault_mock(&vault);
        printf("using built-in normalized artwork\n");
    }

    check_fonts(&vault);
    check_copy_contract_width();
    ui_artwork_set_data(&vault);
    refresh();
    check_pixels();
    check_whole_deck_renders(&vault);
    write_bmp(path);
    check_partial_full_buffer_flush(display);

    printf("%s — %d artwork problem(s); %s\n",
           failures ? "FAILED" : "ok", failures, path);
    return failures ? 1 : 0;
}
