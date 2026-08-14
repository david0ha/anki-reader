/*
 * ui_card_question.c — the question side: the headword alone, on black.
 *
 * This is the "video" of the Shorts player. Everything about it is in service
 * of one thing being legible from across a room, so the panel is inverted and
 * the headword gets the 56 px face; the rail, the caption and the scrubber are
 * the same furniture the web app puts around a Short, drawn at the only two
 * tones this display has.
 *
 * Nothing here decides what a button does — kanji_nav.c does — but the prompt
 * and the rail captions name the buttons anyway, because a board with no touch
 * panel and no labels is a board nobody presses.
 */
#include <stdio.h>

#include "ui_icons.h"
#include "ui_internal.h"

#define RAIL_CHIP  44
#define RAIL_ICON  26

typedef struct {
    lv_obj_t *root;
    lv_obj_t *hero;
    lv_obj_t *prompt;
    lv_obj_t *deck;
    lv_obj_t *level;
    lv_obj_t *queue;
    lv_obj_t *scrub_fill;
} question_ui_t;

static question_ui_t q;

static void build_rail(lv_obj_t *par)
{
    const kanji_question_layout_t *l = kanji_question_layout();
    static const struct { ui_icon_t icon; const char *caption; } ITEMS[3] = {
        { ICON_BOOK,    S_SHEET_DESC },
        { ICON_COMMENT, S_SHEET_COMMENTS },
        { ICON_CLOCK,   S_HINT_FSRS },
    };

    const int chip_x = l->rail.x + (l->rail.w - RAIL_CHIP) / 2;
    for (int i = 0; i < l->rail_items && i < 3; i++) {
        const int y = LOCAL_Y(l->rail.y) + i * l->rail_step;

        /* A white chip with a black glyph, not a white glyph on the fill: at
         * 26 px a white stroke on black loses its thin parts to the panel's
         * binarization, and the chip is also what makes the rail read as three
         * buttons rather than three decorations. */
        ui_fill_white(par, chip_x, y, RAIL_CHIP, RAIL_CHIP);
        lv_obj_t *icon = ui_icon(par, ITEMS[i].icon, RAIL_ICON, 0);
        lv_obj_set_pos(icon, chip_x + (RAIL_CHIP - RAIL_ICON) / 2,
                       y + (RAIL_CHIP - RAIL_ICON) / 2);

        ui_lab_inv(par, l->rail.x, y + RAIL_CHIP + 2, l->rail.w, UI_F_BODY,
                   LV_TEXT_ALIGN_CENTER, ITEMS[i].caption);
    }
}

lv_obj_t *ui_card_question_create(lv_obj_t *par)
{
    const kanji_question_layout_t *l = kanji_question_layout();

    q.root = ui_fill(par, 0, 0, l->player.w, l->player.h);

    q.hero = ui_lab_w(q.root, l->hero.x, LOCAL_Y(l->hero.y), l->hero.w,
                      UI_F_HERO, LV_TEXT_ALIGN_CENTER, "");
    lv_obj_set_style_text_color(q.hero, lv_color_white(), 0);

    q.prompt = ui_lab_inv(q.root, l->prompt.x, LOCAL_Y(l->prompt.y), l->prompt.w,
                          UI_F_BODY, LV_TEXT_ALIGN_CENTER, S_TAP_TO_REVEAL);

    /* The caption block: the deck is the "channel" and the level is its badge,
     * exactly the two things a Short puts under the video. */
    q.deck = ui_lab_inv(q.root, l->caption.x, LOCAL_Y(l->caption.y),
                        l->caption.w, UI_F_HEAD, LV_TEXT_ALIGN_LEFT, "");
    q.level = ui_lab_inv(q.root, l->caption.x, LOCAL_Y(l->caption.y) + 28,
                         l->caption.w, UI_F_BODY, LV_TEXT_ALIGN_LEFT, "");
    q.queue = ui_lab_inv(q.root, l->queue.x, LOCAL_Y(l->queue.y), l->queue.w,
                         UI_F_BODY, LV_TEXT_ALIGN_LEFT, "");

    build_rail(q.root);

    /* A hairline track with a thicker bar over it: the two tones this panel has
     * cannot express "dim red", but they can express "thin" and "thick". */
    ui_fill_white(q.root, l->scrubber.x,
                  LOCAL_Y(l->scrubber.y) + l->scrubber.h - 1,
                  l->scrubber.w, 1);
    q.scrub_fill = ui_fill_white(q.root, l->scrubber.x, LOCAL_Y(l->scrubber.y),
                                 l->scrubber.w, l->scrubber.h);
    return q.root;
}

void ui_card_question_update(const kanji_t *k)
{
    const kanji_question_layout_t *l = kanji_question_layout();
    const bool have = k && k->card.valid;

    /* The face follows the word: a headword too long for 56 px, or one the
     * Japanese-only hero face cannot draw, drops to the 28 px face rather than
     * being ellipsized or drawn as tofu. */
    const lv_font_t *face = ui_hero_face(have ? k->card.front : "");
    lv_obj_set_style_text_font(q.hero, face, 0);
    lv_obj_set_height(q.hero, lv_font_get_line_height(face));
    ui_set(q.hero, have ? k->card.front : "");

    if (have) {
        ui_set(q.prompt, S_TAP_TO_REVEAL);
    } else if (k && k->session.complete) {
        ui_set(q.prompt, S_SESSION_DONE);
    } else if (k) {
        ui_set(q.prompt, S_NO_CARD);
    } else {
        ui_set(q.prompt, S_WAITING);
    }

    ui_set(q.deck, k ? k->session.deck : "");
    if (k && k->session.level[0]) {
        ui_setf(q.level, "%s", k->session.level);
    } else {
        ui_set(q.level, "");
    }

    if (k) {
        ui_setf(q.queue, "%s %d · %s %d · %s %d",
                S_LEFT_NEW, k->session.left_new,
                S_LEFT_REVIEW, k->session.left_review,
                S_RETRY, k->session.retry);
    } else {
        ui_set(q.queue, "");
    }

    /* The scrubber is the one filled bar on this board that carries a number,
     * so it is clamped rather than trusted: a track past its total would draw
     * a bar wider than the panel. */
    int filled = 0;
    if (k && k->session.track_total > 0) {
        int track = k->session.track;
        if (track > k->session.track_total) track = k->session.track_total;
        if (track < 0) track = 0;
        filled = l->scrubber.w * track / k->session.track_total;
    }
    lv_obj_set_width(q.scrub_fill, filled > 0 ? filled : 1);
    ui_show(q.scrub_fill, filled > 0);
}
