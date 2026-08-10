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
    snprintf(st->model, sizeof(st->model), "Obsidian Board");
    snprintf(st->fw, sizeof(st->fw), "0.1.0");
    snprintf(st->device_id, sizeof(st->device_id), "1A2B");
    snprintf(st->ip, sizeof(st->ip), "192.168.0.42");
    st->page = 2;
    snprintf(st->page_title, sizeof(st->page_title), "에이전트");

    st->vault_valid = true;
    st->demo = false;
    snprintf(st->vault, sizeof(st->vault), "second-brain");
    snprintf(st->generated_at, sizeof(st->generated_at), "21:04");
    st->notes = 1428;
    st->links = 3910;
    st->orphans = 37;
    st->tags = 212;
    st->added_today = 6;
    st->added_7d = 41;
    st->agents_total = 5;
    st->agents_running = 2;
    st->recent_count = 8;
    st->inbox_total = 11;

    snprintf(st->vault_url, sizeof(st->vault_url), "http://mac.local:8123/vault.json");
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
    int n = device_api_json_info(buf, sizeof(buf), "1A2B", "Obsidian Board",
                                 "0.1.0", "192.168.0.42");
    CHECK(n > 0);
    CHECK_INT((int)strlen(buf), n);

    /* The discovery probe reads these four names off every candidate host on
     * the LAN. Renaming one is an app release, not a firmware change. */
    CHECK_STR(buf, "{\"deviceId\":\"1A2B\",\"model\":\"Obsidian Board\","
                   "\"fw\":\"0.1.0\",\"ip\":\"192.168.0.42\"}");
}

static void test_state_shape(void)
{
    device_state_t st;
    fill(&st);

    char buf[2048];
    int n = device_api_json_state(&st, buf, sizeof(buf));
    CHECK(n > 0);
    CHECK_INT((int)strlen(buf), n);

    cJSON *r = cJSON_Parse(buf);
    CHECK(r != NULL);
    if (!r) return;

    check_str(r, "deviceId", "1A2B");
    check_str(r, "model", "Obsidian Board");
    check_str(r, "fw", "0.1.0");
    check_str(r, "ip", "192.168.0.42");
    check_int(r, "page", 2);
    check_str(r, "pageTitle", "에이전트");

    cJSON *v = obj(r, "vault");
    check_bool(v, "valid", true);
    check_bool(v, "demo", false);
    check_str(v, "name", "second-brain");
    check_str(v, "generatedAt", "21:04");
    check_int(v, "notes", 1428);
    check_int(v, "links", 3910);
    check_int(v, "orphans", 37);
    check_int(v, "tags", 212);
    check_int(v, "addedToday", 6);
    check_int(v, "added7d", 41);
    check_int(v, "agents", 5);
    check_int(v, "agentsRunning", 2);
    check_int(v, "recent", 8);
    check_int(v, "inbox", 11);

    cJSON *s = obj(r, "source");
    check_str(s, "url", "http://mac.local:8123/vault.json");
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

static void test_korean_passes_through_as_utf8(void)
{
    /* Vault names and note titles are Korean. JSON strings are defined over
     * Unicode, so escaping them to \u would be legal and pointless — but the
     * escaper must not mangle them either. */
    device_state_t st;
    fill(&st);
    snprintf(st.vault, sizeof(st.vault), "두번째 뇌");
    snprintf(st.page_title, sizeof(st.page_title), "최근 노트");

    char buf[2048];
    CHECK(device_api_json_state(&st, buf, sizeof(buf)) > 0);
    CHECK(strstr(buf, "두번째 뇌") != NULL);

    cJSON *r = cJSON_Parse(buf);
    CHECK(r != NULL);
    if (r) {
        check_str(r, "pageTitle", "최근 노트");
        check_str(obj(r, "vault"), "name", "두번째 뇌");
        cJSON_Delete(r);
    }
}

static void test_control_characters_are_escaped(void)
{
    /* A note title with a newline in it is not exotic — Obsidian will happily
     * let you make one, and an unescaped 0x0A is invalid JSON that would break
     * the app's parser rather than just looking odd. */
    device_state_t st;
    fill(&st);
    snprintf(st.vault, sizeof(st.vault), "a\"b\\c\nd\te");

    char buf[2048];
    CHECK(device_api_json_state(&st, buf, sizeof(buf)) > 0);
    cJSON *r = cJSON_Parse(buf);
    CHECK(r != NULL);
    if (r) {
        check_str(obj(r, "vault"), "name", "a\"b\\c\nd\te");
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

    char tiny[4];
    CHECK_INT(device_api_json_info(tiny, sizeof(tiny), "1A2B", "Obsidian Board",
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

    #define FILL_ASCII(field) do { \
        size_t n = sizeof(st.field) - 1; \
        memset(st.field, 'W', n); \
        st.field[n] = '\0'; \
    } while (0)

    FILL_ASCII(model);
    FILL_ASCII(fw);
    FILL_ASCII(device_id);
    FILL_ASCII(ip);
    FILL_ASCII(vault_url);
    FILL_ASCII(last_result);

    /* Korean is 3 bytes a syllable, and each of those bytes passes through the
     * escaper untouched — so a Korean field is the same length on the wire as
     * an ASCII one. A field full of quotes is NOT: each becomes two bytes. */
    for (size_t i = 0; i + 1 < sizeof(st.vault); i++) st.vault[i] = '"';
    st.vault[sizeof(st.vault) - 1] = '\0';
    for (size_t i = 0; i + 1 < sizeof(st.page_title); i++) st.page_title[i] = '\\';
    st.page_title[sizeof(st.page_title) - 1] = '\0';
    for (size_t i = 0; i + 1 < sizeof(st.generated_at); i++) st.generated_at[i] = '\n';
    st.generated_at[sizeof(st.generated_at) - 1] = '\0';

    #undef FILL_ASCII

    st.page = 3;
    st.vault_valid = true;
    st.notes = st.links = 999999999;
    st.orphans = st.tags = st.added_today = st.added_7d = 999999999;
    st.agents_total = st.agents_running = st.recent_count = st.inbox_total = 999999999;
    st.poll_seconds = st.age_seconds = 999999999;
    st.battery_pct = st.battery_mv = 999999999;
    st.partial_chain = st.full_refresh_ms = st.partial_refresh_ms = 999999999;

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

    char buf[2048];
    CHECK(device_api_json_state(&st, buf, sizeof(buf)) > 0);
    cJSON *r = cJSON_Parse(buf);
    CHECK(r != NULL);
    if (r) {
        check_bool(obj(r, "vault"), "valid", false);
        check_str(r, "deviceId", "");
        cJSON_Delete(r);
    }
}

int main(void)
{
    test_info();
    test_state_shape();
    test_korean_passes_through_as_utf8();
    test_control_characters_are_escaped();
    test_worst_case_fits_the_servers_buffer();
    test_overflow_yields_an_empty_string_not_half_a_document();
    test_null_state_is_rejected();
    test_zeroed_state_still_parses();
    TH_REPORT("api_json");
}
