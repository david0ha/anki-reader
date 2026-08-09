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
