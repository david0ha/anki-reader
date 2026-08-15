/*
 * ui_card_front.c — 문제, the plate.
 *
 * This face is an art print. The board stands on a desk all day and is looked at far more often
 * than it is pressed, so the question side is composed to be worth looking at while nobody is
 * studying: one centred axis, a 3.5x step from the hero down to the labels, a hairline broken by
 * a mark, and a label|value plate at the foot. Everything it prints is either Japanese or the
 * learner's own record with this exact character.
 *
 * ## The front is spoiler-bound
 *
 * The one rule this file must not get wrong. It may print the headword, a Japanese example, that
 * example's かな, and the FSRS numbers — and nothing else. Never `senses`, never
 * `parts[].meaning`, never `examples[].gloss`: each of those is Korean and each is the answer.
 * `examples[].gloss` is the trap, because it reads as harmless context right up until it prints
 * 우연히 만나다 under 会う and the card the learner was about to recall is spoiled. There is
 * therefore no widget on this face that a Korean sense could reach even by accident — the quote
 * pair takes `.text` and `.reading` and the plate takes integers and `state_label`, and no other
 * card field is touched.
 *
 * ## Two calls, and why creating in the second one is a leak
 *
 * create() builds every widget once into a pane the full size of the panel and returns it;
 * update() only ever rewrites text and toggles visibility. This face is redrawn on every poll —
 * every five minutes, forever — so a single lv_*_create() reached from the update path leaks an
 * LVGL object per poll and takes the board's heap out days later, silently, on a device nobody is
 * watching. Every handle lives in the file-static struct below and is built exactly once.
 *
 * Because the pane is the whole 648x480, every rectangle from kanji_front_layout() is used
 * verbatim. Nothing in this file computes, offsets or invents a coordinate: the layout is pure
 * integers precisely so the host test and the simulator can assert on the same numbers the
 * firmware draws with, and a hand-written constant here would be outside both.
 */
#include "ui_internal.h"

typedef struct {
    lv_obj_t *root;

    /* The masthead. `badge_text` is a CHILD of `badge`, so one ui_show() hides the chip and its
     * text together and they can never disagree about whether the level is on the glass. */
    lv_obj_t *badge;
    lv_obj_t *badge_text;
    lv_obj_t *brand;
    lv_obj_t *status;
    lv_obj_t *counters;

    /* The hero slot carries the headword, and the completion notice when there is no headword —
     * see the font note in ui_card_front_update(). */
    lv_obj_t *hero;

    /* The ornament is three objects rather than one grouping pane: a pane spanning all three
     * would need a bounding rectangle the layout does not name, and inventing one here is exactly
     * what the pure-integer layout exists to prevent. */
    lv_obj_t *orn_left;
    lv_obj_t *orn_mark;
    lv_obj_t *orn_right;

    lv_obj_t *quote;
    lv_obj_t *quote_reading;

    lv_obj_t *plate_label[KANJI_PLATE_ROWS];
    lv_obj_t *plate_value[KANJI_PLATE_ROWS];
    lv_obj_t *plate_rule;
    lv_obj_t *plate_empty;

    lv_obj_t *queue;
    lv_obj_t *prompt;
} front_ui_t;

static front_ui_t f;

/* --- the ornament's mark ---------------------------------------------------------------------
 *
 * A filled diamond, drawn rather than set.
 *
 * Not a text glyph. U+25C6 ◆ is in no face this board ships: gen_fonts.py derives the symbol set
 * from ui_strings.h — 완성형 Hangul, ASCII, kana, both JIS X 0208 levels, the curated punctuation
 * and component forms — and a geometric shape is in none of those tables. A label here would put
 * a tofu box at the one point on this face the whole composition leads the eye to, and it would
 * do it on every card rather than on an unlucky one.
 *
 * And not ui_fill() either. The layout's rectangle is a 10 px square; filled, it reads as a
 * rendering fault between two hairlines, where a diamond reads as a printer's ornament — which is
 * the entire job this 100 px of paper has.
 *
 * The runs are ODD widths — 1,3,5,7,9,7,5,3,1 — because an odd run has a centre pixel and an even
 * one does not. Mixing the two puts the top half of the shape half a pixel off the bottom half,
 * and on a 1-bit panel there is no anti-aliasing to absorb that: it prints as a visibly bent
 * diamond rather than as a soft edge.
 *
 * Both centres come from the rectangle itself, and they recover the two numbers ui_kanji_layout.c
 * built the mark from: orn_mark is placed at (F_CENTER, F_ORN_Y) less half its size, so the
 * rectangle's own mid-point is F_CENTER and the row the two hairlines run along. Deriving them
 * rather than repeating them is what keeps the mark on the rule if the mark ever changes size.
 */
static void orn_mark_draw(lv_event_t *e)
{
    lv_layer_t *L = lv_event_get_layer(e);
    if (!L) return;

    lv_area_t a;
    lv_obj_get_coords(lv_event_get_target(e), &a);

    const int w = a.x2 - a.x1 + 1;
    const int h = a.y2 - a.y1 + 1;

    /* The largest odd square the rectangle holds. */
    int n = w < h ? w : h;
    if (n % 2 == 0) n--;
    if (n < 1) return;

    const int cx = a.x1 + w / 2;
    const int cy = a.y1 + h / 2;
    const int arm = n / 2;

    for (int j = -arm; j <= arm; j++) {
        const int half = arm - (j < 0 ? -j : j);
        /* One row per pass, as a filled rect: ui_draw_rect_abs() takes INCLUSIVE bounds, so a run
         * of width 2*half+1 is [cx-half, cx+half] and a single row is y1 == y2. */
        ui_draw_rect_abs(L, cx - half, cy + j, cx + half, cy + j, true, 0, false);
    }
}

/* --- create ------------------------------------------------------------------------------- */

lv_obj_t *ui_card_front_create(lv_obj_t *par)
{
    const kanji_front_layout_t *l = kanji_front_layout();

    /* Opaque white, and the full panel. The root is what covers the other face's pixels when the
     * router swaps them: a transparent pane would leave the answer spread's rules and dock
     * showing through the paper of the plate, and on e-Paper those pixels are still physically
     * there until something else is written over them. */
    f.root = ui_fill_white(par, 0, 0, UI_W, UI_H);

    /* The level chip is the ONE inverted block on this face. Two inverted blocks on a sheet and
     * neither of them is read first, so the whole budget for the loudest device the panel has is
     * spent here and nowhere else.
     *
     * Its text is a CHILD of the fill, centred by LVGL's own measurement rather than by an offset
     * written here. The chip is 24 px tall and the 16 px face's line box is 20, so the text wants
     * to start 2 px down — but that 2 is derived from a font metric, not from the grid. The layout
     * does not know it and should not carry it, and a hand-written copy here would be exactly the
     * kind of magic number the pure-integer layout exists to keep out of a screen file. */
    f.badge = ui_fill(f.root, l->badge.x, l->badge.y, l->badge.w, l->badge.h);
    f.badge_text = ui_lab_inv(f.badge, 0, 0, l->badge.w,
                              UI_F_BODY, LV_TEXT_ALIGN_CENTER, "");
    lv_obj_center(f.badge_text);

    f.brand    = ui_lab_r(f.root, l->brand,    UI_F_BODY, LV_TEXT_ALIGN_LEFT,   "");
    f.status   = ui_lab_r(f.root, l->status,   UI_F_BODY, LV_TEXT_ALIGN_CENTER, "");
    f.counters = ui_lab_r(f.root, l->counters, UI_F_BODY, LV_TEXT_ALIGN_RIGHT,  "");
    ui_rule(f.root, l->head_rule.x, l->head_rule.y, l->head_rule.w, l->head_rule.h);

    /* The headword hangs inside a pane the size of the hero rectangle rather than BEING that
     * rectangle, so LVGL centres it vertically as well as horizontally.
     *
     * The slot is 72 px because that is the 56 px face's line box plus its leading. A headword
     * too long for that face falls back to the 28 px one — ui_apply_headword() decides, on length
     * AND on glyph coverage, and nothing here picks a face by size — and its line box is half as
     * tall. Pinned to the top of the slot, the fallback hangs a word from the ceiling with 37 px
     * of paper under it, on the one axis this whole face is composed around. The offset that
     * fixes it is the difference between a face's line height and the slot, which is a fact about
     * the font rather than about the grid: the layout does not know it, should not carry it, and
     * a copy of it here would be a constant nothing asserts on. So it is measured, not written.
     *
     * The box still wraps rather than ellipsizing, because a headword that ended in "..." is a
     * different word — and it never actually wraps, because both faces set every model-valid
     * headword on one line (see kanji_hero_is_large()). */
    lv_obj_t *hero_slot = ui_pane(f.root, l->hero.x, l->hero.y, l->hero.w, l->hero.h);
    f.hero = ui_lab_headword(hero_slot, 0, 0, l->hero.w, l->hero.h, UI_F_HERO, "");
    lv_obj_set_height(f.hero, LV_SIZE_CONTENT);
    lv_obj_set_style_text_align(f.hero, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(f.hero);

    f.orn_left  = ui_rule(f.root, l->orn_left.x,  l->orn_left.y,
                          l->orn_left.w,  l->orn_left.h);
    f.orn_mark  = ui_pane(f.root, l->orn_mark.x,  l->orn_mark.y,
                          l->orn_mark.w,  l->orn_mark.h);
    lv_obj_add_event_cb(f.orn_mark, orn_mark_draw, LV_EVENT_DRAW_MAIN, NULL);
    f.orn_right = ui_rule(f.root, l->orn_right.x, l->orn_right.y,
                          l->orn_right.w, l->orn_right.h);

    f.quote         = ui_lab_r(f.root, l->quote,         UI_F_TITLE,
                               LV_TEXT_ALIGN_CENTER, "");
    f.quote_reading = ui_lab_r(f.root, l->quote_reading, UI_F_HEAD,
                               LV_TEXT_ALIGN_CENTER, "");

    /* The plate is the print's ORIGIN | USAGE | NOTE block: right-aligned labels, a vertical
     * hairline, left-aligned values. The labels never change, so they are written once here —
     * update() has no business touching a string that is the same on every card. */
    static const char *const PLATE[KANJI_PLATE_ROWS] = {
        S_PLATE_STATE, S_PLATE_REPS, S_PLATE_STABILITY, S_PLATE_LAPSES,
    };
    /* The label is the small tracked half and the value is the large half, and the two-step gap
     * between them (16 -> 28) is what makes the block read as a plate rather than as four rows
     * of a table. Set at one size the pair reads as a list of settings; set like this it reads
     * the way a dictionary sets ORIGIN | text, which is the whole reference for this face.
     *
     * The label takes tracking because it is a short fixed word doing a caption's job. The value
     * never does — it carries numbers and a state word that arrived from the network. */
    for (int i = 0; i < KANJI_PLATE_ROWS; i++) {
        f.plate_label[i] = ui_lab_r(f.root, l->plate_label[i], UI_F_BODY,
                                    LV_TEXT_ALIGN_RIGHT, PLATE[i]);
        ui_track(f.plate_label[i], 2);
        f.plate_value[i] = ui_lab_r(f.root, l->plate_value[i], UI_F_TITLE,
                                    LV_TEXT_ALIGN_LEFT, "");
    }
    f.plate_rule  = ui_rule(f.root, l->plate_rule.x, l->plate_rule.y,
                            l->plate_rule.w, l->plate_rule.h);
    f.plate_empty = ui_lab_r(f.root, l->plate_empty, UI_F_HEAD,
                             LV_TEXT_ALIGN_CENTER, S_NEW_CARD);

    ui_rule(f.root, l->foot_rule.x, l->foot_rule.y, l->foot_rule.w, l->foot_rule.h);
    f.queue  = ui_lab_r(f.root, l->queue,  UI_F_BODY, LV_TEXT_ALIGN_LEFT,  "");
    f.prompt = ui_lab_r(f.root, l->prompt, UI_F_BODY, LV_TEXT_ALIGN_RIGHT, "");

    /* The reveal prompt is the KEY0 legend, taken from the state machine rather than written out
     * here: a fixed legend on a board whose KEY0 means 정답 보기 on this face and 다시 on the next
     * is a lie printed in 16 px. A NULL nav is the front's own state by definition — kanji_nav.c's
     * hint() answers for the un-revealed face when it is given nothing — so this is the one legend
     * on the board that is genuinely constant, and it is still read from the one table. */
    ui_setf(f.prompt, "%s →", kanji_nav_hint_key0(NULL));

    return f.root;
}

/* --- update ------------------------------------------------------------------------------- */

void ui_card_front_update(const kanji_t *k, const ui_status_t *st)
{
    const bool snap = k != NULL;
    const bool have = k && k->card.valid;

    /* --- the masthead ---------------------------------------------------------------------- */

    const char *level = have ? k->card.level : "";
    ui_set(f.badge_text, level);
    ui_show(f.badge, level[0] != '\0');

    if (!snap)                        ui_set(f.brand, "");
    else if (k->session.deck[0])      ui_setf(f.brand, "%s · %s", S_BRAND, k->session.deck);
    else                              ui_set(f.brand, S_BRAND);
    ui_show(f.brand, snap);

    /* Offline outranks DEMO outranks 오래됨, and the order is the order of what the learner can
     * do about it. Offline means the card on the glass is one the board cannot replace, which is
     * the only one of the three that changes whether the numbers below can be trusted; a demo
     * card is never stale in a sense anybody can fix; and 오래됨 on a board that is plainly
     * offline is the same fact said twice. A NULL status is treated as online rather than as
     * offline, because a missing report is not evidence of a missing network. */
    const char *stamp = "";
    if (st && !st->online)          stamp = S_BADGE_OFFLINE;
    else if (snap && k->demo)       stamp = S_BADGE_DEMO;
    else if (st && st->stale)       stamp = S_BADGE_STALE;
    ui_set(f.status, stamp);
    ui_show(f.status, stamp[0] != '\0');

    if (snap) {
        ui_setf(f.counters, "%s %d · %s %d",
                S_STREAK, k->session.streak,
                S_REVIEWED_TODAY, k->session.reviewed_today);
    } else {
        ui_set(f.counters, "");
    }
    ui_show(f.counters, snap);

    /* --- the hero -------------------------------------------------------------------------- */

    if (have) {
        ui_apply_headword(f.hero, k->card.front);
    } else if (snap) {
        /* The completion notice stands where the headword stood, and the face MUST be set
         * explicitly on the way in. ui_apply_headword() leaves whichever face the last headword
         * chose on the label, and that is routinely the 56 px hero — which is Japanese-only,
         * because 56 px of Hangul is flash this board does not have. Setting a Korean sentence
         * without changing the face would print 오늘 학습 완료 as a row of tofu boxes across
         * the middle of the frame, on the one screen a learner sees at the end of every session. */
        lv_obj_set_style_text_font(f.hero, UI_F_TITLE, 0);
        ui_set(f.hero, S_SESSION_DONE);
    } else {
        /* No snapshot at all: blank it. A stale headword left on the glass is worse than an empty
         * frame, because it is indistinguishable from a current one. */
        ui_apply_headword(f.hero, "");
    }

    /* --- the ornament and the pull-quote --------------------------------------------------- */

    /* The ornament belongs to the rule below the headword, not to the quote under it, so it
     * survives a card with no example and goes only when there is no card. The layout's unequal
     * gaps — 24 px above it, 28 px below — say the same thing in numbers. */
    ui_show(f.orn_left,  have);
    ui_show(f.orn_mark,  have);
    ui_show(f.orn_right, have);

    /* 「」 and not quotation marks: the catalog is written in Japanese typography and the corner
     * brackets are what a Japanese sentence is quoted in. Both are in S_DATA_PUNCT, so the faces
     * carry them. The example's GLOSS is deliberately absent — see the spoiler note above. */
    const bool quoted = have && k->card.example_count > 0;
    if (quoted) ui_setf(f.quote, "「%s」", k->card.examples[0].text);
    else        ui_set(f.quote, "");
    ui_show(f.quote, quoted);

    const bool voiced = quoted && k->card.examples[0].reading[0] != '\0';
    ui_set(f.quote_reading, voiced ? k->card.examples[0].reading : "");
    ui_show(f.quote_reading, voiced);

    /* --- the plate ------------------------------------------------------------------------- */

    const kanji_fsrs_t *hist = have ? &k->card.fsrs : NULL;

    /* A card the scheduler has never touched has nothing to say in four rows, and four rows of
     * 실패 0회 under a character seen for the first time is noise dressed as a record. One centred
     * 새 카드 says the same thing and says it truthfully. */
    const bool fresh = hist && hist->state_label[0] == '\0' && hist->reps == 0
                    && hist->stability_days < 0;
    const bool rows  = hist && !fresh;

    for (int i = 0; i < KANJI_PLATE_ROWS; i++) {
        ui_show(f.plate_label[i], rows);
        ui_show(f.plate_value[i], rows);
    }
    ui_show(f.plate_rule,  rows);
    ui_show(f.plate_empty, fresh);

    if (rows) {
        /* Every row prints something. A label with an empty value beside it reads as a rendering
         * fault rather than as a missing fact, so a state the proxy did not word falls back to
         * the same "—" an unknown stability gets. */
        ui_set(f.plate_value[0], hist->state_label[0] ? hist->state_label : S_VALUE_UNKNOWN);
        ui_setf(f.plate_value[1], "%d%s", hist->reps, S_UNIT_TIMES);

        /* -1 is "the scheduler has no stability for this card", and it is NOT zero. A new card
         * has none and a same-day interval has one that rounds to zero, and printing 0 for the
         * first makes the board claim to know something it does not. See docs/kanji-contract.md. */
        if (hist->stability_days < 0) ui_set(f.plate_value[2], S_VALUE_UNKNOWN);
        else ui_setf(f.plate_value[2], "%d%s", hist->stability_days, S_UNIT_DAYS);

        ui_setf(f.plate_value[3], "%d%s", hist->lapses, S_UNIT_TIMES);
    }

    /* --- the footer ------------------------------------------------------------------------ */

    if (snap) {
        ui_setf(f.queue, "%s %d · %s %d · %s %d",
                S_LEFT_NEW,    k->session.left_new,
                S_LEFT_REVIEW, k->session.left_review,
                S_RETRY,       k->session.retry);
    } else {
        ui_set(f.queue, "");
    }
    ui_show(f.queue, snap);

    /* The prompt goes with the card, not with the face. kanji_nav.c refuses a reveal when there
     * is nothing to reveal, so printing 정답 보기 → over a finished session would advertise a
     * press that does nothing — which is how a learner is taught to stop believing the legend. */
    ui_show(f.prompt, have);
}
