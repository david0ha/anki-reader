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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HOR KANJI_SCREEN_W
#define VER KANJI_SCREEN_H

static uint8_t  fb[HOR * VER * 2];
static uint16_t capture[HOR * VER];
static uint32_t tick_now;
static int      failures;
static char     shot_dir[512] = "shots";

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

static void want_filled(const char *name, kanji_rect_t r, int pct)
{
    int found = ink_rect(r);
    if (found * 100 < r.w * r.h * pct) {
        FAILV("%s: %d%% filled, wanted at least %d%%",
              name, found * 100 / (r.w * r.h), pct);
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

static void check_chrome(const char *screen)
{
    const kanji_chrome_t *c = kanji_chrome_layout();
    char label[128];

    /* The header is a full-bleed fill with white text punched out of it. Both
     * halves matter: a header that stopped filling would still show its text,
     * and a header whose text stopped rendering would still look like a band. */
    snprintf(label, sizeof label, "%s: header band", screen);
    want_filled(label, c->header, 70);
    snprintf(label, sizeof label, "%s: header text", screen);
    want_paper(label, c->header, 400);

    snprintf(label, sizeof label, "%s: footer legend", screen);
    want_ink(label, c->footer, 300);

    /* All four key hints, so a legend that lost one is caught. */
    for (int i = 0; i < 4; i++) {
        snprintf(label, sizeof label, "%s: key hint %d", screen, i);
        want_ink(label, c->key[i], 60);
    }
}

/* --- per-screen pixel checks ---------------------------------------------- */

static void check_question(void)
{
    const kanji_question_layout_t *q = kanji_question_layout();

    check_chrome("question");
    want_filled("question: the player is inverted", q->player, 60);
    want_paper("question: the headword", q->hero, 600);
    want_paper("question: the reveal prompt", q->prompt, 120);
    want_paper("question: the deck caption", q->caption, 120);
    want_paper("question: the queue counters", q->queue, 100);
    want_paper("question: the action rail", q->rail, 2000);

    /* The scrubber is a partial bar: the demo card is 35 of 60, so the left
     * third must be white and the right end must not be. */
    kanji_rect_t left = { 8, q->scrubber.y, 40, q->scrubber.h };
    kanji_rect_t right = { HOR - 48, q->scrubber.y, 40, q->scrubber.h };
    want_paper("question: the scrubber's filled head", left, 100);
    if (paper_count(right) > 60) {
        FAIL("question: the scrubber is filled past the card's position");
    }
}

static void check_answer(kanji_grade_t cursor)
{
    const kanji_answer_layout_t *a = kanji_answer_layout();

    check_chrome("answer");
    want_filled("answer: the headword band is inverted", a->band, 55);
    want_paper("answer: the headword", a->hero, 600);
    want_paper("answer: the reading", a->reading, 90);
    want_paper("answer: the JLPT chip", a->level, 40);

    want_mostly_paper("answer: the meaning is on white", a->meaning, 30);
    want_ink("answer: the meaning", a->meaning, 300);
    want_ink("answer: the examples", a->examples, 300);

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

static void check_sheet(const char *name, bool with_stats)
{
    const kanji_sheet_layout_t *l = kanji_sheet_layout(with_stats);
    char label[128];

    check_chrome(name);
    snprintf(label, sizeof label, "%s: the band is inverted", name);
    want_filled(label, l->band, 55);
    snprintf(label, sizeof label, "%s: the band names the card", name);
    want_paper(label, l->band_word, 150);
    snprintf(label, sizeof label, "%s: the band names the sheet", name);
    want_paper(label, l->band_title, 60);

    snprintf(label, sizeof label, "%s: the body is on white", name);
    want_mostly_paper(label, l->body, 35);
    snprintf(label, sizeof label, "%s: the body has text", name);
    want_ink(label, l->body, 1500);

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

    const ui_status_t online = { true, false, true, 78 };
    ui_kanji_set_status(&online);

    /* The five screens, in the order a learner meets them. */
    kanji_nav_t nav = nav_question();
    shot(&card, &nav, "01-question");
    check_question();

    nav = nav_answer(KANJI_GRADE_GOOD);
    shot(&card, &nav, "02-answer");
    check_answer(KANJI_GRADE_GOOD);

    /* KEY0 walks the cursor; the dock is the only thing that may change. */
    nav = nav_answer(KANJI_GRADE_EASY);
    shot(&card, &nav, "03-answer-easy");
    check_answer(KANJI_GRADE_EASY);

    nav = nav_answer(KANJI_GRADE_AGAIN);
    shot(&card, &nav, "04-answer-again");
    check_answer(KANJI_GRADE_AGAIN);

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
    shot(&card, &nav, "05-description");
    check_sheet("description", false);

    nav = nav_sheet(KANJI_SHEET_COMMENTS, 0, true);
    shot(&card, &nav, "06-comments");
    check_sheet("comments", false);

    kanji_t three = with_three_comments(&card);
    nav = nav_sheet(KANJI_SHEET_COMMENTS, 1, true);
    shot(&three, &nav, "07-comments-page2");
    check_sheet("comments page 2", false);

    for (int page = 0; page < 3; page++) {
        char name[32];
        snprintf(name, sizeof name, "%02d-fsrs-%d", 8 + page, page + 1);
        nav = nav_sheet(KANJI_SHEET_FSRS, page, true);
        shot(&card, &nav, name);
        check_sheet("fsrs", true);
    }

    /* The states a learner meets on a bad day, and the one they meet at the
     * end of a good one. */
    kanji_t done = card;
    done.card.valid = false;
    done.session.complete = true;
    nav = nav_question();
    shot(&done, &nav, "11-session-complete");
    check_chrome("session complete");
    want_paper("session complete: the message", kanji_question_layout()->prompt, 200);

    /* A ten-character headword: the longest the catalog holds, and the case
     * kanji_hero_is_large() exists for. It must still fit the hero box. */
    kanji_t longword = card;
    kanji_str_copy(longword.card.front, KANJI_FRONT_MAX, "取り替えるつもり");
    nav = nav_question();
    shot(&longword, &nav, "12-long-headword");
    {
        const kanji_question_layout_t *q = kanji_question_layout();
        want_paper("long headword: still drawn", q->hero, 600);

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

        /* And the pixel proof: the headword is centred, so a word that ran past
         * its box would leave ink in the player's LEFT margin, which is the one
         * strip of the question screen nothing else draws in. */
        kanji_rect_t left_margin = { 0, q->hero.y, q->hero.x, q->hero.h };
        if (paper_count(left_margin) > 40) {
            FAIL("long headword: the hero overflowed its box");
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
    want_paper("ascii headword: drawn, not tofu", kanji_question_layout()->hero, 500);

    const ui_status_t offline = { false, true, false, 0 };
    ui_kanji_set_status(&offline);
    nav = nav_question();
    shot(&card, &nav, "13-offline");
    check_chrome("offline");
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

    printf("%s — %d problem(s); shots in %s/\n",
           failures ? "FAILED" : "ok", failures, shot_dir);
    return failures ? 1 : 0;
}
