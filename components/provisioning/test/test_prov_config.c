#include "tf.h"
#include "prov_config.h"

/* prov_validate_credentials is the whole of prov_config now that the watchlist
 * is gone, and it is the gate the companion app's error codes are derived from
 * (POST /api/provision maps each result to a typed Esp32Error). */

TEST(credentials_accept_a_normal_network)
{
    CHECK(prov_validate_credentials("My Net", "hunter2") == PROV_CRED_OK);
}

TEST(credentials_allow_an_open_network)
{
    /* An empty password is a legitimate open network, not a mistake. */
    CHECK(prov_validate_credentials("Cafe WiFi", "") == PROV_CRED_OK);
    CHECK(prov_validate_credentials("Cafe WiFi", NULL) == PROV_CRED_OK);
}

TEST(credentials_reject_an_empty_ssid)
{
    CHECK(prov_validate_credentials("", "pw") == PROV_CRED_SSID_EMPTY);
    CHECK(prov_validate_credentials(NULL, "pw") == PROV_CRED_SSID_EMPTY);
}

TEST(credentials_enforce_the_802_11_limits)
{
    char ssid[PROV_SSID_MAX_LEN + 8];
    char pass[PROV_PASS_MAX_LEN + 8];

    /* Exactly at the limit is valid — off-by-one here would reject legitimate
     * networks with no way for the user to tell why. */
    for (int i = 0; i < PROV_SSID_MAX_LEN; i++) ssid[i] = 'a';
    ssid[PROV_SSID_MAX_LEN] = '\0';
    CHECK(prov_validate_credentials(ssid, "pw") == PROV_CRED_OK);

    ssid[PROV_SSID_MAX_LEN] = 'a';
    ssid[PROV_SSID_MAX_LEN + 1] = '\0';
    CHECK(prov_validate_credentials(ssid, "pw") == PROV_CRED_SSID_TOO_LONG);

    for (int i = 0; i < PROV_PASS_MAX_LEN; i++) pass[i] = 'x';
    pass[PROV_PASS_MAX_LEN] = '\0';
    CHECK(prov_validate_credentials("net", pass) == PROV_CRED_OK);

    pass[PROV_PASS_MAX_LEN] = 'x';
    pass[PROV_PASS_MAX_LEN + 1] = '\0';
    CHECK(prov_validate_credentials("net", pass) == PROV_CRED_PASS_TOO_LONG);
}

TEST(credentials_check_the_ssid_before_the_password)
{
    /* Both wrong: the caller shows one message, and "enter a network name" is
     * the more useful one. */
    char pass[PROV_PASS_MAX_LEN + 8];
    for (int i = 0; i < PROV_PASS_MAX_LEN + 1; i++) pass[i] = 'x';
    pass[PROV_PASS_MAX_LEN + 1] = '\0';
    CHECK(prov_validate_credentials("", pass) == PROV_CRED_SSID_EMPTY);
}

/* prov_validate_study_url is the gate on the one field a user types by hand and
 * gets wrong. Its job is not to be a URL parser — it is to catch the two
 * mistakes people actually make (no scheme at all, and a path with no host) and
 * to let everything else through, because a device that rejects a legitimate
 * URL it does not recognise is worse than one that fails at connect time with a
 * message the user can read in the log. */

TEST(study_url_accepts_empty_as_the_demo_screen)
{
    /* Not an oversight: an unconfigured board is a complete product that shows
     * the built-in snapshot. Rejecting empty would force everyone to stand up a
     * server before the display would boot into anything. */
    CHECK(prov_validate_study_url("") == true);
    CHECK(prov_validate_study_url(NULL) == true);
}

TEST(study_url_accepts_the_shapes_a_lan_actually_uses)
{
    CHECK(prov_validate_study_url("http://macbook.local:8123/vault.json") == true);
    CHECK(prov_validate_study_url("http://192.168.1.42:8123/vault.json") == true);
    CHECK(prov_validate_study_url("https://vault.example.com/snapshot") == true);
    CHECK(prov_validate_study_url("http://host") == true);          /* no path */
    CHECK(prov_validate_study_url("http://h/a?b=c&d=e") == true);    /* query   */
}

TEST(study_url_rejects_a_missing_scheme)
{
    /* The single most common paste. Without a scheme esp_http_client does not
     * fail loudly — it fails obscurely. */
    CHECK(prov_validate_study_url("macbook.local:8123/vault.json") == false);
    CHECK(prov_validate_study_url("192.168.1.42") == false);
    CHECK(prov_validate_study_url("file:///Users/me/vault.json") == false);
    CHECK(prov_validate_study_url("ftp://host/x") == false);
}

TEST(study_url_rejects_a_scheme_with_no_host)
{
    CHECK(prov_validate_study_url("http://") == false);
    CHECK(prov_validate_study_url("https://") == false);
    CHECK(prov_validate_study_url("http:///vault.json") == false);
}

TEST(study_url_enforces_the_stored_length)
{
    /* Exactly at the limit must pass: prov_config_t has room for
     * PROV_URL_MAX_LEN characters plus the NUL, and rejecting the last one
     * would waste a byte nobody can see. */
    char url[PROV_URL_MAX_LEN + 8];
    const char *prefix = "http://h/";
    size_t plen = strlen(prefix);
    memcpy(url, prefix, plen);
    for (size_t i = plen; i < PROV_URL_MAX_LEN; i++) url[i] = 'a';
    url[PROV_URL_MAX_LEN] = '\0';
    CHECK(prov_validate_study_url(url) == true);

    url[PROV_URL_MAX_LEN] = 'a';
    url[PROV_URL_MAX_LEN + 1] = '\0';
    CHECK(prov_validate_study_url(url) == false);
}
