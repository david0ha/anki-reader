/*
 * Host unit tests for the companion-app JSON serializers.
 *
 * These bytes go straight onto the wire to a phone, so the tests check two
 * different things: that the document parses (via the vendored cJSON the
 * firmware itself uses) and that the *field names* are what the app reads.
 * A serializer that emits valid JSON with a renamed key is still a broken API.
 */
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "device_api_json.h"

static int g_total = 0, g_fail = 0;

#define CHECK(cond) do { g_total++; if (!(cond)) { g_fail++; \
    printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

#define CHECK_STR(a, b) do { g_total++; const char *_a = (a); \
    if (_a == NULL || strcmp(_a, (b)) != 0) { g_fail++; \
    printf("  FAIL %s:%d  %s == \"%s\"  got \"%s\"\n", __FILE__, __LINE__, #a, (b), _a ? _a : "(null)"); } } while (0)

static void fill(device_state_t *st)
{
    memset(st, 0, sizeof(*st));
    snprintf(st->model, sizeof(st->model), "Ticker Board");
    snprintf(st->fw, sizeof(st->fw), "0.2.0");
    snprintf(st->device_id, sizeof(st->device_id), "1A2B");
    snprintf(st->ip, sizeof(st->ip), "192.168.0.42");
    st->page = 1;
    st->partial_chain = 3;

    st->fortune_valid = true;
    st->rank = 6;
    snprintf(st->rank_hanja, sizeof(st->rank_hanja), "大吉");
    snprintf(st->rank_hangul, sizeof(st->rank_hangul), "대길");
    snprintf(st->message, sizeof(st->message), "바라던 일이\n이루어집니다");

    st->iljin_index = 50;
    snprintf(st->iljin_hanja, sizeof(st->iljin_hanja), "甲寅");
    snprintf(st->iljin_hangul, sizeof(st->iljin_hangul), "갑인");

    snprintf(st->location, sizeof(st->location), "Seoul");
    st->wx_valid = true;
    st->wx_kind = 1;
    st->wx_temp_c = 28;
    snprintf(st->city, sizeof(st->city), "Seoul, KR");
    st->forecast_count = 3;
    const char *dows[3] = { "FRI", "SAT", "SUN" };
    for (int i = 0; i < 3; i++) {
        snprintf(st->forecast[i].dow, sizeof(st->forecast[i].dow), "%s", dows[i]);
        st->forecast[i].wx = i;
        st->forecast[i].lo = 15 + i;
        st->forecast[i].hi = 24 + i;
    }

    st->battery_valid = true;
    st->battery_pct = 84;
    st->battery_mv = 4012;
}

static void test_info(void)
{
    printf("test_info\n");
    char buf[256];
    int n = device_api_json_info(buf, sizeof(buf), "1A2B", "Ticker Board", "0.2.0", "192.168.0.42");
    CHECK(n > 0);
    CHECK((size_t)n == strlen(buf));

    cJSON *r = cJSON_Parse(buf);
    CHECK(r != NULL);
    if (!r) return;
    /* These four names are load-bearing: app/src/lib/discovery.ts probes this
     * endpoint on every candidate host and picks by `ip`. */
    CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(r, "deviceId")), "1A2B");
    CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(r, "model")), "Ticker Board");
    CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(r, "fw")), "0.2.0");
    CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(r, "ip")), "192.168.0.42");
    cJSON_Delete(r);

    /* An empty ip (not yet associated) is still a valid document. */
    CHECK(device_api_json_info(buf, sizeof(buf), "1A2B", "M", "1", "") > 0);
    cJSON *r2 = cJSON_Parse(buf);
    CHECK(r2 != NULL);
    CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(r2, "ip")), "");
    cJSON_Delete(r2);
}

static void test_state(void)
{
    printf("test_state\n");
    device_state_t st;
    fill(&st);

    char buf[2048];
    int n = device_api_json_state(&st, buf, sizeof(buf));
    CHECK(n > 0);
    CHECK((size_t)n == strlen(buf));

    cJSON *r = cJSON_Parse(buf);
    CHECK(r != NULL);
    if (!r) { printf("  document was: %s\n", buf); return; }

    CHECK(cJSON_GetObjectItem(r, "page")->valueint == 1);
    CHECK(cJSON_GetObjectItem(r, "partialChain")->valueint == 3);

    cJSON *f = cJSON_GetObjectItem(r, "fortune");
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(f, "valid")));
    CHECK(cJSON_GetObjectItem(f, "rank")->valueint == 6);
    /* Korean passes through as UTF-8, not \u escapes — and survives the
     * round trip byte for byte, including the embedded newline. */
    CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(f, "hanja")), "大吉");
    CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(f, "hangul")), "대길");
    CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(f, "message")),
              "바라던 일이\n이루어집니다");
    CHECK(strstr(buf, "\\n") != NULL);       /* the newline was escaped */
    CHECK(strstr(buf, "\\u") == NULL);       /* the Korean was not      */

    cJSON *ij = cJSON_GetObjectItem(r, "iljin");
    CHECK(cJSON_GetObjectItem(ij, "index")->valueint == 50);
    CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(ij, "hanja")), "甲寅");

    cJSON *w = cJSON_GetObjectItem(r, "weather");
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(w, "valid")));
    CHECK(cJSON_GetObjectItem(w, "tempC")->valueint == 28);
    CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(w, "city")), "Seoul, KR");
    cJSON *fc = cJSON_GetObjectItem(w, "forecast");
    CHECK(cJSON_IsArray(fc));
    CHECK(cJSON_GetArraySize(fc) == 3);
    cJSON *d0 = cJSON_GetArrayItem(fc, 0);
    CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(d0, "dow")), "FRI");
    CHECK(cJSON_GetObjectItem(d0, "hi")->valueint == 24);

    cJSON *b = cJSON_GetObjectItem(r, "battery");
    CHECK(cJSON_GetObjectItem(b, "percent")->valueint == 84);
    CHECK(cJSON_GetObjectItem(b, "millivolts")->valueint == 4012);
    cJSON_Delete(r);
}

static void test_edges(void)
{
    printf("test_edges\n");
    device_state_t st;
    char buf[2048];

    /* Zeroed state (before the first draw / first forecast) is still valid. */
    memset(&st, 0, sizeof(st));
    CHECK(device_api_json_state(&st, buf, sizeof(buf)) > 0);
    cJSON *r = cJSON_Parse(buf);
    CHECK(r != NULL);
    if (r) {
        cJSON *fc = cJSON_GetObjectItem(cJSON_GetObjectItem(r, "weather"), "forecast");
        CHECK(cJSON_IsArray(fc) && cJSON_GetArraySize(fc) == 0);
        CHECK(cJSON_IsFalse(cJSON_GetObjectItem(cJSON_GetObjectItem(r, "fortune"), "valid")));
        cJSON_Delete(r);
    }

    /* An out-of-range forecast_count is clamped, not trusted. */
    fill(&st);
    st.forecast_count = 99;
    CHECK(device_api_json_state(&st, buf, sizeof(buf)) > 0);
    r = cJSON_Parse(buf);
    CHECK(r != NULL);
    if (r) {
        CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(cJSON_GetObjectItem(r, "weather"),
                                                     "forecast")) == DEV_FORECAST_MAX);
        cJSON_Delete(r);
    }
    st.forecast_count = -5;
    CHECK(device_api_json_state(&st, buf, sizeof(buf)) > 0);

    /* Quotes and backslashes in a city name (Open-Meteo returns free text) are
     * escaped rather than breaking the document. */
    fill(&st);
    snprintf(st.city, sizeof(st.city), "A\"B\\C\tD");
    CHECK(device_api_json_state(&st, buf, sizeof(buf)) > 0);
    r = cJSON_Parse(buf);
    CHECK(r != NULL);
    if (r) {
        CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(r, "weather"),
                                                           "city")), "A\"B\\C\tD");
        cJSON_Delete(r);
    }

    /* Every truncation point: a buffer one byte short at any stage must report
     * failure AND leave an empty string, never a half-written document that a
     * caller might send anyway. */
    fill(&st);
    int full = device_api_json_state(&st, buf, sizeof(buf));
    CHECK(full > 0);
    for (int cap = 1; cap < full; cap++) {
        char small[2048];
        memset(small, 'X', sizeof(small));
        int rc = device_api_json_state(&st, small, (size_t)cap);
        if (rc != -1 || small[0] != '\0') {
            g_fail++;
            printf("  FAIL cap=%d: rc=%d first byte=%d (want -1 and '')\n", cap, rc, small[0]);
            break;
        }
        g_total++;
    }
    CHECK(device_api_json_state(&st, buf, 0) == -1);
    CHECK(device_api_json_state(NULL, buf, sizeof(buf)) == -1);
    CHECK(device_api_json_info(buf, 4, "1A2B", "Ticker Board", "0.2.0", "1.2.3.4") == -1);
    CHECK(buf[0] == '\0');
}

int main(void)
{
    test_info();
    test_state();
    test_edges();
    printf("%s  %d checks, %d failed\n", g_fail ? "FAILED" : "ok", g_total, g_fail);
    return g_fail ? 1 : 0;
}
