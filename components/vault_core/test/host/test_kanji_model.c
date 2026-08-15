/*
 * The snapshot's own arithmetic: UTF-8-safe truncation, character counting,
 * the grade vocabulary, and the fingerprint that decides whether the panel
 * refreshes at all.
 *
 * The fingerprint tests are the ones that matter. On a board that polls
 * forever, "the payload changed but the pixels did not" has to be cheap and
 * exact in both directions: miss a change and the glass lies, see a change
 * that is not there and the panel flashes at nobody all day.
 */
#include "th.h"

#include "kanji_model.h"

/* --- UTF-8-safe copy ------------------------------------------------------ */

static void test_copy_truncates_on_a_character_boundary(void)
{
    char dst[8];

    /* 会う is 3 + 3 bytes. Into 8 bytes both fit with the NUL. */
    CHECK_INT(kanji_str_copy(dst, sizeof dst, "会う"), 6);
    CHECK_STR(dst, "会う");

    /* Seven bytes is the exact fit: six of content plus the NUL. */
    char exact[7];
    CHECK_INT(kanji_str_copy(exact, sizeof exact, "会う"), 6);
    CHECK_STR(exact, "会う");

    /* Six is one short, so only the first character survives — and the second
     * must not be half-written. A lone lead byte is not "the word was long",
     * it is a tofu box, or LVGL's decoder walking off the end of the buffer. */
    char small[6];
    CHECK_INT(kanji_str_copy(small, sizeof small, "会う"), 3);
    CHECK_STR(small, "会");

    /* Every prefix length of a 3-byte character truncates to nothing rather
     * than to one or two orphaned bytes. */
    for (size_t n = 1; n <= 3; n++) {
        char tiny[4];
        CHECK_INT(kanji_str_copy(tiny, n, "会"), 0);
        CHECK_STR(tiny, "");
    }
}

static void test_copy_is_defensive_about_its_arguments(void)
{
    char dst[8] = "keep";
    CHECK_INT(kanji_str_copy(dst, sizeof dst, NULL), 0);
    CHECK_STR(dst, "");
    CHECK_INT(kanji_str_copy(NULL, 8, "x"), 0);
    CHECK_INT(kanji_str_copy(dst, 0, "x"), 0);
}

/* Display prose is a single visual paragraph even when the catalog formatted
 * it across lines. This catches a normalizer that leaves leading/trailing
 * space, splits words with tabs/newlines, or cuts a CJK glyph in half. */
static void test_display_prose_collapses_ascii_whitespace(void)
{
    char out[64];
    CHECK_INT(kanji_text_collapse_whitespace(
                  out, sizeof out, "  글자 \n\t 유래\r\n 입니다  "),
              strlen("글자 유래 입니다"));
    CHECK_STR(out, "글자 유래 입니다");
    CHECK(kanji_text_has_content(out));
    CHECK(!kanji_text_has_content(" \t\r\n\f\v "));
    CHECK(!kanji_text_has_content(NULL));
    CHECK_INT(kanji_text_collapse_whitespace(out, sizeof out, NULL), 0);
    CHECK_STR(out, "");
    CHECK_INT(kanji_text_collapse_whitespace(NULL, sizeof out, "text"), 0);
    CHECK_INT(kanji_text_collapse_whitespace(out, 0, "text"), 0);

    char exact[sizeof "会 う"];
    CHECK_INT(kanji_text_collapse_whitespace(exact, sizeof exact, "会 \tう"), 7);
    CHECK_STR(exact, "会 う");

    char short_dst[6] = { 'x', 'x', 'x', 'x', 'x', 'x' };
    CHECK_INT(kanji_text_collapse_whitespace(short_dst, sizeof short_dst, "会 う"), 3);
    CHECK_STR(short_dst, "会");
    CHECK_INT((unsigned char)short_dst[3], '\0');

    char inplace[32] = "  日本語\t한국어  ";
    CHECK_INT(kanji_text_collapse_whitespace(inplace, sizeof inplace, inplace),
              strlen("日本語 한국어"));
    CHECK_STR(inplace, "日本語 한국어");

    /* Leading whitespace shifts a multibyte source glyph left over its own
     * bytes. These one- and two-byte displacements must be overlap-safe. */
    char shifted_one[] = "\t会う";
    CHECK_INT(kanji_text_collapse_whitespace(shifted_one, sizeof shifted_one,
                                              shifted_one), strlen("会う"));
    CHECK_STR(shifted_one, "会う");

    char shifted_two[] = " \t한국";
    CHECK_INT(kanji_text_collapse_whitespace(shifted_two, sizeof shifted_two,
                                              shifted_two), strlen("한국"));
    CHECK_STR(shifted_two, "한국");

    char too_small[7] = { 'x', 'x', 'x', 'x', 'x', 'x', 'x' };
    CHECK_INT(kanji_text_collapse_whitespace(too_small, sizeof too_small,
                                              "日本語 한국어"), 6);
    CHECK_STR(too_small, "日本");
    CHECK_INT((unsigned char)too_small[6], '\0');
}

/* --- character counting --------------------------------------------------- */

static void test_utf8_len_counts_characters_not_bytes(void)
{
    CHECK_INT(kanji_utf8_len(""), 0);
    CHECK_INT(kanji_utf8_len(NULL), 0);
    CHECK_INT(kanji_utf8_len("N5"), 2);
    CHECK_INT(kanji_utf8_len("会う"), 2);          /* the hero picks 56 px */
    CHECK_INT(kanji_utf8_len("取り替える"), 5);
    CHECK_INT(kanji_utf8_len("만나다"), 3);
    /* A 4-byte character (an astral-plane component glyph) is still one. */
    CHECK_INT(kanji_utf8_len("\xF0\xA0\x82\x89"), 1);
}

/* There is exactly one answer on this board to "where does the next character
 * start", and it is this function.
 *
 * It used to be two: kanji_model.c counted characters to decide whether the
 * headword FITS the 56 px hero face, and ui_common.c walked the same string to
 * decide whether the hero face can DRAW it. Both halves decide one thing —
 * which face the headword gets — so two decoders that disagreed by a byte would
 * have one half reading a character the other half never saw, and the symptom
 * is a tofu box at 56 px in the middle of the card. */
static void test_one_decoder_decides_where_a_character_starts(void)
{
    CHECK_INT(kanji_utf8_seq_len('N'), 1);
    CHECK_INT(kanji_utf8_seq_len(0x00), 1);
    CHECK_INT(kanji_utf8_seq_len(0x7F), 1);
    CHECK_INT(kanji_utf8_seq_len(0xC3), 2);        /* é         */
    CHECK_INT(kanji_utf8_seq_len(0xE4), 3);        /* 会        */
    CHECK_INT(kanji_utf8_seq_len(0xEA), 3);        /* 만        */
    CHECK_INT(kanji_utf8_seq_len(0xF0), 4);        /* astral    */

    /* A byte that cannot start a sequence advances by one rather than by zero.
     * Zero would loop forever on the first bad byte off the network. */
    CHECK_INT(kanji_utf8_seq_len(0x80), 1);        /* lone continuation */
    CHECK_INT(kanji_utf8_seq_len(0xBF), 1);
    CHECK_INT(kanji_utf8_seq_len(0xF8), 1);
    CHECK_INT(kanji_utf8_seq_len(0xFF), 1);

    /* Every byte value has an answer, and it is always a usable stride. */
    for (int c = 0; c <= 0xFF; c++) {
        const size_t n = kanji_utf8_seq_len((unsigned char)c);
        CHECK(n >= 1 && n <= 4);
    }

    /* And it is the same walk kanji_utf8_len() does. */
    const char *s = "会うN만\xF0\xA0\x82\x89";
    size_t at = 0;
    int steps = 0;
    while (s[at]) { at += kanji_utf8_seq_len((unsigned char)s[at]); steps++; }
    CHECK_INT(steps, kanji_utf8_len(s));
    CHECK_INT(at, strlen(s));
}

/* --- the grade vocabulary ------------------------------------------------- */

static void test_grade_words_match_the_backend_and_the_dock(void)
{
    CHECK_INT(KANJI_GRADE_AGAIN, 1);
    CHECK_INT(KANJI_GRADE_EASY, 4);

    CHECK_STR(kanji_grade_name(KANJI_GRADE_AGAIN), "AGAIN");
    CHECK_STR(kanji_grade_name(KANJI_GRADE_HARD), "HARD");
    CHECK_STR(kanji_grade_name(KANJI_GRADE_GOOD), "GOOD");
    CHECK_STR(kanji_grade_name(KANJI_GRADE_EASY), "EASY");

    CHECK_STR(kanji_grade_wire(KANJI_GRADE_AGAIN), "again");
    CHECK_STR(kanji_grade_wire(KANJI_GRADE_EASY), "easy");

    CHECK_STR(kanji_grade_label(KANJI_GRADE_AGAIN), "다시");
    CHECK_STR(kanji_grade_label(KANJI_GRADE_HARD), "어려움");
    CHECK_STR(kanji_grade_label(KANJI_GRADE_GOOD), "보통");
    CHECK_STR(kanji_grade_label(KANJI_GRADE_EASY), "쉬움");

    /* Never NULL, whatever the caller passes. A dock that prints "(null)"
     * beside a rating is worse than one that prints nothing. */
    CHECK(kanji_grade_name((kanji_grade_t)0) != NULL);
    CHECK(kanji_grade_wire((kanji_grade_t)99) != NULL);
    CHECK(kanji_grade_label((kanji_grade_t)-1) != NULL);
}

static void test_preview_span_is_indexed_by_grade(void)
{
    kanji_t k = {0};
    kanji_str_copy(k.card.preview.span[0], KANJI_LABEL_MAX, "10분 뒤");
    kanji_str_copy(k.card.preview.span[3], KANJI_LABEL_MAX, "21일 뒤");

    CHECK_STR(kanji_preview_span(&k, KANJI_GRADE_AGAIN), "10분 뒤");
    CHECK_STR(kanji_preview_span(&k, KANJI_GRADE_HARD), "");
    CHECK_STR(kanji_preview_span(&k, KANJI_GRADE_EASY), "21일 뒤");

    CHECK_STR(kanji_preview_span(&k, (kanji_grade_t)0), "");
    CHECK_STR(kanji_preview_span(NULL, KANJI_GRADE_GOOD), "");
}

/* --- the fingerprint ------------------------------------------------------ */

static kanji_t sample(void)
{
    kanji_t k = {0};
    k.valid = true;
    k.source = KANJI_SOURCE_REMOTE;
    kanji_str_copy(k.session.deck, KANJI_DECK_MAX, "JLPT N5 Vocabulary");
    k.session.streak = 12;
    k.session.track = 35;
    k.session.track_total = 60;

    k.card.valid = true;
    kanji_str_copy(k.card.id, KANJI_ID_MAX, "f00c539e");
    kanji_str_copy(k.card.front, KANJI_FRONT_MAX, "会う");
    kanji_str_copy(k.card.reading, KANJI_READING_MAX, "あう");
    kanji_str_copy(k.card.gloss, KANJI_SENSE_MAX, "만날 회");
    kanji_str_copy(k.card.on_reading, KANJI_READING_MAX, "カイ");
    kanji_str_copy(k.card.kun_reading, KANJI_READING_MAX, "あう");
    kanji_str_copy(k.card.composition, KANJI_FORMULA_MAX, "人 + 云 = 会");
    kanji_str_copy(k.card.senses[0], KANJI_SENSE_MAX, "만나다");
    k.card.sense_count = 1;
    kanji_str_copy(k.card.fsrs.state_label, KANJI_LABEL_MAX, "복습");
    k.card.fsrs.reps = 5;
    kanji_str_copy(k.card.preview.span[2], KANJI_LABEL_MAX, "9일 뒤");
    return k;
}

static void test_full_fidelity_model_limits_are_public(void)
{
    CHECK_INT(KANJI_READING_MAX, 144);
    CHECK_INT(KANJI_SENSE_MAX, 144);
    CHECK_INT(KANJI_BODY_MAX, 832);
    CHECK_INT(KANJI_FORMULA_MAX, 96);
    CHECK_INT(KANJI_SENSES_MAX, 5);
    CHECK_INT(KANJI_PARTS_MAX, 6);
}

static void test_hash_is_stable_and_order_independent_of_the_call(void)
{
    kanji_t a = sample(), b = sample();
    CHECK_INT(kanji_hash(&a), kanji_hash(&b));
    CHECK_INT(kanji_hash(&a), kanji_hash(&a));
    CHECK_INT(kanji_hash(NULL), 0);
}

static void test_hash_changes_for_everything_that_reaches_the_glass(void)
{
    const uint32_t base = kanji_hash(&(kanji_t){0}) + 0; /* silence unused */
    (void)base;

    struct { const char *what; kanji_t k; } cases[13];
    int n = 0;

    cases[n].what = "the headword";
    cases[n].k = sample();
    kanji_str_copy(cases[n].k.card.front, KANJI_FRONT_MAX, "合う");
    n++;

    cases[n].what = "a sense";
    cases[n].k = sample();
    kanji_str_copy(cases[n].k.card.senses[0], KANJI_SENSE_MAX, "마주치다");
    n++;

    cases[n].what = "the FSRS state label";
    cases[n].k = sample();
    kanji_str_copy(cases[n].k.card.fsrs.state_label, KANJI_LABEL_MAX, "학습 중");
    n++;

    cases[n].what = "a rating preview";
    cases[n].k = sample();
    kanji_str_copy(cases[n].k.card.preview.span[2], KANJI_LABEL_MAX, "12일 뒤");
    n++;

    cases[n].what = "the streak";
    cases[n].k = sample();
    cases[n].k.session.streak = 13;
    n++;

    cases[n].what = "the queue position";
    cases[n].k = sample();
    cases[n].k.session.track = 36;
    n++;

    cases[n].what = "the demo flag";
    cases[n].k = sample();
    cases[n].k.demo = true;
    n++;

    cases[n].what = "the review count";
    cases[n].k = sample();
    cases[n].k.card.fsrs.reps = 6;
    n++;

    cases[n].what = "the short gloss";
    cases[n].k = sample();
    kanji_str_copy(cases[n].k.card.gloss, KANJI_SENSE_MAX, "합할 회");
    n++;

    cases[n].what = "the on reading";
    cases[n].k = sample();
    kanji_str_copy(cases[n].k.card.on_reading, KANJI_READING_MAX, "エ");
    n++;

    cases[n].what = "the kun reading";
    cases[n].k = sample();
    kanji_str_copy(cases[n].k.card.kun_reading, KANJI_READING_MAX, "あえる");
    n++;

    cases[n].what = "the composition equation";
    cases[n].k = sample();
    kanji_str_copy(cases[n].k.card.composition, KANJI_FORMULA_MAX, "人 + 口 = 会");
    n++;

    cases[n].what = "the card source";
    cases[n].k = sample();
    cases[n].k.source = KANJI_SOURCE_CATALOG;
    n++;

    kanji_t base_k = sample();
    const uint32_t h0 = kanji_hash(&base_k);
    for (int i = 0; i < n; i++) {
        const uint32_t h = kanji_hash(&cases[i].k);
        if (h == h0) {
            printf("  FAIL hash ignores %s\n", cases[i].what);
            g_fail++;
        }
        g_total++;
    }
}

static void test_hash_ignores_what_never_reaches_the_glass(void)
{
    kanji_t a = sample(), b = sample();

    /* The card id is routing, not pixels: the same card re-served under a new
     * study_card_id must not flash the panel. */
    kanji_str_copy(b.card.id, KANJI_ID_MAX, "0000ffff-different");
    CHECK_INT(kanji_hash(&a), kanji_hash(&b));
}

int main(void)
{
    test_full_fidelity_model_limits_are_public();
    test_copy_truncates_on_a_character_boundary();
    test_copy_is_defensive_about_its_arguments();
    test_display_prose_collapses_ascii_whitespace();
    test_utf8_len_counts_characters_not_bytes();
    test_one_decoder_decides_where_a_character_starts();
    test_grade_words_match_the_backend_and_the_dock();
    test_preview_span_is_indexed_by_grade();
    test_hash_is_stable_and_order_independent_of_the_call();
    test_hash_changes_for_everything_that_reaches_the_glass();
    test_hash_ignores_what_never_reaches_the_glass();
    TH_REPORT("kanji_model");
}
