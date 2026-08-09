/*
 * LVGL desktop simulator for the fortune board (headless -> BMP).
 *
 * The panel is 122x250 and 1-bit, which leaves no room for "it'll probably
 * fit". This renders the real ui_fortune.c at the real resolution, binarizes
 * with the device's exact rule (px < 0x7FFF ? black : white), and writes one
 * bitmap per screen — so a clipped label or a missing glyph shows up here
 * instead of two seconds into an e-Paper refresh.
 *
 * It draws every one of the seven ranks, not a sample: each rank swaps the
 * grade, the seal, the verse and the fortune-table values, and all of them
 * have to land inside the 만세력 frame.
 *
 *   ./sim.sh                 # sample weather
 *   LOCATION="Seoul" ./sim.sh   # real Open-Meteo, exactly the device's path
 */
#include "lvgl.h"
#include "ui_fortune.h"
#include "ui_fonts.h"
#include "omikuji.h"
#include "saju.h"
#include "weather_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#define HOR 122
#define VER 250

static uint8_t  fb[HOR * VER * 2];
static uint16_t capture[HOR * VER];
static uint32_t g_tick = 0;

static uint32_t tick_cb(void) { return g_tick; }

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px) {
    int w = a->x2 - a->x1 + 1, h = a->y2 - a->y1 + 1;
    if (w == HOR && h == VER) memcpy(capture, px, sizeof(capture));
    lv_display_flush_ready(d);
}

static void run_refresh(int steps) {
    for (int i = 0; i < steps; i++) { g_tick += 16; lv_timer_handler(); }
}

static int is_black(int x, int y) { return capture[y * HOR + x] < 0x7FFF; }

/* capture[] (RGB565) -> 24bit BMP, device binarization rule. */
static void write_mono_bmp(const char *path) {
    int W = HOR, H = VER, rowsize = (W * 3 + 3) & ~3, datasize = rowsize * H;
    int filesize = 54 + datasize;
    uint8_t hdr[54] = {0};
    hdr[0]='B'; hdr[1]='M';
    hdr[2]=filesize; hdr[3]=filesize>>8; hdr[4]=filesize>>16; hdr[5]=filesize>>24;
    hdr[10]=54; hdr[14]=40;
    hdr[18]=W; hdr[19]=W>>8; hdr[22]=H; hdr[23]=H>>8;
    hdr[26]=1; hdr[28]=24;
    hdr[34]=datasize; hdr[35]=datasize>>8; hdr[36]=datasize>>16; hdr[37]=datasize>>24;
    FILE *f = fopen(path, "wb");
    if (!f) { printf("cannot open %s\n", path); return; }
    fwrite(hdr, 1, 54, f);
    uint8_t *row = calloc(1, rowsize);
    if (!row) { fclose(f); return; }
    for (int y = H - 1; y >= 0; y--) {
        for (int x = 0; x < W; x++) {
            uint8_t v = is_black(x, y) ? 0 : 255;
            row[x*3]=v; row[x*3+1]=v; row[x*3+2]=v;
        }
        fwrite(row, 1, rowsize, f);
    }
    free(row); fclose(f);
}

/* How much of the panel is inked. A screen that renders nothing (missing font,
 * mis-sized container) comes out at 0%; a screen that has gone solid black
 * comes out near 100%. Both are bugs a human skimming filenames would miss. */
static double ink_pct(void) {
    long on = 0;
    for (int i = 0; i < HOR * VER; i++) if (capture[i] < 0x7FFF) on++;
    return 100.0 * on / (HOR * VER);
}

static void shot(const char *dir, const char *name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.bmp", dir, name);
    write_mono_bmp(path);
    printf("  %-22s %5.1f%% ink   %s\n", name, ink_pct(), path);
}

/* Bounding box of the inked pixels in [x0,x1) x [y0,y1). Returns 0 if empty. */
static int ink_box(int x0, int y0, int x1, int y1,
                   int *bx0, int *by0, int *bx1, int *by1) {
    int rx0 = HOR, ry0 = VER, rx1 = -1, ry1 = -1;
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            if (is_black(x, y)) {
                if (x < rx0) rx0 = x;
                if (x > rx1) rx1 = x;
                if (y < ry0) ry0 = y;
                if (y > ry1) ry1 = y;
            }
        }
    }
    if (rx1 < 0) return 0;
    *bx0 = rx0; *by0 = ry0; *bx1 = rx1; *by1 = ry1;
    return 1;
}

/* ---- checks ------------------------------------------------------------- */

static int g_fail;

#define FAILV(fmt, ...) do { g_fail++; printf("  FAIL " fmt "\n", __VA_ARGS__); } while (0)

/*
 * Tofu detection, done properly.
 *
 * The tempting version of this check looks at the bitmap for the hollow
 * rectangle LVGL draws in place of a missing glyph — which is unreliable, and
 * unnecessary, because LVGL will simply tell us. Ask the font whether it has
 * each codepoint of each string it is going to be asked to draw.
 */
/* LVGL's own UTF-8 iterator lives in a private header, so decode here rather
 * than reach into its internals. */
static uint32_t utf8_next(const char *s, int *i) {
    unsigned char c = (unsigned char)s[*i];
    int extra = c < 0x80 ? 0 : (c < 0xE0 ? 1 : (c < 0xF0 ? 2 : 3));
    uint32_t cp = c < 0x80 ? c : (c & (0x3F >> extra));
    int k = 0;
    while (k < extra && s[*i + 1 + k]) {
        cp = (cp << 6) | ((unsigned char)s[*i + 1 + k] & 0x3F);
        k++;
    }
    /* Advance by the bytes actually consumed (mirrors ui_vtext.c) — a
     * truncated multi-byte tail must not walk past the NUL. */
    *i += k + 1;
    return cp;
}

static void check_font_coverage(const lv_font_t *font, const char *label, const char *text) {
    int i = 0;
    while (text[i]) {
        int at = i;
        uint32_t cp = utf8_next(text, &i);
        if (cp == '\n' || cp == '\r') continue;
        lv_font_glyph_dsc_t dsc;
        if (!lv_font_get_glyph_dsc(font, &dsc, cp, 0)) {
            FAILV("%s: U+%04X (byte %d) missing from the font -> tofu", label, cp, at);
        }
    }
}

static void check_all_glyphs(void) {
    printf("checking glyph coverage\n");

    /* Per rank: the grade, the seal (the rank's last character), the verse
     * halves, and the four table values — each against the exact face that
     * will draw it. */
    for (int i = 0; i < OMIKUJI_COUNT; i++) {
        const omikuji_result_t *r = omikuji_by_rank((omikuji_rank_t)i);
        check_font_coverage(&ui_font_kr_hanja_34, "grade", r->hanja);
        check_font_coverage(&ui_font_kr_hanja_16, "seal", omikuji_seal(r));
        check_font_coverage(&ui_font_kr_12, "haeseok", r->haeseok);
        check_font_coverage(&ui_font_kr_12, "joeon", r->joeon);
        for (int c = 0; c < 4; c++) {
            check_font_coverage(&ui_font_kr_12, "cat value", r->cats[c]);
        }
        check_font_coverage(&ui_font_kr_16, "rank message", r->message);
    }

    /* The 흐름 verse, all five elements, plus the tags. */
    for (int e = 0; e < 5; e++) {
        check_font_coverage(&ui_font_kr_12, "flow", omikuji_flow_text(e));
    }
    check_font_coverage(&ui_font_kr_12, "tag", MANSE_TAG_FLOW);
    check_font_coverage(&ui_font_kr_12, "tag", MANSE_TAG_HAESEOK);
    check_font_coverage(&ui_font_kr_12, "tag", MANSE_TAG_JOEON);

    /* Fixed chrome. */
    check_font_coverage(&ui_font_kr_hanja_16, "title", MANSE_TITLE);
    check_font_coverage(&ui_font_kr_hanja_12, "cat head", MANSE_CAT_JAE);
    check_font_coverage(&ui_font_kr_hanja_12, "cat head", MANSE_CAT_SA);
    check_font_coverage(&ui_font_kr_hanja_12, "cat head", MANSE_CAT_DAE);
    check_font_coverage(&ui_font_kr_hanja_12, "cat head", MANSE_CAT_GEON);
    check_font_coverage(&ui_font_kr_16, "iljin label", FORTUNE_ILJIN_LABEL);
    check_font_coverage(&ui_font_kr_16, "wifi label", FORTUNE_WIFI_LABEL);

    /* Runtime-composed text — characters no source literal carries. The date
     * line's digits and punctuation, every weekday syllable, and the pillar
     * suffixes. This is the check that caught the vanished-space bug's whole
     * class, so it stays paranoid. */
    check_font_coverage(&ui_font_kr_12, "date digits", "0123456789(). ");
    check_font_coverage(&ui_font_kr_12, "weekdays", FORTUNE_WEEKDAYS);
    check_font_coverage(&ui_font_kr_12, "pillar suffix", MANSE_SIDE_YEAR);
    check_font_coverage(&ui_font_kr_12, "pillar suffix", MANSE_SIDE_DAY);

    /* All 60 pillars: the composed "<hanja> <hangul>" line on the home page
     * (16 px), and the Hangul syllables the side pillars stack (12 px). A full
     * cycle is 60 consecutive days. */
    static const int mlen[13] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
    int y = 2019, m = 1, d = 27;                 /* a 甲子 day */
    for (int i = 0; i < 60; i++) {
        saju_iljin_t ij;
        saju_iljin_for_date(y, m, d, &ij);
        char line[32];
        snprintf(line, sizeof(line), "%s %s", ij.hanja, ij.hangul);
        check_font_coverage(&ui_font_kr_16, "iljin", line);
        check_font_coverage(&ui_font_kr_12, "pillar", ij.hangul);
        if (++d > mlen[m]) { d = 1; if (++m > 12) { m = 1; y++; } }
    }
}

/* Page 1 has no frame, so the old rule holds there: nothing may reach the
 * panel edge. */
static void check_bounds(const char *name, int require_ink) {
    int x0, y0, x1, y1;
    if (!ink_box(0, 0, HOR, VER, &x0, &y0, &x1, &y1)) {
        if (require_ink) FAILV("%s: rendered nothing", name);
        return;
    }
    if (x0 < 2 || x1 > HOR - 3 || y0 < 1 || y1 > VER - 2) {
        FAILV("%s: ink at x[%d..%d] y[%d..%d] reaches the panel edge (%dx%d)",
              name, x0, x1, y0, y1, HOR, VER);
    }
}

/* Page 0 is framed to the panel edge on purpose, so its checks are about the
 * frame being there and the content staying inside it. The row/column numbers
 * mirror ui_fortune.c's P0_* grid; if that grid moves, move these with it. */
static void check_manse(const char *name) {
    /* The double frame: outer 2 px border, white gap, 1 px inner border. */
    if (!is_black(0, 0) || !is_black(1, 1) || !is_black(HOR - 1, VER - 1)) {
        FAILV("%s: outer frame missing at the panel corners", name);
    }
    if (is_black(2, 2) || is_black(HOR - 3, VER - 3)) {
        FAILV("%s: the frame gap at (2,2)/(-3,-3) is inked — borders bled together", name);
    }
    if (!is_black(4, 4) || !is_black(HOR - 5, VER - 5)) {
        FAILV("%s: inner frame missing at (4,4)/(-5,-5)", name);
    }

    /* The title band: a mostly-black row through the 제호. */
    int black = 0;
    for (int x = 8; x < HOR - 8; x++) black += is_black(x, 15);
    if (black < (HOR - 16) / 2) {
        FAILV("%s: title band row y=15 is only %d/%d black — band missing", name, black, HOR - 16);
    }

    /* The grade must actually render between the pillar boxes, unclipped:
     * two-Hanja grades ink ~68 px wide, one-Hanja ~34, clipped ones neither. */
    int x0, y0, x1, y1;
    if (!ink_box(25, 47, 97, 89, &x0, &y0, &x1, &y1)) {
        FAILV("%s: no grade glyph rendered", name);
    } else {
        int w = x1 - x0 + 1;
        if (w < 24 || w > 74) {
            FAILV("%s: grade ink %d px wide (x %d..%d) — clipped or misplaced", name, w, x0, x1);
        }
    }

    /* Breathing rows the grid promises blank: between rule and grade row,
     * right of the seal between the verse and the seal's strip, and between
     * the seal strip and the fortune table. A label that outgrows its slot
     * lands here first. */
    for (int x = 6; x < HOR - 6; x++) {
        if (is_black(x, 44)) { FAILV("%s: ink bled into gap row y=44 at x=%d", name, x); break; }
    }
    for (int x = 32; x < HOR - 6; x++) {               /* x<32 is the seal's */
        if (is_black(x, 199) || is_black(x, 200)) {
            FAILV("%s: ink bled into gap rows y=199..200 at x=%d", name, x);
            break;
        }
    }
    for (int x = 6; x < HOR - 6; x++) {
        if (is_black(x, 214) || is_black(x, 215)) {
            FAILV("%s: ink bled into gap rows y=214..215 at x=%d", name, x);
            break;
        }
    }

    /* Adjacencies that hold only by font metrics — the verse's topmost ink
     * row sits just below the diamond rule and drifts silently if a font
     * regeneration changes the 12 px face's line height — plus the fixed
     * furniture: the table's borders and the seal in its strip. */
    for (int x = 8; x <= 113; x++) {
        if (x >= 55 && x <= 65) continue;               /* the diamond's cell */
        if (is_black(x, 92) || is_black(x, 93)) {
            FAILV("%s: verse ink reached rows y=92..93 (x=%d) — line-height drift", name, x);
            break;
        }
    }
    if (!is_black(60, 216) || !is_black(60, 243)) {
        FAILV("%s: fortune-table borders missing at (60,216)/(60,243)", name);
    }
    int fx0, fy0, fx1, fy1;
    if (!ink_box(6, 192, 32, 216, &fx0, &fy0, &fx1, &fy1)) {
        FAILV("%s: the seal rendered nothing in its strip", name);
    }
}

/* The verse, column by column. Mirrors ui_vtext.c's advance arithmetic over
 * the same MANSE_VERSE_* constants test_omikuji.c budgets against — but this
 * checks the *glass*, not the copy: every column the section list implies
 * must have ink, and so must the final glyph cell. That last assertion exists
 * because the first version of this page shipped a green build in which the
 * seal's opaque white circle painted over the 조언 verse's verb ending on all
 * seven ranks: the budget certified glyphs a later widget then deleted. */
static void check_verse(const char *name, const omikuji_result_t *r, int element) {
    /* ui_fortune.c's geometry: the verse spans the full content width and
     * ui_vtext justifies its columns across it — same stride formula. */
    const int RIGHT = 115, W = 108, TOP = 95, PITCH = MANSE_VERSE_GLYPH_PX, GLYPH = 12;
    const char *texts[3] = { omikuji_flow_text(element), r->haeseok, r->joeon };
    int col = 0;
    int last_x = -1, last_top = -1;                 /* the final glyph cell */

    int ncols = 0;
    for (int si = 0; si < 3; si++) {
        const char *text = texts[si] ? texts[si] : "";
        ncols++;
        for (int ti = 0; text[ti]; ti++) if (text[ti] == '\n') ncols++;
    }
    int stride = ncols > 0 ? W / ncols : PITCH;
    if (stride < PITCH) stride = PITCH;
    int inset = (stride - PITCH) / 2;

    for (int si = 0; si < 3; si++) {
        const char *text = texts[si] ? texts[si] : "";
        int ti = 0;
        bool first = true, end = false;
        while (!end) {
            int cell_x = RIGHT - (col + 1) * stride + inset;
            int y = first ? MANSE_VERSE_TAG_PX : 0;
            first = false;
            int glyph_top = -1;
            for (;;) {
                unsigned char c = (unsigned char)text[ti];
                if (c == '\0') { end = true; break; }
                if (c == '\n') { ti++; break; }
                uint32_t cp = utf8_next(text, &ti);
                if (cp == ' ') { y += MANSE_VERSE_SPACE_PX; continue; }
                glyph_top = y;
                y += PITCH;
            }
            int x0, y0, x1, y1;
            if (!ink_box(cell_x, TOP, cell_x + PITCH, TOP + MANSE_VERSE_COL_H,
                         &x0, &y0, &x1, &y1)) {
                FAILV("%s: verse column %d (x %d..%d) rendered nothing",
                      name, col, cell_x, cell_x + PITCH - 1);
            }
            if (glyph_top >= 0) { last_x = cell_x; last_top = TOP + glyph_top; }
            col++;
        }
    }
    if (col > MANSE_VERSE_MAX_COLS) {
        FAILV("%s: %d verse columns exceed MANSE_VERSE_MAX_COLS", name, col);
    }
    int x0, y0, x1, y1;
    if (last_x >= 0 &&
        !ink_box(last_x, last_top, last_x + PITCH, last_top + GLYPH + 2,
                 &x0, &y0, &x1, &y1)) {
        FAILV("%s: the verse's final glyph cell (x %d, y %d) has no ink — "
              "painted over?", name, last_x, last_top);
    }
}

int main(int argc, char **argv) {
    const char *outdir = (argc > 1) ? argv[1] : "shots";

    lv_init();
    lv_tick_set_cb(tick_cb);
    lv_display_t *disp = lv_display_create(HOR, VER);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_buffers(disp, fb, NULL, sizeof(fb), LV_DISPLAY_RENDER_MODE_FULL);

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_screen_load(scr);
    ui_fortune_create(scr);

    /* --- the day: date, weekday, year + day pillars ------------------------ */
    time_t now = time(NULL);
    manse_day_t day;
    struct tm lt; localtime_r(&now, &lt);
    day.year  = lt.tm_year + 1900;
    day.month = lt.tm_mon + 1;
    day.day   = lt.tm_mday;
    day.wday  = lt.tm_wday;
    saju_iljin_for_time(now, &day.iljin);
    saju_yearju_for_date(day.year, day.month, day.day, &day.yearju);
    ui_fortune_set_day(&day);
    ui_fortune_set_iljin(&day.iljin);
    printf("day %04d-%02d-%02d -> 일진 %s (%s), 년주 %s (%s), element %d\n",
           day.year, day.month, day.day, day.iljin.hanja, day.iljin.hangul,
           day.yearju.hanja, day.yearju.hangul, saju_element_of_gan(day.iljin.gan));

    ui_fortune_set_battery(true, 84);

    /* --- weather: real Open-Meteo when LOCATION is set --------------------- */
    bool wx_done = false;
    const char *loc = getenv("LOCATION");
    if (loc && *loc) {
        geo_loc_t g;
        weather_t w;
        if (weather_service_geocode(loc, &g) && weather_service_fetch(g.lat, g.lon, &w) && w.valid) {
            char city[64];
            if (g.country[0]) snprintf(city, sizeof city, "%s, %s", g.name, g.country);
            else              snprintf(city, sizeof city, "%s", g.name);
            ui_fortune_set_weather(w.now_valid, w.now_wx, w.now_temp_c, city);
            ui_fortune_set_forecast(w.days, w.day_count);
            printf("weather %s -> %s  %d C, %d-day forecast\n",
                   loc, city, w.now_temp_c, w.day_count);
            wx_done = true;
        }
        if (!wx_done) printf("weather lookup failed for '%s' -> sample\n", loc);
    }
    if (!wx_done) {
        static const wx_day_t sample[WX_FORECAST_MAX] = {
            { "FRI", WX_PARTLY, 15, 22 }, { "SAT", WX_SUN,    16, 24 },
            { "SUN", WX_CLOUD,  14, 20 }, { "MON", WX_RAIN,   12, 17 },
            { "TUE", WX_SUN,    14, 21 }, { "WED", WX_PARTLY, 15, 22 },
            { "THU", WX_SUN,    16, 24 },
        };
        ui_fortune_set_weather(true, WX_SUN, 28, "Seoul, KR");
        ui_fortune_set_forecast(sample, WX_FORECAST_MAX);
        printf("weather: sample (set LOCATION=... for live Open-Meteo)\n");
    }

    check_all_glyphs();
    printf("rendering %s/\n", outdir);

    /* --- all seven ranks on the 만세력 slip -------------------------------- */
    ui_fortune_show_page(UI_PAGE_OMIKUJI);
    int elem = saju_element_of_gan(day.iljin.gan);
    for (int r = 0; r < OMIKUJI_COUNT; r++) {
        const omikuji_result_t *res = omikuji_by_rank((omikuji_rank_t)r);
        ui_fortune_set_omikuji(res);
        run_refresh(8);
        char name[32];
        snprintf(name, sizeof(name), "omikuji_%d_%s", r, res->hanja);
        shot(outdir, name);
        check_manse(name);
        check_verse(name, res, elem);
    }

    /* --- home -------------------------------------------------------------- */
    ui_fortune_show_page(UI_PAGE_HOME);
    ui_fortune_tick();
    run_refresh(8);
    shot(outdir, "home");
    check_bounds("home", 1);

    /* --- provisioning overlay ---------------------------------------------- */
    ui_fortune_set_overlay(FORTUNE_WIFI_LABEL, "Join Wi-Fi:\nTicker Board-1A2B\nthen open the app");
    run_refresh(8);
    shot(outdir, "setup");
    check_bounds("setup", 1);
    ui_fortune_set_overlay(NULL, NULL);

    printf("%s — %d layout/glyph problem(s)\n", g_fail ? "FAILED" : "ok", g_fail);
    return g_fail ? 1 : 0;
}
