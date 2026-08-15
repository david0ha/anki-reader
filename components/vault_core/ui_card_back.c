/*
 * ui_card_back.c — 정답, the dictionary spread.
 *
 * Fourteen blocks composed into 300 px of two columns, with nothing behind a button. The
 * five-screen UI this replaced paged 유래, 구성요소 and the FSRS numbers into sheets of their
 * own, so seeing what a character was made of cost three presses and three full refreshes —
 * nine seconds of a panel strobing to read two lines that fit beside the senses all along.
 * Every field those sheets paged through was already in kanji_t. This file is what that
 * observation costs: one create() that builds every widget once, and one update() that decides
 * which of them have anything to say about this particular card.
 *
 * NOT ONE COORDINATE IS COMPUTED HERE. Every rectangle is a named field of
 * kanji_back_layout_t, used verbatim, in panel coordinates — the pane is the full 648x480, so
 * there is no translation to get wrong. The grid those fields describe is _Static_assert-ed in
 * ui_kanji_layout.c and measured again by the simulator, and both of those checks are worth
 * exactly nothing the moment a screen file offsets one of them by hand.
 *
 * Hierarchy comes from scale, from the eyebrows, and from the two rule weights. There are no
 * boxes, no radii, no borders outside the dock's cells and no greys: this panel has no grey,
 * and a mid-tone binarizes into a dashed line rather than into a lighter one.
 */
#include <stdio.h>

#include "ui_internal.h"

/* The dock walks PHYSICAL BUTTONS and asks kanji_button_grade() what each one means, so the
 * legend on the glass cannot drift from the state machine. That walk indexes the layout's
 * per-grade arrays with a button ordinal, which is only sound while the two enums are the same
 * length — and a button added without a rating (or the reverse) would otherwise read one cell
 * past the end of cell[] rather than failing to build. */
_Static_assert(KANJI_BTN_COUNT == KANJI_GRADE_COUNT,
               "the dock prints one cell per button and one rating per cell; the four buttons "
               "and the four grades are the same four things");

/* The button glyph each cell wears, in button order. This is what makes the dock
 * self-documenting: a fixed legend printed elsewhere is a strip of text somebody has to keep
 * true by hand, and the first time it is wrong the learner stops believing the other three. */
static const char *const KEYCAP[KANJI_BTN_COUNT] = { S_KEY0, S_KEY1, S_KEY2, S_BOOT };

typedef struct {
    lv_obj_t *root;
    lv_obj_t *body;   /* everything drawable, so "no card" is one call rather than thirty */

    /* masthead */
    lv_obj_t *hero;
    lv_obj_t *status;
    lv_obj_t *due;
    lv_obj_t *reading;
    lv_obj_t *badge_fill;
    lv_obj_t *badge;

    /* left column — the prose */
    lv_obj_t *sense_eyebrow, *senses, *sense_rule;
    lv_obj_t *build_eyebrow, *principle, *build, *build_rule;
    lv_obj_t *example_eyebrow, *example[KANJI_EXAMPLES_SHOWN];

    /* right rail — the figures */
    lv_obj_t *read_eyebrow, *on_label, *on_value, *kun_label, *kun_value, *read_rule;
    lv_obj_t *part_eyebrow, *part_glyph[KANJI_PARTS_SHOWN], *part_gloss[KANJI_PARTS_SHOWN];
    lv_obj_t *part_rule;
    lv_obj_t *stat_eyebrow, *stat_value[KANJI_STATS_SHOWN];

    /* the dock */
    lv_obj_t *cell_frame[KANJI_GRADE_COUNT];
    lv_obj_t *cell_fill[KANJI_GRADE_COUNT];
    lv_obj_t *cell_key[KANJI_GRADE_COUNT];
    lv_obj_t *cell_name[KANJI_GRADE_COUNT];
    lv_obj_t *cell_span[KANJI_GRADE_COUNT];
} back_ui_t;

static back_ui_t b;

/* --- the small decisions -------------------------------------------------- */

/* Whether a section prints at all. A section on this face is eyebrow -> hairline -> content, and
 * the three travel together: an eyebrow over nothing announces a block the card does not have,
 * and a hairline with nothing above it is two rules with white between them, which reads as a
 * rendering fault rather than as air. Vocab cards carry no components at all — 구성 · つくり over
 * an empty rail is the common case here, not the corner one. */
static void section(lv_obj_t *eyebrow, lv_obj_t *rule, bool on)
{
    ui_show(eyebrow, on);
    ui_show(rule, on);   /* NULL for a block whose section is closed by the dock */
}

/* Flip a label to white so it survives an inverted cell.
 *
 * ui_lab_inv() is this call plus a creation, and creating is the one thing update() may not do:
 * a second, inverted copy of every dock label would put two labels on the same paper, which is
 * precisely what the simulator's no-overlap walk exists to fail on — and black-on-black is the
 * one collision pixels cannot catch. So the label is created once and only its ink moves. */
static void ink(lv_obj_t *label, bool inverted)
{
    if (!label) return;
    lv_obj_set_style_text_color(label, inverted ? lv_color_white() : lv_color_black(), 0);
}

/* The exceptional-state word, or nothing at all.
 *
 * Offline outranks stale and stale outranks nothing, in that order, because they are causally
 * nested: a board that cannot reach the proxy is offline first and its card is stale only as a
 * consequence, and printing the consequence instead of the cause sends the learner to look at
 * the card rather than at the Wi-Fi. DEMO sits between them — a demo card is never stale, it is
 * the card the board was built with. */
static const char *status_text(const kanji_t *k, const ui_status_t *st)
{
    if (st && !st->online)  return S_BADGE_OFFLINE;
    if (k && k->demo)       return S_BADGE_DEMO;
    if (st && st->stale)    return S_BADGE_STALE;
    return "";
}

/* The senses, joined. Its own function so its 730-byte buffer lives in its own frame: the model
 * is deliberately full fidelity and this component is compiled with -Werror=frame-larger-than,
 * so a join like this belongs anywhere except on the update path's frame. */
static void set_senses(lv_obj_t *label, const kanji_card_t *c)
{
    char joined[KANJI_SENSES_MAX * (KANJI_SENSE_MAX + 2)];
    size_t at = 0;

    joined[0] = '\0';
    for (int i = 0; i < c->sense_count && i < KANJI_SENSES_MAX; i++) {
        const int n = snprintf(joined + at, sizeof joined - at, "%s%s",
                               at ? ", " : "", c->senses[i]);
        if (n <= 0 || (size_t)n >= sizeof joined - at) break;
        at += (size_t)n;
    }
    ui_set(label, joined);
}

/* One 예문 row: the Japanese, its かな, and the Korean gloss.
 *
 * Each separator is dropped with the field it introduces, for the same reason the masthead drops
 * its middot when the card has no due span: a trailing " — " on a card whose gloss the catalog
 * never carried reads as a string that failed to render, and nothing on the glass distinguishes
 * that from a bug. Both separators reach the shipped faces through ui_strings.h — the middot in
 * S_COMPOSED_CHARS, the em dash in S_VALUE_UNKNOWN and S_DATA_PUNCT — so composing with them
 * cannot put a tofu box in the middle of a row. */
static void set_example(lv_obj_t *label, const kanji_example_t *e)
{
    const bool reading = kanji_text_has_content(e->reading);
    const bool gloss   = kanji_text_has_content(e->gloss);

    if (reading && gloss) ui_setf(label, "%s  %s — %s", e->text, e->reading, e->gloss);
    else if (gloss)       ui_setf(label, "%s — %s", e->text, e->gloss);
    else if (reading)     ui_setf(label, "%s  %s", e->text, e->reading);
    else                  ui_set(label, e->text);
}

/* --- create --------------------------------------------------------------- */

lv_obj_t *ui_card_back_create(lv_obj_t *par)
{
    const kanji_back_layout_t *l = kanji_back_layout();

    /* The root is opaque white rather than transparent: it has to cover the pixels the other
     * face left in the framebuffer, and an e-Paper full refresh redraws what LVGL renders, not
     * what LVGL forgot to. Everything drawable then hangs off one child pane, so a session with
     * no card blanks the whole spread with a single ui_show() instead of thirty ui_set("")s
     * that a new widget can be added without. */
    b.root = ui_fill_white(par, 0, 0, UI_W, UI_H);
    b.body = ui_pane(b.root, 0, 0, UI_W, UI_H);

    /* --- masthead --- */
    b.hero    = ui_lab_headword(b.body, l->hero.x, l->hero.y, l->hero.w, l->hero.h,
                                UI_F_HERO, "");
    b.status  = ui_lab_r(b.body, l->status, UI_F_BODY, LV_TEXT_ALIGN_LEFT, "");
    b.due     = ui_lab_r(b.body, l->due, UI_F_BODY, LV_TEXT_ALIGN_RIGHT, "");
    /* The glance reading, RIGHT-aligned so it joins the level chip and the due span as one
     * block of metadata hanging off the right margin. Left-aligned it started at a fixed x
     * chosen for a five-character headword, which for a one-character headword like 語 left it
     * stranded in the middle of the masthead with white on both sides — attached to nothing.
     *
     * It ellipsizes, and that is not a loss: the rail below carries 음독 and 훈독 in full, so
     * this row's job is only to be read at the same moment as the headword. */
    b.reading = ui_lab_r(b.body, l->reading, UI_F_HEAD, LV_TEXT_ALIGN_RIGHT, "");

    /* The level chip, and the one inverted block on this face — until a grade is committed, at
     * which point the chosen dock cell inverts too. Two inverted blocks is normally how a page
     * ends up with neither being read first; that one moment is the exception, and it is
     * deliberate, because it is the board acknowledging a press.
     *
     * Its text is a child of the fill and centred by LVGL's own measurement, exactly as the
     * front's chip is. The chip is 26 px tall and the 16 px face's line box is 20, and those
     * 6 px are a fact about the font rather than about the grid: the layout does not know them,
     * should not carry them, and writing half of them here by hand would be a constant nothing
     * asserts on. Both faces build the chip the same way so it is the same chip. */
    b.badge_fill = ui_fill(b.body, l->badge.x, l->badge.y, l->badge.w, l->badge.h);
    b.badge      = ui_lab_inv(b.badge_fill, 0, 0, l->badge.w,
                              UI_F_BODY, LV_TEXT_ALIGN_CENTER, "");
    lv_obj_center(b.badge);

    /* 2 px, and the only band rule on the board — ui_rule() would refuse it, which is the point
     * of that helper: a hairline is 1 px by definition and anything else is a different weight
     * that has to be asked for on purpose. */
    ui_fill(b.body, l->band_rule.x, l->band_rule.y, l->band_rule.w, l->band_rule.h);
    /* The spine. It is drawn on every card, including one whose right rail is empty, because it
     * is what makes the page two columns rather than one column and some stragglers. */
    ui_rule(b.body, l->col_rule.x, l->col_rule.y, l->col_rule.w, l->col_rule.h);

    /* --- left column, the prose --- */
    b.sense_eyebrow = ui_eyebrow(b.body, l->sense_eyebrow, S_EB_MEANING);
    /* The one thing on this face read from across the room, and the only block set in the 28 px
     * face. Its rectangle is exactly two of that face's lines, so an overrun clips on a line
     * boundary rather than through the middle of a row of Hangul. */
    b.senses     = ui_lab_wrap_r(b.body, l->senses, UI_F_TITLE, LV_TEXT_ALIGN_LEFT, "");
    b.sense_rule = ui_rule(b.body, l->sense_rule.x, l->sense_rule.y,
                           l->sense_rule.w, l->sense_rule.h);

    b.build_eyebrow = ui_eyebrow(b.body, l->build_eyebrow, S_EB_BUILD);
    /* 상형 / 회의 / 형성 / 지사 / 가차 / 전주, right-aligned onto the eyebrow's own row. It is
     * tracked for the same reason the eyebrow beside it is: it is a two-character cut label
     * sharing that baseline, and left untracked it reads as a different weight sitting on the
     * heading line. Tracking stops here — the prose below it must never take it, because a CJK
     * glyph's own advance is the word spacing and adding to it takes a sentence apart. */
    b.principle = ui_lab_r(b.body, l->principle, UI_F_BODY, LV_TEXT_ALIGN_RIGHT, "");
    ui_track(b.principle, 2);
    b.build       = ui_lab_wrap_r(b.body, l->build, UI_F_BODY, LV_TEXT_ALIGN_LEFT, "");
    b.build_rule  = ui_rule(b.body, l->build_rule.x, l->build_rule.y,
                            l->build_rule.w, l->build_rule.h);

    b.example_eyebrow = ui_eyebrow(b.body, l->example_eyebrow, S_EB_EXAMPLE);
    for (int i = 0; i < KANJI_EXAMPLES_SHOWN; i++) {
        b.example[i] = ui_lab_r(b.body, l->example[i], UI_F_BODY, LV_TEXT_ALIGN_LEFT, "");
    }

    /* --- right rail, the figures --- */
    b.read_eyebrow = ui_eyebrow(b.body, l->read_eyebrow, S_EB_READING);
    /* 16 px, not 20. Two reasons, and the first is measurable: a kun list is ・-joined and
     * 「かたる · かたらう」 is nine glyphs, which at 20 px ellipsizes in this rail on a card as
     * ordinary as 語 — and a rail whose whole job is to carry the readings IN FULL, ellipsizing
     * the common case, is worse than one set a step smaller. The second is that everything else
     * in this column — the part glosses, the three figures — is 16 px, so 20 px here made the
     * readings look like a heading for the blocks beneath them rather than a block of their own. */
    b.on_label  = ui_lab_r(b.body, l->on_label,  UI_F_BODY, LV_TEXT_ALIGN_LEFT, S_ON_READING);
    b.on_value  = ui_lab_r(b.body, l->on_value,  UI_F_BODY, LV_TEXT_ALIGN_LEFT, "");
    b.kun_label = ui_lab_r(b.body, l->kun_label, UI_F_BODY, LV_TEXT_ALIGN_LEFT, S_KUN_READING);
    b.kun_value = ui_lab_r(b.body, l->kun_value, UI_F_BODY, LV_TEXT_ALIGN_LEFT, "");
    b.read_rule = ui_rule(b.body, l->read_rule.x, l->read_rule.y,
                          l->read_rule.w, l->read_rule.h);

    b.part_eyebrow = ui_eyebrow(b.body, l->part_eyebrow, S_EB_PARTS);
    for (int i = 0; i < KANJI_PARTS_SHOWN; i++) {
        b.part_glyph[i] = ui_lab_r(b.body, l->part_glyph[i], UI_F_HEAD,
                                   LV_TEXT_ALIGN_LEFT, "");
        b.part_gloss[i] = ui_lab_r(b.body, l->part_gloss[i], UI_F_BODY,
                                   LV_TEXT_ALIGN_LEFT, "");
    }
    b.part_rule = ui_rule(b.body, l->part_rule.x, l->part_rule.y,
                          l->part_rule.w, l->part_rule.h);

    b.stat_eyebrow = ui_eyebrow(b.body, l->stat_eyebrow, S_EB_MEMORY);
    /* 안정 is deliberately not among the three: at the backend's 0.9 desired retention an FSRS
     * interval is within a percent of the stability it came from, so the masthead's due span
     * already prints that number and a rail row would print the same fact twice. */
    static const char *const STAT[KANJI_STATS_SHOWN] = {
        S_STAT_REPS, S_STAT_LAPSES, S_STAT_DIFFICULTY,
    };
    for (int i = 0; i < KANJI_STATS_SHOWN; i++) {
        ui_lab_r(b.body, l->stat_label[i], UI_F_BODY, LV_TEXT_ALIGN_LEFT, STAT[i]);
        b.stat_value[i] = ui_lab_r(b.body, l->stat_value[i], UI_F_BODY,
                                   LV_TEXT_ALIGN_LEFT, "");
    }

    /* --- the dock --- */
    for (int i = 0; i < KANJI_BTN_COUNT; i++) {
        const kanji_grade_t g = kanji_button_grade((kanji_button_t)i);

        /* Frame and fill are the same rectangle and exactly one of them is ever shown. They are
         * created in this order, before the labels, because LVGL draws siblings in creation
         * order and a fill built after its labels would bury them. */
        b.cell_frame[i] = ui_frame(b.body, l->cell[i].x, l->cell[i].y,
                                   l->cell[i].w, l->cell[i].h, KANJI_RULE_HAIR);
        b.cell_fill[i]  = ui_fill(b.body, l->cell[i].x, l->cell[i].y,
                                  l->cell[i].w, l->cell[i].h);

        /* The key glyph, the rating, and what the rating would schedule. All three are fixed for
         * the life of the widget except the span, which is the card's. The cap takes the Latin
         * numeral face: 1 / 2 / 3 / i are what is silkscreened beside the buttons, and a run of
         * numerals set in a CJK face reads as three characters rather than three keys. */
        b.cell_key[i]  = ui_lab_r(b.body, l->cell_key[i], UI_F_UTILITY,
                                  LV_TEXT_ALIGN_CENTER, KEYCAP[i]);
        b.cell_name[i] = ui_lab_r(b.body, l->cell_name[i], UI_F_HEAD,
                                  LV_TEXT_ALIGN_LEFT, kanji_grade_label(g));
        b.cell_span[i] = ui_lab_r(b.body, l->cell_span[i], UI_F_BODY,
                                  LV_TEXT_ALIGN_LEFT, "");
    }

    /* Every widget on this face is visible by default, so the blank state has to be entered
     * rather than assumed: a face built and never updated would otherwise render its eyebrows
     * over an empty page for one refresh, and one refresh on e-Paper is several seconds. */
    ui_card_back_update(NULL, NULL, NULL);
    return b.root;
}

/* --- the dock ------------------------------------------------------------- */

/* Redraws the dock and NOTHING else, which is why the simulator XORs the whole frame against the
 * previous one and fails on a single changed pixel outside layout->dock. That rectangle goes
 * verbatim to epd_refresh_partial_area(), so a widget touched here and living outside it changes
 * the framebuffer in a strip the panel is never told to redraw: the board then shows a card that
 * is half stale, and nothing logs it. */
void ui_card_back_dock(const kanji_t *k, const kanji_nav_t *nav)
{
    const bool committed = nav && nav->committed;

    for (int i = 0; i < KANJI_BTN_COUNT; i++) {
        /* Button order, and the grade comes from the state machine rather than from `i`. The
         * two happen to run in the same direction today — 다시 / 어려움 / 보통 / 쉬움, left to
         * right, which is Anki's canonical order — but that is a fact about kanji_button_grade()
         * and the dock exists to print it, not to assume it. */
        const kanji_grade_t g = kanji_button_grade((kanji_button_t)i);
        const bool inked = committed && nav->grade == g;

        ui_show(b.cell_frame[i], !inked);
        ui_show(b.cell_fill[i], inked);
        ink(b.cell_key[i], inked);
        ink(b.cell_name[i], inked);
        ink(b.cell_span[i], inked);

        /* Empty is normal and common: the offline catalog carries no previews, and a cell with
         * no span under its rating is honest where an invented one would not be. */
        ui_set(b.cell_span[i], kanji_preview_span(k, g));
    }
}

/* --- update --------------------------------------------------------------- */

void ui_card_back_update(const kanji_t *k, const kanji_nav_t *nav, const ui_status_t *st)
{
    const bool have = k && k->valid && k->card.valid;

    ui_show(b.body, have);
    if (!have) {
        /* A finished session and a snapshot that never loaded are the same picture here: this
         * face is the answer to a card, and there is no card. The router puts the front up in
         * both cases; blanking rather than trusting it is what keeps a stale answer from
         * flashing past during the redraw that follows. */
        ui_card_back_dock(NULL, NULL);
        return;
    }

    const kanji_card_t *c = &k->card;

    /* --- masthead --- */
    /* Never picks the hero face by size: it is Japanese-only, so a short headword carrying one
     * character it happens not to have would be a tofu box at 56 px in the corner of the page. */
    ui_apply_headword(b.hero, c->front);
    ui_set(b.status, status_text(k, st));
    ui_set(b.reading, c->reading);

    /* 복습 · 9일 뒤, and 복습 alone when the card has no due span. The dangling separator is the
     * defect this guards: the board has no RTC and every span is worded by the proxy against the
     * server's clock, so an absent one is a real and ordinary state rather than a failure to
     * format. */
    if (kanji_text_has_content(c->fsrs.state_label) && kanji_text_has_content(c->fsrs.due)) {
        ui_setf(b.due, "%s · %s", c->fsrs.state_label, c->fsrs.due);
    } else if (kanji_text_has_content(c->fsrs.due)) {
        ui_set(b.due, c->fsrs.due);
    } else {
        ui_set(b.due, c->fsrs.state_label);
    }

    /* Hiding the chip hides its text with it — the label is the fill's child — so an unlevelled
     * card leaves paper here rather than a black rectangle with nothing written in it. */
    ui_set(b.badge, c->level);
    ui_show(b.badge_fill, kanji_text_has_content(c->level));

    /* --- left column --- */
    set_senses(b.senses, c);
    section(b.sense_eyebrow, b.sense_rule, c->sense_count > 0);
    ui_show(b.senses, c->sense_count > 0);

    /* hook_body first, and this preference is arithmetic rather than taste. The rectangle is
     * 384 px of the 16 px face — 24 characters a line, three lines, about 72 characters.
     * Measured over the catalog, hint.reason averages 54-62 characters on kanji cards and fits;
     * shape_explanation averages 111-137 and runs to 371 and cannot. Preferring the field that
     * fits means most cards print a whole thought, and the ones that fall back print the opening
     * of one, which is the shape of the story and the sentence that carries it. */
    const char *build_text = kanji_text_has_content(c->hook_body) ? c->hook_body
                                                                 : c->description;
    ui_set(b.build, build_text);
    ui_show(b.build, kanji_text_has_content(build_text));

    ui_set(b.principle, c->hook_title);
    /* Empty on every vocab card and present on all 989 kanji cards. */
    ui_show(b.principle, kanji_text_has_content(c->hook_title));

    section(b.build_eyebrow, b.build_rule,
            kanji_text_has_content(build_text) || kanji_text_has_content(c->hook_title));

    /* Two rows, and a card carrying three prints two and DROPS the third. Shrinking all three to
     * fit is how a page ends up with a size the scale has no job for, and an ellipsized stub of
     * a third example teaches nothing the first two did not. */
    for (int i = 0; i < KANJI_EXAMPLES_SHOWN; i++) {
        const bool on = i < c->example_count;
        if (on) set_example(b.example[i], &c->examples[i]);
        else    ui_set(b.example[i], "");
        ui_show(b.example[i], on);
    }
    section(b.example_eyebrow, NULL, c->example_count > 0);

    /* --- right rail --- */
    /* on_reading is empty on every vocab card, where the card's own reading IS the 음독 row's
     * answer; printing an empty 음독 beside a reading the masthead already ellipsized would be
     * hiding the full form the rail exists to carry. */
    const char *on = kanji_text_has_content(c->on_reading) ? c->on_reading : c->reading;
    const bool on_row  = kanji_text_has_content(on);
    const bool kun_row = kanji_text_has_content(c->kun_reading);

    ui_set(b.on_value, on);
    ui_show(b.on_value, on_row);
    ui_show(b.on_label, on_row);
    ui_set(b.kun_value, c->kun_reading);
    ui_show(b.kun_value, kun_row);
    ui_show(b.kun_label, kun_row);
    section(b.read_eyebrow, b.read_rule, on_row || kun_row);

    /* Three rows. A card carrying six components prints three and drops the rest, for the same
     * reason the examples do: six rows squeezed into a slot that holds three ellipsize into
     * stubs, and a rail of stubs is worse than a rail of three honest rows. */
    for (int i = 0; i < KANJI_PARTS_SHOWN; i++) {
        const bool on_part = i < c->part_count;
        ui_set(b.part_glyph[i], on_part ? c->parts[i].glyph : "");
        ui_set(b.part_gloss[i], on_part ? c->parts[i].meaning : "");
        ui_show(b.part_glyph[i], on_part);
        ui_show(b.part_gloss[i], on_part);
    }
    section(b.part_eyebrow, b.part_rule, c->part_count > 0);

    /* 반복 and 실패 are counts and print zero honestly — a card reviewed no times has been
     * reviewed no times, which is why this is the one section that never drops. 난이도 is the
     * one figure the scheduler may genuinely not have yet, and -1 is not zero: a card whose
     * difficulty is unknown and one that is trivially easy are different claims, and only one of
     * them has earned a number. */
    ui_setf(b.stat_value[0], "%d%s", c->fsrs.reps, S_UNIT_TIMES);
    ui_setf(b.stat_value[1], "%d%s", c->fsrs.lapses, S_UNIT_TIMES);
    if (c->fsrs.difficulty_pct < 0) ui_set(b.stat_value[2], S_VALUE_UNKNOWN);
    else                            ui_setf(b.stat_value[2], "%d%%", c->fsrs.difficulty_pct);

    ui_card_back_dock(k, nav);
}
