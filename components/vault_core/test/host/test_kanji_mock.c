/*
 * Host unit tests tying the built-in demo card to the wire contract.
 *
 * kanji_mock.c is what an unconfigured board shows; tools/mock_kanji_server.py
 * is the reference producer, and fixtures/kanji.json is its committed output.
 * Those are two hand-written descriptions of the same card, in two languages,
 * and they will drift the first time somebody edits one of them.
 *
 * So the main test here is: parse the fixture, and assert it fingerprints
 * identically to the C snapshot. If someone changes the demo card and forgets
 * the server (or vice versa), this fails with the diverging field named rather
 * than showing up as a screenshot that no longer matches the docs.
 *
 * The rest of the file checks that the demo card is internally legal — it is,
 * after all, the one snapshot that never goes through the parser's clamping, so
 * nothing else would catch an out-of-range value in it.
 */
#include "th.h"

#include "kanji_mock.h"
#include "kanji_model.h"
#include "kanji_parse.h"

static void test_mock_matches_the_wire_fixture(void)
{
    size_t len = 0;
    char *json = th_slurp(FIXDIR "/kanji.json", &len);

    kanji_t wire;
    bool parsed = kanji_parse(json, len, &wire);
    CHECK(parsed == true);
    free(json);
    if (!parsed) return;

    kanji_t mock;
    kanji_mock(&mock);

    /* The one field that legitimately differs: `demo` badges the header so a
     * learner can tell a built-in card from their own queue. Normalize it and
     * everything else must match exactly. */
    CHECK(mock.demo == true);
    CHECK(wire.demo == false);
    mock.demo = false;

    if (kanji_hash(&mock) != kanji_hash(&wire)) {
        g_total++; g_fail++;
        printf("  FAIL kanji_mock.c and tools/mock_kanji_server.py have diverged\n");
        /* Narrow it down for whoever has to fix it, rather than leaving them to
         * diff a C file against a Python one by eye. */
        CHECK_STR(mock.session.deck, wire.session.deck);
        CHECK_STR(mock.session.level, wire.session.level);
        CHECK_INT(mock.session.streak, wire.session.streak);
        CHECK_INT(mock.session.reviewed_today, wire.session.reviewed_today);
        CHECK_INT(mock.session.left_new, wire.session.left_new);
        CHECK_INT(mock.session.left_review, wire.session.left_review);
        CHECK_INT(mock.session.retry, wire.session.retry);
        CHECK_INT(mock.session.track, wire.session.track);
        CHECK_INT(mock.session.track_total, wire.session.track_total);
        CHECK_INT(mock.session.complete, wire.session.complete);

        CHECK_STR(mock.card.front, wire.card.front);
        CHECK_STR(mock.card.reading, wire.card.reading);
        CHECK_STR(mock.card.level, wire.card.level);
        CHECK_INT(mock.card.sense_count, wire.card.sense_count);
        for (int i = 0; i < mock.card.sense_count &&
                        i < wire.card.sense_count; i++) {
            CHECK_STR(mock.card.senses[i], wire.card.senses[i]);
        }
        CHECK_INT(mock.card.example_count, wire.card.example_count);
        for (int i = 0; i < mock.card.example_count &&
                        i < wire.card.example_count; i++) {
            CHECK_STR(mock.card.examples[i].text, wire.card.examples[i].text);
            CHECK_STR(mock.card.examples[i].reading, wire.card.examples[i].reading);
            CHECK_STR(mock.card.examples[i].gloss, wire.card.examples[i].gloss);
        }
        CHECK_STR(mock.card.description, wire.card.description);
        CHECK_STR(mock.card.hook_title, wire.card.hook_title);
        CHECK_STR(mock.card.hook_body, wire.card.hook_body);
        CHECK_INT(mock.card.part_count, wire.card.part_count);
        for (int i = 0; i < mock.card.part_count &&
                        i < wire.card.part_count; i++) {
            CHECK_STR(mock.card.parts[i].glyph, wire.card.parts[i].glyph);
            CHECK_STR(mock.card.parts[i].meaning, wire.card.parts[i].meaning);
            CHECK_STR(mock.card.parts[i].reading, wire.card.parts[i].reading);
        }
        CHECK_INT(mock.card.comment_count, wire.card.comment_count);
        for (int i = 0; i < mock.card.comment_count &&
                        i < wire.card.comment_count; i++) {
            CHECK_STR(mock.card.comments[i].author, wire.card.comments[i].author);
            CHECK_STR(mock.card.comments[i].body, wire.card.comments[i].body);
            CHECK_INT(mock.card.comments[i].likes, wire.card.comments[i].likes);
        }
        CHECK_INT(mock.card.comment_total, wire.card.comment_total);
        CHECK_STR(mock.card.fsrs.state, wire.card.fsrs.state);
        CHECK_STR(mock.card.fsrs.state_label, wire.card.fsrs.state_label);
        CHECK_STR(mock.card.fsrs.due, wire.card.fsrs.due);
        CHECK_INT(mock.card.fsrs.reps, wire.card.fsrs.reps);
        CHECK_INT(mock.card.fsrs.lapses, wire.card.fsrs.lapses);
        CHECK_INT(mock.card.fsrs.stability_days, wire.card.fsrs.stability_days);
        CHECK_INT(mock.card.fsrs.difficulty_pct, wire.card.fsrs.difficulty_pct);
        for (int g = KANJI_GRADE_AGAIN; g <= KANJI_GRADE_EASY; g++) {
            CHECK_STR(kanji_preview_span(&mock, (kanji_grade_t)g),
                      kanji_preview_span(&wire, (kanji_grade_t)g));
        }
    } else {
        g_total++;      /* one passing check for the hash comparison itself */
    }
}

/* The demo card never goes through the parser, so its own legality is only
 * checked here. Everything the UI indexes by a count must be within bounds. */
static void test_the_demo_card_is_internally_legal(void)
{
    kanji_t k;
    kanji_mock(&k);

    CHECK(k.valid);
    CHECK(k.demo);
    CHECK(k.card.valid);

    CHECK(k.card.sense_count >= 0 && k.card.sense_count <= KANJI_SENSES_MAX);
    CHECK(k.card.example_count >= 0 && k.card.example_count <= KANJI_EXAMPLES_MAX);
    CHECK(k.card.part_count >= 0 && k.card.part_count <= KANJI_PARTS_MAX);
    CHECK(k.card.comment_count >= 0 && k.card.comment_count <= KANJI_COMMENTS_MAX);
    CHECK(k.card.comment_total >= k.card.comment_count);

    CHECK(k.session.track >= 0);
    CHECK(k.session.track <= k.session.track_total);
    CHECK(k.card.fsrs.difficulty_pct <= 100);

    /* Every string is NUL-terminated inside its buffer — kanji_str_copy
     * guarantees it, but the mock is where a hand-written literal would land. */
    CHECK(strlen(k.card.front) < KANJI_FRONT_MAX);
    CHECK(strlen(k.card.reading) < KANJI_READING_MAX);
    CHECK(strlen(k.card.description) < KANJI_BODY_MAX);
    CHECK(strlen(k.card.hook_body) < KANJI_BODY_MAX);
}

/* The demo card is also the layout's reference specimen: it is what the
 * simulator renders by default and what the docs screenshot. If it stopped
 * exercising a widget, that widget would stop being checked by anything. */
static void test_the_demo_card_exercises_every_widget(void)
{
    kanji_t k;
    kanji_mock(&k);

    CHECK(k.card.front[0] != '\0');
    CHECK(k.card.reading[0] != '\0');
    CHECK(k.card.level[0] != '\0');
    CHECK(k.card.sense_count >= 2);          /* the joined-sense line     */
    CHECK(k.card.example_count >= 2);        /* more than one 예문 row    */
    CHECK(k.card.description[0] != '\0');    /* 글자의 유래               */
    CHECK(k.card.hook_body[0] != '\0');      /* 기억 힌트                 */
    CHECK(k.card.part_count >= 1);           /* 구성 요소                 */
    CHECK(k.card.comment_count >= 2);        /* a full comments page      */
    CHECK(k.card.comment_total > k.card.comment_count);  /* the "외 N" case */
    CHECK(k.card.fsrs.state_label[0] != '\0');
    CHECK(k.card.fsrs.stability_days >= 0);
    CHECK(k.card.fsrs.difficulty_pct >= 0);
    CHECK(k.card.fsrs.reps > 0);
    CHECK(k.card.fsrs.lapses > 0);           /* the "(2)" in the reps cell */
    for (int g = KANJI_GRADE_AGAIN; g <= KANJI_GRADE_EASY; g++) {
        CHECK(kanji_preview_span(&k, (kanji_grade_t)g)[0] != '\0');
    }
    CHECK(k.session.track_total > 0);        /* the scrubber has a length  */
    CHECK(k.session.track > 0);
}

int main(void)
{
    test_mock_matches_the_wire_fixture();
    test_the_demo_card_is_internally_legal();
    test_the_demo_card_exercises_every_widget();
    TH_REPORT("kanji_mock");
}
