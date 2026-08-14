/*
 * Host unit tests for kanji_service.c.
 *
 * The layer is thin — http_get() plus kanji_parse() — but it owns three things
 * nothing else does, and all three are the kind that only misbehave on a bad
 * day: which failures are distinguished from each other, whether *out survives
 * a failure, and whether the grading URL is built correctly. That last one is
 * the expensive one: a mangled URL does not fail loudly, it grades the wrong
 * card, and the damage lands in somebody's review history where nothing on the
 * board will ever show it.
 *
 * The HTTP port is the project's one platform seam, so a test can simply BE the
 * port: this file defines http_get() itself, and the linker takes it instead of
 * either the esp_http_client or the libcurl implementation.
 */
#include "th.h"

#include "http_port.h"
#include "kanji_model.h"
#include "kanji_service.h"

/* --- the fake port -------------------------------------------------------- */

static const char *g_body;      /* NULL = transport failure */
static int         g_status;
static int         g_calls;
static int         g_init_calls;
static int         g_deinit_calls;
static char        g_last_url[512];

bool http_port_init(void)
{
    g_init_calls++;
    return true;
}

void http_port_deinit(void)
{
    g_deinit_calls++;
}

char *http_get(const char *url, int *out_status)
{
    g_calls++;
    g_last_url[0] = '\0';
    if (url) {
        size_t n = strlen(url);
        if (n >= sizeof g_last_url) n = sizeof g_last_url - 1;
        memcpy(g_last_url, url, n);
        g_last_url[n] = '\0';
    }
    if (out_status) *out_status = g_status;
    if (!g_body) return NULL;

    /* Returned on the heap, exactly as the real ports do, so every path runs
     * against a real allocation and a double free would trap. */
    size_t n = strlen(g_body);
    char *p = (char *)malloc(n + 1);
    memcpy(p, g_body, n + 1);
    return p;
}

/* --- fixtures ------------------------------------------------------------- */

static void test_port_lifecycle_contract_is_explicit(void)
{
    g_init_calls = 0;
    g_deinit_calls = 0;
    CHECK(http_port_init());
    http_port_deinit();
    CHECK_INT(g_init_calls, 1);
    CHECK_INT(g_deinit_calls, 1);
}

static const char *CARD =
    "{\"v\":1,\"session\":{\"deck\":\"N5\",\"streak\":3},"
    "\"card\":{\"front\":\"会う\",\"senses\":[\"만나다\"],"
    "\"preview\":{\"good\":\"9일 뒤\"}}}";

static const char *OTHER_CARD =
    "{\"v\":1,\"session\":{\"deck\":\"N5\",\"streak\":4},"
    "\"card\":{\"front\":\"行く\",\"senses\":[\"가다\"]}}";

static void expect(const char *label, const char *body, int status,
                   kanji_fetch_result_t want)
{
    g_body = body;
    g_status = status;

    kanji_t k;
    memset(&k, 0, sizeof k);
    kanji_fetch_result_t got = kanji_service_fetch("http://pc:8123/kanji.json", &k);
    if (got != want) {
        printf("  FAIL %s: wanted %s, got %s\n", label,
               kanji_fetch_result_name(want), kanji_fetch_result_name(got));
        g_fail++;
    }
    g_total++;
}

/* --- which failures are which --------------------------------------------- */

static void test_each_failure_is_told_apart_from_the_others(void)
{
    expect("a good payload",        CARD, 200, KANJI_FETCH_OK);
    expect("a 204 is still 2xx",    CARD, 204, KANJI_FETCH_OK);
    expect("no connection",         NULL, 0,   KANJI_FETCH_TRANSPORT);
    expect("a 404 page",            "<html>404</html>", 404, KANJI_FETCH_HTTP_STATUS);
    expect("a 500",                 CARD, 500, KANJI_FETCH_HTTP_STATUS);
    /* The proxy says 409 when the board grades a card it is no longer serving.
     * It must not be confused with a bad payload: one is a race worth retrying,
     * the other is a contract break worth logging. */
    expect("a 409 from a stale grade", "{}", 409, KANJI_FETCH_HTTP_STATUS);
    expect("a 200 of nonsense",     "not json", 200, KANJI_FETCH_BAD_PAYLOAD);
    expect("a captive portal",      "{\"detail\":\"login\"}", 200,
                                    KANJI_FETCH_BAD_PAYLOAD);

    /* Status is checked before the body is parsed: a 404 page and a captive
     * portal redirect are both perfectly good documents that happen not to be
     * a card, and "your URL is wrong" and "your JSON is wrong" are different
     * messages in the log. */
    g_body = "{\"session\":{\"deck\":\"d\"}}";
    g_status = 404;
    kanji_t k;
    memset(&k, 0, sizeof k);
    CHECK_INT(kanji_service_fetch("http://x/", &k), KANJI_FETCH_HTTP_STATUS);
}

static void test_no_url_is_not_a_network_call(void)
{
    kanji_t k;
    memset(&k, 0, sizeof k);
    g_body = CARD;
    g_status = 200;

    g_calls = 0;
    CHECK_INT(kanji_service_fetch(NULL, &k), KANJI_FETCH_NO_URL);
    CHECK_INT(kanji_service_fetch("", &k), KANJI_FETCH_NO_URL);
    CHECK_INT(kanji_service_fetch("http://x/", NULL), KANJI_FETCH_NO_URL);
    CHECK_INT(g_calls, 0);
}

/* --- a failure never disturbs the card on the glass ----------------------- */

static void test_a_failed_fetch_leaves_the_previous_card_intact(void)
{
    kanji_t k;
    memset(&k, 0, sizeof k);
    g_body = CARD;
    g_status = 200;
    CHECK_INT(kanji_service_fetch("http://x/", &k), KANJI_FETCH_OK);
    const uint32_t before = kanji_hash(&k);
    CHECK_STR(k.card.front, "会う");

    struct { const char *body; int status; } bad[] = {
        { NULL, 0 }, { "<html>", 404 }, { CARD, 503 }, { "garbage", 200 },
        { "{}", 200 }, { "", 200 },
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        g_body = bad[i].body;
        g_status = bad[i].status;
        CHECK(kanji_service_fetch("http://x/", &k) != KANJI_FETCH_OK);
        CHECK_INT(kanji_hash(&k), before);
        CHECK_STR(k.card.front, "会う");
    }
}

/* --- the grading URL ------------------------------------------------------ */

static void test_the_grade_is_appended_with_the_right_separator(void)
{
    char url[KANJI_URL_MAX];

    CHECK(kanji_service_grade_url("http://pc:8123/kanji.json",
                                  KANJI_GRADE_GOOD, NULL, url, sizeof url));
    CHECK_STR(url, "http://pc:8123/kanji.json?grade=good");

    /* A URL that already carries a query gets & rather than a second ?. A
     * proxy behind a path prefix or a cache-buster is not exotic. */
    CHECK(kanji_service_grade_url("http://pc:8123/kanji.json?deck=n5",
                                  KANJI_GRADE_AGAIN, NULL, url, sizeof url));
    CHECK_STR(url, "http://pc:8123/kanji.json?deck=n5&grade=again");

    CHECK(kanji_service_grade_url("http://x/", KANJI_GRADE_HARD, NULL,
                                  url, sizeof url));
    CHECK_STR(url, "http://x/?grade=hard");
    CHECK(kanji_service_grade_url("http://x/", KANJI_GRADE_EASY, NULL,
                                  url, sizeof url));
    CHECK_STR(url, "http://x/?grade=easy");
}

/* The board names the card it is rating.
 *
 * Without it the proxy grades whatever card it happens to be serving. That is
 * not hypothetical: the proxy advances when anything grades — including the web
 * app, in another tab — so a learner who reveals card A, gets distracted, and
 * presses KEY1 after the session has moved on would have their rating for A
 * recorded against B, silently and permanently. The id turns that into a 409
 * the board can leave on screen. It is also what makes a retried request
 * recognisable as a retry rather than a second grade. */
static void test_the_card_being_rated_is_named_in_the_url(void)
{
    char url[KANJI_URL_MAX];

    CHECK(kanji_service_grade_url("http://pc:8123/kanji.json", KANJI_GRADE_GOOD,
                                  "f00c539e-23f9-4294-bee1-c642189b105f",
                                  url, sizeof url));
    CHECK_STR(url, "http://pc:8123/kanji.json?grade=good"
                   "&card=f00c539e-23f9-4294-bee1-c642189b105f");

    /* Still one ? for the whole query, whichever part came first. */
    CHECK(kanji_service_grade_url("http://pc/k.json?deck=n5", KANJI_GRADE_EASY,
                                  "abc123", url, sizeof url));
    CHECK_STR(url, "http://pc/k.json?deck=n5&grade=easy&card=abc123");

    /* An empty id is the same as none: the demo card has no id, and refusing to
     * grade it would break the one screen a board with no proxy can show. */
    CHECK(kanji_service_grade_url("http://x/", KANJI_GRADE_GOOD, "", url, sizeof url));
    CHECK_STR(url, "http://x/?grade=good");
}

/* A card id is a UUID from the backend. If one ever arrives carrying a & or a #
 * it would split the query and change which parameters the proxy sees, so the
 * id is dropped rather than escaped — the grade still lands, and it lands
 * without the guard rather than against the wrong card. */
static void test_a_card_id_that_would_corrupt_the_query_is_dropped(void)
{
    char url[KANJI_URL_MAX];

    CHECK(kanji_service_grade_url("http://x/k.json", KANJI_GRADE_GOOD,
                                  "abc&grade=easy", url, sizeof url));
    CHECK_STR(url, "http://x/k.json?grade=good");

    CHECK(kanji_service_grade_url("http://x/k.json", KANJI_GRADE_GOOD,
                                  "abc#frag", url, sizeof url));
    CHECK_STR(url, "http://x/k.json?grade=good");

    CHECK(kanji_service_grade_url("http://x/k.json", KANJI_GRADE_GOOD,
                                  "a b", url, sizeof url));
    CHECK_STR(url, "http://x/k.json?grade=good");

    /* The characters a UUID actually uses are kept. */
    CHECK(kanji_service_grade_url("http://x/k.json", KANJI_GRADE_GOOD,
                                  "A-9_z", url, sizeof url));
    CHECK_STR(url, "http://x/k.json?grade=good&card=A-9_z");
}

/* The id is a courtesy, not a requirement: if naming the card would overflow
 * the buffer, the grade goes without it rather than not at all. Losing the
 * guard costs a rare mis-grade; losing the grade costs the learner every
 * rating they give from then on. */
static void test_an_id_that_does_not_fit_is_dropped_but_the_grade_survives(void)
{
    const char *want = "http://a/?grade=good";
    char just_enough[sizeof "http://a/?grade=good"];

    CHECK(kanji_service_grade_url("http://a/", KANJI_GRADE_GOOD,
                                  "an-id-far-too-long-to-fit-here",
                                  just_enough, sizeof just_enough));
    CHECK_STR(just_enough, want);
}

/* A truncated URL is strictly worse than no request: it reaches some other path
 * with the grade silently dropped, and the damage lands in a review history
 * nothing on the board will ever show. */
static void test_a_url_that_would_not_fit_is_refused_outright(void)
{
    char small[16];
    CHECK(!kanji_service_grade_url("http://pc:8123/kanji.json",
                                   KANJI_GRADE_GOOD, NULL, small, sizeof small));
    CHECK_STR(small, "");

    /* An exact fit is a fit, and exactly one byte short refuses. */
    const char *base = "http://a/";
    const char *want = "http://a/?grade=good";
    const size_t fits = strlen(want) + 1;

    char exact[sizeof "http://a/?grade=good"];         /* strlen + NUL */
    CHECK_INT(sizeof exact, fits);
    CHECK(kanji_service_grade_url(base, KANJI_GRADE_GOOD, NULL,
                                  exact, sizeof exact));
    CHECK_STR(exact, want);

    char one_short[sizeof "http://a/?grade=good" - 1];
    CHECK(!kanji_service_grade_url(base, KANJI_GRADE_GOOD, NULL,
                                   one_short, sizeof one_short));
    CHECK_STR(one_short, "");
}

static void test_a_bad_grade_or_base_is_refused(void)
{
    char url[KANJI_URL_MAX];
    CHECK(!kanji_service_grade_url("", KANJI_GRADE_GOOD, NULL, url, sizeof url));
    CHECK_STR(url, "");
    CHECK(!kanji_service_grade_url(NULL, KANJI_GRADE_GOOD, NULL, url, sizeof url));
    CHECK(!kanji_service_grade_url("http://x/", (kanji_grade_t)0, NULL,
                                   url, sizeof url));
    CHECK_STR(url, "");
    CHECK(!kanji_service_grade_url("http://x/", (kanji_grade_t)9, NULL,
                                   url, sizeof url));
    CHECK(!kanji_service_grade_url("http://x/", KANJI_GRADE_GOOD, NULL, NULL, 100));
    CHECK(!kanji_service_grade_url("http://x/", KANJI_GRADE_GOOD, NULL, url, 0));
}

/* --- grading -------------------------------------------------------------- */

static void test_grading_calls_the_grade_url_and_returns_the_next_card(void)
{
    kanji_t k;
    memset(&k, 0, sizeof k);
    g_body = CARD;
    g_status = 200;
    CHECK_INT(kanji_service_fetch("http://pc/k.json", &k), KANJI_FETCH_OK);

    g_body = OTHER_CARD;
    g_calls = 0;
    CHECK_INT(kanji_service_grade("http://pc/k.json", KANJI_GRADE_EASY, NULL, &k),
              KANJI_FETCH_OK);
    CHECK_INT(g_calls, 1);
    CHECK_STR(g_last_url, "http://pc/k.json?grade=easy");
    CHECK_STR(k.card.front, "行く");
    CHECK_INT(k.session.streak, 4);
}

static void test_a_rejected_grade_leaves_the_answer_on_the_glass(void)
{
    kanji_t k;
    memset(&k, 0, sizeof k);
    g_body = CARD;
    g_status = 200;
    CHECK_INT(kanji_service_fetch("http://pc/k.json", &k), KANJI_FETCH_OK);
    const uint32_t before = kanji_hash(&k);

    /* The proxy is serving a different card now. */
    g_body = "{\"detail\":\"stale card\"}";
    g_status = 409;
    CHECK_INT(kanji_service_grade("http://pc/k.json", KANJI_GRADE_GOOD, NULL, &k),
              KANJI_FETCH_HTTP_STATUS);
    CHECK_INT(kanji_hash(&k), before);
    CHECK_STR(k.card.front, "会う");
}

static void test_an_unbuildable_grade_url_never_reaches_the_network(void)
{
    kanji_t k;
    memset(&k, 0, sizeof k);
    g_body = CARD;
    g_status = 200;

    g_calls = 0;
    CHECK_INT(kanji_service_grade("", KANJI_GRADE_GOOD, NULL, &k), KANJI_FETCH_NO_URL);
    CHECK_INT(kanji_service_grade(NULL, KANJI_GRADE_GOOD, NULL, &k), KANJI_FETCH_NO_URL);
    CHECK_INT(kanji_service_grade("http://x/", (kanji_grade_t)7, NULL, &k),
              KANJI_FETCH_NO_URL);
    CHECK_INT(g_calls, 0);
}

/* --- names ---------------------------------------------------------------- */

static void test_every_result_has_a_stable_name(void)
{
    CHECK_STR(kanji_fetch_result_name(KANJI_FETCH_OK), "ok");
    CHECK_STR(kanji_fetch_result_name(KANJI_FETCH_NO_URL), "no_url");
    CHECK_STR(kanji_fetch_result_name(KANJI_FETCH_TRANSPORT), "transport");
    CHECK_STR(kanji_fetch_result_name(KANJI_FETCH_HTTP_STATUS), "http_status");
    CHECK_STR(kanji_fetch_result_name(KANJI_FETCH_BAD_PAYLOAD), "bad_payload");
    CHECK_STR(kanji_fetch_result_name((kanji_fetch_result_t)42), "unknown");
}

int main(void)
{
    test_port_lifecycle_contract_is_explicit();
    test_each_failure_is_told_apart_from_the_others();
    test_no_url_is_not_a_network_call();
    test_a_failed_fetch_leaves_the_previous_card_intact();
    test_the_grade_is_appended_with_the_right_separator();
    test_the_card_being_rated_is_named_in_the_url();
    test_a_card_id_that_would_corrupt_the_query_is_dropped();
    test_an_id_that_does_not_fit_is_dropped_but_the_grade_survives();
    test_a_url_that_would_not_fit_is_refused_outright();
    test_a_bad_grade_or_base_is_refused();
    test_grading_calls_the_grade_url_and_returns_the_next_card();
    test_a_rejected_grade_leaves_the_answer_on_the_glass();
    test_an_unbuildable_grade_url_never_reaches_the_network();
    test_every_result_has_a_stable_name();
    TH_REPORT("kanji_service");
}
