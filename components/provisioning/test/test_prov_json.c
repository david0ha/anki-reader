#include "tf.h"
#include "prov_json.h"
#include "prov_config.h"
#include "prov_wifi.h"   // prov_ap_t

// --- prov_json_escape --------------------------------------------------------

TEST(json_escape_passes_plain_text)
{
    char out[64];
    int n = prov_json_escape("Home 5G", out, sizeof(out));
    CHECK_INT(n, 7);
    CHECK_STR(out, "Home 5G");
}

TEST(json_escape_escapes_quote_and_backslash)
{
    char out[64];
    prov_json_escape("a\"b\\c", out, sizeof(out));
    CHECK_STR(out, "a\\\"b\\\\c");
}

TEST(json_escape_escapes_control_chars)
{
    char out[64];
    prov_json_escape("a\nb\tc\x01""d", out, sizeof(out));
    CHECK_STR(out, "a\\nb\\tc\\u0001d");
}

TEST(json_escape_passes_utf8_untouched)
{
    char out[64];
    // Korean text (U+C6B0 U+B9AC U+C9D1) must pass through byte-for-byte (no mojibake).
    prov_json_escape("\xec\x9a\xb0\xeb\xa6\xac\xec\xa7\x91", out, sizeof(out));
    CHECK_STR(out, "\xec\x9a\xb0\xeb\xa6\xac\xec\xa7\x91");
}

TEST(json_escape_null_is_empty)
{
    char out[8];
    CHECK_INT(prov_json_escape(NULL, out, sizeof(out)), 0);
    CHECK_STR(out, "");
}

TEST(json_escape_overflow_yields_empty)
{
    char out[4];
    CHECK_INT(prov_json_escape("toolong", out, sizeof(out)), -1);
    CHECK_STR(out, "");
}

// --- prov_json_info ----------------------------------------------------------

TEST(json_info_basic)
{
    char out[256];
    int n = prov_json_info(out, sizeof(out), "9F3A", "Ticker Board", "Ticker Board-9F3A");
    CHECK(n > 0);
    CHECK_STR(out, "{\"deviceId\":\"9F3A\",\"model\":\"Ticker Board\",\"apSsid\":\"Ticker Board-9F3A\"}");
}

TEST(json_info_null_fields_empty)
{
    char out[128];
    prov_json_info(out, sizeof(out), NULL, NULL, NULL);
    CHECK_STR(out, "{\"deviceId\":\"\",\"model\":\"\",\"apSsid\":\"\"}");
}

// --- prov_json_status --------------------------------------------------------

TEST(json_status_state_only)
{
    char out[64];
    prov_json_status(out, sizeof(out), "idle", NULL, NULL);
    CHECK_STR(out, "{\"state\":\"idle\"}");
}

TEST(json_status_with_ssid)
{
    char out[128];
    prov_json_status(out, sizeof(out), "connecting", "Home 5G", NULL);
    CHECK_STR(out, "{\"state\":\"connecting\",\"ssid\":\"Home 5G\"}");
}

TEST(json_status_with_ssid_and_reason)
{
    char out[128];
    prov_json_status(out, sizeof(out), "failed", "Home 5G", "auth_failed");
    CHECK_STR(out, "{\"state\":\"failed\",\"ssid\":\"Home 5G\",\"reason\":\"auth_failed\"}");
}

TEST(json_status_empty_optional_fields_omitted)
{
    char out[64];
    prov_json_status(out, sizeof(out), "idle", "", "");
    CHECK_STR(out, "{\"state\":\"idle\"}");
}

// --- prov_json_networks ------------------------------------------------------

TEST(json_networks_empty)
{
    char out[64];
    prov_json_networks(NULL, 0, out, sizeof(out));
    CHECK_STR(out, "{\"networks\":[]}");
}

TEST(json_networks_two_entries)
{
    prov_ap_t aps[2] = {
        { .ssid = "Home 5G", .rssi = -48, .secure = true },
        { .ssid = "Cafe",    .rssi = -72, .secure = false },
    };
    char out[256];
    prov_json_networks(aps, 2, out, sizeof(out));
    CHECK_STR(out,
        "{\"networks\":["
        "{\"ssid\":\"Home 5G\",\"rssi\":-48,\"secure\":true},"
        "{\"ssid\":\"Cafe\",\"rssi\":-72,\"secure\":false}]}");
}

TEST(json_networks_truncates_on_overflow_but_stays_valid)
{
    prov_ap_t aps[3] = {
        { .ssid = "AAAAAAAA", .rssi = -40, .secure = true },
        { .ssid = "BBBBBBBB", .rssi = -50, .secure = true },
        { .ssid = "CCCCCCCC", .rssi = -60, .secure = true },
    };
    // Big enough for one entry + the closing "]}" but not all three.
    char out[80];
    int n = prov_json_networks(aps, 3, out, sizeof(out));
    CHECK(n > 0);
    // Ends with a valid array close, never a dangling comma.
    size_t len = strlen(out);
    CHECK(len >= 2);
    CHECK_STR(out + len - 2, "]}");
    CHECK(out[len - 3] != ',');
}

// --- prov_validate_credentials ----------------------------------------------

TEST(validate_ok)
{
    CHECK_INT(prov_validate_credentials("Home", "secret123"), PROV_CRED_OK);
}

TEST(validate_empty_password_allowed)
{
    CHECK_INT(prov_validate_credentials("OpenNet", ""), PROV_CRED_OK);
    CHECK_INT(prov_validate_credentials("OpenNet", NULL), PROV_CRED_OK);
}

TEST(validate_empty_ssid_rejected)
{
    CHECK_INT(prov_validate_credentials("", "x"), PROV_CRED_SSID_EMPTY);
    CHECK_INT(prov_validate_credentials(NULL, "x"), PROV_CRED_SSID_EMPTY);
}

TEST(validate_ssid_too_long)
{
    char ssid[40];
    memset(ssid, 'A', sizeof(ssid));
    ssid[33] = '\0';                  // 33 chars > PROV_SSID_MAX_LEN (32)
    CHECK_INT(prov_validate_credentials(ssid, "x"), PROV_CRED_SSID_TOO_LONG);
    ssid[PROV_SSID_MAX_LEN] = '\0';   // exactly 32 chars -> OK
    CHECK_INT(prov_validate_credentials(ssid, "x"), PROV_CRED_OK);
}

TEST(validate_pass_too_long)
{
    char pass[80];
    memset(pass, 'p', sizeof(pass));
    pass[65] = '\0';                  // 65 chars > PROV_PASS_MAX_LEN (64)
    CHECK_INT(prov_validate_credentials("Net", pass), PROV_CRED_PASS_TOO_LONG);
    pass[PROV_PASS_MAX_LEN] = '\0';   // exactly 64 chars -> OK
    CHECK_INT(prov_validate_credentials("Net", pass), PROV_CRED_OK);
}
