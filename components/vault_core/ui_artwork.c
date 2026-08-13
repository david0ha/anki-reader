/*
 * ui_artwork.c — one native-pixel tarot card and its daily reading.
 *
 * The 272 x 464 I1 card is never scaled: it consumes 96.7% of the panel
 * height and keeps every authored pixel.  The narrow gutter is treated as a
 * deck spine; the rest becomes a cut-corner reading frame.  All prose and line
 * breaks arrive normalized from the producer, so this file only places pixels.
 */
#include "ui_artwork.h"

#include "tarot_cards.h"
#include "ui_artwork_layout.h"
#include "ui_internal.h"

#include <stdio.h>
#include <string.h>

typedef enum {
    DECK_MARK_MAJOR,
    DECK_MARK_CUPS,
    DECK_MARK_PENTACLES,
    DECK_MARK_SWORDS,
    DECK_MARK_WANDS,
} deck_mark_t;

typedef struct {
    lv_obj_t *root;
    lv_obj_t *ink;
    lv_obj_t *card;
    lv_obj_t *date;
    lv_obj_t *deck_ref;
    lv_obj_t *headline[TAROT_LINES_MAX];
    lv_obj_t *card_name;
    lv_obj_t *card_name_en;
    lv_obj_t *flow[TAROT_LINES_MAX];
    lv_obj_t *caution[TAROT_LINES_MAX];
    lv_obj_t *action[TAROT_LINES_MAX];
    lv_obj_t *orientation;
    lv_obj_t *overlay;
    lv_obj_t *overlay_title;
    lv_obj_t *overlay_body;
    deck_mark_t deck_mark;
} artwork_ui_t;

static artwork_ui_t s;

static void draw_diamond(lv_layer_t *layer, int cx, int cy, int r, bool fill)
{
    if (fill) {
        for (int dy = -r; dy <= r; dy++) {
            int half = r - (dy < 0 ? -dy : dy);
            ui_draw_line_abs(layer, cx - half, cy + dy, cx + half, cy + dy, 1, false);
        }
        return;
    }
    ui_draw_line_abs(layer, cx, cy - r, cx + r, cy, 1, false);
    ui_draw_line_abs(layer, cx + r, cy, cx, cy + r, 1, false);
    ui_draw_line_abs(layer, cx, cy + r, cx - r, cy, 1, false);
    ui_draw_line_abs(layer, cx - r, cy, cx, cy - r, 1, false);
}

static void draw_cut_frame(lv_layer_t *layer, artwork_rect_t r, int inset)
{
    int x1 = r.x + inset;
    int y1 = r.y + inset;
    int x2 = r.x + r.w - 1 - inset;
    int y2 = r.y + r.h - 1 - inset;
    int cut = inset ? 5 : 7;
    ui_draw_line_abs(layer, x1 + cut, y1, x2 - cut, y1, 1, false);
    ui_draw_line_abs(layer, x2 - cut, y1, x2, y1 + cut, 1, false);
    ui_draw_line_abs(layer, x2, y1 + cut, x2, y2 - cut, 1, false);
    ui_draw_line_abs(layer, x2, y2 - cut, x2 - cut, y2, 1, false);
    ui_draw_line_abs(layer, x2 - cut, y2, x1 + cut, y2, 1, false);
    ui_draw_line_abs(layer, x1 + cut, y2, x1, y2 - cut, 1, false);
    ui_draw_line_abs(layer, x1, y2 - cut, x1, y1 + cut, 1, false);
    ui_draw_line_abs(layer, x1, y1 + cut, x1 + cut, y1, 1, false);
}

static void draw_reading_rule(lv_layer_t *layer, int y)
{
    ui_draw_line_abs(layer, 319, y, 468, y, 1, false);
    draw_diamond(layer, 474, y, 3, false);
    ui_draw_line_abs(layer, 480, y, 629, y, 1, false);
}

static void draw_deck_spine(lv_layer_t *layer, artwork_rect_t r)
{
    int x1 = r.x + 1;
    int x2 = r.x + r.w - 2;
    ui_draw_line_abs(layer, x1, r.y, x1, r.y + r.h, 1, false);
    ui_draw_line_abs(layer, x2, r.y, x2, r.y + r.h, 1, false);
    for (int y = r.y + 8; y < r.y + r.h - 8; y += 18)
        ui_draw_line_abs(layer, x1 + 1, y, x2 - 1, y + 6, 1, false);
    for (int y = r.y + 36; y < r.y + r.h; y += 88)
        draw_diamond(layer, (x1 + x2) / 2, y, 4, true);
}

static void draw_seal_glyph(lv_layer_t *layer, int cx, int cy)
{
    ui_draw_ring_abs(layer, cx, cy, 10, 1, 0, 360);
    switch (s.deck_mark) {
    case DECK_MARK_CUPS:
        ui_draw_line_abs(layer, cx - 5, cy - 4, cx - 3, cy + 3, 1, false);
        ui_draw_line_abs(layer, cx - 3, cy + 3, cx + 3, cy + 3, 1, false);
        ui_draw_line_abs(layer, cx + 3, cy + 3, cx + 5, cy - 4, 1, false);
        ui_draw_line_abs(layer, cx, cy + 3, cx, cy + 7, 1, false);
        break;
    case DECK_MARK_PENTACLES:
        ui_draw_line_abs(layer, cx, cy - 6, cx + 5, cy + 5, 1, false);
        ui_draw_line_abs(layer, cx + 5, cy + 5, cx - 6, cy - 2, 1, false);
        ui_draw_line_abs(layer, cx - 6, cy - 2, cx + 6, cy - 2, 1, false);
        ui_draw_line_abs(layer, cx + 6, cy - 2, cx - 5, cy + 5, 1, false);
        ui_draw_line_abs(layer, cx - 5, cy + 5, cx, cy - 6, 1, false);
        break;
    case DECK_MARK_SWORDS:
        ui_draw_line_abs(layer, cx, cy - 7, cx, cy + 6, 2, false);
        ui_draw_line_abs(layer, cx - 5, cy + 2, cx + 5, cy + 2, 1, false);
        draw_diamond(layer, cx, cy - 7, 2, true);
        break;
    case DECK_MARK_WANDS:
        ui_draw_line_abs(layer, cx - 2, cy + 7, cx + 2, cy - 7, 2, false);
        ui_draw_line_abs(layer, cx, cy - 2, cx - 5, cy - 5, 1, false);
        ui_draw_line_abs(layer, cx - 1, cy + 3, cx + 5, cy, 1, false);
        break;
    case DECK_MARK_MAJOR:
    default:
        ui_draw_ring_abs(layer, cx, cy, 3, 1, 0, 360);
        ui_draw_line_abs(layer, cx, cy - 8, cx, cy - 5, 1, false);
        ui_draw_line_abs(layer, cx, cy + 5, cx, cy + 8, 1, false);
        ui_draw_line_abs(layer, cx - 8, cy, cx - 5, cy, 1, false);
        ui_draw_line_abs(layer, cx + 5, cy, cx + 8, cy, 1, false);
        break;
    }
}

static void artwork_draw_cb(lv_event_t *event)
{
    lv_layer_t *layer = lv_event_get_layer(event);
    if (!layer) return;
    const artwork_layout_t *l = artwork_tarot_layout();

    /* A one-pixel reveal makes the card a physical object without consuming
     * any of the native image area. */
    ui_draw_rect_abs(layer, l->card.x - 1, l->card.y - 1,
                     l->card.x + l->card.w, l->card.y + l->card.h,
                     false, 1, false);
    draw_deck_spine(layer, l->deck_spine);
    draw_cut_frame(layer, l->reading_frame, 0);
    draw_cut_frame(layer, l->reading_frame, 4);
    for (int i = 0; i < ARTWORK_READING_RULES; i++)
        draw_reading_rule(layer, l->rule_y[i]);

    /* Corner registration marks and the active-deck seal give the otherwise
     * unused frame edges the feel of an engraved plate, never app chrome. */
    ui_draw_line_abs(layer, 313, 16, 330, 16, 2, false);
    ui_draw_line_abs(layer, 617, 464, 634, 464, 2, false);
    draw_diamond(layer, 474, 14, 3, true);
    draw_seal_glyph(layer, 615, 445);
}

static void hide_lines(lv_obj_t *lines[TAROT_LINES_MAX])
{
    for (int i = 0; i < TAROT_LINES_MAX; i++) ui_show(lines[i], false);
}

static void set_lines(lv_obj_t *labels[TAROT_LINES_MAX], const tarot_lines_t *lines)
{
    hide_lines(labels);
    if (!lines) return;
    int count = lines->line_count > TAROT_LINES_MAX ? TAROT_LINES_MAX : lines->line_count;
    for (int i = 0; i < count; i++) {
        ui_set(labels[i], lines->lines[i]);
        ui_show(labels[i], true);
    }
}

static void format_date(char *out, size_t size, const char *date, bool demo)
{
    if (demo) {
        snprintf(out, size, "운세 디자인 미리보기");
        return;
    }
    int year, month, day;
    if (date && sscanf(date, "%4d-%2d-%2d", &year, &month, &day) == 3)
        snprintf(out, size, "오늘의 운세 · %04d.%02d.%02d", year, month, day);
    else
        snprintf(out, size, "오늘의 운세");
}

static deck_mark_t mark_for(const tarot_card_metadata_t *meta)
{
    if (!meta || !meta->suit) return DECK_MARK_MAJOR;
    if (strcmp(meta->suit, "cups") == 0) return DECK_MARK_CUPS;
    if (strcmp(meta->suit, "pentacles") == 0) return DECK_MARK_PENTACLES;
    if (strcmp(meta->suit, "swords") == 0) return DECK_MARK_SWORDS;
    return DECK_MARK_WANDS;
}

static void set_deck_reference(const daily_tarot_t *tarot,
                               const tarot_card_metadata_t *meta)
{
    char suit[12] = "MAJOR";
    if (meta && meta->suit) {
        size_t i = 0;
        for (; meta->suit[i] && i < sizeof(suit) - 1; i++) {
            char c = meta->suit[i];
            suit[i] = c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c;
        }
        suit[i] = '\0';
    }
    const char *number = tarot ? strrchr(tarot->card_id, '-') : NULL;
    ui_setf(s.deck_ref, "%s · %s", suit, number ? number + 1 : "--");
}

static lv_obj_t *section_tag(lv_obj_t *parent, int y, const char *text)
{
    ui_fill(parent, 319, y, 39, 19);
    return ui_lab_inv(parent, 319, y, 39, UI_F_BODY, LV_TEXT_ALIGN_CENTER, text);
}

void ui_artwork_create(lv_obj_t *parent)
{
    memset(&s, 0, sizeof(s));
    const artwork_layout_t *l = artwork_tarot_layout();
    s.root = parent;
    lv_obj_remove_style_all(parent);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(parent, ARTWORK_SCREEN_W, ARTWORK_SCREEN_H);
    lv_obj_set_style_bg_color(parent, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    s.ink = ui_pane(parent, 0, 0, ARTWORK_SCREEN_W, ARTWORK_SCREEN_H);
    lv_obj_add_event_cb(s.ink, artwork_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    s.card = lv_image_create(parent);
    lv_obj_remove_style_all(s.card);
    lv_obj_set_pos(s.card, l->card.x, l->card.y);
    lv_image_set_antialias(s.card, false);
    lv_image_set_src(s.card, tarot_card_image("major-00"));

    s.date = ui_lab_w(parent, 319, 21, 218, UI_F_BODY, LV_TEXT_ALIGN_LEFT, "오늘의 운세");
    s.deck_ref = ui_lab_w(parent, 535, 21, 94, UI_F_BODY, LV_TEXT_ALIGN_RIGHT, "MAJOR · 00");
    for (int i = 0; i < TAROT_LINES_MAX; i++) {
        s.headline[i] = ui_lab_w(parent, 319, 49 + i * 32, 310,
                                 UI_F_ART_HEAD, LV_TEXT_ALIGN_LEFT, "");
        s.flow[i] = ui_lab_w(parent, 366, 180 + i * 24, 263,
                             UI_F_BODY, LV_TEXT_ALIGN_LEFT, "");
        s.caution[i] = ui_lab_w(parent, 366, 282 + i * 24, 263,
                                UI_F_BODY, LV_TEXT_ALIGN_LEFT, "");
        s.action[i] = ui_lab_w(parent, 366, 379 + i * 24, 263,
                               UI_F_BODY, LV_TEXT_ALIGN_LEFT, "");
    }
    s.card_name = ui_lab_w(parent, 319, 119, 180, UI_F_HEAD, LV_TEXT_ALIGN_LEFT, "");
    s.card_name_en = ui_lab_w(parent, 469, 123, 160, UI_F_NUM_SM, LV_TEXT_ALIGN_RIGHT, "");

    section_tag(parent, 180, "흐름");
    section_tag(parent, 282, "주의");
    section_tag(parent, 379, "실천");
    s.orientation = ui_lab_w(parent, 319, 438, 278, UI_F_BODY,
                             LV_TEXT_ALIGN_LEFT, "정방향 · RIDER-WAITE");

    s.overlay = ui_frame(parent, 0, 0, ARTWORK_SCREEN_W, ARTWORK_SCREEN_H, 0);
    ui_frame(s.overlay, 129, 125, 390, 230, 2);
    s.overlay_title = ui_lab_w(s.overlay, 159, 164, 330, UI_F_HEAD,
                               LV_TEXT_ALIGN_CENTER, "");
    s.overlay_body = ui_lab_w(s.overlay, 159, 207, 330, UI_F_BODY,
                              LV_TEXT_ALIGN_CENTER, "");
    ui_lab_wrap(s.overlay_body, 112);
    ui_show(s.overlay, false);
}

void ui_artwork_set_data(const vault_t *vault)
{
    const daily_tarot_t *tarot =
        (vault && vault->valid && vault->daily_tarot.valid) ? &vault->daily_tarot : NULL;
    const lv_image_dsc_t *image = tarot ? tarot_card_image(tarot->card_id) : NULL;
    const tarot_card_metadata_t *meta = tarot ? tarot_card_metadata(tarot->card_id) : NULL;

    hide_lines(s.headline);
    hide_lines(s.flow);
    hide_lines(s.caution);
    hide_lines(s.action);

    if (!tarot || !image || !meta) {
        lv_image_set_src(s.card, tarot_card_image("major-00"));
        ui_set(s.date, "오늘의 운세");
        ui_set(s.deck_ref, "DECK · --");
        ui_set(s.headline[0], "오늘의 카드를");
        ui_set(s.headline[1], "불러오는 중");
        ui_show(s.headline[0], true);
        ui_show(s.headline[1], true);
        ui_set(s.card_name, "연결 대기");
        ui_set(s.card_name_en, "WAITING");
        ui_set(s.orientation, "정방향 · RIDER-WAITE");
        s.deck_mark = DECK_MARK_MAJOR;
        lv_obj_invalidate(s.ink);
        return;
    }

    char date[48];
    format_date(date, sizeof(date), tarot->date, vault->demo);
    ui_set(s.date, date);
    set_deck_reference(tarot, meta);
    set_lines(s.headline, &tarot->headline);
    set_lines(s.flow, &tarot->flow);
    set_lines(s.caution, &tarot->caution);
    set_lines(s.action, &tarot->action);
    ui_set(s.card_name, meta->name_ko);
    ui_set(s.card_name_en, meta->name_en);
    ui_set(s.orientation, vault->demo ? "예시 카드 · RIDER-WAITE" :
                                       "정방향 · RIDER-WAITE");
    lv_image_set_src(s.card, image);
    s.deck_mark = mark_for(meta);
    lv_obj_invalidate(s.ink);
}

void ui_artwork_set_overlay(const char *title, const char *body)
{
    if (!s.overlay) return;
    if (!title && !body) {
        ui_show(s.overlay, false);
        return;
    }
    ui_set(s.overlay_title, title);
    ui_set(s.overlay_body, body);
    ui_show(s.overlay, true);
    lv_obj_move_foreground(s.overlay);
}
