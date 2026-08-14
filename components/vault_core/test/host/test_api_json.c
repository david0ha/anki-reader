/*
 * Host unit tests for the companion-app JSON serializers.
 *
 * These bytes go straight onto the wire to a phone, so the tests check two
 * different things: that the document parses (via the vendored cJSON the
 * firmware itself uses) and that the *field names* are what the app reads. A
 * serializer that emits valid JSON with a renamed key is still a broken API,
 * and nothing else in the build would notice.
 */
#include "th.h"

#include "cJSON.h"
#include "device_api_json.h"

static void fill(device_state_t *st)
{
    memset(st, 0, sizeof(*st));
    snprintf(st->model, sizeof(st->model), "Kanjis Board");
    snprintf(st->fw, sizeof(st->fw), "0.1.0");
    snprintf(st->device_id, sizeof(st->device_id), "1A2B");
    snprintf(st->ip, sizeof(st->ip), "192.168.0.42");

    st->screen = 1;                     /* KANJI_SCREEN_ANSWER */
    snprintf(st->screen_title, sizeof(st->screen_title), "정답");
    st->revealed = true;
    st->grade = 3;                      /* KANJI_GRADE_GOOD */

    st->card_valid = true;
    st->demo = false;
    snprintf(st->front, sizeof(st->front), "会う");
    snprintf(st->reading, sizeof(st->reading), "あう");
    snprintf(st->meaning, sizeof(st->meaning), "만나다");
    snprintf(st->fsrs_state, sizeof(st->fsrs_state), "review");
    snprintf(st->due, sizeof(st->due), "9일 뒤");
    st->reps = 5;
    st->lapses = 1;
    st->stability_days = 9;
    st->difficulty_pct = 47;

    snprintf(st->deck, sizeof(st->deck), "JLPT N5 Vocabulary");
    st->streak = 12;
    st->reviewed_today = 34;
    st->left_new = 7;
    st->left_review = 18;
    st->track = 35;
    st->track_total = 60;
    st->session_complete = false;

    snprintf(st->kanji_url, sizeof(st->kanji_url), "http://mac.local:8123/kanji.json");
    snprintf(st->last_result, sizeof(st->last_result), "ok");
    st->poll_seconds = 300;
    st->age_seconds = 42;
    st->stale = false;

    st->battery_present = true;
    st->battery_pct = 84;
    st->battery_mv = 4012;

    st->partial_chain = 3;
    st->full_refresh_ms = 4120;
    st->partial_refresh_ms = 780;
}

static cJSON *obj(cJSON *root, const char *key)
{
    cJSON *o = cJSON_GetObjectItem(root, key);
    if (!cJSON_IsObject(o)) {
        g_total++; g_fail++;
        printf("  FAIL missing object \"%s\"\n", key);
        return NULL;
    }
    g_total++;
    return o;
}

static void check_int(cJSON *o, const char *key, int want)
{
    cJSON *v = o ? cJSON_GetObjectItem(o, key) : NULL;
    g_total++;
    if (!cJSON_IsNumber(v)) {
        g_fail++;
        printf("  FAIL \"%s\" missing or not a number\n", key);
    } else if ((int)cJSON_GetNumberValue(v) != want) {
        g_fail++;
        printf("  FAIL \"%s\" == %d  got %d\n", key, want, (int)cJSON_GetNumberValue(v));
    }
}

static void check_str(cJSON *o, const char *key, const char *want)
{
    cJSON *v = o ? cJSON_GetObjectItem(o, key) : NULL;
    g_total++;
    if (!cJSON_IsString(v)) {
        g_fail++;
        printf("  FAIL \"%s\" missing or not a string\n", key);
    } else if (strcmp(cJSON_GetStringValue(v), want) != 0) {
        g_fail++;
        printf("  FAIL \"%s\" == \"%s\"  got \"%s\"\n", key, want, cJSON_GetStringValue(v));
    }
}

static void check_bool(cJSON *o, const char *key, bool want)
{
    cJSON *v = o ? cJSON_GetObjectItem(o, key) : NULL;
    g_total++;
    if (!cJSON_IsBool(v)) {
        g_fail++;
        printf("  FAIL \"%s\" missing or not a bool\n", key);
    } else if (cJSON_IsTrue(v) != want) {
        g_fail++;
        printf("  FAIL \"%s\" == %s\n", key, want ? "true" : "false");
    }
}

static void test_info(void)
{
    char buf[256];
    int n = device_api_json_info(buf, sizeof(buf), "1A2B", "Kanjis Board",
                                 "0.1.0", "192.168.0.42");
    CHECK(n > 0);
    CHECK_INT((int)strlen(buf), n);

    /* The discovery probe reads these four names off every candidate host on
     * the LAN. Renaming one is an app release, not a firmware change. */
    CHECK_STR(buf, "{\"deviceId\":\"1A2B\",\"model\":\"Kanjis Board\","
                   "\"fw\":\"0.1.0\",\"ip\":\"192.168.0.42\"}");
}

static void test_state_shape(void)
{
    device_state_t st;
    fill(&st);

    char buf[DEVICE_API_STATE_BUF_SZ];
    int n = device_api_json_state(&st, buf, sizeof(buf));
    CHECK(n > 0);
    CHECK_INT((int)strlen(buf), n);

    cJSON *r = cJSON_Parse(buf);
    CHECK(r != NULL);
    if (!r) return;

    check_str(r, "deviceId", "1A2B");
    check_str(r, "model", "Kanjis Board");
    check_str(r, "fw", "0.1.0");
    check_str(r, "ip", "192.168.0.42");

    /* The interaction state. A phone that cannot see which screen is up and
     * whether the answer is revealed would have to guess what its own POST
     * /api/screen just did. */
    check_int(r, "screen", 1);
    check_str(r, "screenTitle", "정답");
    check_bool(r, "revealed", true);
    check_int(r, "grade", 3);

    cJSON *c = obj(r, "card");
    check_bool(c, "valid", true);
    check_bool(c, "demo", false);
    check_str(c, "front", "会う");
    check_str(c, "reading", "あう");
    check_str(c, "meaning", "만나다");
    check_str(c, "fsrsState", "review");
    check_str(c, "due", "9일 뒤");
    check_int(c, "reps", 5);
    check_int(c, "lapses", 1);
    check_int(c, "stabilityDays", 9);
    check_int(c, "difficultyPct", 47);

    cJSON *e = obj(r, "session");
    check_str(e, "deck", "JLPT N5 Vocabulary");
    check_int(e, "streak", 12);
    check_int(e, "reviewedToday", 34);
    check_int(e, "leftNew", 7);
    check_int(e, "leftReview", 18);
    check_int(e, "track", 35);
    check_int(e, "trackTotal", 60);
    check_bool(e, "complete", false);

    cJSON *s = obj(r, "source");
    check_str(s, "url", "http://mac.local:8123/kanji.json");
    check_str(s, "lastResult", "ok");
    check_int(s, "pollSeconds", 300);
    check_int(s, "ageSeconds", 42);
    check_bool(s, "stale", false);

    cJSON *b = obj(r, "battery");
    check_bool(b, "present", true);
    check_int(b, "percent", 84);
    check_int(b, "millivolts", 4012);

    /* The panel timings are the whole reason the refresh policy can be decided
     * from measurement rather than guessed, so they are part of the contract. */
    cJSON *p = obj(r, "panel");
    check_int(p, "partialChain", 3);
    check_int(p, "fullRefreshMs", 4120);
    check_int(p, "partialRefreshMs", 780);

    cJSON_Delete(r);
}

/* Walk every number in the document and insist it is whole. Returns how many it
 * found, so a caller can tell "all integral" from "the numbers all became
 * strings and nothing was checked". */
static int check_all_numbers_are_integers(cJSON *node)
{
    int found = 0;
    for (cJSON *it = node->child; it != NULL; it = it->next) {
        if (cJSON_IsNumber(it)) {
            double d = cJSON_GetNumberValue(it);
            found++;
            g_total++;
            if (d != (double)(long long)d) {
                g_fail++;
                printf("  FAIL \"%s\" is fractional (%f)\n",
                       it->string ? it->string : "?", d);
            }
        } else if (cJSON_IsObject(it) || cJSON_IsArray(it)) {
            found += check_all_numbers_are_integers(it);
        }
    }
    return found;
}

static void test_every_number_is_an_integer(void)
{
    /* device_api_model.h's rule, pinned. A "%.2f" of a large magnitude
     * truncates on the decimal point and emits a token strict parsers reject,
     * and the failure would land on the phone rather than in this build — so
     * the fix is that no field is ever a fraction, and this is what says so. */
    device_state_t st;
    fill(&st);

    char buf[DEVICE_API_STATE_BUF_SZ];
    CHECK(device_api_json_state(&st, buf, sizeof(buf)) > 0);
    cJSON *r = cJSON_Parse(buf);
    CHECK(r != NULL);
    if (r) {
        CHECK(check_all_numbers_are_integers(r) == 19);
        cJSON_Delete(r);
    }
}

static void test_unscheduled_card_reports_minus_one(void)
{
    /* -1 is the contract's "the scheduler has not decided yet", and it is not
     * zero: a card whose stability is unknown and one whose interval rounds to
     * the same day are different cards. Same for a board that has never
     * completed a fetch versus one that synced this second. */
    device_state_t st;
    fill(&st);
    st.stability_days = -1;
    st.difficulty_pct = -1;
    st.age_seconds = -1;

    char buf[DEVICE_API_STATE_BUF_SZ];
    CHECK(device_api_json_state(&st, buf, sizeof(buf)) > 0);
    cJSON *r = cJSON_Parse(buf);
    CHECK(r != NULL);
    if (r) {
        check_int(obj(r, "card"), "stabilityDays", -1);
        check_int(obj(r, "card"), "difficultyPct", -1);
        check_int(obj(r, "source"), "ageSeconds", -1);
        cJSON_Delete(r);
    }
}

static void test_cjk_passes_through_as_utf8(void)
{
    /* The headword is Japanese, the gloss and the deck name are Korean. JSON
     * strings are defined over Unicode, so escaping them to \u would be legal
     * and pointless — but the escaper must not mangle them either. */
    device_state_t st;
    fill(&st);
    snprintf(st.deck, sizeof(st.deck), "일본어 상용한자");
    snprintf(st.front, sizeof(st.front), "生まれ変わる");
    snprintf(st.reading, sizeof(st.reading), "うまれかわる");

    char buf[DEVICE_API_STATE_BUF_SZ];
    CHECK(device_api_json_state(&st, buf, sizeof(buf)) > 0);
    CHECK(strstr(buf, "生まれ変わる") != NULL);

    cJSON *r = cJSON_Parse(buf);
    CHECK(r != NULL);
    if (r) {
        check_str(obj(r, "session"), "deck", "일본어 상용한자");
        check_str(obj(r, "card"), "front", "生まれ変わる");
        check_str(obj(r, "card"), "reading", "うまれかわる");
        cJSON_Delete(r);
    }
}

static void test_control_characters_are_escaped(void)
{
    /* A deck name with a newline in it is not exotic — it is one paste away at
     * the far end, and kanji_parse copies it through UTF-8-safely without
     * filtering C0. An unescaped 0x0A is invalid JSON that would break the
     * app's parser rather than just looking odd; 0x01 has no short form at all
     * and has to become a six-byte \\u escape. */
    device_state_t st;
    fill(&st);
    snprintf(st.deck, sizeof(st.deck), "a\"b\\c\nd\te\x01" "f");

    char buf[DEVICE_API_STATE_BUF_SZ];
    CHECK(device_api_json_state(&st, buf, sizeof(buf)) > 0);
    CHECK(strstr(buf, "\\u0001") != NULL);
    cJSON *r = cJSON_Parse(buf);
    CHECK(r != NULL);
    if (r) {
        check_str(obj(r, "session"), "deck", "a\"b\\c\nd\te\x01" "f");
        cJSON_Delete(r);
    }
}

static void test_overflow_yields_an_empty_string_not_half_a_document(void)
{
    /* Half a JSON document is worse than none: the app would try to parse it,
     * fail somewhere in the middle, and report a confusing error. The contract
     * is -1 and out[0] == '\0'. */
    device_state_t st;
    fill(&st);

    char buf[64];
    CHECK_INT(device_api_json_state(&st, buf, sizeof(buf)), -1);
    CHECK_STR(buf, "");

    /* One byte short of the real document is the interesting case: the sink has
     * to latch on the last append rather than on the first. */
    char full[DEVICE_API_STATE_BUF_SZ];
    int n = device_api_json_state(&st, full, sizeof(full));
    CHECK(n > 0);
    CHECK_INT(device_api_json_state(&st, full, (size_t)n), -1);
    CHECK_STR(full, "");
    CHECK_INT(device_api_json_state(&st, full, (size_t)n + 1), n);

    char tiny[4];
    CHECK_INT(device_api_json_info(tiny, sizeof(tiny), "1A2B", "Kanjis Board",
                                   "0.1.0", "1.2.3.4"), -1);
    CHECK_STR(tiny, "");

    /* Zero capacity must not write at all. */
    CHECK_INT(device_api_json_state(&st, buf, 0), -1);
    CHECK_INT(device_api_json_info(buf, 0, "a", "b", "c", "d"), -1);
}

static void test_worst_case_fits_the_servers_buffer(void)
{
    /* device_api.c serialises into a DEVICE_API_STATE_BUF_SZ buffer. If the
     * worst case does not fit, the serializer returns -1 and an EMPTY body —
     * so the symptom is "the app shows nothing", with no error anywhere to
     * suggest a length problem. Every string is therefore filled to its
     * declared maximum here, in the widest bytes it can hold. */
    device_state_t st;
    memset(&st, 0, sizeof(st));

    /* Locally produced: a compile-time constant, a MAC, an IP, a string from a
     * fixed table. None can carry a byte the escaper expands, so their worst
     * case is one wire byte per stored byte — which is also what a full field
     * of Korean costs, since UTF-8 passes through untouched. */
    #define FILL_LOCAL(field) do { \
        size_t n = sizeof(st.field) - 1; \
        memset(st.field, 'W', n); \
        st.field[n] = '\0'; \
    } while (0)

    FILL_LOCAL(model);
    FILL_LOCAL(fw);
    FILL_LOCAL(device_id);
    FILL_LOCAL(ip);
    FILL_LOCAL(screen_title);
    FILL_LOCAL(last_result);

    /* Everything else is typed by a human into the portal form or copied out of
     * a payload the board did not write. A C0 control has no short escape and
     * costs six bytes on the wire for one in the struct, which is the real
     * worst case and the only one worth sizing the buffer against. */
    #define FILL_HOSTILE(field) do { \
        size_t n = sizeof(st.field) - 1; \
        memset(st.field, '\x01', n); \
        st.field[n] = '\0'; \
    } while (0)

    FILL_HOSTILE(front);
    FILL_HOSTILE(reading);
    FILL_HOSTILE(meaning);
    FILL_HOSTILE(fsrs_state);
    FILL_HOSTILE(due);
    FILL_HOSTILE(deck);
    FILL_HOSTILE(kanji_url);

    #undef FILL_LOCAL
    #undef FILL_HOSTILE

    /* Every integer at its widest textual form, which is the negative one. */
    st.screen = st.grade = -2147483647 - 1;
    st.reps = st.lapses = st.stability_days = st.difficulty_pct = -2147483647 - 1;
    st.streak = st.reviewed_today = st.left_new = st.left_review = -2147483647 - 1;
    st.track = st.track_total = -2147483647 - 1;
    st.poll_seconds = st.age_seconds = -2147483647 - 1;
    st.battery_pct = st.battery_mv = -2147483647 - 1;
    st.partial_chain = st.full_refresh_ms = st.partial_refresh_ms = -2147483647 - 1;

    char buf[DEVICE_API_STATE_BUF_SZ];
    int n = device_api_json_state(&st, buf, sizeof(buf));
    if (n < 0) {
        g_total++; g_fail++;
        printf("  FAIL worst-case state does not fit DEVICE_API_STATE_BUF_SZ (%d) — "
               "raise it\n", DEVICE_API_STATE_BUF_SZ);
    } else {
        g_total++;
        printf("  worst-case state document: %d of %d bytes\n", n, DEVICE_API_STATE_BUF_SZ);
        cJSON *r = cJSON_Parse(buf);
        CHECK(r != NULL);
        cJSON_Delete(r);
    }

    char ibuf[DEVICE_API_INFO_BUF_SZ];
    char wide[DEV_MODEL_MAXLEN];
    memset(wide, '"', sizeof(wide) - 1);
    wide[sizeof(wide) - 1] = '\0';
    CHECK(device_api_json_info(ibuf, sizeof(ibuf), wide, wide, wide, wide) > 0);
}

static void test_null_state_is_rejected(void)
{
    char buf[256];
    CHECK_INT(device_api_json_state(NULL, buf, sizeof(buf)), -1);
    CHECK_STR(buf, "");
}

static void test_zeroed_state_still_parses(void)
{
    /* This is what /api/state returns before the first poll — every string
     * empty, every number zero. It must still be a valid document. */
    device_state_t st;
    memset(&st, 0, sizeof(st));

    char buf[DEVICE_API_STATE_BUF_SZ];
    CHECK(device_api_json_state(&st, buf, sizeof(buf)) > 0);
    cJSON *r = cJSON_Parse(buf);
    CHECK(r != NULL);
    if (r) {
        check_bool(obj(r, "card"), "valid", false);
        check_bool(obj(r, "session"), "complete", false);
        check_str(r, "deviceId", "");
        cJSON_Delete(r);
    }
}

int main(void)
{
    test_info();
    test_state_shape();
    test_every_number_is_an_integer();
    test_unscheduled_card_reports_minus_one();
    test_cjk_passes_through_as_utf8();
    test_control_characters_are_escaped();
    test_worst_case_fits_the_servers_buffer();
    test_overflow_yields_an_empty_string_not_half_a_document();
    test_null_state_is_rejected();
    test_zeroed_state_still_parses();
    TH_REPORT("api_json");
}
