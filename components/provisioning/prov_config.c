#include "prov_config.h"

#include <string.h>

prov_cred_result_t prov_validate_credentials(const char *ssid, const char *password)
{
    if (ssid == NULL) {
        ssid = "";
    }
    if (password == NULL) {
        password = "";
    }
    if (ssid[0] == '\0') {
        return PROV_CRED_SSID_EMPTY;
    }
    if (strlen(ssid) > PROV_SSID_MAX_LEN) {
        return PROV_CRED_SSID_TOO_LONG;
    }
    if (strlen(password) > PROV_PASS_MAX_LEN) {
        return PROV_CRED_PASS_TOO_LONG;
    }
    return PROV_CRED_OK;
}

bool prov_validate_vault_url(const char *url)
{
    if (url == NULL || url[0] == '\0') {
        return true;                      // empty == run on the demo snapshot
    }
    if (strlen(url) > PROV_URL_MAX_LEN) {
        return false;
    }

    const char *rest;
    if (strncmp(url, "http://", 7) == 0) {
        rest = url + 7;
    } else if (strncmp(url, "https://", 8) == 0) {
        rest = url + 8;
    } else {
        return false;
    }

    // Require at least one character of host before the path. "http://" and
    // "http:///vault.json" both parse as a URL and both fail at connect time
    // with an error the user cannot act on.
    return rest[0] != '\0' && rest[0] != '/';
}
