/*
 * The wire format. Everything the producer can do wrong lands here.
 *
 * The producer is a Python script on somebody's laptop talking to a REST API
 * over the open internet. It will send a float where an int belongs, a null
 * where a string belongs, an empty array, a 900-entry array, a truncated body
 * the moment the laptop sleeps, and — because half the strings are Japanese
 * arriving from a database — a multi-byte character cut in half.
 *
 * None of that may take the board down, and none of it may leave a half-written
 * card on the glass.
 */
#include "th.h"

#include "kanji_parse.h"

static bool parse(const char *json, kanji_t *out)
{
    return kanji_parse(json, strlen(json), out);
}

/* A payload with every documented field populated. Kept as one string so the
 * tests below can say "like GOOD but with X wrong" by parsing a variant. */
static const char *GOOD =
"{"
"  \"v\": 1,"
"  \"session\": {"
"    \"deck\": \"JLPT N5 Vocabulary\","
"    \"level\": \"N5\","
"    \"streak\": 12,"
"    \"reviewed_today\": 34,"
"    \"left_new\": 7,"
"    \"left_review\": 18,"
"    \"retry\": 2,"
"    \"track\": 35,"
"    \"track_total\": 60,"
"    \"complete\": false"
"  },"
"  \"card\": {"
"    \"id\": \"f00c539e-23f9-4294-bee1-c642189b105f\","
"    \"front\": \"会う\","
"    \"reading\": \"あう\","
"    \"level\": \"N5\","
"    \"senses\": [\"만나다\", \"대면하다\", \"우연히 만나다\"],"
"    \"examples\": ["
"      { \"text\": \"出会う\", \"reading\": \"であう\", \"gloss\": \"우연히 만나다\" },"
"      { \"text\": \"出会い\", \"reading\": \"であい\", \"gloss\": \"만남\" }"
"    ],"
"    \"description\": \"会는 사람들이 모여 교류하는 모습입니다.\","
"    \"hook_title\": \"기억 힌트\","
"    \"hook_body\": \"위는 모임을, 아래는 말함을 나타냅니다.\","
"    \"parts\": [ { \"glyph\": \"会\", \"meaning\": \"모이다\", \"reading\": \"あう\" } ],"
"    \"comments\": ["
"      { \"author\": \"카나 선생\", \"body\": \"사람을 만날 때 씁니다.\", \"likes\": 12 }"
"    ],"
"    \"comment_total\": 12,"
"    \"fsrs\": {"
"      \"state\": \"review\","
"      \"state_label\": \"복습\","
"      \"due\": \"9일 뒤\","
"      \"reps\": 5,"
"      \"lapses\": 1,"
"      \"stability_days\": 9,"
"      \"difficulty_pct\": 47"
"    },"
"    \"preview\": {"
"      \"again\": \"10분 뒤\", \"hard\": \"4일 뒤\","
"      \"good\": \"9일 뒤\", \"easy\": \"21일 뒤\""
"    }"
"  }"
"}";

/* --- the happy path ------------------------------------------------------- */

static void test_a_full_payload_lands_field_for_field(void)
{
    kanji_t k;
    memset(&k, 0xAA, sizeof k);
    CHECK(parse(GOOD, &k));

    CHECK(k.valid);
    CHECK(!k.demo);

    CHECK_STR(k.session.deck, "JLPT N5 Vocabulary");
    CHECK_STR(k.session.level, "N5");
    CHECK_INT(k.session.streak, 12);
    CHECK_INT(k.session.reviewed_today, 34);
    CHECK_INT(k.session.left_new, 7);
    CHECK_INT(k.session.left_review, 18);
    CHECK_INT(k.session.retry, 2);
    CHECK_INT(k.session.track, 35);
    CHECK_INT(k.session.track_total, 60);
    CHECK(!k.session.complete);

    CHECK(k.card.valid);
    CHECK_STR(k.card.id, "f00c539e-23f9-4294-bee1-c642189b105f");
    CHECK_STR(k.card.front, "会う");
    CHECK_STR(k.card.reading, "あう");
    CHECK_STR(k.card.level, "N5");

    CHECK_INT(k.card.sense_count, 3);
    CHECK_STR(k.card.senses[0], "만나다");
    CHECK_STR(k.card.senses[2], "우연히 만나다");

    CHECK_INT(k.card.example_count, 2);
    CHECK_STR(k.card.examples[0].text, "出会う");
    CHECK_STR(k.card.examples[0].reading, "であう");
    CHECK_STR(k.card.examples[0].gloss, "우연히 만나다");
    CHECK_STR(k.card.examples[1].text, "出会い");

    CHECK_STR(k.card.description, "会는 사람들이 모여 교류하는 모습입니다.");
    CHECK_STR(k.card.hook_title, "기억 힌트");
    CHECK_STR(k.card.hook_body, "위는 모임을, 아래는 말함을 나타냅니다.");

    CHECK_INT(k.card.part_count, 1);
    CHECK_STR(k.card.parts[0].glyph, "会");
    CHECK_STR(k.card.parts[0].meaning, "모이다");
    CHECK_STR(k.card.parts[0].reading, "あう");

    CHECK_INT(k.card.comment_count, 1);
    CHECK_STR(k.card.comments[0].author, "카나 선생");
    CHECK_STR(k.card.comments[0].body, "사람을 만날 때 씁니다.");
    CHECK_INT(k.card.comments[0].likes, 12);
    CHECK_INT(k.card.comment_total, 12);

    CHECK_STR(k.card.fsrs.state, "review");
    CHECK_STR(k.card.fsrs.state_label, "복습");
    CHECK_STR(k.card.fsrs.due, "9일 뒤");
    CHECK_INT(k.card.fsrs.reps, 5);
    CHECK_INT(k.card.fsrs.lapses, 1);
    CHECK_INT(k.card.fsrs.stability_days, 9);
    CHECK_INT(k.card.fsrs.difficulty_pct, 47);

    CHECK_STR(kanji_preview_span(&k, KANJI_GRADE_AGAIN), "10분 뒤");
    CHECK_STR(kanji_preview_span(&k, KANJI_GRADE_HARD), "4일 뒤");
    CHECK_STR(kanji_preview_span(&k, KANJI_GRADE_GOOD), "9일 뒤");
    CHECK_STR(kanji_preview_span(&k, KANJI_GRADE_EASY), "21일 뒤");
}

/* --- refusal leaves the glass alone --------------------------------------- */

static void test_a_rejected_payload_does_not_touch_the_caller(void)
{
    kanji_t good;
    CHECK(parse(GOOD, &good));

    static const char *junk[] = {
        "",
        "   ",
        "not json at all",
        "[]",                                  /* an array, not an object   */
        "\"a string\"",
        "42",
        "null",
        "{",                                   /* truncated                 */
        "{\"session\":{\"deck\":\"x\"",         /* truncated mid-object      */
        "{}",                                  /* neither session nor card  */
        "{\"v\":1}",                           /* an envelope with nothing  */
        "{\"detail\":\"Not authenticated\"}",  /* what the API sends on 401 */
    };

    for (size_t i = 0; i < sizeof junk / sizeof junk[0]; i++) {
        kanji_t k = good;
        if (kanji_parse(junk[i], strlen(junk[i]), &k)) {
            printf("  FAIL accepted junk: %s\n", junk[i]);
            g_fail++;
        }
        g_total++;
        /* Not merely "returned false" — the struct must be BYTE-identical.
         * A parser that half-writes on the way to failing is the one bug that
         * blanks somebody's panel. */
        CHECK_INT(kanji_hash(&k), kanji_hash(&good));
        CHECK_STR(k.card.front, "会う");
    }
}

static void test_a_truncated_multibyte_tail_is_refused_not_half_copied(void)
{
    kanji_t good;
    CHECK(parse(GOOD, &good));

    /* The laptop slept mid-response: the body ends inside a 3-byte kanji. */
    const size_t n = strlen(GOOD);
    for (size_t cut = 1; cut < 3; cut++) {
        kanji_t k = good;
        CHECK(!kanji_parse(GOOD, n - cut, &k));
        CHECK_INT(kanji_hash(&k), kanji_hash(&good));
    }

    /* And a body whose bytes are not valid UTF-8 at all. */
    kanji_t k = good;
    CHECK(!parse("{\"session\":{\"deck\":\"\xC3\x28\"}}", &k));
    CHECK_INT(kanji_hash(&k), kanji_hash(&good));
}

/* --- a session with no card is a success ---------------------------------- */

static void test_a_finished_session_parses_without_a_card(void)
{
    kanji_t k;
    memset(&k, 0xAA, sizeof k);
    CHECK(parse("{\"v\":1,\"session\":{\"deck\":\"N5\",\"streak\":3,"
                "\"complete\":true},\"card\":null}", &k));
    CHECK(k.valid);
    CHECK(!k.card.valid);
    CHECK(k.session.complete);
    CHECK_INT(k.session.streak, 3);
    CHECK_STR(k.card.front, "");

    /* A missing `card` key means the same thing as an explicit null. */
    kanji_t k2;
    memset(&k2, 0xAA, sizeof k2);
    CHECK(parse("{\"v\":1,\"session\":{\"deck\":\"N5\",\"complete\":true}}", &k2));
    CHECK(k2.valid);
    CHECK(!k2.card.valid);
}

/* A card with no headword is not a card. Drawing an empty hero over a real
 * session is worse than showing the completion screen. */
static void test_a_card_without_a_headword_is_not_a_card(void)
{
    kanji_t k;
    memset(&k, 0xAA, sizeof k);
    CHECK(parse("{\"session\":{\"deck\":\"N5\"},\"card\":{\"front\":\"\","
                "\"senses\":[\"뜻\"]}}", &k));
    CHECK(k.valid);
    CHECK(!k.card.valid);
}

/* --- clamping ------------------------------------------------------------- */

static void test_arrays_take_their_first_n_and_drop_the_rest(void)
{
    kanji_t k;
    CHECK(parse("{\"session\":{\"deck\":\"d\"},\"card\":{\"front\":\"会\","
                "\"senses\":[\"1\",\"2\",\"3\",\"4\",\"5\"],"
                "\"examples\":[{\"text\":\"a\"},{\"text\":\"b\"},{\"text\":\"c\"},"
                "{\"text\":\"d\"}],"
                "\"parts\":[{\"glyph\":\"1\"},{\"glyph\":\"2\"},{\"glyph\":\"3\"},"
                "{\"glyph\":\"4\"}],"
                "\"comments\":[{\"body\":\"a\"},{\"body\":\"b\"},{\"body\":\"c\"},"
                "{\"body\":\"d\"}]}}", &k));

    CHECK_INT(k.card.sense_count, KANJI_SENSES_MAX);
    CHECK_STR(k.card.senses[KANJI_SENSES_MAX - 1], "3");
    CHECK_INT(k.card.example_count, KANJI_EXAMPLES_MAX);
    CHECK_STR(k.card.examples[KANJI_EXAMPLES_MAX - 1].text, "c");
    CHECK_INT(k.card.part_count, KANJI_PARTS_MAX);
    CHECK_INT(k.card.comment_count, KANJI_COMMENTS_MAX);
}

static void test_an_overlong_string_loses_its_tail_on_a_character_boundary(void)
{
    /* 40 Hangul syllables = 120 bytes into a 48-byte sense. */
    const char *long_sense =
        "가나다라마바사아자차카타파하가나다라마바사아자차카타파하가나다라마바사아자차카타파하";

    char json[512];
    snprintf(json, sizeof json,
             "{\"session\":{\"deck\":\"d\"},\"card\":{\"front\":\"会\","
             "\"senses\":[\"%s\"]}}", long_sense);

    kanji_t k;
    CHECK(parse(json, &k));
    CHECK_INT(k.card.sense_count, 1);
    CHECK(strlen(k.card.senses[0]) < KANJI_SENSE_MAX);
    CHECK_INT(strlen(k.card.senses[0]) % 3, 0);   /* whole syllables only */
    CHECK(strncmp(k.card.senses[0], long_sense, strlen(k.card.senses[0])) == 0);
}

static void test_counters_are_non_negative_and_bounded(void)
{
    kanji_t k;
    CHECK(parse("{\"session\":{\"deck\":\"d\",\"streak\":-5,"
                "\"reviewed_today\":999999999,\"left_new\":-1,"
                "\"track\":99,\"track_total\":10},"
                "\"card\":{\"front\":\"会\",\"comment_total\":-3,"
                "\"fsrs\":{\"reps\":-2,\"lapses\":-9,"
                "\"difficulty_pct\":420}}}", &k));

    CHECK_INT(k.session.streak, 0);
    CHECK(k.session.reviewed_today <= 9999);
    CHECK_INT(k.session.left_new, 0);
    CHECK_INT(k.card.fsrs.reps, 0);
    CHECK_INT(k.card.fsrs.lapses, 0);
    CHECK_INT(k.card.comment_total, 0);
    CHECK(k.card.fsrs.difficulty_pct <= 100);

    /* A queue position past its own total is a producer bug that would print
     * "TRK 99/10" on the glass. */
    CHECK(k.session.track <= k.session.track_total);
}

/* -1 and 0 are different answers and the sheet prints them differently: a new
 * card has no stability, a same-day card has one that rounds to zero. */
static void test_an_unscheduled_card_keeps_its_minus_one(void)
{
    kanji_t k;
    CHECK(parse("{\"session\":{\"deck\":\"d\"},\"card\":{\"front\":\"会\","
                "\"fsrs\":{\"state\":\"new\",\"stability_days\":-1,"
                "\"difficulty_pct\":-1}}}", &k));
    CHECK_INT(k.card.fsrs.stability_days, -1);
    CHECK_INT(k.card.fsrs.difficulty_pct, -1);

    /* Absent means the same as -1, not zero. */
    kanji_t k2;
    CHECK(parse("{\"session\":{\"deck\":\"d\"},\"card\":{\"front\":\"会\"}}", &k2));
    CHECK_INT(k2.card.fsrs.stability_days, -1);
    CHECK_INT(k2.card.fsrs.difficulty_pct, -1);

    /* But an explicit zero survives as zero. */
    kanji_t k3;
    CHECK(parse("{\"session\":{\"deck\":\"d\"},\"card\":{\"front\":\"会\","
                "\"fsrs\":{\"stability_days\":0}}}", &k3));
    CHECK_INT(k3.card.fsrs.stability_days, 0);
}

static void test_a_wrong_type_takes_the_default_and_keeps_the_rest(void)
{
    kanji_t k;
    CHECK(parse("{\"session\":{\"deck\":123,\"streak\":\"twelve\","
                "\"complete\":\"yes\"},"
                "\"card\":{\"front\":\"会う\",\"senses\":\"만나다\","
                "\"examples\":{},\"comments\":42,"
                "\"fsrs\":[1,2,3],\"preview\":\"soon\"}}", &k));

    CHECK(k.valid);
    CHECK(k.card.valid);
    CHECK_STR(k.card.front, "会う");     /* the good field survived */
    CHECK_STR(k.session.deck, "");
    CHECK_INT(k.session.streak, 0);
    CHECK(!k.session.complete);
    CHECK_INT(k.card.sense_count, 0);
    CHECK_INT(k.card.example_count, 0);
    CHECK_INT(k.card.comment_count, 0);
    CHECK_STR(kanji_preview_span(&k, KANJI_GRADE_GOOD), "");
}

static void test_an_array_holding_the_wrong_shape_skips_those_entries(void)
{
    kanji_t k;
    CHECK(parse("{\"session\":{\"deck\":\"d\"},\"card\":{\"front\":\"会\","
                "\"senses\":[\"good\", 5, null, \"also good\"],"
                "\"examples\":[7, {\"text\":\"出\"}],"
                "\"comments\":[\"a string\", {\"body\":\"real\"}]}}", &k));

    CHECK_INT(k.card.sense_count, 2);
    CHECK_STR(k.card.senses[0], "good");
    CHECK_STR(k.card.senses[1], "also good");
    CHECK_INT(k.card.example_count, 1);
    CHECK_STR(k.card.examples[0].text, "出");
    CHECK_INT(k.card.comment_count, 1);
    CHECK_STR(k.card.comments[0].body, "real");
}

/* An example with nothing in it is a blank row on the glass, which reads as a
 * rendering bug rather than as missing data. */
static void test_empty_rows_are_dropped_rather_than_drawn_blank(void)
{
    kanji_t k;
    CHECK(parse("{\"session\":{\"deck\":\"d\"},\"card\":{\"front\":\"会\","
                "\"senses\":[\"\", \"real\", \"   \"],"
                "\"examples\":[{\"text\":\"\",\"gloss\":\"\"},{\"text\":\"出\"}],"
                "\"parts\":[{\"glyph\":\"\"},{\"glyph\":\"会\"}],"
                "\"comments\":[{\"body\":\"\",\"author\":\"x\"},{\"body\":\"ok\"}]}}",
                &k));

    CHECK_INT(k.card.sense_count, 1);
    CHECK_STR(k.card.senses[0], "real");
    CHECK_INT(k.card.example_count, 1);
    CHECK_INT(k.card.part_count, 1);
    CHECK_INT(k.card.comment_count, 1);
}

/* --- the total is never less than what was sent --------------------------- */

static void test_comment_total_is_at_least_the_comments_that_came(void)
{
    kanji_t k;
    CHECK(parse("{\"session\":{\"deck\":\"d\"},\"card\":{\"front\":\"会\","
                "\"comments\":[{\"body\":\"a\"},{\"body\":\"b\"}],"
                "\"comment_total\":0}}", &k));
    CHECK_INT(k.card.comment_count, 2);
    CHECK_INT(k.card.comment_total, 2);
}

/* --- a real fixture ------------------------------------------------------- */

/* The committed output of tools/mock_kanji_server.py. If the reference producer
 * and this parser ever disagree, it is here that it shows. */
static void test_the_committed_fixture_parses(void)
{
    size_t len = 0;
    char *json = th_slurp(FIXDIR "/kanji.json", &len);
    kanji_t k;
    memset(&k, 0xAA, sizeof k);
    CHECK(kanji_parse(json, len, &k));
    CHECK(k.valid);
    CHECK(k.card.valid);
    CHECK(k.card.front[0] != '\0');
    CHECK(k.card.sense_count > 0);
    CHECK(kanji_preview_span(&k, KANJI_GRADE_GOOD)[0] != '\0');
    free(json);
}

int main(void)
{
    test_a_full_payload_lands_field_for_field();
    test_a_rejected_payload_does_not_touch_the_caller();
    test_a_truncated_multibyte_tail_is_refused_not_half_copied();
    test_a_finished_session_parses_without_a_card();
    test_a_card_without_a_headword_is_not_a_card();
    test_arrays_take_their_first_n_and_drop_the_rest();
    test_an_overlong_string_loses_its_tail_on_a_character_boundary();
    test_counters_are_non_negative_and_bounded();
    test_an_unscheduled_card_keeps_its_minus_one();
    test_a_wrong_type_takes_the_default_and_keeps_the_rest();
    test_an_array_holding_the_wrong_shape_skips_those_entries();
    test_empty_rows_are_dropped_rather_than_drawn_blank();
    test_comment_total_is_at_least_the_comments_that_came();
    test_the_committed_fixture_parses();
    TH_REPORT("kanji_parse");
}
