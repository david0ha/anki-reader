/*
 * Host unit tests for vault_service.c.
 *
 * The layer is thin — http_get() plus vault_parse() — but it owns two things
 * nothing else does, and both are the kind that only misbehave on a bad day:
 * which failures are distinguished from each other, and whether *out survives
 * a failure.
 *
 * The HTTP port is the project's one platform seam, so a test can simply BE the
 * port: this file defines http_get() itself, and the linker takes it instead of
 * either the esp_http_client or the libcurl implementation. That is the same
 * property the simulator relies on, exercised here without a server.
 */
#include "th.h"

#include "http_port.h"
#include "vault_model.h"
#include "vault_service.h"

/* --- the fake port -------------------------------------------------------- */

static const char *g_body;      /* NULL = transport failure */
static int         g_status;
static int         g_calls;

void http_port_init(void) { }

char *http_get(const char *url, int *out_status)
{
    (void)url;
    g_calls++;
    if (out_status) *out_status = g_status;
    if (!g_body) return NULL;

    /* Returned on the heap, exactly as the real ports do. Whether
     * vault_service actually frees it is NOT asserted here — free() cannot be
     * intercepted portably, and macOS's ASan has no leak detector — so that one
     * stays a matter of reading the code. What this does buy is that every path
     * runs against a real allocation, so a double free or a use-after-free does
     * trap. */
    size_t n = strlen(g_body);
    char *p = (char *)malloc(n + 1);
    memcpy(p, g_body, n + 1);
    return p;
}

static void expect(const char *label, const char *body, int status,
                   vault_fetch_result_t want)
{
    g_body = body;
    g_status = status;

    vault_t v;
    memset(&v, 0, sizeof(v));
    vault_fetch_result_t got = vault_service_fetch("http://host/vault.json", &v);
    g_total++;
    if (got != want) {
        g_fail++;
        printf("  FAIL %s: got %s, want %s\n", label,
               vault_fetch_result_name(got), vault_fetch_result_name(want));
    }
}

/* --- tests ---------------------------------------------------------------- */

static void test_success(void)
{
    size_t len = 0;
    char *json = th_slurp(FIXDIR "/vault.json", &len);
    expect("good payload", json, 200, VAULT_FETCH_OK);

    /* And it must actually be parsed, not merely accepted. */
    g_body = json;
    g_status = 200;
    vault_t v;
    memset(&v, 0, sizeof(v));
    CHECK(vault_service_fetch("http://host/vault.json", &v) == VAULT_FETCH_OK);
    CHECK_INT(v.stats.notes, 1428);
    CHECK(v.valid == true);
    CHECK(v.demo == false);

    free(json);
}

static void test_no_url_is_not_a_failure(void)
{
    /* An unconfigured board is a supported, complete state — it shows the demo
     * snapshot. The transport must not even be reached. */
    vault_t v;
    memset(&v, 0, sizeof(v));
    g_calls = 0;
    g_body = "{}";
    g_status = 200;

    CHECK(vault_service_fetch("", &v) == VAULT_FETCH_NO_URL);
    CHECK(vault_service_fetch(NULL, &v) == VAULT_FETCH_NO_URL);
    CHECK_INT(g_calls, 0);

    /* A NULL destination is a programming error, not a fetch. */
    CHECK(vault_service_fetch("http://host/x", NULL) == VAULT_FETCH_NO_URL);
    CHECK_INT(g_calls, 0);
}

static void test_the_three_failures_are_distinguished(void)
{
    /* These are three different mistakes and they need three different names:
     * the log line is all a user has to go on, and "your URL is wrong" and
     * "your JSON is wrong" send them to different places. */
    expect("transport", NULL, 0, VAULT_FETCH_TRANSPORT);
    expect("404", "<html>Not Found</html>", 404, VAULT_FETCH_HTTP_STATUS);
    expect("500", "{\"stats\":{\"notes\":5}}", 500, VAULT_FETCH_HTTP_STATUS);
    expect("302", "", 302, VAULT_FETCH_HTTP_STATUS);
    expect("captive portal", "<html>Sign in</html>", 200, VAULT_FETCH_BAD_PAYLOAD);
    expect("empty object", "{}", 200, VAULT_FETCH_BAD_PAYLOAD);
    expect("truncated", "{\"stats\":{\"notes\":14", 200, VAULT_FETCH_BAD_PAYLOAD);
}

static void test_status_is_checked_before_the_body(void)
{
    /* A 500 whose body happens to be a perfectly good snapshot must still be a
     * status error. Servers do return stale caches with error codes, and
     * silently rendering one would hide an outage the user could otherwise see
     * in the header. */
    size_t len = 0;
    char *json = th_slurp(FIXDIR "/vault.json", &len);
    expect("valid body behind a 503", json, 503, VAULT_FETCH_HTTP_STATUS);
    free(json);
}

static void test_the_whole_2xx_range_is_accepted(void)
{
    size_t len = 0;
    char *json = th_slurp(FIXDIR "/vault.json", &len);
    expect("200", json, 200, VAULT_FETCH_OK);
    expect("201", json, 201, VAULT_FETCH_OK);
    expect("299", json, 299, VAULT_FETCH_OK);
    expect("199", json, 199, VAULT_FETCH_HTTP_STATUS);
    expect("300", json, 300, VAULT_FETCH_HTTP_STATUS);
    free(json);
}

static void test_a_failure_leaves_the_destination_untouched(void)
{
    /* The product requirement behind every failure path: a bad poll keeps the
     * previous dashboard on the glass. "Returns non-OK" does not guarantee it;
     * writing *out before validating would. */
    size_t len = 0;
    char *json = th_slurp(FIXDIR "/vault.json", &len);

    vault_t v;
    memset(&v, 0, sizeof(v));
    g_body = json;
    g_status = 200;
    CHECK(vault_service_fetch("http://host/x", &v) == VAULT_FETCH_OK);
    uint32_t before = vault_hash(&v);
    free(json);

    const struct { const char *body; int status; } bad[] = {
        { NULL,                   0   },
        { "<html>404</html>",     404 },
        { "{}",                   200 },
        { "not json at all",      200 },
        { "{\"stats\":{\"notes\"", 200 },
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        g_body = bad[i].body;
        g_status = bad[i].status;
        CHECK(vault_service_fetch("http://host/x", &v) != VAULT_FETCH_OK);
        CHECK_INT(vault_hash(&v), before);
    }
}

static void test_result_names_are_stable(void)
{
    /* These strings go into the log AND into /api/state's `lastResult`, where a
     * client may branch on them. */
    CHECK_STR(vault_fetch_result_name(VAULT_FETCH_OK), "ok");
    CHECK_STR(vault_fetch_result_name(VAULT_FETCH_NO_URL), "no_url");
    CHECK_STR(vault_fetch_result_name(VAULT_FETCH_TRANSPORT), "transport");
    CHECK_STR(vault_fetch_result_name(VAULT_FETCH_HTTP_STATUS), "http_status");
    CHECK_STR(vault_fetch_result_name(VAULT_FETCH_BAD_PAYLOAD), "bad_payload");
    CHECK_STR(vault_fetch_result_name((vault_fetch_result_t)99), "unknown");
}

int main(void)
{
    test_success();
    test_no_url_is_not_a_failure();
    test_the_three_failures_are_distinguished();
    test_status_is_checked_before_the_body();
    test_the_whole_2xx_range_is_accepted();
    test_a_failure_leaves_the_destination_untouched();
    test_result_names_are_stable();
    TH_REPORT("vault_service");
}
