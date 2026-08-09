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
